/* Copyright 2001 Matej Pfajfar.
 * Copyright 2001-2004 Roger Dingledine.
 * Copyright 2004-2005 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char router_c_id[] = "$Id$";

#include "or.h"

/**
 * \file router.c
 * \brief OR functionality, including key maintenance, generating
 * and uploading server descriptors, retrying OR connections.
 **/

extern long stats_n_seconds_working;

/* Exposed for test.c. */ void get_platform_str(char *platform, size_t len);

/************************************************************/

/*****
 * Key management: ORs only.
 *****/

/** Private keys for this OR.  There is also an SSL key managed by tortls.c.
 */
static tor_mutex_t *key_lock=NULL;
static time_t onionkey_set_at=0; /* When was onionkey last changed? */
static crypto_pk_env_t *onionkey=NULL;
static crypto_pk_env_t *lastonionkey=NULL;
static crypto_pk_env_t *identitykey=NULL;

/** Replace the current onion key with <b>k</b>.  Does not affect lastonionkey;
 * to update onionkey correctly, call rotate_onion_key().
 */
void
set_onion_key(crypto_pk_env_t *k)
{
  tor_mutex_acquire(key_lock);
  onionkey = k;
  onionkey_set_at = time(NULL);
  tor_mutex_release(key_lock);
  mark_my_descriptor_dirty();
}

/** Return the current onion key.  Requires that the onion key has been
 * loaded or generated. */
crypto_pk_env_t *
get_onion_key(void)
{
  tor_assert(onionkey);
  return onionkey;
}

/** Return the onion key that was current before the most recent onion
 * key rotation.  If no rotation has been performed since this process
 * started, return NULL.
 */
crypto_pk_env_t *
get_previous_onion_key(void)
{
  return lastonionkey;
}

/** Store a copy of the current onion key into *<b>key</b>, and a copy
 * of the most recent onion key into *<b>last</b>.
 */
void
dup_onion_keys(crypto_pk_env_t **key, crypto_pk_env_t **last)
{
  tor_assert(key);
  tor_assert(last);
  tor_mutex_acquire(key_lock);
  *key = crypto_pk_dup_key(onionkey);
  if (lastonionkey)
    *last = crypto_pk_dup_key(lastonionkey);
  else
    *last = NULL;
  tor_mutex_release(key_lock);
}

/** Return the time when the onion key was last set.  This is either the time
 * when the process launched, or the time of the most recent key rotation since
 * the process launched.
 */
time_t
get_onion_key_set_at(void)
{
  return onionkey_set_at;
}

/** Set the current identity key to k.
 */
void
set_identity_key(crypto_pk_env_t *k)
{
  identitykey = k;
}

/** Returns the current identity key; requires that the identity key has been
 * set.
 */
crypto_pk_env_t *
get_identity_key(void)
{
  tor_assert(identitykey);
  return identitykey;
}

/** Return true iff the identity key has been set. */
int
identity_key_is_set(void)
{
  return identitykey != NULL;
}

/** Replace the previous onion key with the current onion key, and generate
 * a new previous onion key.  Immediately after calling this function,
 * the OR should:
 *   - schedule all previous cpuworkers to shut down _after_ processing
 *     pending work.  (This will cause fresh cpuworkers to be generated.)
 *   - generate and upload a fresh routerinfo.
 */
void
rotate_onion_key(void)
{
  char fname[512];
  char fname_prev[512];
  crypto_pk_env_t *prkey;
  tor_snprintf(fname,sizeof(fname),
           "%s/keys/secret_onion_key",get_options()->DataDirectory);
  tor_snprintf(fname_prev,sizeof(fname_prev),
           "%s/keys/secret_onion_key.old",get_options()->DataDirectory);
  if (!(prkey = crypto_new_pk_env())) {
    log(LOG_ERR, "Error creating crypto environment.");
    goto error;
  }
  if (crypto_pk_generate_key(prkey)) {
    log(LOG_ERR, "Error generating onion key");
    goto error;
  }
  if (file_status(fname) == FN_FILE) {
    if (replace_file(fname, fname_prev))
      goto error;
  }
  if (crypto_pk_write_private_key_to_filename(prkey, fname)) {
    log(LOG_ERR, "Couldn't write generated key to \"%s\".", fname);
    goto error;
  }
  log_fn(LOG_INFO, "Rotating onion key");
  tor_mutex_acquire(key_lock);
  if (lastonionkey)
    crypto_free_pk_env(lastonionkey);
  lastonionkey = onionkey;
  onionkey = prkey;
  onionkey_set_at = time(NULL);
  tor_mutex_release(key_lock);
  mark_my_descriptor_dirty();
  return;
 error:
  log_fn(LOG_WARN, "Couldn't rotate onion key.");
}

/* Read an RSA secret key key from a file that was once named fname_old,
 * but is now named fname_new.  Rename the file from old to new as needed.
 */
static crypto_pk_env_t *
init_key_from_file_name_changed(const char *fname_old,
                                const char *fname_new)
{
  if (file_status(fname_new) == FN_FILE || file_status(fname_old) != FN_FILE)
    /* The new filename is there, or both are, or neither is. */
    return init_key_from_file(fname_new);

  /* The old filename exists, and the new one doesn't.  Rename and load. */
  if (rename(fname_old, fname_new) < 0) {
    log_fn(LOG_ERR, "Couldn't rename \"%s\" to \"%s\": %s", fname_old, fname_new,
           strerror(errno));
    return NULL;
  }
  return init_key_from_file(fname_new);
}

/** Try to read an RSA key from <b>fname</b>.  If <b>fname</b> doesn't exist,
 * create a new RSA key and save it in <b>fname</b>.  Return the read/created
 * key, or NULL on error.
 */
crypto_pk_env_t *
init_key_from_file(const char *fname)
{
  crypto_pk_env_t *prkey = NULL;
  FILE *file = NULL;

  if (!(prkey = crypto_new_pk_env())) {
    log(LOG_ERR, "Error creating crypto environment.");
    goto error;
  }

  switch (file_status(fname)) {
    case FN_DIR:
    case FN_ERROR:
      log(LOG_ERR, "Can't read key from \"%s\"", fname);
      goto error;
    case FN_NOENT:
      log(LOG_INFO, "No key found in \"%s\"; generating fresh key.", fname);
      if (crypto_pk_generate_key(prkey)) {
        log(LOG_ERR, "Error generating onion key");
        goto error;
      }
      if (crypto_pk_check_key(prkey) <= 0) {
        log(LOG_ERR, "Generated key seems invalid");
        goto error;
      }
      log(LOG_INFO, "Generated key seems valid");
      if (crypto_pk_write_private_key_to_filename(prkey, fname)) {
        log(LOG_ERR, "Couldn't write generated key to \"%s\".", fname);
        goto error;
      }
      return prkey;
    case FN_FILE:
      if (crypto_pk_read_private_key_from_filename(prkey, fname)) {
        log(LOG_ERR, "Error loading private key.");
        goto error;
      }
      return prkey;
    default:
      tor_assert(0);
  }

 error:
  if (prkey)
    crypto_free_pk_env(prkey);
  if (file)
    fclose(file);
  return NULL;
}

/** Initialize all OR private keys, and the TLS context, as necessary.
 * On OPs, this only initializes the tls context.
 */
int
init_keys(void)
{
  /* XXX009 Two problems with how this is called:
   * 1. It should be idempotent for servers, so we can call init_keys
   *    as much as we need to.
   */
  char keydir[512];
  char keydir2[512];
  char fingerprint[FINGERPRINT_LEN+1];
  char fingerprint_line[FINGERPRINT_LEN+MAX_NICKNAME_LEN+3];/*nickname fp\n\0 */
  char *cp;
  const char *tmp, *mydesc, *datadir;
  crypto_pk_env_t *prkey;
  char digest[20];
  or_options_t *options = get_options();

  if (!key_lock)
    key_lock = tor_mutex_new();

  /* OP's don't need persistent keys; just make up an identity and
   * initialize the TLS context. */
  if (!server_mode(options)) {
    if (!(prkey = crypto_new_pk_env()))
      return -1;
    if (crypto_pk_generate_key(prkey))
      return -1;
    set_identity_key(prkey);
    /* Create a TLS context; default the client nickname to "client". */
    if (tor_tls_context_new(get_identity_key(), 1,
                            options->Nickname ? options->Nickname : "client",
                            MAX_SSL_KEY_LIFETIME) < 0) {
      log_fn(LOG_ERR, "Error creating TLS context for OP.");
      return -1;
    }
    return 0;
  }
  /* Make sure DataDirectory exists, and is private. */
  datadir = options->DataDirectory;
  if (check_private_dir(datadir, CPD_CREATE)) {
    return -1;
  }
  /* Check the key directory. */
  tor_snprintf(keydir,sizeof(keydir),"%s/keys", datadir);
  if (check_private_dir(keydir, CPD_CREATE)) {
    return -1;
  }
  cp = keydir + strlen(keydir); /* End of string. */

  /* 1. Read identity key. Make it if none is found. */
  tor_snprintf(keydir,sizeof(keydir),"%s/keys/identity.key",datadir);
  tor_snprintf(keydir2,sizeof(keydir2),"%s/keys/secret_id_key",datadir);
  log_fn(LOG_INFO,"Reading/making identity key \"%s\"...",keydir2);
  prkey = init_key_from_file_name_changed(keydir,keydir2);
  if (!prkey) return -1;
  set_identity_key(prkey);
  /* 2. Read onion key.  Make it if none is found. */
  tor_snprintf(keydir,sizeof(keydir),"%s/keys/onion.key",datadir);
  tor_snprintf(keydir2,sizeof(keydir2),"%s/keys/secret_onion_key",datadir);
  log_fn(LOG_INFO,"Reading/making onion key \"%s\"...",keydir2);
  prkey = init_key_from_file_name_changed(keydir,keydir2);
  if (!prkey) return -1;
  set_onion_key(prkey);
  tor_snprintf(keydir,sizeof(keydir),"%s/keys/secret_onion_key.old",datadir);
  if (file_status(keydir) == FN_FILE) {
    prkey = init_key_from_file(keydir);
    if (prkey)
      lastonionkey = prkey;
  }

  /* 3. Initialize link key and TLS context. */
  if (tor_tls_context_new(get_identity_key(), 1, options->Nickname,
                          MAX_SSL_KEY_LIFETIME) < 0) {
    log_fn(LOG_ERR, "Error initializing TLS context");
    return -1;
  }
  /* 4. Dump router descriptor to 'router.desc' */
  /* Must be called after keys are initialized. */
  tmp = mydesc = router_get_my_descriptor();
  if (!mydesc) {
    log_fn(LOG_ERR, "Error initializing descriptor.");
    return -1;
  }
  if (authdir_mode(options)) {
    const char *m;
    /* We need to add our own fingerprint so it gets recognized. */
    if (dirserv_add_own_fingerprint(options->Nickname, get_identity_key())) {
      log_fn(LOG_ERR, "Error adding own fingerprint to approved set");
      return -1;
    }
    if (dirserv_add_descriptor(&tmp, &m) != 1) {
      log(LOG_ERR, "Unable to add own descriptor to directory: %s",
          m?m:"<unknown error>");
      return -1;
    }
  }

  tor_snprintf(keydir,sizeof(keydir),"%s/router.desc", datadir);
  log_fn(LOG_INFO,"Dumping descriptor to \"%s\"...",keydir);
  if (write_str_to_file(keydir, mydesc,0)) {
    return -1;
  }
  /* 5. Dump fingerprint to 'fingerprint' */
  tor_snprintf(keydir,sizeof(keydir),"%s/fingerprint", datadir);
  log_fn(LOG_INFO,"Dumping fingerprint to \"%s\"...",keydir);
  if (crypto_pk_get_fingerprint(get_identity_key(), fingerprint, 1)<0) {
    log_fn(LOG_ERR, "Error computing fingerprint");
    return -1;
  }
  tor_assert(strlen(options->Nickname) <= MAX_NICKNAME_LEN);
  if (tor_snprintf(fingerprint_line, sizeof(fingerprint_line),
                   "%s %s\n",options->Nickname, fingerprint) < 0) {
    log_fn(LOG_ERR, "Error writing fingerprint line");
    return -1;
  }
  if (write_str_to_file(keydir, fingerprint_line, 0))
    return -1;
  if (!authdir_mode(options))
    return 0;
  /* 6. [authdirserver only] load approved-routers file */
  tor_snprintf(keydir,sizeof(keydir),"%s/approved-routers", datadir);
  log_fn(LOG_INFO,"Loading approved fingerprints from \"%s\"...",keydir);
  if (dirserv_parse_fingerprint_file(keydir) < 0) {
    log_fn(LOG_ERR, "Error loading fingerprints");
    return -1;
  }
  /* 6b. [authdirserver only] add own key to approved directories. */
  crypto_pk_get_digest(get_identity_key(), digest);
  if (!router_digest_is_trusted_dir(digest)) {
    add_trusted_dir_server(options->Address, (uint16_t)options->DirPort, digest);
  }
  /* 7. [authdirserver only] load old directory, if it's there */
  tor_snprintf(keydir,sizeof(keydir),"%s/cached-directory", datadir);
  log_fn(LOG_INFO,"Loading cached directory from \"%s\"...",keydir);
  cp = read_file_to_str(keydir,0);
  if (!cp) {
    log_fn(LOG_INFO,"Cached directory \"%s\" not present. Ok.",keydir);
  } else {
    if (dirserv_load_from_directory_string(cp) < 0) {
      log_fn(LOG_WARN, "Cached directory \"%s\" is corrupt, only loaded part of it.", keydir);
      tor_free(cp);
      return 0;
    }
    tor_free(cp);
  }
  /* success */
  return 0;
}

/* Keep track of whether we should upload our server descriptor,
 * and what type of server we are.
 */

/** Whether we can reach our ORPort from the outside. */
static int can_reach_or_port = 0;
/** Whether we can reach our DirPort from the outside. */
static int can_reach_dir_port = 0;

/** Return 1 if or port is known reachable; else return 0. */
int
check_whether_orport_reachable(void)
{
  or_options_t *options = get_options();
  return clique_mode(options) ||
         options->AssumeReachable ||
         can_reach_or_port;
}

/** Return 1 if we don't have a dirport configured, or if it's reachable. */
int
check_whether_dirport_reachable(void)
{
  or_options_t *options = get_options();
  return !options->DirPort ||
         options->AssumeReachable ||
         can_reach_dir_port;
}

/**DOCDOC*/
void
consider_testing_reachability(void)
{
  routerinfo_t *me = router_get_my_routerinfo();
  if (!me) {
    log_fn(LOG_WARN,"Bug: router_get_my_routerinfo() did not find my routerinfo?");
    return;
  }

  if (!check_whether_orport_reachable()) {
    circuit_launch_by_router(CIRCUIT_PURPOSE_TESTING, me, 0, 1, 1);
  }

  if (!check_whether_dirport_reachable()) {
    if (me) {
      directory_initiate_command_router(me, DIR_PURPOSE_FETCH_DIR, 1, NULL, NULL, 0);
    } else {
      log(LOG_NOTICE,"Delaying checking DirPort reachability; can't build descriptor.");
    }
  }
}

/** Annotate that we found our ORPort reachable. */
void
router_orport_found_reachable(void)
{
  if (!can_reach_or_port) {
    if (!clique_mode(get_options()))
      log(LOG_NOTICE,"Your ORPort is reachable from the outside. Excellent.%s",
          get_options()->NoPublish ? "" : " Publishing server descriptor.");
    can_reach_or_port = 1;
    mark_my_descriptor_dirty();
    consider_publishable_server(time(NULL), 1);
  }
}

/** Annotate that we found our DirPort reachable. */
void
router_dirport_found_reachable(void)
{
  if (!can_reach_dir_port) {
    log(LOG_NOTICE,"Your DirPort is reachable from the outside. Excellent.");
    can_reach_dir_port = 1;
  }
}

/** Our router has just moved to a new IP. Reset stats. */
void
server_has_changed_ip(void)
{
  stats_n_seconds_working = 0;
  can_reach_or_port = 0;
  can_reach_dir_port = 0;
  mark_my_descriptor_dirty();
}

/** Return true iff we believe ourselves to be an authoritative
 * directory server.
 */
int
authdir_mode(or_options_t *options)
{
  return options->AuthoritativeDir != 0;
}
/** Return true iff we try to stay connected to all ORs at once.
 */
int
clique_mode(or_options_t *options)
{
  return authdir_mode(options);
}

/** Return true iff we are trying to be a server.
 */
int
server_mode(or_options_t *options)
{
  if (options->ClientOnly) return 0;
  return (options->ORPort != 0 || options->ORBindAddress);
}

/** Remember if we've advertised ourselves to the dirservers. */
static int server_is_advertised=0;

/** Return true iff we have published our descriptor lately.
 */
int
advertised_server_mode(void)
{
  return server_is_advertised;
}

/**
 * Called with a boolean: set whether we have recently published our descriptor.
 */
static void
set_server_advertised(int s)
{
  server_is_advertised = s;
}

/** Return true iff we are trying to be a socks proxy. */
int
proxy_mode(or_options_t *options)
{
  return (options->SocksPort != 0 || options->SocksBindAddress);
}

/** Decide if we're a publishable server. We are a publishable server if:
 * - We don't have the ClientOnly option set
 * and
 * - We don't have the NoPublish option set
 * and
 * - We have ORPort set
 * and
 * - We believe we are reachable from the outside; or
 * - We have the AuthoritativeDirectory option set.
 */
static int
decide_if_publishable_server(time_t now)
{
  or_options_t *options = get_options();

  if (options->ClientOnly)
    return 0;
  if (options->NoPublish)
    return 0;
  if (!server_mode(options))
    return 0;
  if (options->AuthoritativeDir)
    return 1;

  return check_whether_orport_reachable();
}

/** Initiate server descriptor upload as reasonable (if server is publishable,
 * etc).  <b>force</b> is as for router_upload_dir_desc_to_dirservers.
 */
void
consider_publishable_server(time_t now, int force)
{
  if (decide_if_publishable_server(now)) {
    set_server_advertised(1);
    if (router_rebuild_descriptor(0) == 0)
      router_upload_dir_desc_to_dirservers(force);
  } else {
    set_server_advertised(0);
  }
}

/*
 * Clique maintenance
 */

/** OR only: if in clique mode, try to open connections to all of the
 * other ORs we know about. Otherwise, open connections to those we
 * think are in clique mode.o
 *
 * If <b>force</b> is zero, only open the connection if we don't already
 * have one.
 */
void
router_retry_connections(int force)
{
  int i;
  time_t now = time(NULL);
  routerinfo_t *router;
  routerlist_t *rl;
  or_options_t *options = get_options();

  tor_assert(server_mode(options));

  router_get_routerlist(&rl);
  if (!rl) return;
  for (i=0;i < smartlist_len(rl->routers);i++) {
    router = smartlist_get(rl->routers, i);
    if (router_is_me(router))
      continue;
    if (!clique_mode(options) && !router_is_clique_mode(router))
      continue;
    if (force ||
        !connection_get_by_identity_digest(router->identity_digest,
                                           CONN_TYPE_OR)) {
      log_fn(LOG_INFO,"%sconnecting to %s at %s:%u.",
             clique_mode(options) ? "(forced) " : "",
             router->nickname, router->address, router->or_port);
      dirserv_router_has_begun_reachability_testing(router->identity_digest, now);
      connection_or_connect(router->addr, router->or_port, router->identity_digest);
    }
  }
}

/** Return true iff this OR should try to keep connections open to all
 * other ORs. */
int
router_is_clique_mode(routerinfo_t *router)
{
  if (router_digest_is_trusted_dir(router->identity_digest))
    return 1;
  return 0;
}

/*
 * OR descriptor generation.
 */

/** My routerinfo. */
static routerinfo_t *desc_routerinfo = NULL;
/** Since when has our descriptor been "clean"?  0 if we need to regenerate it
 * now. */
static time_t desc_clean_since = 0;
/** Boolean: do we need to regenerate the above? */
static int desc_needs_upload = 0;

/** OR only: If <b>force</b> is true, or we haven't uploaded this
 * descriptor successfully yet, try to upload our signed descriptor to
 * all the directory servers we know about.
 */
void
router_upload_dir_desc_to_dirservers(int force)
{
  const char *s;

  s = router_get_my_descriptor();
  if (!s) {
    log_fn(LOG_WARN, "No descriptor; skipping upload");
    return;
  }
  if (!force && !desc_needs_upload)
    return;
  desc_needs_upload = 0;
  directory_post_to_dirservers(DIR_PURPOSE_UPLOAD_DIR, s, strlen(s));
}

/** OR only: Check whether my exit policy says to allow connection to
 * conn.  Return 0 if we accept; non-0 if we reject.
 */
int
router_compare_to_my_exit_policy(connection_t *conn)
{
  tor_assert(desc_routerinfo);

  /* make sure it's resolved to something. this way we can't get a
     'maybe' below. */
  if (!conn->addr)
    return -1;

  return router_compare_addr_to_addr_policy(conn->addr, conn->port,
                   desc_routerinfo->exit_policy) != ADDR_POLICY_ACCEPTED;
}

/** Return true iff I'm a server and <b>digest</b> is equal to
 * my identity digest. */
int
router_digest_is_me(const char *digest)
{
  routerinfo_t *me = router_get_my_routerinfo();
  if (!me || memcmp(me->identity_digest, digest, DIGEST_LEN))
    return 0;
  return 1;
}

/** A wrapper around router_digest_is_me(). */
int
router_is_me(routerinfo_t *router)
{
  return router_digest_is_me(router->identity_digest);
}

/** Return a routerinfo for this OR, rebuilding a fresh one if
 * necessary.  Return NULL on error, or if called on an OP. */
routerinfo_t *
router_get_my_routerinfo(void)
{
  if (!server_mode(get_options()))
    return NULL;

  if (!desc_routerinfo) {
    if (router_rebuild_descriptor(1))
      return NULL;
  }
  return desc_routerinfo;
}

/** OR only: Return a signed server descriptor for this OR, rebuilding a fresh
 * one if necessary.  Return NULL on error.
 */
const char *
router_get_my_descriptor(void)
{
  if (!desc_routerinfo) {
    if (router_rebuild_descriptor(1))
      return NULL;
  }
  log_fn(LOG_DEBUG,"my desc is '%s'",desc_routerinfo->signed_descriptor);
  return desc_routerinfo->signed_descriptor;
}

/** If <b>force</b> is true, or our descriptor is out-of-date, rebuild
 * a fresh routerinfo and signed server descriptor for this OR.
 * Return 0 on success, -1 on error.
 */
int
router_rebuild_descriptor(int force)
{
  routerinfo_t *ri;
  uint32_t addr;
  char platform[256];
  int hibernating = we_are_hibernating();
  or_options_t *options = get_options();

  if (desc_clean_since && !force)
    return 0;

  if (resolve_my_address(options, &addr, NULL) < 0) {
    log_fn(LOG_WARN,"options->Address didn't resolve into an IP.");
    return -1;
  }

  ri = tor_malloc_zero(sizeof(routerinfo_t));
  ri->address = tor_dup_addr(addr);
  ri->nickname = tor_strdup(options->Nickname);
  ri->addr = addr;
  ri->or_port = options->ORPort;
  ri->dir_port = hibernating ?
                 0 : options->DirPort;
  ri->published_on = time(NULL);
  ri->onion_pkey = crypto_pk_dup_key(get_onion_key()); /* must invoke from main thread */
  ri->identity_pkey = crypto_pk_dup_key(get_identity_key());
  if (crypto_pk_get_digest(ri->identity_pkey, ri->identity_digest)<0) {
    routerinfo_free(ri);
    return -1;
  }
  get_platform_str(platform, sizeof(platform));
  ri->platform = tor_strdup(platform);
  ri->bandwidthrate = (int)options->BandwidthRate;
  ri->bandwidthburst = (int)options->BandwidthBurst;
  ri->bandwidthcapacity = hibernating ? 0 : rep_hist_bandwidth_assess();

  if (options->BandwidthRate > options->MaxAdvertisedBandwidth)
    ri->bandwidthrate = (int)options->MaxAdvertisedBandwidth;

  config_parse_addr_policy(get_options()->ExitPolicy, &ri->exit_policy, -1);
  options_append_default_exit_policy(&ri->exit_policy);

  if (desc_routerinfo) /* inherit values */
    ri->is_verified = desc_routerinfo->is_verified;
  if (options->MyFamily) {
    ri->declared_family = smartlist_create();
    smartlist_split_string(ri->declared_family, options->MyFamily, ",",
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  }
  ri->signed_descriptor = tor_malloc(8192);
  if (router_dump_router_to_string(ri->signed_descriptor, 8192,
                                   ri, get_identity_key())<0) {
    log_fn(LOG_WARN, "Couldn't dump router to string.");
    return -1;
  }
  ri->signed_descriptor_len = strlen(ri->signed_descriptor);
  crypto_digest(ri->signed_descriptor_digest,
                ri->signed_descriptor, ri->signed_descriptor_len);

  if (desc_routerinfo)
    routerinfo_free(desc_routerinfo);
  desc_routerinfo = ri;

  desc_clean_since = time(NULL);
  desc_needs_upload = 1;
  return 0;
}

/** Mark descriptor out of date if it's older than <b>when</b> */
void
mark_my_descriptor_dirty_if_older_than(time_t when)
{
  if (desc_clean_since < when)
    mark_my_descriptor_dirty();
}

/** Call when the current descriptor is out of date. */
void
mark_my_descriptor_dirty(void)
{
  desc_clean_since = 0;
}

#define MAX_BANDWIDTH_CHANGE_FREQ 20*60
/** Check whether bandwidth has changed a lot since the last time we announced
 * bandwidth.  If so, mark our descriptor dirty.*/
void
check_descriptor_bandwidth_changed(time_t now)
{
  static time_t last_changed = 0;
  uint64_t prev, cur;
  if (!desc_routerinfo)
    return;

  prev = desc_routerinfo->bandwidthcapacity;
  cur = we_are_hibernating() ? 0 : rep_hist_bandwidth_assess();
  if ((prev != cur && (!prev || !cur)) ||
      cur > prev*2 ||
      cur < prev/2) {
    if (last_changed+MAX_BANDWIDTH_CHANGE_FREQ < now) {
      log_fn(LOG_INFO,"Measured bandwidth has changed; rebuilding descriptor.");
      mark_my_descriptor_dirty();
      last_changed = now;
    }
  }
}

/** Set <b>platform</b> (max length <b>len</b>) to a NUL-terminated short
 * string describing the version of Tor and the operating system we're
 * currently running on.
 */
void
get_platform_str(char *platform, size_t len)
{
  tor_snprintf(platform, len, "Tor %s on %s",
           VERSION, get_uname());
  return;
}

/* XXX need to audit this thing and count fenceposts. maybe
 *     refactor so we don't have to keep asking if we're
 *     near the end of maxlen?
 */
#define DEBUG_ROUTER_DUMP_ROUTER_TO_STRING

/** OR only: Given a routerinfo for this router, and an identity key to sign
 * with, encode the routerinfo as a signed server descriptor and write the
 * result into <b>s</b>, using at most <b>maxlen</b> bytes.  Return -1 on
 * failure, and the number of bytes used on success.
 */
int
router_dump_router_to_string(char *s, size_t maxlen, routerinfo_t *router,
                             crypto_pk_env_t *ident_key)
{
  char *onion_pkey; /* Onion key, PEM-encoded. */
  char *identity_pkey; /* Identity key, PEM-encoded. */
  char digest[DIGEST_LEN];
  char published[ISO_TIME_LEN+1];
  char fingerprint[FINGERPRINT_LEN+1];
  struct in_addr in;
  char addrbuf[INET_NTOA_BUF_LEN];
  size_t onion_pkeylen, identity_pkeylen;
  size_t written;
  int result=0;
  addr_policy_t *tmpe;
  char *bandwidth_usage;
  char *family_line;
#ifdef DEBUG_ROUTER_DUMP_ROUTER_TO_STRING
  char *s_tmp, *s_dup;
  const char *cp;
  routerinfo_t *ri_tmp;
#endif
  or_options_t *options = get_options();

  /* Make sure the identity key matches the one in the routerinfo. */
  if (crypto_pk_cmp_keys(ident_key, router->identity_pkey)) {
    log_fn(LOG_WARN,"Tried to sign a router with a private key that didn't match router's public key!");
    return -1;
  }

  /* record our fingerprint, so we can include it in the descriptor */
  if (crypto_pk_get_fingerprint(router->identity_pkey, fingerprint, 1)<0) {
    log_fn(LOG_ERR, "Error computing fingerprint");
    return -1;
  }

  /* PEM-encode the onion key */
  if (crypto_pk_write_public_key_to_string(router->onion_pkey,
                                           &onion_pkey,&onion_pkeylen)<0) {
    log_fn(LOG_WARN,"write onion_pkey to string failed!");
    return -1;
  }

  /* PEM-encode the identity key key */
  if (crypto_pk_write_public_key_to_string(router->identity_pkey,
                                           &identity_pkey,&identity_pkeylen)<0) {
    log_fn(LOG_WARN,"write identity_pkey to string failed!");
    tor_free(onion_pkey);
    return -1;
  }

  /* Encode the publication time. */
  format_iso_time(published, router->published_on);

  /* How busy have we been? */
  bandwidth_usage = rep_hist_get_bandwidth_lines();

  if (router->declared_family && smartlist_len(router->declared_family)) {
    size_t n;
    char *s = smartlist_join_strings(router->declared_family, " ", 0, &n);
    n += strlen("family ") + 2; /* 1 for \n, 1 for \0. */
    family_line = tor_malloc(n);
    tor_snprintf(family_line, n, "family %s\n", s);
    tor_free(s);
  } else {
    family_line = tor_strdup("");
  }

  /* Generate the easy portion of the router descriptor. */
  result = tor_snprintf(s, maxlen,
                    "router %s %s %d 0 %d\n"
                    "platform %s\n"
                    "published %s\n"
                    "opt fingerprint %s\n"
                    "uptime %ld\n"
                    "bandwidth %d %d %d\n"
                    "onion-key\n%s"
                    "signing-key\n%s%s%s%s",
    router->nickname,
    router->address,
    router->or_port,
    (authdir_mode(options) || check_whether_dirport_reachable()) ?
      router->dir_port : 0,
    router->platform,
    published,
    fingerprint,
    stats_n_seconds_working,
    (int) router->bandwidthrate,
    (int) router->bandwidthburst,
    (int) router->bandwidthcapacity,
    onion_pkey, identity_pkey,
    family_line, bandwidth_usage,
    we_are_hibernating() ? "opt hibernating 1\n" : "");
  tor_free(family_line);
  tor_free(onion_pkey);
  tor_free(identity_pkey);
  tor_free(bandwidth_usage);

  if (result < 0)
    return -1;
  /* From now on, we use 'written' to remember the current length of 's'. */
  written = result;

  if (options->ContactInfo && strlen(options->ContactInfo)) {
    result = tor_snprintf(s+written,maxlen-written, "contact %s\n",
                          options->ContactInfo);
    if (result<0)
      return -1;
    written += result;
  }

  /* Write the exit policy to the end of 's'. */
  for (tmpe=router->exit_policy; tmpe; tmpe=tmpe->next) {
    /* Write: "accept 1.2.3.4" */
    in.s_addr = htonl(tmpe->addr);
    tor_inet_ntoa(&in, addrbuf, sizeof(addrbuf));
    result = tor_snprintf(s+written, maxlen-written, "%s %s",
        tmpe->policy_type == ADDR_POLICY_ACCEPT ? "accept" : "reject",
        tmpe->msk == 0 ? "*" : addrbuf);
    if (result < 0)
      return -1;
    written += result;
    if (tmpe->msk != 0xFFFFFFFFu && tmpe->msk != 0) {
      /* Write "/255.255.0.0" */
      in.s_addr = htonl(tmpe->msk);
      tor_inet_ntoa(&in, addrbuf, sizeof(addrbuf));
      result = tor_snprintf(s+written, maxlen-written, "/%s", addrbuf);
      if (result<0)
        return -1;
      written += result;
    }
    if (tmpe->prt_min <= 1 && tmpe->prt_max == 65535) {
      /* There is no port set; write ":*" */
      if (written+4 > maxlen)
        return -1;
      strlcat(s+written, ":*\n", maxlen-written);
      written += 3;
    } else if (tmpe->prt_min == tmpe->prt_max) {
      /* There is only one port; write ":80". */
      result = tor_snprintf(s+written, maxlen-written, ":%d\n", tmpe->prt_min);
      if (result<0)
        return -1;
      written += result;
    } else {
      /* There is a range of ports; write ":79-80". */
      result = tor_snprintf(s+written, maxlen-written, ":%d-%d\n", tmpe->prt_min,
                        tmpe->prt_max);
      if (result<0)
        return -1;
      written += result;
    }
    if (tmpe->msk == 0 && tmpe->prt_min <= 1 && tmpe->prt_max == 65535)
      /* This was a catch-all rule, so future rules are irrelevant. */
      break;
  } /* end for */
  if (written+256 > maxlen) /* Not enough room for signature. */
    return -1;

  /* Sign the directory */
  strlcat(s+written, "router-signature\n", maxlen-written);
  written += strlen(s+written);
  s[written] = '\0';
  if (router_get_router_hash(s, digest) < 0)
    return -1;

  if (router_append_dirobj_signature(s+written,maxlen-written,
                                     digest,ident_key)<0) {
    log_fn(LOG_WARN, "Couldn't sign router descriptor");
    return -1;
  }
  written += strlen(s+written);

  if (written+2 > maxlen)
    return -1;
  /* include a last '\n' */
  s[written] = '\n';
  s[written+1] = 0;

#ifdef DEBUG_ROUTER_DUMP_ROUTER_TO_STRING
  cp = s_tmp = s_dup = tor_strdup(s);
  ri_tmp = router_parse_entry_from_string(cp, NULL);
  if (!ri_tmp) {
    log_fn(LOG_ERR, "We just generated a router descriptor we can't parse: <<%s>>",
           s);
    return -1;
  }
  tor_free(s_dup);
  routerinfo_free(ri_tmp);
#endif

  return written+1;
}

/** Return true iff <b>s</b> is a legally valid server nickname. */
int
is_legal_nickname(const char *s)
{
  size_t len;
  tor_assert(s);
  len = strlen(s);
  return len > 0 && len <= MAX_NICKNAME_LEN &&
    strspn(s,LEGAL_NICKNAME_CHARACTERS)==len;
}
/** Return true iff <b>s</b> is a legally valid server nickname or
 * hex-encoded identity-key digest. */
int
is_legal_nickname_or_hexdigest(const char *s)
{
  size_t len;
  tor_assert(s);
  if (*s!='$')
    return is_legal_nickname(s);

  len = strlen(s);
  return len == HEX_DIGEST_LEN+1 && strspn(s+1,HEX_CHARACTERS)==len-1;
}

/** Release all resources held in router keys. */
void
router_free_all_keys(void)
{
  if (onionkey)
    crypto_free_pk_env(onionkey);
  if (lastonionkey)
    crypto_free_pk_env(lastonionkey);
  if (identitykey)
    crypto_free_pk_env(identitykey);
  if (key_lock)
    tor_mutex_free(key_lock);
  if (desc_routerinfo)
    routerinfo_free(desc_routerinfo);
}

