/* Copyright 2001,2002,2003 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

/**
 * \file dirserv.c
 * \brief Directory server core implementation.
 **/

/** How far in the future do we allow a router to get? (seconds) */
#define ROUTER_ALLOW_SKEW (30*60)
/** How many seconds do we wait before regenerating the directory? */
#define DIR_REGEN_SLACK_TIME 10

extern or_options_t options; /**< command-line and config-file options */

/** Do we need to regenerate the directory when someone asks for it? */
static int the_directory_is_dirty = 1;
static int runningrouters_is_dirty = 1;

static int list_server_status(char **running_routers_out,
                              char **router_status_out);
static void directory_remove_unrecognized(void);
static int dirserv_regenerate_directory(void);

/************** Fingerprint handling code ************/

typedef struct fingerprint_entry_t {
  char *nickname;
  char *fingerprint; /**< Stored as HEX_DIGEST_LEN characters, followed by a NUL */
} fingerprint_entry_t;

/** List of nickname-\>identity fingerprint mappings for all the routers
 * that we recognize. Used to prevent Sybil attacks. */
static smartlist_t *fingerprint_list = NULL;

/** Add the fingerprint <b>fp</b> for the nickname <b>nickname</b> to
 * the global list of recognized identity key fingerprints.
 */
void /* Should be static; exposed for testing */
add_fingerprint_to_dir(const char *nickname, const char *fp)
{
  int i;
  fingerprint_entry_t *ent;
  if (!fingerprint_list)
    fingerprint_list = smartlist_create();

  for (i = 0; i < smartlist_len(fingerprint_list); ++i) {
    ent = smartlist_get(fingerprint_list, i);
    if (!strcasecmp(ent->nickname,nickname)) {
      tor_free(ent->fingerprint);
      ent->fingerprint = tor_strdup(fp);
      return;
    }
  }
  ent = tor_malloc(sizeof(fingerprint_entry_t));
  ent->nickname = tor_strdup(nickname);
  ent->fingerprint = tor_strdup(fp);
  tor_strstrip(ent->fingerprint, " ");
  smartlist_add(fingerprint_list, ent);
}

/** Add the nickname and fingerprint for this OR to the recognized list.
 */
int
dirserv_add_own_fingerprint(const char *nickname, crypto_pk_env_t *pk)
{
  char fp[FINGERPRINT_LEN+1];
  if (crypto_pk_get_fingerprint(pk, fp, 0)<0) {
    log_fn(LOG_ERR, "Error computing fingerprint");
    return -1;
  }
  add_fingerprint_to_dir(nickname, fp);
  return 0;
}

/** Parse the nickname-\>fingerprint mappings stored in the file named
 * <b>fname</b>.  The file format is line-based, with each non-blank
 * holding one nickname, some space, and a fingerprint for that
 * nickname.  On success, replace the current fingerprint list with
 * the contents of <b>fname</b> and return 0.  On failure, leave the
 * current fingerprint list untouched, and return -1. */
int
dirserv_parse_fingerprint_file(const char *fname)
{
  FILE *file;
  char line[FINGERPRINT_LEN+MAX_NICKNAME_LEN+20+1];
  char *nickname, *fingerprint;
  smartlist_t *fingerprint_list_new;
  int i, result;
  fingerprint_entry_t *ent;

  if(!(file = fopen(fname, "r"))) {
    log_fn(LOG_WARN, "Cannot open fingerprint file %s", fname);
    return -1;
  }
  fingerprint_list_new = smartlist_create();
  while( (result=parse_line_from_file(line, sizeof(line),file,&nickname,&fingerprint)) > 0) {
    if (strlen(nickname) > MAX_NICKNAME_LEN) {
      log(LOG_WARN, "Nickname %s too long in fingerprint file. Skipping.", nickname);
      continue;
    }
    if(strlen(fingerprint) != FINGERPRINT_LEN ||
       !crypto_pk_check_fingerprint_syntax(fingerprint)) {
      log_fn(LOG_WARN, "Invalid fingerprint (nickname %s, fingerprint %s). Skipping.",
             nickname, fingerprint);
      continue;
    }
    for (i = 0; i < smartlist_len(fingerprint_list_new); ++i) {
      ent = smartlist_get(fingerprint_list_new, i);
      if (0==strcasecmp(ent->nickname, nickname)) {
        log(LOG_WARN, "Duplicate nickname %s. Skipping.",nickname);
        break; /* out of the for. the 'if' below means skip to the next line. */
      }
    }
    if(i == smartlist_len(fingerprint_list_new)) { /* not a duplicate */
      ent = tor_malloc(sizeof(fingerprint_entry_t));
      ent->nickname = tor_strdup(nickname);
      ent->fingerprint = tor_strdup(fingerprint);
      tor_strstrip(ent->fingerprint, " ");
      smartlist_add(fingerprint_list_new, ent);
    }
  }
  fclose(file);
  if(result == 0) { /* eof; replace the global fingerprints list. */
    dirserv_free_fingerprint_list();
    fingerprint_list = fingerprint_list_new;
    /* Delete any routers whose fingerprints we no longer recognize */
    directory_remove_unrecognized();
    return 0;
  }
  /* error */
  log_fn(LOG_WARN, "Error reading from fingerprint file");
  for (i = 0; i < smartlist_len(fingerprint_list_new); ++i) {
    ent = smartlist_get(fingerprint_list_new, i);
    tor_free(ent->nickname);
    tor_free(ent->fingerprint);
    tor_free(ent);
  }
  smartlist_free(fingerprint_list_new);
  return -1;
}

/** Check whether <b>router</b> has a nickname/identity key combination that
 * we recognize from the fingerprint list.  Return 1 if router's
 * identity and nickname match, -1 if we recognize the nickname but
 * the identity key is wrong, and 0 if the nickname is not known. */
int
dirserv_router_fingerprint_is_known(const routerinfo_t *router)
{
  int i, found=0;
  fingerprint_entry_t *ent =NULL;
  char fp[FINGERPRINT_LEN+1];

  if (!fingerprint_list)
    fingerprint_list = smartlist_create();

  log_fn(LOG_DEBUG, "%d fingerprints known.", smartlist_len(fingerprint_list));
  for (i=0;i<smartlist_len(fingerprint_list);++i) {
    ent = smartlist_get(fingerprint_list, i);
    log_fn(LOG_DEBUG,"%s vs %s", router->nickname, ent->nickname);
    if (!strcasecmp(router->nickname,ent->nickname)) {
      found = 1;
      break;
    }
  }

  if (!found) { /* No such server known */
    log_fn(LOG_INFO,"no fingerprint found for %s",router->nickname);
    return 0;
  }
  if (crypto_pk_get_fingerprint(router->identity_pkey, fp, 0)) {
    log_fn(LOG_WARN,"error computing fingerprint");
    return -1;
  }
  if (0==strcasecmp(ent->fingerprint, fp)) {
    log_fn(LOG_DEBUG,"good fingerprint for %s",router->nickname);
    return 1; /* Right fingerprint. */
  } else {
    log_fn(LOG_WARN,"mismatched fingerprint for %s",router->nickname);
    return -1; /* Wrong fingerprint. */
  }
}

/** If we are an authoritative dirserver, and the list of approved
 * servers contains one whose identity key digest is <b>digest</b>,
 * return that router's nickname.  Otherwise return NULL. */
const char *dirserv_get_nickname_by_digest(const char *digest)
{
  char hexdigest[HEX_DIGEST_LEN+1];
  if (!fingerprint_list)
    return NULL;
  tor_assert(digest);

  base16_encode(hexdigest, HEX_DIGEST_LEN+1, digest, DIGEST_LEN);
  SMARTLIST_FOREACH(fingerprint_list, fingerprint_entry_t*, ent,
                    { if (!strcasecmp(hexdigest, ent->fingerprint))
                         return ent->nickname; } );
  return NULL;
}

#if 0
/** Return true iff any router named <b>nickname</b> with <b>digest</b>
 * is in the verified fingerprint list. */
static int
router_nickname_is_approved(const char *nickname, const char *digest)
{
  const char *n;

  n = dirserv_get_nickname_by_digest(digest);
  if (n && !strcasecmp(n,nickname))
    return 1;
  else
    return 0;
}
#endif

/** Clear the current fingerprint list. */
void
dirserv_free_fingerprint_list()
{
  int i;
  fingerprint_entry_t *ent;
  if (!fingerprint_list)
    return;

  for (i = 0; i < smartlist_len(fingerprint_list); ++i) {
    ent = smartlist_get(fingerprint_list, i);
    tor_free(ent->nickname);
    tor_free(ent->fingerprint);
    tor_free(ent);
  }
  smartlist_free(fingerprint_list);
  fingerprint_list = NULL;
}

/*
 *    Descriptor list
 */

/** A directory server's view of a server descriptor.  Contains both
 * parsed and unparsed versions. */
typedef struct descriptor_entry_t {
  char *nickname;
  time_t published;
  size_t desc_len;
  char *descriptor;
  int verified;
  routerinfo_t *router;
} descriptor_entry_t;

/** List of all server descriptors that this dirserv is holding. */
static smartlist_t *descriptor_list = NULL;

/** Release the storage held by <b>desc</b> */
static void free_descriptor_entry(descriptor_entry_t *desc)
{
  tor_free(desc->descriptor);
  tor_free(desc->nickname);
  routerinfo_free(desc->router);
  tor_free(desc);
}

/** Release all storage that the dirserv is holding for server
 * descriptors. */
void
dirserv_free_descriptors()
{
  if (!descriptor_list)
    return;
  SMARTLIST_FOREACH(descriptor_list, descriptor_entry_t *, d,
                    free_descriptor_entry(d));
  smartlist_clear(descriptor_list);
}

/** Parse the server descriptor at *desc and maybe insert it into the
 * list of server descriptors, and (if the descriptor is well-formed)
 * advance *desc immediately past the descriptor's end.
 *
 * Return 1 if descriptor is well-formed and accepted;
 * 0 if well-formed and server is unapproved;
 * -1 if not well-formed or other error.
 */
int
dirserv_add_descriptor(const char **desc)
{
  descriptor_entry_t *ent = NULL;
  routerinfo_t *ri = NULL;
  int i, r, found=-1;
  char *start, *end;
  char *desc_tmp = NULL;
  const char *cp;
  size_t desc_len;
  time_t now;
  int verified=1; /* whether we knew its fingerprint already */

  if (!descriptor_list)
    descriptor_list = smartlist_create();

  start = strstr(*desc, "router ");
  if (!start) {
    log_fn(LOG_WARN, "no 'router' line found. This is not a descriptor.");
    return -1;
  }
  if ((end = strstr(start+6, "\nrouter "))) {
    ++end; /* Include NL. */
  } else if ((end = strstr(start+6, "\ndirectory-signature"))) {
    ++end;
  } else {
    end = start+strlen(start);
  }
  desc_len = end-start;
  cp = desc_tmp = tor_strndup(start, desc_len);

  /* Check: is the descriptor syntactically valid? */
  ri = router_parse_entry_from_string(cp, NULL);
  tor_free(desc_tmp);
  if (!ri) {
    log(LOG_WARN, "Couldn't parse descriptor");
    return -1;
  }
  /* Okay.  Now check whether the fingerprint is recognized. */
  r = dirserv_router_fingerprint_is_known(ri);
  if(r==-1) {
    log_fn(LOG_WARN, "Known nickname %s, wrong fingerprint. Not adding.", ri->nickname);
    routerinfo_free(ri);
    *desc = end;
    return 0;
  }
  if(r==0) {
    char fp[FINGERPRINT_LEN+1];
    log_fn(LOG_INFO, "Unknown nickname %s (%s:%d). Adding.",
           ri->nickname, ri->address, ri->or_port);
    if (crypto_pk_get_fingerprint(ri->identity_pkey, fp, 1) < 0) {
      log_fn(LOG_WARN, "Error computing fingerprint for %s", ri->nickname);
    } else {
      log_fn(LOG_INFO, "Fingerprint line: %s %s", ri->nickname, fp);
    }
    verified = 0;
  }
  /* Is there too much clock skew? */
  now = time(NULL);
  if (ri->published_on > now+ROUTER_ALLOW_SKEW) {
    log_fn(LOG_WARN, "Publication time for nickname %s is too far in the future; possible clock skew. Not adding.", ri->nickname);
    routerinfo_free(ri);
    *desc = end;
    return 0;
  }
  if (ri->published_on < now-ROUTER_MAX_AGE) {
    log_fn(LOG_WARN, "Publication time for router with nickname %s is too far in the past. Not adding.", ri->nickname);
    routerinfo_free(ri);
    *desc = end;
    return 0;
  }

  /* Do we already have an entry for this router? */
  for (i = 0; i < smartlist_len(descriptor_list); ++i) {
    ent = smartlist_get(descriptor_list, i);
    if (!strcasecmp(ri->nickname, ent->nickname)) {
      found = i;
      break;
    }
  }
  if (found >= 0) {
    /* if so, decide whether to update it. */
    if (ent->published >= ri->published_on) {
      /* We already have a newer or equal-time descriptor */
      log_fn(LOG_INFO,"We already have a new enough desc for nickname %s. Not adding.",ri->nickname);
      /* This isn't really an error; return success. */
      routerinfo_free(ri);
      *desc = end;
      return 1;
    }
    /* We don't have a newer one; we'll update this one. */
    log_fn(LOG_INFO,"Dirserv updating desc for nickname %s",ri->nickname);
    free_descriptor_entry(ent);
    smartlist_del_keeporder(descriptor_list, found);
  } else {
    /* Add at the end. */
    log_fn(LOG_INFO,"Dirserv adding desc for nickname %s",ri->nickname);
  }

  ent = tor_malloc(sizeof(descriptor_entry_t));
  ent->nickname = tor_strdup(ri->nickname);
  ent->published = ri->published_on;
  ent->desc_len = desc_len;
  ent->descriptor = tor_strndup(start,desc_len);
  ent->router = ri;
  /* XXX008 is ent->verified useful/used for anything? */
  ent->verified = verified; /* XXXX008 support other possibilities. */
  smartlist_add(descriptor_list, ent);

  *desc = end;
  directory_set_dirty();

  return 1;
}

/** Remove all descriptors whose nicknames or fingerprints we don't
 * recognize.  (Descriptors that used to be good can become
 * unrecognized when we reload the fingerprint list.)
 */
static void
directory_remove_unrecognized(void)
{
  int i;
  descriptor_entry_t *ent;
  if (!descriptor_list)
    descriptor_list = smartlist_create();

  for (i = 0; i < smartlist_len(descriptor_list); ++i) {
    ent = smartlist_get(descriptor_list, i);
    if (dirserv_router_fingerprint_is_known(ent->router)<=0) {
      log(LOG_INFO, "Router %s is no longer recognized",
          ent->nickname);
      free_descriptor_entry(ent);
      smartlist_del(descriptor_list, i--);
    }
  }
}

/** Mark the directory as <b>dirty</b> -- when we're next asked for a
 * directory, we will rebuild it instead of reusing the most recently
 * generated one.
 */
void
directory_set_dirty()
{
  time_t now = time(NULL);

  if(!the_directory_is_dirty)
    the_directory_is_dirty = now;
  if(!runningrouters_is_dirty)
    runningrouters_is_dirty = now;
}

/** Load all descriptors from a directory stored in the string
 * <b>dir</b>.
 */
int
dirserv_load_from_directory_string(const char *dir)
{
  const char *cp = dir;
  while(1) {
    cp = strstr(cp, "\nrouter ");
    if (!cp) break;
    ++cp;
    if (dirserv_add_descriptor(&cp) < 0) {
      return -1;
    }
    --cp; /*Back up to newline.*/
  }
  return 0;
}

/**
 * Allocate and return a description of the status of the server <b>desc</b>,
 * for use in a running-routers line (if <b>rr_format</b> is true), or in a
 * router-status line (if <b>rr_format</b> is false.  The server is listed 
 * as running iff <b>is_live</b> is true.
 */
static char *
list_single_server_status(descriptor_entry_t *desc, int is_live,
                          int rr_format)
{
  char buf[MAX_NICKNAME_LEN+HEX_DIGEST_LEN+4]; /* !nickname=$hexdigest\0 */
  char *cp;

  tor_assert(desc);
  tor_assert(desc->router);

  cp = buf;
  if (!is_live) {
    *cp++ = '!';
  }
  if (desc->verified) {
    strlcpy(cp, desc->nickname, sizeof(buf)-(cp-buf));
    cp += strlen(cp);
    if (!rr_format)
      *cp++ = '=';
  }
  if (!desc->verified || !rr_format) {
    *cp++ = '$';
    base16_encode(cp, HEX_DIGEST_LEN+1, desc->router->identity_digest,
                  DIGEST_LEN);
  }
  return tor_strdup(buf);
}

/** Allocate the contents of a running-routers line and a router-status line,
 * and store them in *<b>running_routers_out</b> and *<b>router_status_out</b>
 * respectively.  Return 0 on success, -1 on failure.
 */
static int
list_server_status(char **running_routers_out, char **router_status_out)
{
  /* List of entries in running-routers style: An optional !, then either
   * a nickname or a dollar-prefixed hexdigest. */
  smartlist_t *rr_entries; 
  /* List of entries in a router-status style: An optional !, then an optional
   * equals-suffixed nickname, then a dollar-prefixed hexdigest. */
  smartlist_t *rs_entries;

  tor_assert(running_routers_out || router_status_out);

  rr_entries = smartlist_create();
  rs_entries = smartlist_create();

  SMARTLIST_FOREACH(descriptor_list, descriptor_entry_t *, d,
  {
    int is_live;
    tor_assert(d->router);
    connection_t *conn = connection_get_by_identity_digest(
                    d->router->identity_digest, CONN_TYPE_OR);
    is_live = (conn && conn->state == OR_CONN_STATE_OPEN);
    smartlist_add(rr_entries, list_single_server_status(d, is_live, 1));
    smartlist_add(rs_entries, list_single_server_status(d, is_live, 0));
  });

  if (running_routers_out)
    *running_routers_out = smartlist_join_strings(rr_entries, " ", 0);
  if (router_status_out)
    *router_status_out = smartlist_join_strings(rs_entries, " ", 0);

  SMARTLIST_FOREACH(rr_entries, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(rs_entries, char *, cp, tor_free(cp));
  smartlist_free(rr_entries);
  smartlist_free(rs_entries);

  return 0;
}

/** Remove any descriptors from the directory that are more than <b>age</b>
 * seconds old.
 */
void
dirserv_remove_old_servers(int age)
{
  int i;
  time_t cutoff;
  descriptor_entry_t *ent;
  if (!descriptor_list)
    descriptor_list = smartlist_create();

  cutoff = time(NULL) - age;
  for (i = 0; i < smartlist_len(descriptor_list); ++i) {
    ent = smartlist_get(descriptor_list, i);
    if (ent->published <= cutoff) {
      /* descriptor_list[i] is too old.  Remove it. */
      free_descriptor_entry(ent);
      smartlist_del(descriptor_list, i--);
      directory_set_dirty();
    }
  }
}

/** Dump all routers currently in the directory into the string
 * <b>s</b>, using at most <b>maxlen</b> characters, and signing the
 * directory with <b>private_key</b>.  Return 0 on success, -1 on
 * failure.
 */
int
dirserv_dump_directory_to_string(char *s, size_t maxlen,
                                 crypto_pk_env_t *private_key)
{
  char *eos, *cp;
  char *running_routers, *router_status;
  char *identity_pkey; /* Identity key, DER64-encoded. */
  char *recommended_versions;
  char digest[20];
  char signature[128];
  char published[33];
  time_t published_on;
  int i;
  eos = s+maxlen;

  if (!descriptor_list)
    descriptor_list = smartlist_create();

  if (list_server_status(&running_routers, &router_status))
    return -1;

  /* ASN.1-encode the public key.  This is a temporary measure; once
   * everyone is running 0.0.9pre3 or later, we can shift to using a
   * PEM-encoded key instead.
   */
#if 1
  if(crypto_pk_DER64_encode_public_key(private_key, &identity_pkey)<0) {
    log_fn(LOG_WARN,"write identity_pkey to string failed!");
    return -1;
  }
#else
  { int l;
    if(crypto_pk_write_public_key_to_string(private_key, &identity_pkey, &l)<0){
      log_fn(LOG_WARN,"write identity_pkey to string failed!");
      return -1;
    }
  }
#endif

  {
    smartlist_t *versions;
    struct config_line_t *ln;
    versions = smartlist_create();
    for (ln = options.RecommendedVersions; ln; ln = ln->next) {
      smartlist_split_string(versions, ln->value, ",", 
                             SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
    }
    recommended_versions = smartlist_join_strings(versions,",",0);
    SMARTLIST_FOREACH(versions,char *,s,tor_free(s));
    smartlist_free(versions);
  }
  
  dirserv_remove_old_servers(ROUTER_MAX_AGE);
  published_on = time(NULL);
  format_iso_time(published, published_on);
  tor_snprintf(s, maxlen,
           "signed-directory\n"
           "published %s\n"
           "recommended-software %s\n"
           "running-routers %s\n"
           "opt router-status %s\n"
           "opt dir-signing-key %s\n\n",
           published, recommended_versions, running_routers, router_status,
           identity_pkey);

  tor_free(running_routers);
  tor_free(router_status);
  tor_free(identity_pkey);
  i = strlen(s);
  cp = s+i;

  SMARTLIST_FOREACH(descriptor_list, descriptor_entry_t *, d,
    if (strlcat(s, d->descriptor, maxlen) >= maxlen)
      goto truncated);

  /* These multiple strlcat calls are inefficient, but dwarfed by the RSA
     signature.
  */
  if (strlcat(s, "directory-signature ", maxlen) >= maxlen)
    goto truncated;
  if (strlcat(s, options.Nickname, maxlen) >= maxlen)
    goto truncated;
  if (strlcat(s, "\n", maxlen) >= maxlen)
    goto truncated;

  if (router_get_dir_hash(s,digest)) {
    log_fn(LOG_WARN,"couldn't compute digest");
    return -1;
  }
  if (crypto_pk_private_sign(private_key, digest, 20, signature) < 0) {
    log_fn(LOG_WARN,"couldn't sign digest");
    return -1;
  }
  log(LOG_DEBUG,"generated directory digest begins with %s",hex_str(digest,4));

  if (strlcat(cp, "-----BEGIN SIGNATURE-----\n", maxlen) >= maxlen)
    goto truncated;

  i = strlen(s);
  cp = s+i;
  if (base64_encode(cp, maxlen-i, signature, 128) < 0) {
    log_fn(LOG_WARN,"couldn't base64-encode signature");
    return -1;
  }

  if (strlcat(s, "-----END SIGNATURE-----\n", maxlen) >= maxlen)
    goto truncated;

  return 0;
 truncated:
  log_fn(LOG_WARN,"tried to exceed string length.");
  return -1;
}

/** Most recently generated encoded signed directory. */
static char *the_directory = NULL;
static size_t the_directory_len = 0;
static char *the_directory_z = NULL;
static size_t the_directory_z_len = 0;

static char *cached_directory = NULL; /* used only by non-auth dirservers */
static size_t cached_directory_len = 0;
static char *cached_directory_z = NULL;
static size_t cached_directory_z_len = 0;
static time_t cached_directory_published = 0;

void dirserv_set_cached_directory(const char *directory, time_t when)
{
  time_t now;
  char filename[512];
  tor_assert(!options.AuthoritativeDir);
  now = time(NULL);
  if (when<=cached_directory_published) {
    log_fn(LOG_INFO, "Ignoring old directory; not caching.");
  } else if (when>=now+ROUTER_ALLOW_SKEW) {
    log_fn(LOG_INFO, "Ignoring future directory; not caching.");
  } else if (when>cached_directory_published &&
        when<now+ROUTER_ALLOW_SKEW) {
    log_fn(LOG_DEBUG, "Caching directory.");
    tor_free(cached_directory);
    cached_directory = tor_strdup(directory);
    cached_directory_len = strlen(cached_directory);
    tor_free(cached_directory_z);
    if (tor_gzip_compress(&cached_directory_z, &cached_directory_z_len,
                          cached_directory, cached_directory_len,
                          ZLIB_METHOD)) {
      log_fn(LOG_WARN,"Error compressing cached directory");
    }
    cached_directory_published = when;
    if(get_data_directory(&options)) {
      tor_snprintf(filename,sizeof(filename),"%s/cached-directory", get_data_directory(&options));
      if(write_str_to_file(filename,cached_directory,0) < 0) {
        log_fn(LOG_WARN, "Couldn't write cached directory to disk. Ignoring.");
      }
    }
  }
}

/** Set *<b>directory</b> to the most recently generated encoded signed
 * directory, generating a new one as necessary. */
size_t dirserv_get_directory(const char **directory, int compress)
{
  if (!options.AuthoritativeDir) {
    if (compress?cached_directory_z:cached_directory) {
      *directory = compress?cached_directory_z:cached_directory;
      return compress?cached_directory_z_len:cached_directory_len;
    } else {
      /* no directory yet retrieved */
      return 0;
    }
  }
  if (the_directory_is_dirty &&
      the_directory_is_dirty + DIR_REGEN_SLACK_TIME < time(NULL)) {
    if (dirserv_regenerate_directory())
      return 0;
  } else {
    log(LOG_INFO,"Directory still clean, reusing.");
  }
  *directory = compress ? the_directory_z : the_directory;
  return compress ? the_directory_z_len : the_directory_len;
}

/**
 * Generate a fresh directory (authdirservers only.)
 */
static int dirserv_regenerate_directory(void)
{
  char *new_directory;
  char filename[512];

  new_directory = tor_malloc(MAX_DIR_SIZE);
  if (dirserv_dump_directory_to_string(new_directory, MAX_DIR_SIZE,
                                       get_identity_key())) {
    log(LOG_WARN, "Error creating directory.");
    tor_free(new_directory);
    return -1;
  }
  tor_free(the_directory);
  the_directory = new_directory;
  the_directory_len = strlen(the_directory);
  log_fn(LOG_INFO,"New directory (size %d):\n%s",(int)the_directory_len,
         the_directory);
  tor_free(the_directory_z);
  if (tor_gzip_compress(&the_directory_z, &the_directory_z_len,
                        the_directory, the_directory_len,
                        ZLIB_METHOD)) {
    log_fn(LOG_WARN, "Error gzipping directory.");
    return -1;
  }

  /* Now read the directory we just made in order to update our own
   * router lists.  This does more signature checking than is strictly
   * necessary, but safe is better than sorry. */
  new_directory = tor_strdup(the_directory);
  /* use a new copy of the dir, since get_dir_from_string scribbles on it */
  if (router_load_routerlist_from_directory(new_directory, get_identity_key(), 1)) {
    log_fn(LOG_ERR, "We just generated a directory we can't parse. Dying.");
    tor_cleanup();
    exit(0);
  }
  tor_free(new_directory);
  if(get_data_directory(&options)) {
    tor_snprintf(filename,sizeof(filename),"%s/cached-directory", get_data_directory(&options));
    if(write_str_to_file(filename,the_directory,0) < 0) {
      log_fn(LOG_WARN, "Couldn't write cached directory to disk. Ignoring.");
    }
  }
  the_directory_is_dirty = 0;

  return 0;
}

static char *runningrouters_string=NULL;
static size_t runningrouters_len=0;

/** Replace the current running-routers list with a newly generated one. */
static int generate_runningrouters(crypto_pk_env_t *private_key)
{
  char *s=NULL, *cp;
  char *router_status=NULL;
  char digest[DIGEST_LEN];
  char signature[PK_BYTES];
  int i;
  char published[33];
  size_t len;
  time_t published_on;
  char *identity_pkey; /* Identity key, DER64-encoded. */

  len = 1024+(MAX_HEX_NICKNAME_LEN+2)*smartlist_len(descriptor_list);
  s = tor_malloc_zero(len);
  if (list_server_status(NULL, &router_status)) {
    goto err;
  }
  /* ASN.1-encode the public key.  This is a temporary measure; once
   * everyone is running 0.0.9pre3 or later, we can shift to using a
   * PEM-encoded key instead.
   */
#if 1
  if(crypto_pk_DER64_encode_public_key(private_key, &identity_pkey)<0) {
    log_fn(LOG_WARN,"write identity_pkey to string failed!");
    goto err;
  }
#else
  { int l;
    if(crypto_pk_write_public_key_to_string(private_key, &identity_pkey, &l)<0){
      log_fn(LOG_WARN,"write identity_pkey to string failed!");
      goto err;
    }
  }
#endif
  published_on = time(NULL);
  format_iso_time(published, published_on);
  tor_snprintf(s, len, "network-status\n"
             "published %s\n"
             "router-status %s\n"
             "opt dir-signing-key %s\n"
             "directory-signature %s\n"
             "-----BEGIN SIGNATURE-----\n",
          published, router_status, identity_pkey, options.Nickname);
  tor_free(router_status);
  tor_free(identity_pkey);
  if (router_get_runningrouters_hash(s,digest)) {
    log_fn(LOG_WARN,"couldn't compute digest");
    goto err;
  }
  if (crypto_pk_private_sign(private_key, digest, 20, signature) < 0) {
    log_fn(LOG_WARN,"couldn't sign digest");
    goto err;
  }

  i = strlen(s);
  cp = s+i;
  if (base64_encode(cp, len-i, signature, 128) < 0) {
    log_fn(LOG_WARN,"couldn't base64-encode signature");
    goto err;
  }
  if (strlcat(s, "-----END SIGNATURE-----\n", len) >= len) {
    goto err;
  }

  tor_free(runningrouters_string);
  runningrouters_string = s;
  runningrouters_len = strlen(s);
  runningrouters_is_dirty = 0;
  return 0;
 err:
  tor_free(s);
  tor_free(router_status);
  return -1;
}

/** Set *<b>rr</b> to the most recently generated encoded signed
 * running-routers list, generating a new one as necessary.  Return the
 * size of the directory on success, and 0 on failure. */
size_t dirserv_get_runningrouters(const char **rr)
{
  if (runningrouters_is_dirty &&
      runningrouters_is_dirty + DIR_REGEN_SLACK_TIME < time(NULL)) {
    if(generate_runningrouters(get_identity_key())) {
      log_fn(LOG_ERR, "Couldn't generate running-routers list?");
      return 0;
    }
  }
  *rr = runningrouters_string;
  return runningrouters_len;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
