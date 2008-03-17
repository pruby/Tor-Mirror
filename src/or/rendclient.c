/* Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2008, The Tor Project, Inc. */
/* See LICENSE for licensing information */
/* $Id$ */
const char rendclient_c_id[] =
  "$Id$";

/**
 * \file rendclient.c
 * \brief Client code to access location-hidden services.
 **/

#include "or.h"

/** Called when we've established a circuit to an introduction point:
 * send the introduction request. */
void
rend_client_introcirc_has_opened(origin_circuit_t *circ)
{
  tor_assert(circ->_base.purpose == CIRCUIT_PURPOSE_C_INTRODUCING);
  tor_assert(circ->cpath);

  log_info(LD_REND,"introcirc is open");
  connection_ap_attach_pending();
}

/** Send the establish-rendezvous cell along a rendezvous circuit. if
 * it fails, mark the circ for close and return -1. else return 0.
 */
static int
rend_client_send_establish_rendezvous(origin_circuit_t *circ)
{
  tor_assert(circ->_base.purpose == CIRCUIT_PURPOSE_C_ESTABLISH_REND);
  log_info(LD_REND, "Sending an ESTABLISH_RENDEZVOUS cell");

  if (crypto_rand(circ->rend_cookie, REND_COOKIE_LEN) < 0) {
    log_warn(LD_BUG, "Internal error: Couldn't produce random cookie.");
    circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_INTERNAL);
    return -1;
  }
  if (relay_send_command_from_edge(0, TO_CIRCUIT(circ),
                                   RELAY_COMMAND_ESTABLISH_RENDEZVOUS,
                                   circ->rend_cookie, REND_COOKIE_LEN,
                                   circ->cpath->prev)<0) {
    /* circ is already marked for close */
    log_warn(LD_GENERAL, "Couldn't send ESTABLISH_RENDEZVOUS cell");
    return -1;
  }

  return 0;
}

/** Called when we're trying to connect an ap conn; sends an INTRODUCE1 cell
 * down introcirc if possible.
 */
int
rend_client_send_introduction(origin_circuit_t *introcirc,
                              origin_circuit_t *rendcirc)
{
  size_t payload_len;
  int r;
  char payload[RELAY_PAYLOAD_SIZE];
  char tmp[RELAY_PAYLOAD_SIZE];
  rend_cache_entry_t *entry;
  crypt_path_t *cpath;
  off_t dh_offset;
  crypto_pk_env_t *intro_key; /* either Bob's public key or an intro key. */

  tor_assert(introcirc->_base.purpose == CIRCUIT_PURPOSE_C_INTRODUCING);
  tor_assert(rendcirc->_base.purpose == CIRCUIT_PURPOSE_C_REND_READY);
  tor_assert(!rend_cmp_service_ids(introcirc->rend_query,
                                   rendcirc->rend_query));

  if (rend_cache_lookup_entry(introcirc->rend_query, -1, &entry) < 1) {
    log_warn(LD_REND,
             "query %s didn't have valid rend desc in cache. Failing.",
             escaped_safe_str(introcirc->rend_query));
    goto err;
  }

  /* first 20 bytes of payload are the hash of bob's pk */
  if (entry->parsed->version == 0) { /* unversioned descriptor */
    intro_key = entry->parsed->pk;
  } else { /* versioned descriptor */
    intro_key = NULL;
    SMARTLIST_FOREACH(entry->parsed->intro_nodes, rend_intro_point_t *,
                      intro, {
      if (!memcmp(introcirc->build_state->chosen_exit->identity_digest,
                  intro->extend_info->identity_digest, DIGEST_LEN)) {
        intro_key = intro->intro_key;
        break;
      }
    });
    if (!intro_key) {
      log_warn(LD_BUG, "Internal error: could not find intro key.");
      goto err;
    }
  }
  if (crypto_pk_get_digest(intro_key, payload)<0) {
    log_warn(LD_BUG, "Internal error: couldn't hash public key.");
    goto err;
  }

  /* Initialize the pending_final_cpath and start the DH handshake. */
  cpath = rendcirc->build_state->pending_final_cpath;
  if (!cpath) {
    cpath = rendcirc->build_state->pending_final_cpath =
      tor_malloc_zero(sizeof(crypt_path_t));
    cpath->magic = CRYPT_PATH_MAGIC;
    if (!(cpath->dh_handshake_state = crypto_dh_new())) {
      log_warn(LD_BUG, "Internal error: couldn't allocate DH.");
      goto err;
    }
    if (crypto_dh_generate_public(cpath->dh_handshake_state)<0) {
      log_warn(LD_BUG, "Internal error: couldn't generate g^x.");
      goto err;
    }
  }

  /* write the remaining items into tmp */
  if (entry->parsed->protocols & (1<<2)) {
    /* version 2 format */
    extend_info_t *extend_info = rendcirc->build_state->chosen_exit;
    int klen;
    tmp[0] = 2; /* version 2 of the cell format */
    /* nul pads */
    set_uint32(tmp+1, htonl(extend_info->addr));
    set_uint16(tmp+5, htons(extend_info->port));
    memcpy(tmp+7, extend_info->identity_digest, DIGEST_LEN);
    klen = crypto_pk_asn1_encode(extend_info->onion_key, tmp+7+DIGEST_LEN+2,
                                 sizeof(tmp)-(7+DIGEST_LEN+2));
    set_uint16(tmp+7+DIGEST_LEN, htons(klen));
    memcpy(tmp+7+DIGEST_LEN+2+klen, rendcirc->rend_cookie,
           REND_COOKIE_LEN);
    dh_offset = 7+DIGEST_LEN+2+klen+REND_COOKIE_LEN;
  } else {
    /* Version 0. */
    strncpy(tmp, rendcirc->build_state->chosen_exit->nickname,
            (MAX_NICKNAME_LEN+1)); /* nul pads */
    memcpy(tmp+MAX_NICKNAME_LEN+1, rendcirc->rend_cookie,
           REND_COOKIE_LEN);
    dh_offset = MAX_NICKNAME_LEN+1+REND_COOKIE_LEN;
  }

  if (crypto_dh_get_public(cpath->dh_handshake_state, tmp+dh_offset,
                           DH_KEY_LEN)<0) {
    log_warn(LD_BUG, "Internal error: couldn't extract g^x.");
    goto err;
  }

  note_crypto_pk_op(REND_CLIENT);
  /*XXX maybe give crypto_pk_public_hybrid_encrypt a max_len arg,
   * to avoid buffer overflows? */
  r = crypto_pk_public_hybrid_encrypt(intro_key, payload+DIGEST_LEN,
                                      tmp,
                                      (int)(dh_offset+DH_KEY_LEN),
                                      PK_PKCS1_OAEP_PADDING, 0);
  if (r<0) {
    log_warn(LD_BUG,"Internal error: hybrid pk encrypt failed.");
    goto err;
  }

  payload_len = DIGEST_LEN + r;
  tor_assert(payload_len <= RELAY_PAYLOAD_SIZE); /* we overran something */

  log_info(LD_REND, "Sending an INTRODUCE1 cell");
  if (relay_send_command_from_edge(0, TO_CIRCUIT(introcirc),
                                   RELAY_COMMAND_INTRODUCE1,
                                   payload, payload_len,
                                   introcirc->cpath->prev)<0) {
    /* introcirc is already marked for close. leave rendcirc alone. */
    log_warn(LD_BUG, "Couldn't send INTRODUCE1 cell");
    return -1;
  }

  /* Now, we wait for an ACK or NAK on this circuit. */
  introcirc->_base.purpose = CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT;

  return 0;
err:
  circuit_mark_for_close(TO_CIRCUIT(introcirc), END_CIRC_REASON_INTERNAL);
  circuit_mark_for_close(TO_CIRCUIT(rendcirc), END_CIRC_REASON_INTERNAL);
  return -1;
}

/** Called when a rendezvous circuit is open; sends a establish
 * rendezvous circuit as appropriate. */
void
rend_client_rendcirc_has_opened(origin_circuit_t *circ)
{
  tor_assert(circ->_base.purpose == CIRCUIT_PURPOSE_C_ESTABLISH_REND);

  log_info(LD_REND,"rendcirc is open");

  /* generate a rendezvous cookie, store it in circ */
  if (rend_client_send_establish_rendezvous(circ) < 0) {
    return;
  }
}

/** Called when get an ACK or a NAK for a REND_INTRODUCE1 cell.
 */
int
rend_client_introduction_acked(origin_circuit_t *circ,
                               const char *request, size_t request_len)
{
  origin_circuit_t *rendcirc;
  (void) request; // XXXX Use this.

  if (circ->_base.purpose != CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT) {
    log_warn(LD_PROTOCOL,
             "Received REND_INTRODUCE_ACK on unexpected circuit %d.",
             circ->_base.n_circ_id);
    circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_TORPROTOCOL);
    return -1;
  }

  tor_assert(circ->build_state->chosen_exit);

  if (request_len == 0) {
    /* It's an ACK; the introduction point relayed our introduction request. */
    /* Locate the rend circ which is waiting to hear about this ack,
     * and tell it.
     */
    log_info(LD_REND,"Received ack. Telling rend circ...");
    rendcirc = circuit_get_by_rend_query_and_purpose(
               circ->rend_query, CIRCUIT_PURPOSE_C_REND_READY);
    if (rendcirc) { /* remember the ack */
      rendcirc->_base.purpose = CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED;
    } else {
      log_info(LD_REND,"...Found no rend circ. Dropping on the floor.");
    }
    /* close the circuit: we won't need it anymore. */
    circ->_base.purpose = CIRCUIT_PURPOSE_C_INTRODUCE_ACKED;
    circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_FINISHED);
  } else {
    /* It's a NAK; the introduction point didn't relay our request. */
    circ->_base.purpose = CIRCUIT_PURPOSE_C_INTRODUCING;
    /* Remove this intro point from the set of viable introduction
     * points. If any remain, extend to a new one and try again.
     * If none remain, refetch the service descriptor.
     */
    if (rend_client_remove_intro_point(circ->build_state->chosen_exit,
                                       circ->rend_query) > 0) {
      /* There are introduction points left. Re-extend the circuit to
       * another intro point and try again. */
      extend_info_t *extend_info;
      int result;
      extend_info = rend_client_get_random_intro(circ->rend_query);
      if (!extend_info) {
        log_warn(LD_REND, "No introduction points left for %s. Closing.",
                 escaped_safe_str(circ->rend_query));
        circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_INTERNAL);
        return -1;
      }
      log_info(LD_REND,
               "Got nack for %s from %s. Re-extending circ %d, "
               "this time to %s.",
               escaped_safe_str(circ->rend_query),
               circ->build_state->chosen_exit->nickname, circ->_base.n_circ_id,
               extend_info->nickname);
      result = circuit_extend_to_new_exit(circ, extend_info);
      extend_info_free(extend_info);
      return result;
    }
  }
  return 0;
}

/** The period for which a hidden service directory cannot be queried for
 * the same descriptor ID again. */
#define REND_HID_SERV_DIR_REQUERY_PERIOD (15 * 60)

/** Contains the last request times to hidden service directories for
 * certain queries; keys are strings consisting of base32-encoded
 * hidden service directory identities and base32-encoded descriptor IDs;
 * values are pointers to timestamps of the last requests. */
static strmap_t *last_hid_serv_requests = NULL;

/** Look up the last request time to hidden service directory <b>hs_dir</b>
 * for descriptor ID <b>desc_id_base32</b>. If <b>set</b> is non-zero,
 * assign the current time <b>now</b> and return that. Otherwise, return
 * the most recent request time, or 0 if no such request has been sent
 * before. */
static time_t
lookup_last_hid_serv_request(routerstatus_t *hs_dir,
                             const char *desc_id_base32, time_t now, int set)
{
  char hsdir_id_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
  char hsdir_desc_comb_id[2 * REND_DESC_ID_V2_LEN_BASE32 + 1];
  time_t *last_request_ptr;
  base32_encode(hsdir_id_base32, sizeof(hsdir_id_base32),
                hs_dir->identity_digest, DIGEST_LEN);
  tor_snprintf(hsdir_desc_comb_id, sizeof(hsdir_desc_comb_id), "%s%s",
               hsdir_id_base32, desc_id_base32);
  if (set) {
    last_request_ptr = tor_malloc_zero(sizeof(time_t *));
    *last_request_ptr = now;
    strmap_set(last_hid_serv_requests, hsdir_desc_comb_id, last_request_ptr);
  } else
    last_request_ptr = strmap_get_lc(last_hid_serv_requests,
                                     hsdir_desc_comb_id);
  return (last_request_ptr) ? *last_request_ptr : 0;
}

/** Clean the history of request times to hidden service directories, so that
 * it does not contain requests older than REND_HID_SERV_DIR_REQUERY_PERIOD
 * seconds any more. */
static void
directory_clean_last_hid_serv_requests(void)
{
  strmap_iter_t *iter;
  time_t cutoff = time(NULL) - REND_HID_SERV_DIR_REQUERY_PERIOD;
  if (!last_hid_serv_requests)
    last_hid_serv_requests = strmap_new();
  for (iter = strmap_iter_init(last_hid_serv_requests);
       !strmap_iter_done(iter); ) {
    const char *key;
    void *val;
    time_t *ent;
    strmap_iter_get(iter, &key, &val);
    ent = (time_t *) val;
    if (*ent < cutoff) {
      iter = strmap_iter_next_rmv(last_hid_serv_requests, iter);
      tor_free(ent);
    } else {
      iter = strmap_iter_next(last_hid_serv_requests, iter);
    }
  }
}

/** Determine the responsible hidden service directories for <b>desc_id</b>
 * and fetch the descriptor belonging to that ID from one of them. Only
 * send a request to hidden service directories that we did not try within
 * the last REND_HID_SERV_DIR_REQUERY_PERIOD seconds; on success, return 1,
 * in the case that no hidden service directory is left to ask for the
 * descriptor, return 0, and in case of a failure -1. <b>query</b> is only
 * passed for pretty log statements. */
static int
directory_get_from_hs_dir(const char *desc_id, const char *query)
{
  smartlist_t *responsible_dirs = smartlist_create();
  routerstatus_t *hs_dir;
  char desc_id_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
  time_t now = time(NULL);
  tor_assert(desc_id);
  tor_assert(query);
  tor_assert(strlen(query) == REND_SERVICE_ID_LEN_BASE32);
  /* Determine responsible dirs. Even if we can't get all we want,
   * work with the ones we have. If it's empty, we'll notice below. */
  (int) hid_serv_get_responsible_directories(responsible_dirs, desc_id);

  base32_encode(desc_id_base32, sizeof(desc_id_base32),
                desc_id, DIGEST_LEN);

  /* Only select those hidden service directories to which we did not send
   * a request recently. */
  directory_clean_last_hid_serv_requests(); /* Clean request history first. */

  SMARTLIST_FOREACH(responsible_dirs, routerstatus_t *, dir, {
    if (lookup_last_hid_serv_request(dir, desc_id_base32, 0, 0) +
        REND_HID_SERV_DIR_REQUERY_PERIOD >= now)
      SMARTLIST_DEL_CURRENT(responsible_dirs, dir);
  });

  hs_dir = smartlist_choose(responsible_dirs);
  smartlist_free(responsible_dirs);
  if (!hs_dir) {
    log_info(LD_REND, "Could not pick one of the responsible hidden "
                      "service directories, because we requested them all "
                      "recently without success.");
    return 0;
  }

  /* Remember, that we are requesting a descriptor from this hidden service
   * directory now. */
  lookup_last_hid_serv_request(hs_dir, desc_id_base32, now, 1);

  /* Send fetch request. (Pass query as payload to write it to the directory
   * connection so that it can be referred to when the response arrives.) */
  directory_initiate_command_routerstatus(hs_dir,
                                          DIR_PURPOSE_FETCH_RENDDESC_V2,
                                          ROUTER_PURPOSE_GENERAL,
                                          1, desc_id_base32, query, 0, 0);
  log_info(LD_REND, "Sending fetch request for v2 descriptor for "
                    "service '%s' with descriptor ID '%s' to hidden "
                    "service directory '%s' on port %d.",
           safe_str(query), safe_str(desc_id_base32), hs_dir->nickname,
           hs_dir->dir_port);
  return 1;
}

/** If we are not currently fetching a rendezvous service descriptor
 * for the service ID <b>query</b>, start a directory connection to fetch a
 * new one.
 */
void
rend_client_refetch_renddesc(const char *query)
{
  if (!get_options()->FetchHidServDescriptors)
    return;
  log_info(LD_REND, "Fetching rendezvous descriptor for service %s",
           escaped_safe_str(query));
  if (connection_get_by_type_state_rendquery(CONN_TYPE_DIR, 0, query, 0)) {
    log_info(LD_REND,"Would fetch a new renddesc here (for %s), but one is "
             "already in progress.", escaped_safe_str(query));
  } else {
    /* not one already; initiate a dir rend desc lookup */
    directory_get_from_dirserver(DIR_PURPOSE_FETCH_RENDDESC,
                                 ROUTER_PURPOSE_GENERAL, query, 1);
  }
}

/** Start a connection to a hidden service directory to fetch a v2
 * rendezvous service descriptor for the base32-encoded service ID
 * <b>query</b>.
 */
void
rend_client_refetch_v2_renddesc(const char *query)
{
  char descriptor_id[DIGEST_LEN];
  int replicas_left_to_try[REND_NUMBER_OF_NON_CONSECUTIVE_REPLICAS];
  int i, tries_left;
  rend_cache_entry_t *e = NULL;
  tor_assert(query);
  tor_assert(strlen(query) == REND_SERVICE_ID_LEN_BASE32);
  /* Are we configured to fetch descriptors? */
  if (!get_options()->FetchHidServDescriptors) {
    log_warn(LD_REND, "We received an onion address for a v2 rendezvous "
        "service descriptor, but are not fetching service descriptors.");
    return;
  }
  /* Before fetching, check if we already have the descriptor here. */
  if (rend_cache_lookup_entry(query, -1, &e) > 0) {
    log_info(LD_REND, "We would fetch a v2 rendezvous descriptor, but we "
                      "already have that descriptor here. Not fetching.");
    return;
  }
  log_debug(LD_REND, "Fetching v2 rendezvous descriptor for service %s",
            safe_str(query));
  /* Randomly iterate over the replicas until a descriptor can be fetched
   * from one of the consecutive nodes, or no options are left. */
  tries_left = REND_NUMBER_OF_NON_CONSECUTIVE_REPLICAS;
  for (i = 0; i < REND_NUMBER_OF_NON_CONSECUTIVE_REPLICAS; i++)
    replicas_left_to_try[i] = i;
  while (tries_left > 0) {
    int rand = crypto_rand_int(tries_left);
    int chosen_replica = replicas_left_to_try[rand];
    replicas_left_to_try[rand] = replicas_left_to_try[--tries_left];

    if (rend_compute_v2_desc_id(descriptor_id, query, NULL, time(NULL),
                                chosen_replica) < 0) {
      log_warn(LD_REND, "Internal error: Computing v2 rendezvous "
                        "descriptor ID did not succeed.");
      return;
    }
    if (directory_get_from_hs_dir(descriptor_id, query) != 0)
      return; /* either success or failure, but we're done */
  }
  /* If we come here, there are no hidden service directories left. */
  log_info(LD_REND, "Could not pick one of the responsible hidden "
                    "service directories to fetch descriptors, because "
                    "we already tried them all unsuccessfully.");
  return;
}

/** Remove failed_intro from ent. If ent now has no intro points, or
 * service is unrecognized, then launch a new renddesc fetch.
 *
 * Return -1 if error, 0 if no intro points remain or service
 * unrecognized, 1 if recognized and some intro points remain.
 */
int
rend_client_remove_intro_point(extend_info_t *failed_intro, const char *query)
{
  int i, r;
  rend_cache_entry_t *ent;
  connection_t *conn;

  r = rend_cache_lookup_entry(query, -1, &ent);
  if (r<0) {
    log_warn(LD_BUG, "Malformed service ID %s.", escaped_safe_str(query));
    return -1;
  }
  if (r==0) {
    log_info(LD_REND, "Unknown service %s. Re-fetching descriptor.",
             escaped_safe_str(query));
    /* Fetch both, v0 and v2 rend descriptors in parallel. Use whichever
     * arrives first. */
    rend_client_refetch_v2_renddesc(query);
    rend_client_refetch_renddesc(query);
    return 0;
  }

  for (i = 0; i < smartlist_len(ent->parsed->intro_nodes); i++) {
    rend_intro_point_t *intro = smartlist_get(ent->parsed->intro_nodes, i);
    if (!memcmp(failed_intro->identity_digest,
                intro->extend_info->identity_digest, DIGEST_LEN)) {
      rend_intro_point_free(intro);
      smartlist_del(ent->parsed->intro_nodes, i);
      break;
    }
  }

  if (smartlist_len(ent->parsed->intro_nodes) == 0) {
    log_info(LD_REND,
             "No more intro points remain for %s. Re-fetching descriptor.",
             escaped_safe_str(query));
    /* Fetch both, v0 and v2 rend descriptors in parallel. Use whichever
     * arrives first. */
    rend_client_refetch_v2_renddesc(query);
    rend_client_refetch_renddesc(query);

    /* move all pending streams back to renddesc_wait */
    while ((conn = connection_get_by_type_state_rendquery(CONN_TYPE_AP,
                                   AP_CONN_STATE_CIRCUIT_WAIT, query, -1))) {
      conn->state = AP_CONN_STATE_RENDDESC_WAIT;
    }

    return 0;
  }
  log_info(LD_REND,"%d options left for %s.",
           smartlist_len(ent->parsed->intro_nodes), escaped_safe_str(query));
  return 1;
}

/** Called when we receive a RENDEZVOUS_ESTABLISHED cell; changes the state of
 * the circuit to C_REND_READY.
 */
int
rend_client_rendezvous_acked(origin_circuit_t *circ, const char *request,
                             size_t request_len)
{
  (void) request;
  (void) request_len;
  /* we just got an ack for our establish-rendezvous. switch purposes. */
  if (circ->_base.purpose != CIRCUIT_PURPOSE_C_ESTABLISH_REND) {
    log_warn(LD_PROTOCOL,"Got a rendezvous ack when we weren't expecting one. "
             "Closing circ.");
    circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_TORPROTOCOL);
    return -1;
  }
  log_info(LD_REND,"Got rendezvous ack. This circuit is now ready for "
           "rendezvous.");
  circ->_base.purpose = CIRCUIT_PURPOSE_C_REND_READY;
  return 0;
}

/** Bob sent us a rendezvous cell; join the circuits. */
int
rend_client_receive_rendezvous(origin_circuit_t *circ, const char *request,
                               size_t request_len)
{
  crypt_path_t *hop;
  char keys[DIGEST_LEN+CPATH_KEY_MATERIAL_LEN];

  if ((circ->_base.purpose != CIRCUIT_PURPOSE_C_REND_READY &&
       circ->_base.purpose != CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED)
      || !circ->build_state->pending_final_cpath) {
    log_warn(LD_PROTOCOL,"Got rendezvous2 cell from hidden service, but not "
             "expecting it. Closing.");
    circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_TORPROTOCOL);
    return -1;
  }

  if (request_len != DH_KEY_LEN+DIGEST_LEN) {
    log_warn(LD_PROTOCOL,"Incorrect length (%d) on RENDEZVOUS2 cell.",
             (int)request_len);
    goto err;
  }

  log_info(LD_REND,"Got RENDEZVOUS2 cell from hidden service.");

  /* first DH_KEY_LEN bytes are g^y from bob. Finish the dh handshake...*/
  tor_assert(circ->build_state);
  tor_assert(circ->build_state->pending_final_cpath);
  hop = circ->build_state->pending_final_cpath;
  tor_assert(hop->dh_handshake_state);
  if (crypto_dh_compute_secret(hop->dh_handshake_state, request, DH_KEY_LEN,
                               keys, DIGEST_LEN+CPATH_KEY_MATERIAL_LEN)<0) {
    log_warn(LD_GENERAL, "Couldn't complete DH handshake.");
    goto err;
  }
  /* ... and set up cpath. */
  if (circuit_init_cpath_crypto(hop, keys+DIGEST_LEN, 0)<0)
    goto err;

  /* Check whether the digest is right... */
  if (memcmp(keys, request+DH_KEY_LEN, DIGEST_LEN)) {
    log_warn(LD_PROTOCOL, "Incorrect digest of key material.");
    goto err;
  }

  crypto_dh_free(hop->dh_handshake_state);
  hop->dh_handshake_state = NULL;

  /* All is well. Extend the circuit. */
  circ->_base.purpose = CIRCUIT_PURPOSE_C_REND_JOINED;
  hop->state = CPATH_STATE_OPEN;
  /* set the windows to default. these are the windows
   * that alice thinks bob has.
   */
  hop->package_window = CIRCWINDOW_START;
  hop->deliver_window = CIRCWINDOW_START;

  onion_append_to_cpath(&circ->cpath, hop);
  circ->build_state->pending_final_cpath = NULL; /* prevent double-free */
  return 0;
 err:
  circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_REASON_TORPROTOCOL);
  return -1;
}

/** Find all the apconns in state AP_CONN_STATE_RENDDESC_WAIT that
 * are waiting on query. If there's a working cache entry here
 * with at least one intro point, move them to the next state;
 * else fail them.
 */
void
rend_client_desc_here(const char *query)
{
  edge_connection_t *conn;
  rend_cache_entry_t *entry;
  time_t now = time(NULL);

  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, _conn,
  {
    if (_conn->type != CONN_TYPE_AP ||
        _conn->state != AP_CONN_STATE_RENDDESC_WAIT ||
        _conn->marked_for_close)
      continue;
    conn = TO_EDGE_CONN(_conn);
    if (rend_cmp_service_ids(query, conn->rend_query))
      continue;
    assert_connection_ok(TO_CONN(conn), now);
    if (rend_cache_lookup_entry(conn->rend_query, -1, &entry) == 1 &&
        smartlist_len(entry->parsed->intro_nodes) > 0) {
      /* either this fetch worked, or it failed but there was a
       * valid entry from before which we should reuse */
      log_info(LD_REND,"Rend desc is usable. Launching circuits.");
      conn->_base.state = AP_CONN_STATE_CIRCUIT_WAIT;

      /* restart their timeout values, so they get a fair shake at
       * connecting to the hidden service. */
      conn->_base.timestamp_created = now;
      conn->_base.timestamp_lastread = now;
      conn->_base.timestamp_lastwritten = now;

      if (connection_ap_handshake_attach_circuit(conn) < 0) {
        /* it will never work */
        log_warn(LD_REND,"Rendezvous attempt failed. Closing.");
        if (!conn->_base.marked_for_close)
          connection_mark_unattached_ap(conn, END_STREAM_REASON_CANT_ATTACH);
      }
    } else { /* 404, or fetch didn't get that far */
      log_notice(LD_REND,"Closing stream for '%s.onion': hidden service is "
                 "unavailable (try again later).", safe_str(query));
      connection_mark_unattached_ap(conn, END_STREAM_REASON_RESOLVEFAILED);
    }
  });
}

/** Return a newly allocated extend_info_t* for a randomly chosen introduction
 * point for the named hidden service.  Return NULL if all introduction points
 * have been tried and failed.
 */
extend_info_t *
rend_client_get_random_intro(const char *query)
{
  int i;
  rend_cache_entry_t *entry;
  rend_intro_point_t *intro;
  routerinfo_t *router;

  if (rend_cache_lookup_entry(query, -1, &entry) < 1) {
    log_warn(LD_REND,
             "Query '%s' didn't have valid rend desc in cache. Failing.",
             safe_str(query));
    return NULL;
  }

 again:
  if (smartlist_len(entry->parsed->intro_nodes) == 0)
    return NULL;

  i = crypto_rand_int(smartlist_len(entry->parsed->intro_nodes));
  intro = smartlist_get(entry->parsed->intro_nodes, i);
  /* Do we need to look up the router or is the extend info complete? */
  if (!intro->extend_info->onion_key) {
    router = router_get_by_nickname(intro->extend_info->nickname, 0);
    if (!router) {
      log_info(LD_REND, "Unknown router with nickname '%s'; trying another.",
               intro->extend_info->nickname);
      rend_intro_point_free(intro);
      smartlist_del(entry->parsed->intro_nodes, i);
      goto again;
    }
    extend_info_free(intro->extend_info);
    intro->extend_info = extend_info_from_router(router);
  }
  return extend_info_dup(intro->extend_info);
}

