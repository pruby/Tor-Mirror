/* Copyright 2004 Roger Dingledine */
/* See LICENSE for licensing information */
/* $Id$ */

/**
 * \file rendservice.c
 * \brief The hidden-service side of rendezvous functionality.
 **/

#include "or.h"

extern or_options_t options; /* command-line and config-file options */

static circuit_t *find_intro_circuit(routerinfo_t *router, const char *pk_digest);

/** Represents the mapping from a virtual port of a rendezvous service to
 * a real port on some IP.
 */
typedef struct rend_service_port_config_t {
  uint16_t virtual_port;
  uint16_t real_port;
  uint32_t real_address;
} rend_service_port_config_t;

/** Try to maintain this many intro points per service if possible. */
#define NUM_INTRO_POINTS 3

/** If we can't build our intro circuits, don't retry for this long. */
#define INTRO_CIRC_RETRY_PERIOD 60*5
/** Don't try to build more than this many circuits before giving up
 * for a while.*/
#define MAX_INTRO_CIRCS_PER_PERIOD 10

/** Represents a single hidden service running at this OP. */
typedef struct rend_service_t {
  /** Fields specified in config file */
  char *directory; /**< where in the filesystem it stores it */
  smartlist_t *ports; /**< List of rend_service_port_config_t */
  char *intro_prefer_nodes; /**< comma-separated list of nicknames */
  char *intro_exclude_nodes; /**< comma-separated list of nicknames */
  /* Other fields */
  crypto_pk_env_t *private_key;
  char service_id[REND_SERVICE_ID_LEN+1];
  char pk_digest[DIGEST_LEN];
  smartlist_t *intro_nodes; /**< list of nicknames for intro points we have,
                             * or are trying to establish. */
  time_t intro_period_started;
  int n_intro_circuits_launched; /**< count of intro circuits we have
                                  * established in this period. */
  rend_service_descriptor_t *desc;
  int desc_is_dirty;
} rend_service_t;

/** A list of rend_service_t's for services run on this OP.
 */
static smartlist_t *rend_service_list = NULL;

/** Release the storage held by <b>service</b>.
 */
static void rend_service_free(rend_service_t *service)
{
  if (!service) return;
  tor_free(service->directory);
  SMARTLIST_FOREACH(service->ports, void*, p, tor_free(p));
  smartlist_free(service->ports);
  if (service->private_key)
    crypto_free_pk_env(service->private_key);
  tor_free(service->intro_prefer_nodes);
  tor_free(service->intro_exclude_nodes);
  SMARTLIST_FOREACH(service->intro_nodes, void*, p, tor_free(p));
  smartlist_free(service->intro_nodes);
  if (service->desc)
    rend_service_descriptor_free(service->desc);
  tor_free(service);
}

/** Release all the storage held in rend_service_list, and allocate a new,
 * empty rend_service_list.
 */
static void rend_service_free_all(void)
{
  if (!rend_service_list) {
    rend_service_list = smartlist_create();
    return;
  }
  SMARTLIST_FOREACH(rend_service_list, rend_service_t*, ptr,
                    rend_service_free(ptr));
  smartlist_free(rend_service_list);
  rend_service_list = smartlist_create();
}

/** Validate <b>service</b> and add it to rend_service_list if possible.
 */
static void add_service(rend_service_t *service)
{
  int i;
  rend_service_port_config_t *p;
  struct in_addr addr;

  if (!service->intro_prefer_nodes)
    service->intro_prefer_nodes = tor_strdup("");
  if (!service->intro_exclude_nodes)
    service->intro_exclude_nodes = tor_strdup("");

  if (!smartlist_len(service->ports)) {
    log_fn(LOG_WARN, "Hidden service with no ports configured; ignoring.");
    rend_service_free(service);
  } else {
    smartlist_set_capacity(service->ports, -1);
    smartlist_add(rend_service_list, service);
    log_fn(LOG_DEBUG,"Configuring service with directory %s",service->directory);
    for (i = 0; i < smartlist_len(service->ports); ++i) {
      p = smartlist_get(service->ports, i);
      addr.s_addr = htonl(p->real_address);
      log_fn(LOG_DEBUG,"Service maps port %d to %s:%d",
             p->virtual_port, inet_ntoa(addr), p->real_port);
    }
  }
}

/** Parses a real-port to virtual-port mapping and returns a new
 * rend_service_port_config_t.
 *
 * The format is: VirtualPort (IP|RealPort|IP:RealPort)?
 *
 * IP defaults to 127.0.0.1; RealPort defaults to VirtualPort.
 */
static rend_service_port_config_t *parse_port_config(const char *string)
{
  int virtport;
  uint16_t realport;
  uint32_t addr;
  char *endptr;
  rend_service_port_config_t *result;

  virtport = (int) strtol(string, &endptr, 10);
  if (endptr == string) {
    log_fn(LOG_WARN, "Missing port in hidden service port configuration");
    return NULL;
  }
  if (virtport < 1 || virtport > 65535) {
    log_fn(LOG_WARN, "Port out of range in hidden service port configuration");
    return NULL;
  }
  string = endptr + strspn(endptr, " \t");
  if (!*string) {
    /* No addr:port part; use default. */
    realport = virtport;
    addr = 0x7F000001u; /* 127.0.0.1 */
  } else if (strchr(string, ':') || strchr(string, '.')) {
    if (parse_addr_port(string, NULL, &addr, &realport)<0) {
      log_fn(LOG_WARN,"Unparseable address in hidden service port configuration");
      return NULL;
    }
    if (!realport)
      realport = virtport;
  } else {
    /* No addr:port, no addr -- must be port. */
    realport = strtol(string, &endptr, 10);
    if (*endptr) {
      log_fn(LOG_WARN, "Unparseable of missing port in hidden service port configuration.");
      return NULL;
    }
    if (realport < 1 || realport > 65535) {
      log_fn(LOG_WARN, "Port out of range");
      return NULL;
    }
    addr = 0x7F000001u; /* Default to 127.0.0.1 */
  }

  result = tor_malloc(sizeof(rend_service_port_config_t));
  result->virtual_port = virtport;
  result->real_port = realport;
  result->real_address = addr;
  return result;
}

/** Set up rend_service_list, based on the values of HiddenServiceDir and
 * HiddenServicePort in <b>options</b>.  Return 0 on success and -1 on
 * failure.
 */
int rend_config_services(or_options_t *options)
{
  struct config_line_t *line;
  rend_service_t *service = NULL;
  rend_service_port_config_t *portcfg;
  rend_service_free_all();

  for (line = options->RendConfigLines; line; line = line->next) {
    if (!strcasecmp(line->key, "HiddenServiceDir")) {
      if (service)
        add_service(service);
      service = tor_malloc_zero(sizeof(rend_service_t));
      service->directory = tor_strdup(line->value);
      service->ports = smartlist_create();
      service->intro_nodes = smartlist_create();
      service->intro_period_started = time(NULL);
      continue;
    }
    if (!service) {
      log_fn(LOG_WARN, "HiddenServicePort with no preceeding HiddenServiceDir directive");
      rend_service_free(service);
      return -1;
    }
    if (!strcasecmp(line->key, "HiddenServicePort")) {
      portcfg = parse_port_config(line->value);
      if (!portcfg) {
        rend_service_free(service);
        return -1;
      }
      smartlist_add(service->ports, portcfg);
    } else if (!strcasecmp(line->key, "HiddenServiceNodes")) {
      if (service->intro_prefer_nodes) {
        log_fn(LOG_WARN, "Got multiple HiddenServiceNodes lines for a single service");
        return -1;
      }
      service->intro_prefer_nodes = tor_strdup(line->value);
    } else {
      tor_assert(!strcasecmp(line->key, "HiddenServiceExcludeNodes"));
      if (service->intro_exclude_nodes) {
        log_fn(LOG_WARN, "Got multiple HiddenServiceExcludedNodes lines for a single service");
        return -1;
      }
      service->intro_exclude_nodes = tor_strdup(line->value);
    }
  }
  if (service)
    add_service(service);

  return 0;
}

/** Replace the old value of <b>service</b>-\>desc with one that reflects
 * the other fields in service.
 */
static void rend_service_update_descriptor(rend_service_t *service)
{
  rend_service_descriptor_t *d;
  circuit_t *circ;
  int i,n;
  routerinfo_t *router;

  if (service->desc) {
    rend_service_descriptor_free(service->desc);
    service->desc = NULL;
  }
  d = service->desc = tor_malloc(sizeof(rend_service_descriptor_t));
  d->pk = crypto_pk_dup_key(service->private_key);
  d->timestamp = time(NULL);
  n = smartlist_len(service->intro_nodes);
  d->n_intro_points = 0;
  d->intro_points = tor_malloc(sizeof(char*)*n);
  for (i=0; i < n; ++i) {
    router = router_get_by_nickname(smartlist_get(service->intro_nodes, i));
    if(!router) {
      log_fn(LOG_WARN,"Router '%s' not found. Skipping.",
             (char*)smartlist_get(service->intro_nodes, i));
      continue;
    }
    circ = find_intro_circuit(router, service->pk_digest);
    if (circ && circ->purpose == CIRCUIT_PURPOSE_S_INTRO) {
      /* We have an entirely established intro circuit. */
      d->intro_points[d->n_intro_points++] = tor_strdup(router->nickname);
    }
  }
}

/** Load and/or generate private keys for all hidden services.  Return 0 on
 * success, -1 on failure.
 */
int rend_service_load_keys(void)
{
  int i;
  rend_service_t *s;
  char fname[512];
  char buf[128];

  for (i=0; i < smartlist_len(rend_service_list); ++i) {
    s = smartlist_get(rend_service_list,i);
    if (s->private_key)
      continue;
    log_fn(LOG_INFO, "Loading hidden-service keys from '%s'", s->directory);

    /* Check/create directory */
    if (check_private_dir(s->directory, 1) < 0)
      return -1;

    /* Load key */
    if (strlcpy(fname,s->directory,sizeof(fname)) >= sizeof(fname) ||
        strlcat(fname,"/private_key",sizeof(fname)) >= sizeof(fname)) {
      log_fn(LOG_WARN, "Directory name too long: '%s'", s->directory);
      return -1;
    }
    s->private_key = init_key_from_file(fname);
    if (!s->private_key)
      return -1;

    /* Create service file */
    if (rend_get_service_id(s->private_key, s->service_id)<0) {
      log_fn(LOG_WARN, "Couldn't encode service ID");
      return -1;
    }
    if (crypto_pk_get_digest(s->private_key, s->pk_digest)<0) {
      log_fn(LOG_WARN, "Couldn't compute hash of public key");
      return -1;
    }
    if (strlcpy(fname,s->directory,sizeof(fname)) >= sizeof(fname) ||
        strlcat(fname,"/hostname",sizeof(fname)) >= sizeof(fname)) {
      log_fn(LOG_WARN, "Directory name too long: '%s'", s->directory);
      return -1;
    }
    sprintf(buf, "%s.onion\n", s->service_id);
    if (write_str_to_file(fname,buf,0)<0)
      return -1;
  }
  return 0;
}

/** Return the service whose public key has a digest of <b>digest</b>. Return
 * NULL if no such service exists.
 */
static rend_service_t *
rend_service_get_by_pk_digest(const char* digest)
{
  SMARTLIST_FOREACH(rend_service_list, rend_service_t*, s,
                    if (!memcmp(s->pk_digest,digest,DIGEST_LEN)) return s);
  return NULL;
}

/******
 * Handle cells
 ******/

/** Respond to an INTRODUCE2 cell by launching a circuit to the chosen
 * rendezvous points.
 */
int
rend_service_introduce(circuit_t *circuit, const char *request, int request_len)
{
  char *ptr, *rp_nickname, *r_cookie;
  char buf[RELAY_PAYLOAD_SIZE];
  char keys[DIGEST_LEN+CPATH_KEY_MATERIAL_LEN]; /* Holds KH, Df, Db, Kf, Kb */
  rend_service_t *service;
  int len, keylen;
  crypto_dh_env_t *dh = NULL;
  circuit_t *launched = NULL;
  crypt_path_t *cpath = NULL;
  char serviceid[REND_SERVICE_ID_LEN+1];
  char hexcookie[9];
  int version;
  int nickname_field_len;

  base32_encode(serviceid, REND_SERVICE_ID_LEN+1,
                circuit->rend_pk_digest,10);
  log_fn(LOG_INFO, "Received INTRODUCE2 cell for service %s on circ %d",
         serviceid, circuit->n_circ_id);

  if (circuit->purpose != CIRCUIT_PURPOSE_S_INTRO) {
    log_fn(LOG_WARN, "Got an INTRODUCE2 over a non-introduction circuit %d",
           circuit->n_circ_id);
    return -1;
  }

  /* min key length plus digest length plus nickname length */
  if (request_len < DIGEST_LEN+REND_COOKIE_LEN+(MAX_NICKNAME_LEN+1)+
      DH_KEY_LEN+42){
    log_fn(LOG_WARN, "Got a truncated INTRODUCE2 cell on circ %d",
           circuit->n_circ_id);
    return -1;
  }

  /* first DIGEST_LEN bytes of request is service pk digest */
  service = rend_service_get_by_pk_digest(request);
  if (!service) {
    log_fn(LOG_WARN, "Got an INTRODUCE2 cell for an unrecognized service %s",
           serviceid);
    return -1;
  }
  if (memcmp(circuit->rend_pk_digest, request, DIGEST_LEN)) {
    base32_encode(serviceid, REND_SERVICE_ID_LEN+1, request, 10);
    log_fn(LOG_WARN, "Got an INTRODUCE2 cell for the wrong service (%s)",
           serviceid);
    return -1;
  }

  keylen = crypto_pk_keysize(service->private_key);
  if (request_len < keylen+DIGEST_LEN) {
    log_fn(LOG_WARN, "PK-encrypted portion of INTRODUCE2 cell was truncated");
    return -1;
  }
  /* Next N bytes is encrypted with service key */
  len = crypto_pk_private_hybrid_decrypt(
       service->private_key,request+DIGEST_LEN,request_len-DIGEST_LEN,buf,
       PK_PKCS1_OAEP_PADDING,1);
  if (len<0) {
    log_fn(LOG_WARN, "Couldn't decrypt INTRODUCE2 cell");
    return -1;
  }
  if (*buf == 1) {
    rp_nickname = buf+1;
    nickname_field_len = HEX_DIGEST_LEN+2;
    version = 1;
  } else {
    nickname_field_len = MAX_NICKNAME_LEN+1;
    rp_nickname = buf;
    version = 0;
  }
  ptr=memchr(rp_nickname,0,nickname_field_len);
  if (!ptr || ptr == rp_nickname) {
    log_fn(LOG_WARN, "Couldn't find a null-padded nickname in INTRODUCE2 cell");
    return -1;
  }
  if ((version == 0 && !is_legal_nickname(rp_nickname)) ||
      (version == 1 && !is_legal_nickname_or_hexdigest(rp_nickname)) ||
      (int)strspn(buf,LEGAL_NICKNAME_CHARACTERS) != ptr-buf) {
    log_fn(LOG_WARN, "Bad nickname in INTRODUCE2 cell.");
    return -1;
  }
  /* Okay, now we know that a nickname is at the start of the buffer. */
  ptr = rp_nickname+nickname_field_len;
  len -= nickname_field_len;
  if (len != REND_COOKIE_LEN+DH_KEY_LEN) {
    log_fn(LOG_WARN, "Bad length for INTRODUCE2 cell.");
    return -1;
  }
  r_cookie = ptr;
  base16_encode(hexcookie,9,r_cookie,4);

  /* Try DH handshake... */
  dh = crypto_dh_new();
  if (!dh || crypto_dh_generate_public(dh)<0) {
    log_fn(LOG_WARN, "Couldn't build DH state or generate public key");
    goto err;
  }
  if (crypto_dh_compute_secret(dh, ptr+REND_COOKIE_LEN, DH_KEY_LEN, keys,
                               DIGEST_LEN+CPATH_KEY_MATERIAL_LEN)<0) {
    log_fn(LOG_WARN, "Couldn't complete DH handshake");
    goto err;
  }

  /* Launch a circuit to alice's chosen rendezvous point.
   */
  launched = circuit_launch_by_nickname(CIRCUIT_PURPOSE_S_CONNECT_REND, rp_nickname);
  log_fn(LOG_INFO,
        "Accepted intro; launching circuit to '%s' (cookie %s) for service %s",
         rp_nickname, hexcookie, serviceid);
  if (!launched) {
    log_fn(LOG_WARN,
           "Can't launch circuit to rendezvous point '%s' for service %s",
           rp_nickname, serviceid);
    return -1;
  }
  tor_assert(launched->build_state);
  /* Fill in the circuit's state. */
  memcpy(launched->rend_pk_digest, circuit->rend_pk_digest,
         DIGEST_LEN);
  memcpy(launched->rend_cookie, r_cookie, REND_COOKIE_LEN);
  strcpy(launched->rend_query, service->service_id);
  launched->build_state->pending_final_cpath = cpath =
    tor_malloc_zero(sizeof(crypt_path_t));

  cpath->handshake_state = dh;
  dh = NULL;
  if (circuit_init_cpath_crypto(cpath,keys+DIGEST_LEN,1)<0)
    goto err;
  memcpy(cpath->handshake_digest, keys, DIGEST_LEN);

  return 0;
 err:
  if (dh) crypto_dh_free(dh);
  if (launched) circuit_mark_for_close(launched);
  return -1;
}

/** How many times will a hidden service operator attempt to connect to
 * a requested rendezvous point before giving up? */
#define MAX_REND_FAILURES 3

/** Called when we fail building a rendezvous circuit at some point other
 * than the last hop: launches a new circuit to the same rendezvous point.
 */
void
rend_service_relaunch_rendezvous(circuit_t *oldcirc)
{
  circuit_t *newcirc;
  cpath_build_state_t *newstate, *oldstate;

  tor_assert(oldcirc->purpose == CIRCUIT_PURPOSE_S_CONNECT_REND);

  if (!oldcirc->build_state ||
      oldcirc->build_state->failure_count > MAX_REND_FAILURES) {
    log_fn(LOG_INFO,"Attempt to build circuit to %s for rendezvous has failed too many times; giving up.",
           oldcirc->build_state->chosen_exit_name);
    return;
  }

  log_fn(LOG_INFO,"Reattempting rendezvous circuit to %s",
         oldcirc->build_state->chosen_exit_name);

  newcirc = circuit_launch_by_nickname(CIRCUIT_PURPOSE_S_CONNECT_REND,
                               oldcirc->build_state->chosen_exit_name);
  if (!newcirc) {
    log_fn(LOG_WARN,"Couldn't relaunch rendezvous circuit to %s",
           oldcirc->build_state->chosen_exit_name);
    return;
  }
  oldstate = oldcirc->build_state;
  newstate = newcirc->build_state;
  tor_assert(newstate && oldstate);
  newstate->failure_count = oldstate->failure_count+1;
  newstate->pending_final_cpath = oldstate->pending_final_cpath;
  oldstate->pending_final_cpath = NULL;

  memcpy(newcirc->rend_query, oldcirc->rend_query, REND_SERVICE_ID_LEN+1);
  memcpy(newcirc->rend_pk_digest, oldcirc->rend_pk_digest, DIGEST_LEN);
  memcpy(newcirc->rend_cookie, oldcirc->rend_cookie, REND_COOKIE_LEN);
}

/** Launch a circuit to serve as an introduction point for the service
 * <b>service</b> at the introduction point <b>nickname</b>
 */
static int
rend_service_launch_establish_intro(rend_service_t *service, const char *nickname)
{
  circuit_t *launched;

  log_fn(LOG_INFO, "Launching circuit to introduction point %s for service %s",
         nickname, service->service_id);

  ++service->n_intro_circuits_launched;
  launched = circuit_launch_by_nickname(CIRCUIT_PURPOSE_S_ESTABLISH_INTRO, nickname);
  if (!launched) {
    log_fn(LOG_WARN, "Can't launch circuit to establish introduction at '%s'",
           nickname);
    return -1;
  }
  strcpy(launched->rend_query, service->service_id);
  memcpy(launched->rend_pk_digest, service->pk_digest, DIGEST_LEN);

  return 0;
}

/** Called when we're done building a circuit to an introduction point:
 *  sends a RELAY_ESTABLISH_INTRO cell.
 */
void
rend_service_intro_has_opened(circuit_t *circuit)
{
  rend_service_t *service;
  int len, r;
  char buf[RELAY_PAYLOAD_SIZE];
  char auth[DIGEST_LEN + 9];
  char serviceid[REND_SERVICE_ID_LEN+1];

  tor_assert(circuit->purpose == CIRCUIT_PURPOSE_S_ESTABLISH_INTRO);
  tor_assert(CIRCUIT_IS_ORIGIN(circuit) && circuit->cpath);

  base32_encode(serviceid, REND_SERVICE_ID_LEN+1,
                circuit->rend_pk_digest,10);

  service = rend_service_get_by_pk_digest(circuit->rend_pk_digest);
  if (!service) {
    log_fn(LOG_WARN, "Unrecognized service ID %s on introduction circuit %d",
           serviceid, circuit->n_circ_id);
    goto err;
  }

  log_fn(LOG_INFO,
         "Established circuit %d as introduction point for service %s",
         circuit->n_circ_id, serviceid);

  /* Build the payload for a RELAY_ESTABLISH_INTRO cell. */
  len = crypto_pk_asn1_encode(service->private_key, buf+2,
                              RELAY_PAYLOAD_SIZE-2);
  set_uint16(buf, htons((uint16_t)len));
  len += 2;
  memcpy(auth, circuit->cpath->prev->handshake_digest, DIGEST_LEN);
  memcpy(auth+DIGEST_LEN, "INTRODUCE", 9);
  if (crypto_digest(auth, DIGEST_LEN+9, buf+len))
    goto err;
  len += 20;
  r = crypto_pk_private_sign_digest(service->private_key, buf, len, buf+len);
  if (r<0) {
    log_fn(LOG_WARN, "Couldn't sign introduction request");
    goto err;
  }
  len += r;

  if (connection_edge_send_command(NULL, circuit,RELAY_COMMAND_ESTABLISH_INTRO,
                                   buf, len, circuit->cpath->prev)<0) {
    log_fn(LOG_WARN,
           "Couldn't send introduction request for service %s on circuit %d",
           serviceid, circuit->n_circ_id);
    goto err;
  }

  return;
 err:
  circuit_mark_for_close(circuit);
}

/** Called when we get an INTRO_ESTABLISHED cell; mark the circuit as a
 * live introduction point, and note that the service descriptor is
 * now out-of-date.*/
int
rend_service_intro_established(circuit_t *circuit, const char *request, int request_len)
{
  rend_service_t *service;

  if (circuit->purpose != CIRCUIT_PURPOSE_S_ESTABLISH_INTRO) {
    log_fn(LOG_WARN, "received INTRO_ESTABLISHED cell on non-intro circuit");
    goto err;
  }
  service = rend_service_get_by_pk_digest(circuit->rend_pk_digest);
  if (!service) {
    log_fn(LOG_WARN, "Unknown service on introduction circuit %d",
           circuit->n_circ_id);
    goto err;
  }
  service->desc_is_dirty = 1;
  circuit->purpose = CIRCUIT_PURPOSE_S_INTRO;

  return 0;
 err:
  circuit_mark_for_close(circuit);
  return -1;
}

/** Called once a circuit to a rendezvous point is established: sends a
 *  RELAY_COMMAND_RENDEZVOUS1 cell.
 */
void
rend_service_rendezvous_has_opened(circuit_t *circuit)
{
  rend_service_t *service;
  char buf[RELAY_PAYLOAD_SIZE];
  crypt_path_t *hop;
  char serviceid[REND_SERVICE_ID_LEN+1];
  char hexcookie[9];

  tor_assert(circuit->purpose == CIRCUIT_PURPOSE_S_CONNECT_REND);
  tor_assert(circuit->cpath);
  tor_assert(circuit->build_state);
  hop = circuit->build_state->pending_final_cpath;
  tor_assert(hop);

  base16_encode(hexcookie,9,circuit->rend_cookie,4);
  base32_encode(serviceid, REND_SERVICE_ID_LEN+1,
                circuit->rend_pk_digest,10);

  log_fn(LOG_INFO,
       "Done building circuit %d to rendezvous with cookie %s for service %s",
         circuit->n_circ_id, hexcookie, serviceid);

  service = rend_service_get_by_pk_digest(circuit->rend_pk_digest);
  if (!service) {
    log_fn(LOG_WARN, "Internal error: unrecognized service ID on introduction circuit");
    goto err;
  }

  /* All we need to do is send a RELAY_RENDEZVOUS1 cell... */
  memcpy(buf, circuit->rend_cookie, REND_COOKIE_LEN);
  if (crypto_dh_get_public(hop->handshake_state,
                           buf+REND_COOKIE_LEN, DH_KEY_LEN)<0) {
    log_fn(LOG_WARN,"Couldn't get DH public key");
    goto err;
  }
  memcpy(buf+REND_COOKIE_LEN+DH_KEY_LEN, hop->handshake_digest,
         DIGEST_LEN);

  /* Send the cell */
  if (connection_edge_send_command(NULL, circuit, RELAY_COMMAND_RENDEZVOUS1,
                                   buf, REND_COOKIE_LEN+DH_KEY_LEN+DIGEST_LEN,
                                   circuit->cpath->prev)<0) {
    log_fn(LOG_WARN, "Couldn't send RENDEZVOUS1 cell");
    goto err;
  }

  crypto_dh_free(hop->handshake_state);
  hop->handshake_state = NULL;

  /* Append the cpath entry. */
  hop->state = CPATH_STATE_OPEN;
  /* set the windows to default. these are the windows
   * that bob thinks alice has.
   */
  hop->package_window = CIRCWINDOW_START;
  hop->deliver_window = CIRCWINDOW_START;

  onion_append_to_cpath(&circuit->cpath, hop);
  circuit->build_state->pending_final_cpath = NULL; /* prevent double-free */

  /* Change the circuit purpose. */
  circuit->purpose = CIRCUIT_PURPOSE_S_REND_JOINED;

  return;
 err:
  circuit_mark_for_close(circuit);
}

/*
 * Manage introduction points
 */

/** Return the (possibly non-open) introduction circuit ending at
 * <b>router</b> for the service whose public key is <b>pk_digest</b>.  Return
 * NULL if no such service is found.
 */
static circuit_t *
find_intro_circuit(routerinfo_t *router, const char *pk_digest)
{
  circuit_t *circ = NULL;

  tor_assert(router);
  while ((circ = circuit_get_next_by_pk_and_purpose(circ,pk_digest,
                                                  CIRCUIT_PURPOSE_S_INTRO))) {
    tor_assert(circ->cpath);
    if (circ->build_state->chosen_exit_name &&
        !strcasecmp(circ->build_state->chosen_exit_name, router->nickname)) {
      return circ;
    }
  }

  circ = NULL;
  while ((circ = circuit_get_next_by_pk_and_purpose(circ,pk_digest,
                                        CIRCUIT_PURPOSE_S_ESTABLISH_INTRO))) {
    tor_assert(circ->cpath);
    if (circ->build_state->chosen_exit_name &&
        !strcasecmp(circ->build_state->chosen_exit_name, router->nickname)) {
      return circ;
    }
  }
  return NULL;
}

/** If the directory servers don't have an up-to-date descriptor for
 * <b>service</b>, encode and sign the service descriptor for <b>service</b>,
 * and upload it to all the dirservers.
 */
static void
upload_service_descriptor(rend_service_t *service)
{
  char *desc;
  int desc_len;
  if (!service->desc_is_dirty)
    return;

  /* Update the descriptor. */
  rend_service_update_descriptor(service);
  if (rend_encode_service_descriptor(service->desc,
                                     service->private_key,
                                     &desc, &desc_len)<0) {
    log_fn(LOG_WARN, "Couldn't encode service descriptor; not uploading");
    return;
  }

  /* Post it to the dirservers */
  directory_post_to_dirservers(DIR_PURPOSE_UPLOAD_RENDDESC, desc, desc_len);
  tor_free(desc);

  service->desc_is_dirty = 0;
}

/* XXXX Make this longer once directories remember service descriptors across
 * restarts.*/
#define MAX_SERVICE_PUBLICATION_INTERVAL (15*60)

/** For every service, check how many intro points it currently has, and:
 *  - Pick new intro points as necessary.
 *  - Launch circuits to any new intro points.
 */
void rend_services_introduce(void) {
  int i,j,r;
  routerinfo_t *router;
  rend_service_t *service;
  char *intro;
  int changed, prev_intro_nodes;
  smartlist_t *intro_routers, *exclude_routers;
  time_t now;

  intro_routers = smartlist_create();
  exclude_routers = smartlist_create();
  now = time(NULL);

  for (i=0; i< smartlist_len(rend_service_list); ++i) {
    smartlist_clear(intro_routers);
    service = smartlist_get(rend_service_list, i);

    tor_assert(service);
    changed = 0;
    if (now > service->intro_period_started+INTRO_CIRC_RETRY_PERIOD) {
      /* One period has elapsed; we can try building circuits again. */
      service->intro_period_started = now;
      service->n_intro_circuits_launched = 0;
    } else if (service->n_intro_circuits_launched>=MAX_INTRO_CIRCS_PER_PERIOD){
      /* We have failed too many times in this period; wait for the next
       * one before we try again. */
      continue;
    }

    /* Find out which introduction points we have in progress for this service. */
    for (j=0;j< smartlist_len(service->intro_nodes); ++j) {
      intro = smartlist_get(service->intro_nodes, j);
      router = router_get_by_nickname(intro);
      if (!router || !find_intro_circuit(router,service->pk_digest)) {
        log_fn(LOG_INFO,"Giving up on %s as intro point for %s.",
                intro, service->service_id);
        smartlist_del(service->intro_nodes,j--);
        changed = service->desc_is_dirty = 1;
      }
      smartlist_add(intro_routers, router);
    }

    /* We have enough intro points, and the intro points we thought we had were
     * all connected.
     */
    if (!changed && smartlist_len(service->intro_nodes) >= NUM_INTRO_POINTS) {
      /* We have all our intro points! Start a fresh period and reset the
       * circuit count. */
      service->intro_period_started = now;
      service->n_intro_circuits_launched = 0;
      continue;
    }

    /* Remember how many introduction circuits we started with. */
    prev_intro_nodes = smartlist_len(service->intro_nodes);

    smartlist_add_all(exclude_routers, intro_routers);
    /* The directory is now here. Pick three ORs as intro points. */
    for (j=prev_intro_nodes; j < NUM_INTRO_POINTS; ++j) {
      router = router_choose_random_node(service->intro_prefer_nodes,
               service->intro_exclude_nodes, exclude_routers, 1, 0,
               options._AllowUnverified & ALLOW_UNVERIFIED_INTRODUCTION, 0);
      if (!router) {
        log_fn(LOG_WARN, "Could only establish %d introduction points for %s",
               smartlist_len(service->intro_nodes), service->service_id);
        break;
      }
      changed = 1;
      smartlist_add(intro_routers, router);
      smartlist_add(exclude_routers, router);
      smartlist_add(service->intro_nodes, tor_strdup(router->nickname));
      log_fn(LOG_INFO,"Picked router %s as an intro point for %s.", router->nickname,
             service->service_id);
    }

    /* Reset exclude_routers, for the next time around the loop. */
    smartlist_clear(exclude_routers);

    /* If there's no need to launch new circuits, stop here. */
    if (!changed)
      continue;

    /* Establish new introduction points. */
    for (j=prev_intro_nodes; j < smartlist_len(service->intro_nodes); ++j) {
      intro = smartlist_get(service->intro_nodes, j);
      r = rend_service_launch_establish_intro(service, intro);
      if (r<0) {
        log_fn(LOG_WARN, "Error launching circuit to node %s for service %s",
               intro, service->service_id);
      }
    }
  }
  smartlist_free(intro_routers);
  smartlist_free(exclude_routers);
}

/** Regenerate and upload rendezvous service descriptors for all
 * services.  If <b>force</b> is false, skip services where we've already
 * uploaded an up-to-date copy; if <b>force</b> is true, regenerate and
 * upload everything.
 */
void
rend_services_upload(int force)
{
  int i;
  rend_service_t *service;

  for (i=0; i< smartlist_len(rend_service_list); ++i) {
    service = smartlist_get(rend_service_list, i);
    if (force)
      service->desc_is_dirty = 1;
    if (service->desc_is_dirty)
      upload_service_descriptor(service);
  }
}

/** Log the status of introduction points for all rendezvous services
 * at log severity <b>serverity</b>.
 */
void
rend_service_dump_stats(int severity)
{
  int i,j;
  routerinfo_t *router;
  rend_service_t *service;
  char *nickname;
  circuit_t *circ;

  for (i=0; i < smartlist_len(rend_service_list); ++i) {
    service = smartlist_get(rend_service_list, i);
    log(severity, "Service configured in %s:", service->directory);
    for (j=0; j < smartlist_len(service->intro_nodes); ++j) {
      nickname = smartlist_get(service->intro_nodes, j);
      router = router_get_by_nickname(smartlist_get(service->intro_nodes,j));
      if (!router) {
        log(severity, "  Intro point at %s: unrecognized router",nickname);
        continue;
      }
      circ = find_intro_circuit(router, service->pk_digest);
      if (!circ) {
        log(severity, "  Intro point at %s: no circuit",nickname);
        continue;
      }
      log(severity, "  Intro point at %s: circuit is %s",nickname,
          circuit_state_to_string[circ->state]);
    }
  }
}

/** Given <b>conn</b>, a rendezvous exit stream, look up the hidden service for
 * 'circ', and look up the port and address based on conn-\>port.
 * Assign the actual conn-\>addr and conn-\>port. Return -1 if failure,
 * or 0 for success.
 */
int
rend_service_set_connection_addr_port(connection_t *conn, circuit_t *circ)
{
  rend_service_t *service;
  int i;
  rend_service_port_config_t *p;
  char serviceid[REND_SERVICE_ID_LEN+1];

  tor_assert(circ->purpose == CIRCUIT_PURPOSE_S_REND_JOINED);
  log_fn(LOG_DEBUG,"beginning to hunt for addr/port");
  base32_encode(serviceid, REND_SERVICE_ID_LEN+1,
                circ->rend_pk_digest,10);
  service = rend_service_get_by_pk_digest(circ->rend_pk_digest);
  if (!service) {
    log_fn(LOG_WARN, "Couldn't find any service associated with pk %s on rendezvous circuit %d; closing",
           serviceid, circ->n_circ_id);
    return -1;
  }
  for (i = 0; i < smartlist_len(service->ports); ++i) {
    p = smartlist_get(service->ports, i);
    if (conn->port == p->virtual_port) {
      conn->addr = p->real_address;
      conn->port = p->real_port;
      return 0;
    }
  }
  log_fn(LOG_INFO, "No virtual port mapping exists for port %d on service %s",
         conn->port,serviceid);
  return -1;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
