/* Copyright 2001-2004 Roger Dingledine.
 * Copyright 2004-2007 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char dirserv_c_id[] =
  "$Id$";

#define DIRSERV_PRIVATE
#include "or.h"

/**
 * \file dirserv.c
 * \brief Directory server core implementation. Manages directory
 * contents and generates directories.
 */

/** How far in the future do we allow a router to get? (seconds) */
#define ROUTER_ALLOW_SKEW (60*60*12)
/** How many seconds do we wait before regenerating the directory? */
#define DIR_REGEN_SLACK_TIME 30
/** If we're a cache, keep this many networkstatuses around from non-trusted
 * directory authorities. */
#define MAX_UNTRUSTED_NETWORKSTATUSES 16

/** If a v1 directory is older than this, discard it. */
#define MAX_V1_DIRECTORY_AGE (30*24*60*60)
/** If a v1 running-routers is older than this, discard it. */
#define MAX_V1_RR_AGE (7*24*60*60)

extern time_t time_of_process_start; /* from main.c */

/** Do we need to regenerate the directory when someone asks for it? */
static int the_directory_is_dirty = 1;
static int runningrouters_is_dirty = 1;
static int the_v2_networkstatus_is_dirty = 1;

/** Most recently generated encoded signed v1 directory. (v1 auth dirservers
 * only.) */
static cached_dir_t *the_directory = NULL;

/** For authoritative directories: the current (v1) network status. */
static cached_dir_t the_runningrouters = { NULL, NULL, 0, 0, 0, -1 };

static void directory_remove_invalid(void);
static cached_dir_t *dirserv_regenerate_directory(void);
static char *format_versions_list(config_line_t *ln);
struct authdir_config_t;
static int add_fingerprint_to_dir(const char *nickname, const char *fp,
                                  struct authdir_config_t *list);
static uint32_t dirserv_router_get_status(const routerinfo_t *router,
                                          const char **msg);
static uint32_t
dirserv_get_status_impl(const char *fp, const char *nickname,
                        const char *address,
                        uint32_t addr, uint16_t or_port,
                        const char *platform, const char *contact,
                        const char **msg, int should_log);
static void dirserv_set_router_is_running(routerinfo_t *router,
                                          time_t now);
static void clear_cached_dir(cached_dir_t *d);

static int dirserv_add_extrainfo(extrainfo_t *ei, const char **msg);

/************** Fingerprint handling code ************/

#define FP_NAMED   1  /**< Listed in fingerprint file. */
#define FP_INVALID 2  /**< Believed invalid. */
#define FP_REJECT  4  /**< We will not publish this router. */
#define FP_BADEXIT 8  /**< We'll tell clients not to use this as an exit. */
#define FP_UNNAMED 16 /**< Another router has this name in fingerprint file. */

/** Encapsulate a nickname and an FP_* status; target of status_by_digest
 * map. */
typedef struct router_status_t {
  char nickname[MAX_NICKNAME_LEN+1];
  uint32_t status;
} router_status_t;

/** List of nickname-\>identity fingerprint mappings for all the routers
 * that we name.  Used to prevent router impersonation. */
typedef struct authdir_config_t {
  strmap_t *fp_by_name; /**< Map from lc nickname to fingerprint. */
  digestmap_t *status_by_digest; /**< Map from digest to router_status_t. */
} authdir_config_t;

/** Should be static; exposed for testing. */
static authdir_config_t *fingerprint_list = NULL;

/** Allocate and return a new, empty, authdir_config_t. */
static authdir_config_t *
authdir_config_new(void)
{
  authdir_config_t *list = tor_malloc_zero(sizeof(authdir_config_t));
  list->fp_by_name = strmap_new();
  list->status_by_digest = digestmap_new();
  return list;
}

/** Add the fingerprint <b>fp</b> for the nickname <b>nickname</b> to
 * the smartlist of fingerprint_entry_t's <b>list</b>. Return 0 if it's
 * new, or 1 if we replaced the old value.
 */
/* static */ int
add_fingerprint_to_dir(const char *nickname, const char *fp,
                       authdir_config_t *list)
{
  char *fingerprint;
  char d[DIGEST_LEN];
  router_status_t *status;
  tor_assert(nickname);
  tor_assert(fp);
  tor_assert(list);

  fingerprint = tor_strdup(fp);
  tor_strstrip(fingerprint, " ");
  if (base16_decode(d, DIGEST_LEN, fingerprint, strlen(fingerprint))) {
    log_warn(LD_DIRSERV, "Couldn't decode fingerprint \"%s\"",
             escaped(fp));
    tor_free(fingerprint);
    return 0;
  }

  if (!strcasecmp(nickname, UNNAMED_ROUTER_NICKNAME)) {
    log_warn(LD_DIRSERV, "Tried to add a mapping for reserved nickname %s",
             UNNAMED_ROUTER_NICKNAME);
    tor_free(fingerprint);
    return 0;
  }

  status = digestmap_get(list->status_by_digest, d);
  if (!status) {
    status = tor_malloc_zero(sizeof(router_status_t));
    digestmap_set(list->status_by_digest, d, status);
  }

  if (nickname[0] != '!') {
    char *old_fp = strmap_get_lc(list->fp_by_name, nickname);
    if (old_fp && !strcasecmp(fingerprint, old_fp)) {
      tor_free(fingerprint);
    } else {
      tor_free(old_fp);
      strmap_set_lc(list->fp_by_name, nickname, fingerprint);
    }
    status->status |= FP_NAMED;
    strlcpy(status->nickname, nickname, sizeof(status->nickname));
  } else {
    tor_free(fingerprint);
    if (!strcasecmp(nickname, "!reject")) {
      status->status |= FP_REJECT;
    } else if (!strcasecmp(nickname, "!invalid")) {
      status->status |= FP_INVALID;
    } else if (!strcasecmp(nickname, "!badexit")) {
      status->status |= FP_BADEXIT;
    }
  }
  return 0;
}

/** Add the nickname and fingerprint for this OR to the
 * global list of recognized identity key fingerprints. */
int
dirserv_add_own_fingerprint(const char *nickname, crypto_pk_env_t *pk)
{
  char fp[FINGERPRINT_LEN+1];
  if (crypto_pk_get_fingerprint(pk, fp, 0)<0) {
    log_err(LD_BUG, "Error computing fingerprint");
    return -1;
  }
  if (!fingerprint_list)
    fingerprint_list = authdir_config_new();
  add_fingerprint_to_dir(nickname, fp, fingerprint_list);
  return 0;
}

/** Load the nickname-\>fingerprint mappings stored in the approved-routers
 * file.  The file format is line-based, with each non-blank holding one
 * nickname, some space, and a fingerprint for that nickname.  On success,
 * replace the current fingerprint list with the new list and return 0.  On
 * failure, leave the current fingerprint list untouched, and
 * return -1. */
int
dirserv_load_fingerprint_file(void)
{
  char *fname;
  char *cf;
  char *nickname, *fingerprint;
  authdir_config_t *fingerprint_list_new;
  int result;
  config_line_t *front=NULL, *list;
  or_options_t *options = get_options();

  fname = get_datadir_fname("approved-routers");
  log_info(LD_GENERAL,
           "Reloading approved fingerprints from \"%s\"...", fname);

  cf = read_file_to_str(fname, RFTS_IGNORE_MISSING, NULL);
  if (!cf) {
    if (options->NamingAuthoritativeDir) {
      log_warn(LD_FS, "Cannot open fingerprint file '%s'. Failing.", fname);
      tor_free(fname);
      return -1;
    } else {
      log_info(LD_FS, "Cannot open fingerprint file '%s'. Returning.", fname);
      tor_free(fname);
      return 0;
    }
  }
  tor_free(fname);

  result = config_get_lines(cf, &front);
  tor_free(cf);
  if (result < 0) {
    log_warn(LD_CONFIG, "Error reading from fingerprint file");
    return -1;
  }

  fingerprint_list_new = authdir_config_new();

  for (list=front; list; list=list->next) {
    nickname = list->key; fingerprint = list->value;
    if (strlen(nickname) > MAX_NICKNAME_LEN) {
      log_notice(LD_CONFIG,
                 "Nickname '%s' too long in fingerprint file. Skipping.",
                 nickname);
      continue;
    }
    if (!is_legal_nickname(nickname) &&
        strcasecmp(nickname, "!reject") &&
        strcasecmp(nickname, "!invalid") &&
        strcasecmp(nickname, "!badexit")) {
      log_notice(LD_CONFIG,
                 "Invalid nickname '%s' in fingerprint file. Skipping.",
                 nickname);
      continue;
    }
    if (strlen(fingerprint) != FINGERPRINT_LEN ||
        !crypto_pk_check_fingerprint_syntax(fingerprint)) {
      log_notice(LD_CONFIG,
                 "Invalid fingerprint (nickname '%s', "
                 "fingerprint %s). Skipping.",
                 nickname, fingerprint);
      continue;
    }
    if (0==strcasecmp(nickname, DEFAULT_CLIENT_NICKNAME)) {
      /* If you approved an OR called "client", then clients who use
       * the default nickname could all be rejected.  That's no good. */
      log_notice(LD_CONFIG,
                 "Authorizing nickname '%s' would break "
                 "many clients; skipping.",
                 DEFAULT_CLIENT_NICKNAME);
      continue;
    }
    if (0==strcasecmp(nickname, UNNAMED_ROUTER_NICKNAME)) {
      /* If you approved an OR called "unnamed", then clients will be
       * confused. */
      log_notice(LD_CONFIG,
                 "Authorizing nickname '%s' is not allowed; skipping.",
                 UNNAMED_ROUTER_NICKNAME);
      continue;
    }
    if (add_fingerprint_to_dir(nickname, fingerprint, fingerprint_list_new)
        != 0)
      log_notice(LD_CONFIG, "Duplicate nickname '%s'.", nickname);
  }

  config_free_lines(front);
  dirserv_free_fingerprint_list();
  fingerprint_list = fingerprint_list_new;
  /* Delete any routers whose fingerprints we no longer recognize */
  directory_remove_invalid();
  return 0;
}

/** Check whether <b>router</b> has a nickname/identity key combination that
 * we recognize from the fingerprint list, or an IP we automatically act on
 * according to our configuration.  Return the appropriate router status.
 *
 * If the status is 'FP_REJECT' and <b>msg</b> is provided, set
 * *<b>msg</b> to an explanation of why. */
static uint32_t
dirserv_router_get_status(const routerinfo_t *router, const char **msg)
{
  char d[DIGEST_LEN];

  if (crypto_pk_get_digest(router->identity_pkey, d)) {
    log_warn(LD_BUG,"Error computing fingerprint");
    if (msg)
      *msg = "Bug: Error computing fingerprint";
    return FP_REJECT;
  }

  return dirserv_get_status_impl(d, router->nickname,
                                 router->address,
                                 router->addr, router->or_port,
                                 router->platform, router->contact_info,
                                 msg, 1);
}

/** Return true if there is no point in downloading the router described by
 * <b>rs</b> because this directory would reject it. */
int
dirserv_would_reject_router(routerstatus_t *rs)
{
  uint32_t res;

  res = dirserv_get_status_impl(rs->identity_digest, rs->nickname,
                                "", /* address is only used in logs */
                                rs->addr, rs->or_port,
                                NULL, NULL,
                                NULL, 0);

  return (res & FP_REJECT) != 0;
}

/** Helper: Based only on the ID/Nickname combination,
 * return FP_UNNAMED (unnamed), FP_NAMED (named), or 0 (neither).
 */
static uint32_t
dirserv_get_name_status(const char *id_digest, const char *nickname)
{
  char fp[HEX_DIGEST_LEN+1];
  char *fp_by_name;

  base16_encode(fp, sizeof(fp), id_digest, DIGEST_LEN);

  if ((fp_by_name =
       strmap_get_lc(fingerprint_list->fp_by_name, nickname))) {
    if (!strcasecmp(fp, fp_by_name)) {
      return FP_NAMED;
    } else {
      return FP_UNNAMED; /* Wrong fingerprint. */
    }
  }
  return 0;
}

/** Helper: As dirserv_get_router_status, but takes the router fingerprint
 * (hex, no spaces), nickname, address (used for logging only), IP address, OR
 * port, platform (logging only) and contact info (logging only) as arguments.
 *
 * If should_log is false, do not log messages.  (There's not much point in
 * logging that we're rejecting servers we'll not download.)
 */
static uint32_t
dirserv_get_status_impl(const char *id_digest, const char *nickname,
                        const char *address,
                        uint32_t addr, uint16_t or_port,
                        const char *platform, const char *contact,
                        const char **msg, int should_log)
{
  int reject_unlisted = get_options()->AuthDirRejectUnlisted;
  uint32_t result = 0;
  router_status_t *status_by_digest;

  if (!fingerprint_list)
    fingerprint_list = authdir_config_new();

  if (should_log)
    log_debug(LD_DIRSERV, "%d fingerprints, %d digests known.",
              strmap_size(fingerprint_list->fp_by_name),
              digestmap_size(fingerprint_list->status_by_digest));

  result = dirserv_get_name_status(id_digest, nickname);
  if (result & FP_NAMED) {
    if (should_log)
      log_debug(LD_DIRSERV,"Good fingerprint for '%s'",nickname);
  }
  if (result & FP_UNNAMED) {
    if (should_log) {
      char *esc_contact = esc_for_log(contact);
      log_info(LD_DIRSERV,
               "Mismatched fingerprint for '%s'. "
               "ContactInfo '%s', platform '%s'.)",
               nickname,
               esc_contact,
               platform ? escaped(platform) : "");
      tor_free(esc_contact);
    }
    if (msg)
      *msg = "Rejected: There is already a named server with this nickname "
        "and a different fingerprint.";
  }

  status_by_digest = digestmap_get(fingerprint_list->status_by_digest,
                                   id_digest);
  if (status_by_digest)
    result |= (status_by_digest->status & ~FP_NAMED);

  if (result & FP_REJECT) {
    if (msg)
      *msg = "Fingerprint is marked rejected";
    return FP_REJECT;
  } else if (result & FP_INVALID) {
    if (msg)
      *msg = "Fingerprint is marked invalid";
  }

  if (authdir_policy_badexit_address(addr, or_port)) {
    if (should_log)
      log_info(LD_DIRSERV, "Marking '%s' as bad exit because of address '%s'",
               nickname, address);
    result |= FP_BADEXIT;
  }

  if (!(result & FP_NAMED)) {
    if (!authdir_policy_permits_address(addr, or_port)) {
      if (should_log)
        log_info(LD_DIRSERV, "Rejecting '%s' because of address '%s'",
                 nickname, address);
      if (msg)
        *msg = "Authdir is rejecting routers in this range.";
      return FP_REJECT;
    }
    if (!authdir_policy_valid_address(addr, or_port)) {
      if (should_log)
        log_info(LD_DIRSERV, "Not marking '%s' valid because of address '%s'",
                 nickname, address);
      result |= FP_INVALID;
    }
    if (reject_unlisted) {
      if (msg)
        *msg = "Authdir rejects unknown routers.";
      return FP_REJECT;
    }
    /* 0.1.0.2-rc was the first version that did enough self-testing that
     * we're willing to take its word about whether it's running. */
    if (platform && !tor_version_as_new_as(platform,"0.1.0.2-rc"))
      result |= FP_INVALID;
  }

  return result;
}

/** If we are an authoritative dirserver, and the list of approved
 * servers contains one whose identity key digest is <b>digest</b>,
 * return that router's nickname.  Otherwise return NULL. */
const char *
dirserv_get_nickname_by_digest(const char *digest)
{
  router_status_t *status;
  if (!fingerprint_list)
    return NULL;
  tor_assert(digest);

  status = digestmap_get(fingerprint_list->status_by_digest, digest);
  return status ? status->nickname : NULL;
}

/** Clear the current fingerprint list. */
void
dirserv_free_fingerprint_list(void)
{
  if (!fingerprint_list)
    return;

  strmap_free(fingerprint_list->fp_by_name, _tor_free);
  digestmap_free(fingerprint_list->status_by_digest, _tor_free);
  tor_free(fingerprint_list);
}

/*
 *    Descriptor list
 */

/** Return -1 if <b>ri</b> has a private or otherwise bad address,
 * unless we're configured to not care. Return 0 if all ok. */
static int
dirserv_router_has_valid_address(routerinfo_t *ri)
{
  struct in_addr iaddr;
  if (get_options()->DirAllowPrivateAddresses)
    return 0; /* whatever it is, we're fine with it */
  if (!tor_inet_aton(ri->address, &iaddr)) {
    log_info(LD_DIRSERV,"Router '%s' published non-IP address '%s'. Refusing.",
             ri->nickname, ri->address);
    return -1;
  }
  if (is_internal_IP(ntohl(iaddr.s_addr), 0)) {
    log_info(LD_DIRSERV,
             "Router '%s' published internal IP address '%s'. Refusing.",
             ri->nickname, ri->address);
    return -1; /* it's a private IP, we should reject it */
  }
  return 0;
}

/** Check whether we, as a directory server, want to accept <b>ri</b>.  If so,
 * set its is_valid,named,running fields and return 0.  Otherwise, return -1.
 *
 * If the router is rejected, set *<b>msg</b> to an explanation of why.
 *
 * If <b>complain</b> then explain at log-level 'notice' why we refused
 * a descriptor; else explain at log-level 'info'.
 */
int
authdir_wants_to_reject_router(routerinfo_t *ri, const char **msg,
                               int complain)
{
  /* Okay.  Now check whether the fingerprint is recognized. */
  uint32_t status = dirserv_router_get_status(ri, msg);
  time_t now;
  int severity = complain ? LOG_NOTICE : LOG_INFO;
  tor_assert(msg);
  if (status & FP_REJECT)
    return -1; /* msg is already set. */

  /* Is there too much clock skew? */
  now = time(NULL);
  if (ri->cache_info.published_on > now+ROUTER_ALLOW_SKEW) {
    log_fn(severity, LD_DIRSERV, "Publication time for nickname '%s' is too "
           "far (%d minutes) in the future; possible clock skew. Not adding "
           "(%s)",
           ri->nickname, (int)((ri->cache_info.published_on-now)/60),
           esc_router_info(ri));
    *msg = "Rejected: Your clock is set too far in the future, or your "
      "timezone is not correct.";
    return -1;
  }
  if (ri->cache_info.published_on < now-ROUTER_MAX_AGE_TO_PUBLISH) {
    log_fn(severity, LD_DIRSERV,
           "Publication time for router with nickname '%s' is too far "
           "(%d minutes) in the past. Not adding (%s)",
           ri->nickname, (int)((now-ri->cache_info.published_on)/60),
           esc_router_info(ri));
    *msg = "Rejected: Server is expired, or your clock is too far in the past,"
      " or your timezone is not correct.";
    return -1;
  }
  if (dirserv_router_has_valid_address(ri) < 0) {
    log_fn(severity, LD_DIRSERV,
           "Router with nickname '%s' has invalid address '%s'. "
           "Not adding (%s).",
           ri->nickname, ri->address,
           esc_router_info(ri));
    *msg = "Rejected: Address is not an IP, or IP is a private address.";
    return -1;
  }
  /* Okay, looks like we're willing to accept this one. */
  ri->is_named = (status & FP_NAMED) ? 1 : 0;
  ri->is_valid = (status & FP_INVALID) ? 0 : 1;
  ri->is_bad_exit = (status & FP_BADEXIT) ? 1 : 0;

  return 0;
}

/** As for dirserv_add_descriptor, but accepts multiple documents, and
 * returns the most severe error that occurred for any one of them. */
int
dirserv_add_multiple_descriptors(const char *desc, uint8_t purpose,
                                 const char *source,
                                 const char **msg)
{
  int r=100; /* higher than any actual return value. */
  int r_tmp;
  const char *msg_out;
  smartlist_t *list;
  const char *s;
  int n_parsed = 0;
  time_t now = time(NULL);
  char annotation_buf[ROUTER_ANNOTATION_BUF_LEN];
  char time_buf[ISO_TIME_LEN+1];
  int general = purpose == ROUTER_PURPOSE_GENERAL;
  tor_assert(msg);

  format_iso_time(time_buf, now);
  if (tor_snprintf(annotation_buf, sizeof(annotation_buf),
                   "@uploaded-at %s\n"
                   "@source %s\n"
                   "%s%s%s", time_buf, escaped(source),
                   !general ? "@purpose " : "",
                   !general ? router_purpose_to_string(purpose) : "",
                   !general ? "\n" : "")<0) {
    *msg = "Couldn't format annotations";
    return -1;
  }

  s = desc;
  list = smartlist_create();
  if (!router_parse_list_from_string(&s, NULL, list, SAVED_NOWHERE, 0, 0,
                                     annotation_buf)) {
    SMARTLIST_FOREACH(list, routerinfo_t *, ri, {
        msg_out = NULL;
        tor_assert(ri->purpose == purpose);
        r_tmp = dirserv_add_descriptor(ri, &msg_out);
        if (r_tmp < r) {
          r = r_tmp;
          *msg = msg_out;
        }
      });
  }
  n_parsed += smartlist_len(list);
  smartlist_clear(list);

  s = desc;
  if (!router_parse_list_from_string(&s, NULL, list, SAVED_NOWHERE, 1, 0,
                                     NULL)) {
    SMARTLIST_FOREACH(list, extrainfo_t *, ei, {
        msg_out = NULL;

        r_tmp = dirserv_add_extrainfo(ei, &msg_out);
        if (r_tmp < r) {
          r = r_tmp;
          *msg = msg_out;
        }
      });
  }
  n_parsed += smartlist_len(list);
  smartlist_free(list);

  if (! *msg) {
    if (!n_parsed) {
      *msg = "No descriptors found in your POST.";
      if (r > -1)
        r = -1;
    } else {
      *msg = "(no message)";
    }
  }

  return r <= 2 ? r : 2;
}

/** Examine the parsed server descriptor in <b>ri</b> and maybe insert it into
 * the list of server descriptors. Set *<b>msg</b> to a message that should be
 * passed back to the origin of this descriptor.
 *
 * Return 2 if descriptor is well-formed and accepted;
 *  1 if well-formed and accepted but origin should hear *msg;
 *  0 if well-formed but redundant with one we already have;
 * -1 if it is rejected and origin should hear *msg;
 *
 * This function is only called when fresh descriptors are posted, not when
 * we re-load the cache.
 */
int
dirserv_add_descriptor(routerinfo_t *ri, const char **msg)
{
  int r;
  routerinfo_t *ri_old;
  char *desc = NULL;
  size_t desclen = 0;

  /* Check whether this descriptor is semantically identical to the last one
   * from this server.  (We do this here and not in router_add_to_routerlist
   * because we want to be able to accept the newest router descriptor that
   * another authority has, so we all converge on the same one.) */
  ri_old = router_get_by_digest(ri->cache_info.identity_digest);
  if (ri_old && ri_old->cache_info.published_on < ri->cache_info.published_on
      && router_differences_are_cosmetic(ri_old, ri)
      && !router_is_me(ri)) {
    log_info(LD_DIRSERV,
             "Not replacing descriptor from '%s'; differences are cosmetic.",
             ri->nickname);
    *msg = "Not replacing router descriptor; no information has changed since "
      "the last one with this identity.";
    control_event_or_authdir_new_descriptor("DROPPED",
                         ri->cache_info.signed_descriptor_body,
                         ri->cache_info.signed_descriptor_len, *msg);
    routerinfo_free(ri);
    return 0;
  }
  if (control_event_is_interesting(EVENT_AUTHDIR_NEWDESCS)) {
    /* Make a copy of desc, since router_add_to_routerlist might free
     * ri and its associated signed_descriptor_t. */
    desclen = ri->cache_info.signed_descriptor_len;
    desc = tor_strndup(ri->cache_info.signed_descriptor_body, desclen);
  }

  if ((r = router_add_to_routerlist(ri, msg, 0, 0))<0) {
    if (r < -1 && desc) /* unless the routerinfo was fine, just out-of-date */
      control_event_or_authdir_new_descriptor("REJECTED", desc, desclen, *msg);
    tor_free(desc);
    return r == -1 ? 0 : -1;
  } else {
    smartlist_t *changed;
    control_event_or_authdir_new_descriptor("ACCEPTED", desc, desclen, *msg);

    changed = smartlist_create();
    smartlist_add(changed, ri);
    control_event_descriptors_changed(changed);
    smartlist_free(changed);
    if (!*msg) {
      *msg =  ri->is_valid ? "Descriptor for valid server accepted" :
        "Descriptor for invalid server accepted";
    }
    tor_free(desc);
    return r == 0 ? 2 : 1;
  }
}

/** As dirserv_add_descriptor, but for an extrainfo_t <b>ei</b>. */
static int
dirserv_add_extrainfo(extrainfo_t *ei, const char **msg)
{
  routerinfo_t *ri;
  int r;
  tor_assert(msg);
  *msg = NULL;

  ri = router_get_by_digest(ei->cache_info.identity_digest);
  if (!ri) {
    *msg = "No corresponding router descriptor for extra-info descriptor";
    extrainfo_free(ei);
    return -1;
  }
  if ((r = routerinfo_incompatible_with_extrainfo(ri, ei, NULL, msg))) {
    extrainfo_free(ei);
    return r < 0 ? 0 : -1;
  }
  router_add_extrainfo_to_routerlist(ei, msg, 0, 0);
  return 2;
}

/** Remove all descriptors whose nicknames or fingerprints no longer
 * are allowed by our fingerprint list. (Descriptors that used to be
 * good can become bad when we reload the fingerprint list.)
 */
static void
directory_remove_invalid(void)
{
  int i;
  int changed = 0;
  routerlist_t *rl = router_get_routerlist();

  routerlist_assert_ok(rl);

  for (i = 0; i < smartlist_len(rl->routers); ++i) {
    const char *msg;
    routerinfo_t *ent = smartlist_get(rl->routers, i);
    uint32_t r = dirserv_router_get_status(ent, &msg);
    if (r & FP_REJECT) {
      log_info(LD_DIRSERV, "Router '%s' is now rejected: %s",
               ent->nickname, msg?msg:"");
      routerlist_remove(rl, ent, 0);
      i--;
      changed = 1;
      continue;
    }
    if (bool_neq((r & FP_NAMED), ent->is_named)) {
      log_info(LD_DIRSERV,
               "Router '%s' is now %snamed.", ent->nickname,
               (r&FP_NAMED)?"":"un");
      ent->is_named = (r&FP_NAMED)?1:0;
      changed = 1;
    }
    if (bool_neq((r & FP_INVALID), !ent->is_valid)) {
      log_info(LD_DIRSERV, "Router '%s' is now %svalid.", ent->nickname,
               (r&FP_INVALID) ? "in" : "");
      ent->is_valid = (r&FP_INVALID)?0:1;
      changed = 1;
    }
    if (bool_neq((r & FP_BADEXIT), ent->is_bad_exit)) {
      log_info(LD_DIRSERV, "Router '%s' is now a %s exit", ent->nickname,
               (r & FP_BADEXIT) ? "bad" : "good");
      ent->is_bad_exit = (r&FP_BADEXIT) ? 1: 0;
      changed = 1;
    }
  }
  if (changed)
    directory_set_dirty();

  routerlist_assert_ok(rl);
}

/** Write a list of unregistered descriptors into a newly allocated
 * string and return it. Used by dirserv operators to keep track of
 * fast nodes that haven't registered.
 */
int
getinfo_helper_dirserv_unregistered(control_connection_t *control_conn,
                                    const char *question, char **answer_out)
{
  smartlist_t *answerlist;
  char buf[1024];
  char *answer;
  int min_bw = atoi(question);
  routerlist_t *rl = router_get_routerlist();

  (void) control_conn;

  if (strcmpstart(question, "unregistered-servers-"))
    return 0;
  question += strlen("unregistered-servers-");

  answerlist = smartlist_create();
  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ent, {
    uint32_t r = dirserv_router_get_status(ent, NULL);
    if (router_get_advertised_bandwidth(ent) >= (size_t)min_bw &&
        !(r & FP_NAMED)) {
      /* then log this one */
      tor_snprintf(buf, sizeof(buf),
                   "%s: BW %d on '%s'.",
                   ent->nickname, router_get_advertised_bandwidth(ent),
                   ent->platform ? ent->platform : "");
      smartlist_add(answerlist, tor_strdup(buf));
    }
  });
  answer = smartlist_join_strings(answerlist, "\r\n", 0, NULL);
  SMARTLIST_FOREACH(answerlist, char *, cp, tor_free(cp));
  smartlist_free(answerlist);
  *answer_out = answer;
  return 0;
}

/** Mark the directory as <b>dirty</b> -- when we're next asked for a
 * directory, we will rebuild it instead of reusing the most recently
 * generated one.
 */
void
directory_set_dirty(void)
{
  time_t now = time(NULL);
  int set_v1_dirty=0;

  /* Regenerate stubs only every 8 hours. XXXX020 */
#define STUB_REGENERATE_INTERVAL (8*60*60)
  if (!the_directory || !the_runningrouters.dir)
    set_v1_dirty = 1;
  else if (the_directory->published < now - STUB_REGENERATE_INTERVAL ||
           the_runningrouters.published < now - STUB_REGENERATE_INTERVAL)
    set_v1_dirty = 1;

  if (set_v1_dirty) {
    if (!the_directory_is_dirty)
      the_directory_is_dirty = now;
    if (!runningrouters_is_dirty)
      runningrouters_is_dirty = now;
  }
  if (!the_v2_networkstatus_is_dirty)
    the_v2_networkstatus_is_dirty = now;
}

/**
 * Allocate and return a description of the status of the server <b>desc</b>,
 * for use in a v1-style router-status line.  The server is listed
 * as running iff <b>is_live</b> is true.
 */
static char *
list_single_server_status(routerinfo_t *desc, int is_live)
{
  char buf[MAX_NICKNAME_LEN+HEX_DIGEST_LEN+4]; /* !nickname=$hexdigest\0 */
  char *cp;

  tor_assert(desc);

  cp = buf;
  if (!is_live) {
    *cp++ = '!';
  }
  if (desc->is_valid) {
    strlcpy(cp, desc->nickname, sizeof(buf)-(cp-buf));
    cp += strlen(cp);
    *cp++ = '=';
  }
  *cp++ = '$';
  base16_encode(cp, HEX_DIGEST_LEN+1, desc->cache_info.identity_digest,
                DIGEST_LEN);
  return tor_strdup(buf);
}

/** Each server needs to have passed a reachability test no more
 * than this number of seconds ago, or he is listed as down in
 * the directory. */
#define REACHABLE_TIMEOUT (45*60)

/** Treat a router as alive if
 *    - It's me, and I'm not hibernating.
 * or - We've found it reachable recently. */
static void
dirserv_set_router_is_running(routerinfo_t *router, time_t now)
{
  int answer;

  if (router_is_me(router) && !we_are_hibernating())
    answer = 1;
  else
    answer = get_options()->AssumeReachable ||
             now < router->last_reachable + REACHABLE_TIMEOUT;

  if (router->is_running && !answer) {
    /* it was running but now it's not. tell rephist. */
    rep_hist_note_router_unreachable(router->cache_info.identity_digest, now);
  }

  router->is_running = answer;
}

/** Based on the routerinfo_ts in <b>routers</b>, allocate the
 * contents of a v1-style router-status line, and store it in
 * *<b>router_status_out</b>.  Return 0 on success, -1 on failure.
 *
 * If for_controller is true, include the routers with very old descriptors.
 * If for_controller is &gt;1, use the verbose nickname format.
 */
int
list_server_status(smartlist_t *routers, char **router_status_out,
                   int for_controller)
{
  /* List of entries in a router-status style: An optional !, then an optional
   * equals-suffixed nickname, then a dollar-prefixed hexdigest. */
  smartlist_t *rs_entries;
  time_t now = time(NULL);
  time_t cutoff = now - ROUTER_MAX_AGE_TO_PUBLISH;
  or_options_t *options = get_options();
  /* We include v2 dir auths here too, because they need to answer
   * controllers. Eventually we'll deprecate this whole function. */
  int authdir = authdir_mode_publishes_statuses(options);
  tor_assert(router_status_out);

  rs_entries = smartlist_create();

  SMARTLIST_FOREACH(routers, routerinfo_t *, ri,
  {
    if (authdir) {
      /* Update router status in routerinfo_t. */
      dirserv_set_router_is_running(ri, now);
    }
    if (for_controller == 1 || ri->cache_info.published_on >= cutoff)
      smartlist_add(rs_entries, list_single_server_status(ri, ri->is_running));
    else if (for_controller > 2) {
      char name_buf[MAX_VERBOSE_NICKNAME_LEN+2];
      char *cp = name_buf;
      if (!ri->is_running)
        *cp++ = '!';
      router_get_verbose_nickname(cp, ri);
      smartlist_add(rs_entries, tor_strdup(name_buf));
    }
  });

  *router_status_out = smartlist_join_strings(rs_entries, " ", 0, NULL);

  SMARTLIST_FOREACH(rs_entries, char *, cp, tor_free(cp));
  smartlist_free(rs_entries);

  return 0;
}

/** Given a (possibly empty) list of config_line_t, each line of which contains
 * a list of comma-separated version numbers surrounded by optional space,
 * allocate and return a new string containing the version numbers, in order,
 * separated by commas.  Used to generate Recommended(Client|Server)?Versions
 */
static char *
format_versions_list(config_line_t *ln)
{
  smartlist_t *versions;
  char *result;
  versions = smartlist_create();
  for ( ; ln; ln = ln->next) {
    smartlist_split_string(versions, ln->value, ",",
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  }
  sort_version_list(versions, 1);
  result = smartlist_join_strings(versions,",",0,NULL);
  SMARTLIST_FOREACH(versions,char *,s,tor_free(s));
  smartlist_free(versions);
  return result;
}

/** Return 1 if <b>ri</b>'s descriptor is "active" -- running, valid,
 * not hibernating, and not too old. Else return 0.
 */
static int
router_is_active(routerinfo_t *ri, time_t now)
{
  time_t cutoff = now - ROUTER_MAX_AGE_TO_PUBLISH;
  if (ri->cache_info.published_on < cutoff)
    return 0;
  if (!ri->is_running || !ri->is_valid || ri->is_hibernating)
    return 0;
  return 1;
}

/** Generate a new v1 directory and write it into a newly allocated string.
 * Point *<b>dir_out</b> to the allocated string.  Sign the
 * directory with <b>private_key</b>.  Return 0 on success, -1 on
 * failure. If <b>complete</b> is set, give us all the descriptors;
 * otherwise leave out non-running and non-valid ones.
 */
int
dirserv_dump_directory_to_string(char **dir_out,
                                 crypto_pk_env_t *private_key)
{
  char *cp;
  char *identity_pkey; /* Identity key, DER64-encoded. */
  char *recommended_versions;
  char digest[DIGEST_LEN];
  char published[ISO_TIME_LEN+1];
  char *buf = NULL;
  size_t buf_len;
  size_t identity_pkey_len;
  time_t now = time(NULL);

  tor_assert(dir_out);
  *dir_out = NULL;

  if (crypto_pk_write_public_key_to_string(private_key,&identity_pkey,
                                           &identity_pkey_len)<0) {
    log_warn(LD_BUG,"write identity_pkey to string failed!");
    return -1;
  }

  recommended_versions =
    format_versions_list(get_options()->RecommendedVersions);

  format_iso_time(published, now);

  buf_len = 2048+strlen(recommended_versions);

  buf = tor_malloc(buf_len);
  /* We'll be comparing against buf_len throughout the rest of the
     function, though strictly speaking we shouldn't be able to exceed
     it.  This is C, after all, so we may as well check for buffer
     overruns.*/

  tor_snprintf(buf, buf_len,
               "signed-directory\n"
               "published %s\n"
               "recommended-software %s\n"
               "router-status %s\n"
               "dir-signing-key\n%s\n",
               published, recommended_versions, "",
               identity_pkey);

  tor_free(recommended_versions);
  tor_free(identity_pkey);

  cp = buf + strlen(buf);
  *cp = '\0';

  /* These multiple strlcat calls are inefficient, but dwarfed by the RSA
     signature. */
  if (strlcat(buf, "directory-signature ", buf_len) >= buf_len)
    goto truncated;
  if (strlcat(buf, get_options()->Nickname, buf_len) >= buf_len)
    goto truncated;
  if (strlcat(buf, "\n", buf_len) >= buf_len)
    goto truncated;

  if (router_get_dir_hash(buf,digest)) {
    log_warn(LD_BUG,"couldn't compute digest");
    tor_free(buf);
    return -1;
  }
  note_crypto_pk_op(SIGN_DIR);
  if (router_append_dirobj_signature(buf,buf_len,digest,private_key)<0) {
    tor_free(buf);
    return -1;
  }

  *dir_out = buf;
  return 0;
 truncated:
  log_warn(LD_BUG,"tried to exceed string length.");
  tor_free(buf);
  return -1;
}

/********************************************************************/

/* A set of functions to answer questions about how we'd like to behave
 * as a directory cache/client. */

/** Return 1 if we want to keep descriptors, networkstatuses, etc around
 * and serve them to others, or 0 otherwise.
 * Also causes us to fetch new networkstatuses, descriptors, etc on the
 * "mirror" schedule rather than the "client" schedule.
 */
int
directory_caches_dir_info(or_options_t *options)
{
  return options->DirPort != 0;
}

/** Return 1 if we fetch our directory material directly from the
 * authorities, rather than some other cache. */
int
directory_fetches_from_authorities(or_options_t *options)
{
  return server_mode(options) && options->DirPort != 0;
}

/** Return 1 if we want to allow remote people to ask us directory
 * requests via the "begin_dir" interface, which doesn't require
 * having any separate port open. */
int
directory_permits_begindir_requests(or_options_t *options)
{
  return options->DirPort != 0;
}

/** Return 1 if we want to allow controllers to ask us directory
 * requests via the controller interface, which doesn't require
 * having any separate port open. */
int
directory_permits_controller_requests(or_options_t *options)
{
  return options->DirPort != 0;
}

/********************************************************************/

/* Used only by non-v1-auth dirservers: The v1 directory and
 * runningrouters we'll serve when requested. */
static cached_dir_t *cached_directory = NULL;
static cached_dir_t cached_runningrouters = { NULL, NULL, 0, 0, 0, -1 };

/** Used for other dirservers' v2 network statuses.  Map from hexdigest to
 * cached_dir_t. */
static digestmap_t *cached_v2_networkstatus = NULL;

/** The v3 consensus network status that we're currently serving. */
static cached_dir_t *cached_v3_networkstatus = NULL;

/** Possibly replace the contents of <b>d</b> with the value of
 * <b>directory</b> published on <b>when</b>, unless <b>when</b> is older than
 * the last value, or too far in the future.
 *
 * Does not copy <b>directory</b>; frees it if it isn't used.
 */
static void
set_cached_dir(cached_dir_t *d, char *directory, time_t when)
{
  time_t now = time(NULL);
  if (when<=d->published) {
    log_info(LD_DIRSERV, "Ignoring old directory; not caching.");
    tor_free(directory);
  } else if (when>=now+ROUTER_MAX_AGE_TO_PUBLISH) {
    log_info(LD_DIRSERV, "Ignoring future directory; not caching.");
    tor_free(directory);
  } else {
    /* if (when>d->published && when<now+ROUTER_MAX_AGE) */
    log_debug(LD_DIRSERV, "Caching directory.");
    tor_free(d->dir);
    d->dir = directory;
    d->dir_len = strlen(directory);
    tor_free(d->dir_z);
    if (tor_gzip_compress(&(d->dir_z), &(d->dir_z_len), d->dir, d->dir_len,
                          ZLIB_METHOD)) {
      log_warn(LD_BUG,"Error compressing cached directory");
    }
    d->published = when;
  }
}

/** Decrement the reference count on <b>d</b>, and free it if it no longer has
 * any references. */
void
cached_dir_decref(cached_dir_t *d)
{
  if (!d || --d->refcnt > 0)
    return;
  clear_cached_dir(d);
  tor_free(d);
}

/** Allocate and return a new cached_dir_t containing the string <b>s</b>,
 * published at <b>published</b>. */
cached_dir_t *
new_cached_dir(char *s, time_t published)
{
  cached_dir_t *d = tor_malloc_zero(sizeof(cached_dir_t));
  d->refcnt = 1;
  d->dir = s;
  d->dir_len = strlen(s);
  d->published = published;
  if (tor_gzip_compress(&(d->dir_z), &(d->dir_z_len), d->dir, d->dir_len,
                        ZLIB_METHOD)) {
    log_warn(LD_BUG, "Error compressing directory");
  }
  return d;
}

/** Remove all storage held in <b>d</b>, but do not free <b>d</b> itself. */
static void
clear_cached_dir(cached_dir_t *d)
{
  tor_free(d->dir);
  tor_free(d->dir_z);
  memset(d, 0, sizeof(cached_dir_t));
}

/** Free all storage held by the cached_dir_t in <b>d</b>. */
static void
_free_cached_dir(void *_d)
{
  cached_dir_t *d = (cached_dir_t *)_d;
  cached_dir_decref(d);
}

/** If we have no cached directory, or it is older than <b>published</b>,
 * then replace it with <b>directory</b>, published at <b>published</b>.
 *
 * If <b>published</b> is too old, do nothing.
 *
 * If <b>is_running_routers</b>, this is really a v1 running_routers
 * document rather than a v1 directory.
 */
void
dirserv_set_cached_directory(const char *directory, time_t published,
                             int is_running_routers)
{
  time_t now = time(NULL);

  if (is_running_routers) {
    if (published >= now - MAX_V1_RR_AGE)
      set_cached_dir(&cached_runningrouters, tor_strdup(directory), published);
  } else {
    if (published >= now - MAX_V1_DIRECTORY_AGE) {
      cached_dir_decref(cached_directory);
      cached_directory = new_cached_dir(tor_strdup(directory), published);
    }
  }
}

/** If <b>networkstatus</b> is non-NULL, we've just received a v2
 * network-status for an authoritative directory with identity digest
 * <b>identity</b> published at <b>published</b> -- store it so we can
 * serve it to others.
 *
 * If <b>networkstatus</b> is NULL, remove the entry with the given
 * identity fingerprint from the v2 cache.
 */
void
dirserv_set_cached_networkstatus_v2(const char *networkstatus,
                                    const char *identity,
                                    time_t published)
{
  cached_dir_t *d, *old_d;
  smartlist_t *trusted_dirs;
  if (!cached_v2_networkstatus)
    cached_v2_networkstatus = digestmap_new();

  old_d = digestmap_get(cached_v2_networkstatus, identity);
  if (!old_d && !networkstatus)
    return;

  if (networkstatus) {
    if (!old_d || published > old_d->published) {
      d = new_cached_dir(tor_strdup(networkstatus), published);
      digestmap_set(cached_v2_networkstatus, identity, d);
      if (old_d)
        cached_dir_decref(old_d);
    }
  } else {
    if (old_d) {
      digestmap_remove(cached_v2_networkstatus, identity);
      cached_dir_decref(old_d);
    }
  }

  /* Now purge old entries. */
  trusted_dirs = router_get_trusted_dir_servers();
  if (digestmap_size(cached_v2_networkstatus) >
      smartlist_len(trusted_dirs) + MAX_UNTRUSTED_NETWORKSTATUSES) {
    /* We need to remove the oldest untrusted networkstatus. */
    const char *oldest = NULL;
    time_t oldest_published = TIME_MAX;
    digestmap_iter_t *iter;

    for (iter = digestmap_iter_init(cached_v2_networkstatus);
         !digestmap_iter_done(iter);
         iter = digestmap_iter_next(cached_v2_networkstatus, iter)) {
      const char *ident;
      void *val;
      digestmap_iter_get(iter, &ident, &val);
      d = val;
      if (d->published < oldest_published &&
          !router_digest_is_trusted_dir(ident)) {
        oldest = ident;
        oldest_published = d->published;
      }
    }
    tor_assert(oldest);
    d = digestmap_remove(cached_v2_networkstatus, oldest);
    if (d)
      cached_dir_decref(d);
  }
}

/** Replace the v3 consensus networkstatus that we're serving with
 * <b>networkstatus</b>, published at <b>published</b>.  No validation is
 * performed. */
void
dirserv_set_cached_networkstatus_v3(const char *networkstatus,
                                    time_t published)
{
  if (cached_v3_networkstatus)
    cached_dir_decref(cached_v3_networkstatus);
  cached_v3_networkstatus = new_cached_dir(
                                  tor_strdup(networkstatus), published);
}

/** Remove any v2 networkstatus from the directory cache that was published
 * before <b>cutoff</b>. */
void
dirserv_clear_old_networkstatuses(time_t cutoff)
{
  digestmap_iter_t *iter;

  if (!cached_v2_networkstatus)
    return;

  for (iter = digestmap_iter_init(cached_v2_networkstatus);
       !digestmap_iter_done(iter); ) {
    const char *ident;
    void *val;
    cached_dir_t *dir;
    digestmap_iter_get(iter, &ident, &val);
    dir = val;
    if (dir->published < cutoff) {
      char *fname;
      iter = digestmap_iter_next_rmv(cached_v2_networkstatus, iter);
      fname = networkstatus_get_cache_filename(ident);
      if (file_status(fname) == FN_FILE) {
        log_info(LD_DIR, "Removing too-old untrusted networkstatus in %s",
                 fname);
        unlink(fname);
      }
      tor_free(fname);
      cached_dir_decref(dir);
    } else {
      iter = digestmap_iter_next(cached_v2_networkstatus, iter);
    }
  }
}

/** Remove any v1 info from the directory cache that was published
 * too long ago. */
void
dirserv_clear_old_v1_info(time_t now)
{
  if (cached_directory &&
      cached_directory->published < (now - MAX_V1_DIRECTORY_AGE)) {
    cached_dir_decref(cached_directory);
    cached_directory = NULL;
  }
  if (cached_runningrouters.published < (now - MAX_V1_RR_AGE)) {
    clear_cached_dir(&cached_runningrouters);
  }
}

/** Helper: If we're an authority for the right directory version
 * (based on <b>auth_type</b>), try to regenerate
 * auth_src as appropriate and return it, falling back to cache_src on
 * failure.  If we're a cache, simply return cache_src.
 */
static cached_dir_t *
dirserv_pick_cached_dir_obj(cached_dir_t *cache_src,
                            cached_dir_t *auth_src,
                            time_t dirty, cached_dir_t *(*regenerate)(void),
                            const char *name,
                            authority_type_t auth_type)
{
  or_options_t *options = get_options();
  int authority = (auth_type == V1_AUTHORITY && authdir_mode_v1(options)) ||
                  (auth_type == V2_AUTHORITY && authdir_mode_v2(options));
  /* XXX020 eventually use authdir_mode_publishes_statuses() here */

  if (!authority || authdir_mode_bridge(options)) {
    return cache_src;
  } else {
    /* We're authoritative. */
    if (regenerate != NULL) {
      if (dirty && dirty + DIR_REGEN_SLACK_TIME < time(NULL)) {
        if (!(auth_src = regenerate())) {
          log_err(LD_BUG, "Couldn't generate %s?", name);
          exit(1);
        }
      } else {
        log_info(LD_DIRSERV, "The %s is still clean; reusing.", name);
      }
    }
    return auth_src ? auth_src : cache_src;
  }
}

/** Return the most recently generated encoded signed v1 directory,
 * generating a new one as necessary.  If not a v1 authoritative directory
 * may return NULL if no directory is yet cached. */
cached_dir_t *
dirserv_get_directory(void)
{
  return dirserv_pick_cached_dir_obj(cached_directory, the_directory,
                                     the_directory_is_dirty,
                                     dirserv_regenerate_directory,
                                     "server directory", V1_AUTHORITY);
}

/** Only called by v1 auth dirservers.
 * Generate a fresh v1 directory; set the_directory and return a pointer
 * to the new value.
 */
static cached_dir_t *
dirserv_regenerate_directory(void)
{
  char *new_directory=NULL;

  if (dirserv_dump_directory_to_string(&new_directory, get_identity_key())) {
    log_warn(LD_BUG, "Error creating directory.");
    tor_free(new_directory);
    return NULL;
  }
  cached_dir_decref(the_directory);
  the_directory = new_cached_dir(new_directory, time(NULL));
  log_info(LD_DIRSERV,"New directory (size %d) has been built.",
           (int)the_directory->dir_len);
  log_debug(LD_DIRSERV,"New directory (size %d):\n%s",
            (int)the_directory->dir_len, the_directory->dir);

  the_directory_is_dirty = 0;

  /* Save the directory to disk so we re-load it quickly on startup.
   */
  dirserv_set_cached_directory(the_directory->dir, time(NULL), 0);

  return the_directory;
}

/** Only called by v1 auth dirservers.
 * Replace the current running-routers list with a newly generated one. */
static cached_dir_t *
generate_runningrouters(void)
{
  char *s=NULL;
  char digest[DIGEST_LEN];
  char published[ISO_TIME_LEN+1];
  size_t len;
  crypto_pk_env_t *private_key = get_identity_key();
  char *identity_pkey; /* Identity key, DER64-encoded. */
  size_t identity_pkey_len;

  if (crypto_pk_write_public_key_to_string(private_key,&identity_pkey,
                                           &identity_pkey_len)<0) {
    log_warn(LD_BUG,"write identity_pkey to string failed!");
    goto err;
  }
  format_iso_time(published, time(NULL));

  len = 2048;
  s = tor_malloc_zero(len);
  tor_snprintf(s, len,
               "network-status\n"
               "published %s\n"
               "router-status %s\n"
               "dir-signing-key\n%s"
               "directory-signature %s\n",
               published, "", identity_pkey,
               get_options()->Nickname);
  tor_free(identity_pkey);
  if (router_get_runningrouters_hash(s,digest)) {
    log_warn(LD_BUG,"couldn't compute digest");
    goto err;
  }
  note_crypto_pk_op(SIGN_DIR);
  if (router_append_dirobj_signature(s, len, digest, private_key)<0)
    goto err;

  set_cached_dir(&the_runningrouters, s, time(NULL));
  runningrouters_is_dirty = 0;

  return &the_runningrouters;
 err:
  tor_free(s);
  return NULL;
}

/** Set *<b>rr</b> to the most recently generated encoded signed
 * running-routers list, generating a new one as necessary.  Return the
 * size of the directory on success, and 0 on failure. */
cached_dir_t *
dirserv_get_runningrouters(void)
{
  return dirserv_pick_cached_dir_obj(
                         &cached_runningrouters, &the_runningrouters,
                         runningrouters_is_dirty,
                         generate_runningrouters,
                         "v1 network status list", V1_AUTHORITY);
}

cached_dir_t *
dirserv_get_consensus(void)
{
  return cached_v3_networkstatus;
}

/** For authoritative directories: the current (v2) network status. */
static cached_dir_t *the_v2_networkstatus = NULL;

/** Return true iff our opinion of the routers has been stale for long
 * enough that we should generate a new v2 network status doc. */
static int
should_generate_v2_networkstatus(void)
{
  return authdir_mode_v2(get_options()) &&
    !authdir_mode_bridge(get_options()) && /* XXX020 RD */
    the_v2_networkstatus_is_dirty &&
    the_v2_networkstatus_is_dirty + DIR_REGEN_SLACK_TIME < time(NULL);
}

/** If a router's uptime is at least this value, then it is always
 * considered stable, regardless of the rest of the network. This
 * way we resist attacks where an attacker doubles the size of the
 * network using allegedly high-uptime nodes, displacing all the
 * current guards. */
#define UPTIME_TO_GUARANTEE_STABLE (3600*24*30)
/** If a router's MTBF is at least this value, then it is always stable.
 * See above.  (Corresponds to about 7 days for current decay rates.) */
#define MTBF_TO_GUARANTEE_STABLE (60*60*24*5)
/** Similarly, we protect sufficiently fast nodes from being pushed
 * out of the set of Fast nodes. */
#define BANDWIDTH_TO_GUARANTEE_FAST (100*1024)
/** Similarly, every node with sufficient bandwidth can be considered
 * for Guard status. */
#define BANDWIDTH_TO_GUARANTEE_GUARD (250*1024)
/** Similarly, every node with at least this much weighted time known can be
 * considered familiar enough to be a guard.  Corresponds to about 20 days for
 * current decay rates.
 */
#define TIME_KNOWN_TO_GUARANTEE_FAMILIAR (8*24*60*60)
/** Similarly, every node with sufficient WFU is around enough to be a guard.
 */
#define WFU_TO_GUARANTEE_GUARD (0.995)

/* Thresholds for server performance: set by
 * dirserv_compute_performance_thresholds, and used by
 * generate_v2_networkstatus */
static uint32_t stable_uptime = 0; /* start at a safe value */
static double stable_mtbf = 0.0;
static int enough_mtbf_info = 0;
static double guard_wfu = 0.0;
static long guard_tk = 0;
static uint32_t fast_bandwidth = 0;
static uint32_t guard_bandwidth_including_exits = 0;
static uint32_t guard_bandwidth_excluding_exits = 0;
static uint64_t total_bandwidth = 0;
static uint64_t total_exit_bandwidth = 0;

/** Helper: estimate the uptime of a router given its stated uptime and the
 * amount of time since it last stated its stated uptime. */
static INLINE int
real_uptime(routerinfo_t *router, time_t now)
{
  if (now < router->cache_info.published_on)
    return router->uptime;
  else
    return router->uptime + (now - router->cache_info.published_on);
}

/** Return 1 if <b>router</b> is not suitable for these parameters, else 0.
 * If <b>need_uptime</b> is non-zero, we require a minimum uptime.
 * If <b>need_capacity</b> is non-zero, we require a minimum advertised
 * bandwidth.
 */
static int
dirserv_thinks_router_is_unreliable(time_t now,
                                    routerinfo_t *router,
                                    int need_uptime, int need_capacity)
{
  if (need_uptime) {
    if (!enough_mtbf_info) {
      /* XXXX Once most authorities are on v3, we should change the rule from
       * "use uptime if we don't have mtbf data" to "don't advertise Stable on
       * v3 if we don't have enough mtbf data." */
      int uptime = real_uptime(router, now);
      if ((unsigned)uptime < stable_uptime &&
          (unsigned)uptime < UPTIME_TO_GUARANTEE_STABLE)
        return 1;
    } else {
      double mtbf =
        rep_hist_get_stability(router->cache_info.identity_digest, now);
      if (mtbf < stable_mtbf)
        return 1;
    }
  }
  if (need_capacity) {
    uint32_t bw = router_get_advertised_bandwidth(router);
    if (bw < fast_bandwidth)
      return 1;
  }
  return 0;
}

/** Return true iff <b>router</b> should be assigned the "HSDir" flag.
 * Right now this means it advertises support for it, it has a high
 * uptime, and it's currently considered Running.
 *
 * This function needs to be called after router->is_running has
 * been set.
 */
static int
dirserv_thinks_router_is_hs_dir(routerinfo_t *router, time_t now)
{
  int uptime = real_uptime(router, now);

  return (router->wants_to_be_hs_dir &&
          uptime > get_options()->MinUptimeHidServDirectoryV2 &&
          router->is_running);
}

/** Look through the routerlist, and assign the median uptime of running valid
 * servers to stable_uptime, and the relative bandwidth capacities to
 * fast_bandwidth and guard_bandwidth.  Set total_bandwidth to the total
 * capacity of all running valid servers and total_exit_bandwidth to the
 * capacity of all running valid exits.  Set the is_exit flag of each router
 * appropriately. */
static void
dirserv_compute_performance_thresholds(routerlist_t *rl)
{
  int n_active, n_active_nonexit, n_familiar;
  uint32_t *uptimes, *bandwidths, *bandwidths_excluding_exits;
  long *tks;
  double *mtbfs, *wfus;
  time_t now = time(NULL);

  /* DOCDOC this is a litle tricky; comment this function better. */

  /* initialize these all here, in case there are no routers */
  stable_uptime = 0;
  stable_mtbf = 0;
  fast_bandwidth = 0;
  guard_bandwidth_including_exits = 0;
  guard_bandwidth_excluding_exits = 0;
  guard_tk = 0;
  guard_wfu = 0;

  total_bandwidth = 0;
  total_exit_bandwidth = 0;

  n_active = n_active_nonexit = 0;
  uptimes = tor_malloc(sizeof(uint32_t)*smartlist_len(rl->routers));
  bandwidths = tor_malloc(sizeof(uint32_t)*smartlist_len(rl->routers));
  bandwidths_excluding_exits =
    tor_malloc(sizeof(uint32_t)*smartlist_len(rl->routers));
  mtbfs = tor_malloc(sizeof(double)*smartlist_len(rl->routers));
  tks = tor_malloc(sizeof(long)*smartlist_len(rl->routers));
  wfus = tor_malloc(sizeof(double)*smartlist_len(rl->routers));

  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ri, {
    if (router_is_active(ri, now)) {
      const char *id = ri->cache_info.identity_digest;
      uint32_t bw;
      ri->is_exit = exit_policy_is_general_exit(ri->exit_policy);
      uptimes[n_active] = real_uptime(ri, now);
      mtbfs[n_active] = rep_hist_get_stability(id, now);
      tks  [n_active] = rep_hist_get_weighted_time_known(id, now);
      bandwidths[n_active] = bw = router_get_advertised_bandwidth(ri);
      total_bandwidth += bw;
      if (ri->is_exit && !ri->is_bad_exit) {
        total_exit_bandwidth += bw;
      } else {
        bandwidths_excluding_exits[n_active_nonexit] = bw;
        ++n_active_nonexit;
      }
      ++n_active;
    }
  });

  if (n_active) {
    stable_uptime = median_uint32(uptimes, n_active);
    stable_mtbf = median_double(mtbfs, n_active);
    fast_bandwidth = find_nth_uint32(bandwidths, n_active, n_active/8);
    /* Now bandwidths is sorted. */
    if (fast_bandwidth < ROUTER_REQUIRED_MIN_BANDWIDTH)
      fast_bandwidth = bandwidths[n_active/4];
    guard_bandwidth_including_exits = bandwidths[(n_active-1)/2];
    guard_tk = find_nth_long(tks, n_active, n_active/8);
  }

  if (guard_tk > TIME_KNOWN_TO_GUARANTEE_FAMILIAR)
    guard_tk = TIME_KNOWN_TO_GUARANTEE_FAMILIAR;

  if (fast_bandwidth > BANDWIDTH_TO_GUARANTEE_FAST)
    fast_bandwidth = BANDWIDTH_TO_GUARANTEE_FAST;

  n_familiar = 0;
  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ri, {
      if (router_is_active(ri, now)) {
        const char *id = ri->cache_info.identity_digest;
        long tk = rep_hist_get_weighted_time_known(id, now);
        if (tk < guard_tk)
          continue;
        wfus[n_familiar++] = rep_hist_get_weighted_fractional_uptime(id, now);
      }
    });
  if (n_familiar)
    guard_wfu = median_double(wfus, n_familiar);
  if (guard_wfu > WFU_TO_GUARANTEE_GUARD)
    guard_wfu = WFU_TO_GUARANTEE_GUARD;

  enough_mtbf_info = rep_hist_have_measured_enough_stability();

  if (n_active_nonexit) {
    guard_bandwidth_excluding_exits =
      median_uint32(bandwidths_excluding_exits, n_active_nonexit);
  }

  log(LOG_INFO, LD_DIRSERV,
      "Cutoffs: For Stable, %lu sec uptime, %lu sec MTBF. "
      "For Fast: %lu bytes/sec."
      "For Guard: WFU %.03lf%%, time-known %lu sec, "
      "and bandwidth %lu or %lu bytes/sec.",
      (unsigned long)stable_uptime,
      (unsigned long)stable_mtbf,
      (unsigned long)fast_bandwidth,
      guard_wfu*100,
      (unsigned long)guard_tk,
      (unsigned long)guard_bandwidth_including_exits,
      (unsigned long)guard_bandwidth_excluding_exits);

  tor_free(uptimes);
  tor_free(mtbfs);
  tor_free(bandwidths);
  tor_free(bandwidths_excluding_exits);
  tor_free(tks);
  tor_free(wfus);
}

/** Given a platform string as in a routerinfo_t (possibly null), return a
 * newly allocated version string for a networkstatus document, or NULL if the
 * platform doesn't give a Tor version. */
static char *
version_from_platform(const char *platform)
{
  if (platform && !strcmpstart(platform, "Tor ")) {
    const char *eos = find_whitespace(platform+4);
    if (eos && !strcmpstart(eos, " (r")) {
      /* XXXX020 Unify this logic with the other version extraction
       * logic */
      eos = find_whitespace(eos+1);
    }
    if (eos) {
      return tor_strndup(platform, eos-platform);
    }
  }
  return NULL;
}

/** Helper: write the router-status information in <b>rs</b> into <b>buf</b>,
 * which has at least <b>buf_len</b> free characters.  Do NUL-termination.
 * Use the same format as in network-status documents.  If <b>version</b> is
 * non-NULL, add a "v" line for the platform.  Return 0 on success, -1 on
 * failure.  If <b>first_line_only<b> is true, don't include any flags
 * or version line.
 */
int
routerstatus_format_entry(char *buf, size_t buf_len,
                          routerstatus_t *rs, const char *version,
                          int first_line_only)
{
  int r;
  struct in_addr in;
  char *cp;

  char published[ISO_TIME_LEN+1];
  char ipaddr[INET_NTOA_BUF_LEN];
  char identity64[BASE64_DIGEST_LEN+1];
  char digest64[BASE64_DIGEST_LEN+1];

  format_iso_time(published, rs->published_on);
  digest_to_base64(identity64, rs->identity_digest);
  digest_to_base64(digest64, rs->descriptor_digest);
  in.s_addr = htonl(rs->addr);
  tor_inet_ntoa(&in, ipaddr, sizeof(ipaddr));

  r = tor_snprintf(buf, buf_len,
                   "r %s %s %s %s %s %d %d\n",
                   rs->nickname,
                   identity64,
                   digest64,
                   published,
                   ipaddr,
                   (int)rs->or_port,
                   (int)rs->dir_port);
  if (r<0) {
    log_warn(LD_BUG, "Not enough space in buffer.");
    return -1;
  }
  if (first_line_only)
    return 0;
  cp = buf + strlen(buf);
  r = tor_snprintf(cp, buf_len - (cp-buf),
                   "s%s%s%s%s%s%s%s%s%s%s%s%s\n",
                  /* These must stay in alphabetical order. */
                   rs->is_authority?" Authority":"",
                   rs->is_bad_exit?" BadExit":"",
                   rs->is_exit?" Exit":"",
                   rs->is_fast?" Fast":"",
                   rs->is_possible_guard?" Guard":"",
                   rs->is_hs_dir?" HSDir":"",
                   rs->is_named?" Named":"",
                   rs->is_running?" Running":"",
                   rs->is_stable?" Stable":"",
                   rs->is_unnamed?" Unnamed":"",
                   rs->is_v2_dir?" V2Dir":"",
                   rs->is_valid?" Valid":"");
  if (r<0) {
    log_warn(LD_BUG, "Not enough space in buffer.");
    return -1;
  }
  cp += strlen(cp);

  if (version) {
    if (tor_snprintf(cp, buf_len - (cp-buf), "opt v %s\n", version)<0) {
      log_warn(LD_BUG, "Unable to print router version.");
      return -1;
    }
  }

  return 0;
}

/** Helper for sorting: compares two routerinfos first by address, and then by
 * descending order of "usefulness".  (An authority is more useful than a
 * non-authority; a running router is more useful than a non-running router;
 * and a router with more bandwidth is more useful than one with less.)
 **/
static int
_compare_routerinfo_by_ip_and_bw(const void **a, const void **b)
{
  routerinfo_t *first = *(routerinfo_t **)a, *second = *(routerinfo_t **)b;
  int first_is_auth, second_is_auth;
  uint32_t bw_first, bw_second;

  /* we return -1 if first should appear before second... that is,
   * if first is a better router. */
  if (first->addr < second->addr)
    return -1;
  else if (first->addr > second->addr)
    return 1;

  /* Potentially, this next bit could cause k n lg n memcmp calls.  But in
   * reality, we will almost never get here, since addresses will usually be
   * different. */

  first_is_auth =
    router_digest_is_trusted_dir(first->cache_info.identity_digest);
  second_is_auth =
    router_digest_is_trusted_dir(second->cache_info.identity_digest);

  if (first_is_auth && !second_is_auth)
    return -1;
  else if (!first_is_auth && second_is_auth)
    return 1;

  else if (first->is_running && !second->is_running)
    return -1;
  else if (!first->is_running && second->is_running)
    return 1;

  bw_first = router_get_advertised_bandwidth(first);
  bw_second = router_get_advertised_bandwidth(second);

  if (bw_first > bw_second)
     return -1;
  else if (bw_first < bw_second)
    return 1;

  /* They're equal! Compare by identity digest, so there's a
   * deterministic order and we avoid flapping. */
  return memcmp(first->cache_info.identity_digest,
                second->cache_info.identity_digest,
                DIGEST_LEN);
}

/** Given a list of routerinfo_t in <b>routers</b>, return a new digestmap_t
 * whose keys are the identity digests of those routers that we're going to
 * exclude for Sybil-like appearance. */
static digestmap_t *
get_possible_sybil_list(const smartlist_t *routers)
{
  digestmap_t *omit_as_sybil;
  smartlist_t *routers_by_ip = smartlist_create();
  uint32_t last_addr;
  int addr_count;
  smartlist_add_all(routers_by_ip, routers);
  smartlist_sort(routers_by_ip, _compare_routerinfo_by_ip_and_bw);
  omit_as_sybil = digestmap_new();

/* Allow at most this number of Tor servers on a single IP address, ... */
#define MAX_WITH_SAME_ADDR 2
/* ... unless it's a directory authority, in which case allow more. */
#define MAX_WITH_SAME_ADDR_ON_AUTHORITY 5
  last_addr = 0;
  addr_count = 0;
  SMARTLIST_FOREACH(routers_by_ip, routerinfo_t *, ri,
    {
      if (last_addr != ri->addr) {
        last_addr = ri->addr;
        addr_count = 1;
      } else if (++addr_count > MAX_WITH_SAME_ADDR) {
        if (!router_addr_is_trusted_dir(ri->addr) ||
            addr_count > MAX_WITH_SAME_ADDR_ON_AUTHORITY)
          digestmap_set(omit_as_sybil, ri->cache_info.identity_digest, ri);
      }
    });

  smartlist_free(routers_by_ip);
  return omit_as_sybil;
}

/** Extract status information from <b>ri</b> and from other authority
 * functions and store it in <b>rs</b>>.  If <b>naming</b>, consider setting
 * the named flag in <b>rs</b>. If not <b>exits_can_be_guards</b>, never mark
 * an exit as a guard.  If <b>listbadexits</b>, consider setting the badexit
 * flag. */
static void
set_routerstatus_from_routerinfo(routerstatus_t *rs,
                                 routerinfo_t *ri, time_t now,
                                 int naming, int exits_can_be_guards,
                                 int listbadexits)
{
  int unstable_version =
    tor_version_as_new_as(ri->platform,"0.1.1.10-alpha") &&
    !tor_version_as_new_as(ri->platform,"0.1.1.16-rc-cvs");
  memset(rs, 0, sizeof(routerstatus_t));

  rs->is_authority =
    router_digest_is_trusted_dir(ri->cache_info.identity_digest);

  /* Already set by compute_performance_thresholds. */
  rs->is_exit = ri->is_exit;
  rs->is_stable = ri->is_stable =
    router_is_active(ri, now) &&
    !dirserv_thinks_router_is_unreliable(now, ri, 1, 0) &&
    !unstable_version;
  rs->is_fast = ri->is_fast =
    router_is_active(ri, now) &&
    !dirserv_thinks_router_is_unreliable(now, ri, 0, 1);
  rs->is_running = ri->is_running; /* computed above */

  if (naming) {
    uint32_t name_status = dirserv_get_name_status(
                         ri->cache_info.identity_digest, ri->nickname);
    rs->is_named = (naming && (name_status & FP_NAMED)) ? 1 : 0;
    rs->is_unnamed = (naming && (name_status & FP_UNNAMED)) ? 1 : 0;
  }
  rs->is_valid = ri->is_valid;

  if (rs->is_fast &&
      (!rs->is_exit || exits_can_be_guards) &&
      (router_get_advertised_bandwidth(ri) >= BANDWIDTH_TO_GUARANTEE_GUARD ||
       router_get_advertised_bandwidth(ri) >=
       (exits_can_be_guards ? guard_bandwidth_including_exits :
        guard_bandwidth_excluding_exits))) {
    long tk = rep_hist_get_weighted_time_known(
                                      ri->cache_info.identity_digest, now);
    double wfu = rep_hist_get_weighted_fractional_uptime(
                                      ri->cache_info.identity_digest, now);
    rs->is_possible_guard = (wfu >= guard_wfu && tk >= guard_tk) ? 1 : 0;
  } else {
    rs->is_possible_guard = 0;
  }
  rs->is_bad_exit = listbadexits && ri->is_bad_exit;
  ri->is_hs_dir = dirserv_thinks_router_is_hs_dir(ri, now);
  rs->is_hs_dir = ri->is_hs_dir;
  /* 0.1.1.9-alpha is the first version to support fetch by descriptor
   * hash. */
  rs->is_v2_dir = ri->dir_port &&
    tor_version_as_new_as(ri->platform,"0.1.1.9-alpha");

  if (!strcasecmp(ri->nickname, UNNAMED_ROUTER_NICKNAME))
    rs->is_named = rs->is_unnamed = 0;

  rs->published_on = ri->cache_info.published_on;
  memcpy(rs->identity_digest, ri->cache_info.identity_digest, DIGEST_LEN);
  memcpy(rs->descriptor_digest, ri->cache_info.signed_descriptor_digest,
         DIGEST_LEN);
  rs->addr = ri->addr;
  strlcpy(rs->nickname, ri->nickname, sizeof(rs->nickname));
  rs->or_port = ri->or_port;
  rs->dir_port = ri->dir_port;
}

/** If we've been around for less than this amount of time, our reachability
 * information is not accurate. */
#define DIRSERV_TIME_TO_GET_REACHABILITY_INFO (30*60)

/** Return a new networkstatus_vote_t* containing our current opinion. (For v3
 * authorities */
networkstatus_vote_t *
dirserv_generate_networkstatus_vote_obj(crypto_pk_env_t *private_key,
                                       authority_cert_t *cert)
{
  or_options_t *options = get_options();
  networkstatus_vote_t *v3_out = NULL;
  uint32_t addr;
  char *hostname = NULL, *client_versions = NULL, *server_versions = NULL;
  const char *contact;
  smartlist_t *routers, *routerstatuses;
  char identity_digest[DIGEST_LEN];
  char signing_key_digest[DIGEST_LEN];
  int naming = options->NamingAuthoritativeDir;
  int listbadexits = options->AuthDirListBadExits;
  int exits_can_be_guards;
  routerlist_t *rl = router_get_routerlist();
  time_t now = time(NULL);
  time_t cutoff = now - ROUTER_MAX_AGE_TO_PUBLISH;
  networkstatus_voter_info_t *voter = NULL;
  vote_timing_t timing;
  digestmap_t *omit_as_sybil = NULL;
  int vote_on_reachability = 1;

  tor_assert(private_key);
  tor_assert(cert);

  if (now - time_of_process_start < DIRSERV_TIME_TO_GET_REACHABILITY_INFO)
    vote_on_reachability = 0;

  if (resolve_my_address(LOG_WARN, options, &addr, &hostname)<0) {
    log_warn(LD_NET, "Couldn't resolve my hostname");
    return NULL;
  }
  if (!strchr(hostname, '.')) {
    tor_free(hostname);
    hostname = tor_dup_addr(addr);
  }
  if (crypto_pk_get_digest(private_key, signing_key_digest)<0) {
    log_err(LD_BUG, "Error computing signing key digest");
    return NULL;
  }
  if (crypto_pk_get_digest(cert->identity_key, identity_digest)<0) {
    log_err(LD_BUG, "Error computing identity key digest");
    return NULL;
  }

  if (options->VersioningAuthoritativeDir) {
    client_versions = format_versions_list(options->RecommendedClientVersions);
    server_versions = format_versions_list(options->RecommendedServerVersions);
  }

  contact = get_options()->ContactInfo;
  if (!contact)
    contact = "(none)";

  /* precompute this part, since we need it to decide what "stable"
   * means. */
  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ri, {
    dirserv_set_router_is_running(ri, now);
  });

  dirserv_compute_performance_thresholds(rl);

  /* XXXX We should take steps to keep this from oscillating if
   * total_exit_bandwidth is close to total_bandwidth/3. */
  exits_can_be_guards = total_exit_bandwidth >= (total_bandwidth / 3);

  routers = smartlist_create();
  smartlist_add_all(routers, rl->routers);
  routers_sort_by_identity(routers);
  omit_as_sybil = get_possible_sybil_list(routers);

  routerstatuses = smartlist_create();

  SMARTLIST_FOREACH(routers, routerinfo_t *, ri, {
    if (ri->cache_info.published_on >= cutoff) {
      routerstatus_t *rs;
      vote_routerstatus_t *vrs;

      vrs = tor_malloc_zero(sizeof(vote_routerstatus_t));
      rs = &vrs->status;
      set_routerstatus_from_routerinfo(rs, ri, now,
                                       naming, exits_can_be_guards,
                                       listbadexits);

      if (digestmap_get(omit_as_sybil, ri->cache_info.identity_digest)) {
        rs->is_authority = rs->is_exit = rs->is_stable = rs->is_fast =
          rs->is_running = rs->is_named = rs->is_valid = rs->is_v2_dir =
          rs->is_hs_dir = rs->is_possible_guard = 0;
        /* FFFF we might want some mechanism to check later on if we
         * missed zeroing any flags: it's easy to add a new flag but
         * forget to add it to this clause. */
      }
      if (!vote_on_reachability)
        rs->is_running = 0;

      vrs->version = version_from_platform(ri->platform);
      smartlist_add(routerstatuses, vrs);
    }
  });
  smartlist_free(routers);
  digestmap_free(omit_as_sybil, NULL);

  v3_out = tor_malloc_zero(sizeof(networkstatus_vote_t));

  v3_out->is_vote = 1;
  dirvote_get_preferred_voting_intervals(&timing);
  v3_out->published = now;
  {
    char tbuf[ISO_TIME_LEN+1];
    networkstatus_vote_t *current_consensus =
      networkstatus_get_live_consensus(now);
    time_t last_consensus_interval; /* only used to pick a valid_after */
    if (current_consensus)
      last_consensus_interval = current_consensus->fresh_until -
        current_consensus->valid_after;
    else
      last_consensus_interval = DEFAULT_VOTING_INTERVAL_WHEN_NO_CONSENSUS;
    v3_out->valid_after =
      dirvote_get_start_of_next_interval(now, last_consensus_interval);
    format_iso_time(tbuf, v3_out->valid_after);
    log_notice(LD_DIR,"Choosing valid-after time in vote as %s: "
               "consensus_set=%d, last_interval=%d",
               tbuf, current_consensus?1:0, (int)last_consensus_interval);
  }
  v3_out->fresh_until = v3_out->valid_after + timing.vote_interval;
  v3_out->valid_until = v3_out->valid_after +
    (timing.vote_interval * timing.n_intervals_valid);
  v3_out->vote_seconds = timing.vote_delay;
  v3_out->dist_seconds = timing.dist_delay;
  tor_assert(v3_out->vote_seconds > 0);
  tor_assert(v3_out->dist_seconds > 0);
  tor_assert(timing.n_intervals_valid > 0);

  v3_out->client_versions = client_versions;
  v3_out->server_versions = server_versions;
  v3_out->known_flags = smartlist_create();
  smartlist_split_string(v3_out->known_flags,
                "Authority Exit Fast Guard HSDir Stable V2Dir Valid",
                0, SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  if (vote_on_reachability)
    smartlist_add(v3_out->known_flags, tor_strdup("Running"));
  if (listbadexits)
    smartlist_add(v3_out->known_flags, tor_strdup("BadExit"));
  if (naming) {
    smartlist_add(v3_out->known_flags, tor_strdup("Named"));
    smartlist_add(v3_out->known_flags, tor_strdup("Unnamed"));
  }
  smartlist_sort_strings(v3_out->known_flags);

  voter = tor_malloc_zero(sizeof(networkstatus_voter_info_t));
  voter->nickname = tor_strdup(options->Nickname);
  memcpy(voter->identity_digest, identity_digest, DIGEST_LEN);
  voter->address = hostname;
  voter->addr = addr;
  voter->dir_port = options->DirPort;
  voter->or_port = options->ORPort;
  voter->contact = tor_strdup(contact);
  memcpy(voter->signing_key_digest, signing_key_digest, DIGEST_LEN);
  v3_out->voters = smartlist_create();
  smartlist_add(v3_out->voters, voter);
  v3_out->cert = authority_cert_dup(cert);
  v3_out->routerstatus_list = routerstatuses;
  /* Note: networkstatus_digest is unset; it won't get set until we actually
   * format the vote. */

  return v3_out;
}

/** For v2 authoritative directories only: Replace the contents of
 * <b>the_v2_networkstatus</b> with a newly generated network status
 * object.  */
static cached_dir_t *
generate_v2_networkstatus_opinion(void)
{
/** Longest status flag name that we generate. */
#define LONGEST_STATUS_FLAG_NAME_LEN 9
/** Maximum number of status flags we'll apply to one router. */
#define N_STATUS_FLAGS 10
/** Amount of space to allocate for each entry. (r line and s line.) */
#define RS_ENTRY_LEN                                                    \
  ( /* first line */                                                    \
   MAX_NICKNAME_LEN+BASE64_DIGEST_LEN*2+ISO_TIME_LEN+INET_NTOA_BUF_LEN+ \
   5*2 /* ports */ + 10 /* punctuation */ +                             \
   /* second line */                                                    \
   (LONGEST_STATUS_FLAG_NAME_LEN+1)*N_STATUS_FLAGS + 2)

  cached_dir_t *r = NULL;
  size_t len, identity_pkey_len;
  char *status = NULL, *client_versions = NULL, *server_versions = NULL,
    *identity_pkey = NULL, *hostname = NULL;
  char *outp, *endp;
  or_options_t *options = get_options();
  char fingerprint[FINGERPRINT_LEN+1];
  char ipaddr[INET_NTOA_BUF_LEN];
  char published[ISO_TIME_LEN+1];
  char digest[DIGEST_LEN];
  struct in_addr in;
  uint32_t addr;
  crypto_pk_env_t *private_key;
  routerlist_t *rl = router_get_routerlist();
  time_t now = time(NULL);
  time_t cutoff = now - ROUTER_MAX_AGE_TO_PUBLISH;
  int naming = options->NamingAuthoritativeDir;
  int versioning = options->VersioningAuthoritativeDir;
  int listbadexits = options->AuthDirListBadExits;
  int exits_can_be_guards;
  const char *contact;
  char *version_lines = NULL;
  smartlist_t *routers = NULL;
  digestmap_t *omit_as_sybil = NULL;

  private_key = get_identity_key();

  if (resolve_my_address(LOG_WARN, options, &addr, &hostname)<0) {
    log_warn(LD_NET, "Couldn't resolve my hostname");
    goto done;
  }
  in.s_addr = htonl(addr);
  tor_inet_ntoa(&in, ipaddr, sizeof(ipaddr));

  format_iso_time(published, now);

  client_versions = format_versions_list(options->RecommendedClientVersions);
  server_versions = format_versions_list(options->RecommendedServerVersions);

  if (crypto_pk_write_public_key_to_string(private_key, &identity_pkey,
                                           &identity_pkey_len)<0) {
    log_warn(LD_BUG,"Writing public key to string failed.");
    goto done;
  }

  if (crypto_pk_get_fingerprint(private_key, fingerprint, 0)<0) {
    log_err(LD_BUG, "Error computing fingerprint");
    goto done;
  }

  contact = get_options()->ContactInfo;
  if (!contact)
    contact = "(none)";

  if (versioning) {
    size_t v_len = 64+strlen(client_versions)+strlen(server_versions);
    version_lines = tor_malloc(v_len);
    tor_snprintf(version_lines, v_len,
                 "client-versions %s\nserver-versions %s\n",
                 client_versions, server_versions);
  } else {
    version_lines = tor_strdup("");
  }

  len = 4096+strlen(client_versions)+strlen(server_versions);
  len += identity_pkey_len*2;
  len += (RS_ENTRY_LEN)*smartlist_len(rl->routers);

  status = tor_malloc(len);
  tor_snprintf(status, len,
               "network-status-version 2\n"
               "dir-source %s %s %d\n"
               "fingerprint %s\n"
               "contact %s\n"
               "published %s\n"
               "dir-options%s%s%s\n"
               "%s" /* client version line, server version line. */
               "dir-signing-key\n%s",
               hostname, ipaddr, (int)options->DirPort,
               fingerprint,
               contact,
               published,
               naming ? " Names" : "",
               listbadexits ? " BadExits" : "",
               versioning ? " Versions" : "",
               version_lines,
               identity_pkey);
  outp = status + strlen(status);
  endp = status + len;

  /* precompute this part, since we need it to decide what "stable"
   * means. */
  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ri, {
    dirserv_set_router_is_running(ri, now);
  });

  dirserv_compute_performance_thresholds(rl);

  /* XXXX We should take steps to keep this from oscillating if
   * total_exit_bandwidth is close to total_bandwidth/3. */
  exits_can_be_guards = total_exit_bandwidth >= (total_bandwidth / 3);

  routers = smartlist_create();
  smartlist_add_all(routers, rl->routers);
  routers_sort_by_identity(routers);

  omit_as_sybil = get_possible_sybil_list(routers);

  SMARTLIST_FOREACH(routers, routerinfo_t *, ri, {
    if (ri->cache_info.published_on >= cutoff) {
      routerstatus_t rs;
      char *version = version_from_platform(ri->platform);

      set_routerstatus_from_routerinfo(&rs, ri, now,
                                       naming, exits_can_be_guards,
                                       listbadexits);

      if (digestmap_get(omit_as_sybil, ri->cache_info.identity_digest)) {
        rs.is_authority = rs.is_exit = rs.is_stable = rs.is_fast =
          rs.is_running = rs.is_named = rs.is_valid = rs.is_v2_dir =
          rs.is_hs_dir = rs.is_possible_guard = 0;
      }

      if (routerstatus_format_entry(outp, endp-outp, &rs, version, 0)) {
        log_warn(LD_BUG, "Unable to print router status.");
        tor_free(version);
        goto done;
      }
      tor_free(version);
      outp += strlen(outp);
    }
  });

  if (tor_snprintf(outp, endp-outp, "directory-signature %s\n",
                   get_options()->Nickname)<0) {
    log_warn(LD_BUG, "Unable to write signature line.");
    goto done;
  }
  if (router_get_networkstatus_v2_hash(status, digest)<0) {
    log_warn(LD_BUG, "Unable to hash network status");
    goto done;
  }
  outp += strlen(outp);

  note_crypto_pk_op(SIGN_DIR);
  if (router_append_dirobj_signature(outp,endp-outp,digest,private_key)<0) {
    log_warn(LD_BUG, "Unable to sign router status.");
    goto done;
  }

  {
    networkstatus_v2_t *ns;
    if (!(ns = networkstatus_v2_parse_from_string(status))) {
      log_err(LD_BUG,"Generated a networkstatus we couldn't parse.");
      goto done;
    }
    networkstatus_v2_free(ns);
  }

  {
    cached_dir_t **ns_ptr = &the_v2_networkstatus;
    if (*ns_ptr)
      cached_dir_decref(*ns_ptr);
    *ns_ptr = new_cached_dir(status, now);
    status = NULL; /* So it doesn't get double-freed. */
    the_v2_networkstatus_is_dirty = 0;
    router_set_networkstatus_v2((*ns_ptr)->dir, now, NS_GENERATED, NULL);
    r = *ns_ptr;
  }

 done:
  tor_free(client_versions);
  tor_free(server_versions);
  tor_free(version_lines);
  tor_free(status);
  tor_free(hostname);
  tor_free(identity_pkey);
  if (routers)
    smartlist_free(routers);
  if (omit_as_sybil)
    digestmap_free(omit_as_sybil, NULL);
  return r;
}

/** Given the portion of a networkstatus request URL after "tor/status/" in
 * <b>key</b>, append to <b>result</b> the digests of the identity keys of the
 * networkstatus objects that the client has requested. */
void
dirserv_get_networkstatus_v2_fingerprints(smartlist_t *result,
                                          const char *key)
{
  tor_assert(result);

  if (!cached_v2_networkstatus)
    cached_v2_networkstatus = digestmap_new();

  if (should_generate_v2_networkstatus())
    generate_v2_networkstatus_opinion();

  if (!strcmp(key,"authority")) {
    if (authdir_mode_v2(get_options())) {
      routerinfo_t *me = router_get_my_routerinfo();
      if (me)
        smartlist_add(result,
                      tor_memdup(me->cache_info.identity_digest, DIGEST_LEN));
    }
  } else if (!strcmp(key, "all")) {
    if (digestmap_size(cached_v2_networkstatus)) {
      digestmap_iter_t *iter;
      iter = digestmap_iter_init(cached_v2_networkstatus);
      while (!digestmap_iter_done(iter)) {
        const char *ident;
        void *val;
        digestmap_iter_get(iter, &ident, &val);
        smartlist_add(result, tor_memdup(ident, DIGEST_LEN));
        iter = digestmap_iter_next(cached_v2_networkstatus, iter);
      }
    } else {
      SMARTLIST_FOREACH(router_get_trusted_dir_servers(),
                  trusted_dir_server_t *, ds,
                  if (ds->type & V2_AUTHORITY)
                    smartlist_add(result, tor_memdup(ds->digest, DIGEST_LEN)));
    }
    smartlist_sort_digests(result);
    if (smartlist_len(result) == 0)
      log_warn(LD_DIRSERV,
               "Client requested 'all' network status objects; we have none.");
  } else if (!strcmpstart(key, "fp/")) {
    dir_split_resource_into_fingerprints(key+3, result, NULL, 1, 1);
  }
}

/** Look for a network status object as specified by <b>key</b>, which should
 * be either "authority" (to find a network status generated by us), a hex
 * identity digest (to find a network status generated by given directory), or
 * "all" (to return all the v2 network status objects we have).
 */
void
dirserv_get_networkstatus_v2(smartlist_t *result,
                             const char *key)
{
  cached_dir_t *cached;
  smartlist_t *fingerprints = smartlist_create();
  tor_assert(result);

  if (!cached_v2_networkstatus)
    cached_v2_networkstatus = digestmap_new();

  dirserv_get_networkstatus_v2_fingerprints(fingerprints, key);
  SMARTLIST_FOREACH(fingerprints, const char *, fp,
    {
      if (router_digest_is_me(fp) && should_generate_v2_networkstatus())
        generate_v2_networkstatus_opinion();
      cached = digestmap_get(cached_v2_networkstatus, fp);
      if (cached) {
        smartlist_add(result, cached);
      } else {
        char hexbuf[HEX_DIGEST_LEN+1];
        base16_encode(hexbuf, sizeof(hexbuf), fp, DIGEST_LEN);
        log_info(LD_DIRSERV, "Don't know about any network status with "
                 "fingerprint '%s'", hexbuf);
      }
    });
  SMARTLIST_FOREACH(fingerprints, char *, cp, tor_free(cp));
  smartlist_free(fingerprints);
}

/** As dirserv_get_routerdescs(), but instead of getting signed_descriptor_t
 * pointers, adds copies of digests to fps_out, and doesn't use the
 * /tor/server/ prefix.  For a /d/ request, adds descriptor digests; for other
 * requests, adds identity digests.
 */
int
dirserv_get_routerdesc_fingerprints(smartlist_t *fps_out, const char *key,
                                    const char **msg)
{
  *msg = NULL;

  if (!strcmp(key, "all")) {
    routerlist_t *rl = router_get_routerlist();
    SMARTLIST_FOREACH(rl->routers, routerinfo_t *, r,
                      smartlist_add(fps_out,
                      tor_memdup(r->cache_info.identity_digest, DIGEST_LEN)));
  } else if (!strcmp(key, "authority")) {
    routerinfo_t *ri = router_get_my_routerinfo();
    if (ri)
      smartlist_add(fps_out,
                    tor_memdup(ri->cache_info.identity_digest, DIGEST_LEN));
  } else if (!strcmpstart(key, "d/")) {
    key += strlen("d/");
    dir_split_resource_into_fingerprints(key, fps_out, NULL, 1, 1);
  } else if (!strcmpstart(key, "fp/")) {
    key += strlen("fp/");
    dir_split_resource_into_fingerprints(key, fps_out, NULL, 1, 1);
  } else {
    *msg = "Key not recognized";
    return -1;
  }

  if (!smartlist_len(fps_out)) {
    *msg = "Servers unavailable";
    return -1;
  }
  return 0;
}

/** Add a signed_descriptor_t to <b>descs_out</b> for each router matching
 * <b>key</b>.  The key should be either
 *   - "/tor/server/authority" for our own routerinfo;
 *   - "/tor/server/all" for all the routerinfos we have, concatenated;
 *   - "/tor/server/fp/FP" where FP is a plus-separated sequence of
 *     hex identity digests; or
 *   - "/tor/server/d/D" where D is a plus-separated sequence
 *     of server descriptor digests, in hex.
 *
 * Return 0 if we found some matching descriptors, or -1 if we do not
 * have any descriptors, no matching descriptors, or if we did not
 * recognize the key (URL).
 * If -1 is returned *<b>msg</b> will be set to an appropriate error
 * message.
 *
 * (Despite its name, this function is also called from the controller, which
 * exposes a similar means to fetch descriptors.)
 */
int
dirserv_get_routerdescs(smartlist_t *descs_out, const char *key,
  const char **msg)
{
  *msg = NULL;

  if (!strcmp(key, "/tor/server/all")) {
    routerlist_t *rl = router_get_routerlist();
    SMARTLIST_FOREACH(rl->routers, routerinfo_t *, r,
                      smartlist_add(descs_out, &(r->cache_info)));
  } else if (!strcmp(key, "/tor/server/authority")) {
    routerinfo_t *ri = router_get_my_routerinfo();
    if (ri)
      smartlist_add(descs_out, &(ri->cache_info));
  } else if (!strcmpstart(key, "/tor/server/d/")) {
    smartlist_t *digests = smartlist_create();
    key += strlen("/tor/server/d/");
    dir_split_resource_into_fingerprints(key, digests, NULL, 1, 1);
    SMARTLIST_FOREACH(digests, const char *, d,
       {
         signed_descriptor_t *sd = router_get_by_descriptor_digest(d);
         if (sd)
           smartlist_add(descs_out,sd);
       });
    SMARTLIST_FOREACH(digests, char *, d, tor_free(d));
    smartlist_free(digests);
  } else if (!strcmpstart(key, "/tor/server/fp/")) {
    smartlist_t *digests = smartlist_create();
    time_t cutoff = time(NULL) - ROUTER_MAX_AGE_TO_PUBLISH;
    key += strlen("/tor/server/fp/");
    dir_split_resource_into_fingerprints(key, digests, NULL, 1, 1);
    SMARTLIST_FOREACH(digests, const char *, d,
       {
         if (router_digest_is_me(d)) {
           smartlist_add(descs_out, &(router_get_my_routerinfo()->cache_info));
         } else {
           routerinfo_t *ri = router_get_by_digest(d);
           /* Don't actually serve a descriptor that everyone will think is
            * expired.  This is an (ugly) workaround to keep buggy 0.1.1.10
            * Tors from downloading descriptors that they will throw away.
            */
           if (ri && ri->cache_info.published_on > cutoff)
             smartlist_add(descs_out, &(ri->cache_info));
         }
       });
    SMARTLIST_FOREACH(digests, char *, d, tor_free(d));
    smartlist_free(digests);
  } else {
    *msg = "Key not recognized";
    return -1;
  }

  if (!smartlist_len(descs_out)) {
    *msg = "Servers unavailable";
    return -1;
  }
  return 0;
}

/** Called when a TLS handshake has completed successfully with a
 * router listening at <b>address</b>:<b>or_port</b>, and has yielded
 * a certificate with digest <b>digest_rcvd</b>.
 *
 * Also, if as_advertised is 1, then inform the reachability checker
 * that we could get to this guy.
 */
void
dirserv_orconn_tls_done(const char *address,
                        uint16_t or_port,
                        const char *digest_rcvd,
                        int as_advertised)
{
  routerlist_t *rl = router_get_routerlist();
  time_t now = time(NULL);
  tor_assert(address);
  tor_assert(digest_rcvd);

  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ri, {
    if (!strcasecmp(address, ri->address) && or_port == ri->or_port &&
        as_advertised &&
        !memcmp(ri->cache_info.identity_digest, digest_rcvd, DIGEST_LEN)) {
      /* correct digest. mark this router reachable! */
      log_info(LD_DIRSERV, "Found router %s to be reachable. Yay.",
               ri->nickname);
      rep_hist_note_router_reachable(digest_rcvd, now);
      ri->last_reachable = now;
    }
  });
  /* FFFF Maybe we should reinstate the code that dumps routers with the same
   * addr/port but with nonmatching keys, but instead of dumping, we should
   * skip testing. */
}

/** Auth dir server only: if <b>try_all</b> is 1, launch connections to
 * all known routers; else we want to load balance such that we only
 * try a few connections per call.
 *
 * The load balancing is such that if we get called once every ten
 * seconds, we will cycle through all the tests in 1280 seconds (a
 * bit over 20 minutes).
 */
void
dirserv_test_reachability(time_t now, int try_all)
{
  /* XXX decide what to do here; see or-talk thread "purging old router
   * information, revocation." -NM
   * We can't afford to mess with this in 0.1.2.x. The reason is that
   * if we stop doing reachability tests on some of routerlist, then
   * we'll for-sure think they're down, which may have unexpected
   * effects in other parts of the code. It doesn't hurt much to do
   * the testing, and directory authorities are easy to upgrade. Let's
   * wait til 0.2.0. -RD */
//  time_t cutoff = now - ROUTER_MAX_AGE_TO_PUBLISH;
  routerlist_t *rl = router_get_routerlist();
  static char ctr = 0;

  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, router, {
    const char *id_digest = router->cache_info.identity_digest;
    if (router_is_me(router))
      continue;
//    if (router->cache_info.published_on > cutoff)
//      continue;
    if (try_all || (((uint8_t)id_digest[0]) % 128) == ctr) {
      log_debug(LD_OR,"Testing reachability of %s at %s:%u.",
                router->nickname, router->address, router->or_port);
      /* Remember when we started trying to determine reachability */
      if (!router->testing_since)
        router->testing_since = now;
      connection_or_connect(router->addr, router->or_port,
                            id_digest);
    }
  });
  if (!try_all) /* increment ctr */
    ctr = (ctr + 1) % 128;
}

/** Remove from <b>fps</b> every networkstatus key where both
 * a) we have a networkstatus document and
 * b) it is not newer than <b>cutoff</b>.
 *
 * Return 1 if any items were present at all; else return 0.
 */
int
dirserv_remove_old_statuses(smartlist_t *fps, time_t cutoff)
{
  int found_any = 0;
  SMARTLIST_FOREACH(fps, char *, digest,
  {
    cached_dir_t *d;
    if (router_digest_is_me(digest) && the_v2_networkstatus)
      d = the_v2_networkstatus;
    else if (tor_digest_is_zero(digest) && cached_v3_networkstatus)
      d = cached_v3_networkstatus;
    else
      d = digestmap_get(cached_v2_networkstatus, digest);
    if (!d)
      continue;
    found_any = 1;
    if (d->published <= cutoff) {
      tor_free(digest);
      SMARTLIST_DEL_CURRENT(fps, digest);
    }
  });

  return found_any;
}

/** Return the cache-info for identity fingerprint <b>fp</b>, or
 * its extra-info document if <b>extrainfo</b> is true. Return
 * NULL if not found or if the descriptor is older than
 * <b>publish_cutoff</b>. */
static signed_descriptor_t *
get_signed_descriptor_by_fp(const char *fp, int extrainfo,
                            time_t publish_cutoff)
{
  if (router_digest_is_me(fp)) {
    if (extrainfo)
      return &(router_get_my_extrainfo()->cache_info);
    else
      return &(router_get_my_routerinfo()->cache_info);
  } else {
    routerinfo_t *ri = router_get_by_digest(fp);
    if (ri &&
        ri->cache_info.published_on > publish_cutoff) {
      if (extrainfo)
        return extrainfo_get_by_descriptor_digest(
                                     ri->cache_info.extra_info_digest);
      else
        return &ri->cache_info;
    }
  }
  return NULL;
}

/** Return true iff we have any of the docments (extrainfo or routerdesc)
 * specified by the fingerprints in <b>fps</b> and <b>spool_src</b>.  Used to
 * decide whether to send a 404.  */
int
dirserv_have_any_serverdesc(smartlist_t *fps, int spool_src)
{
  time_t publish_cutoff = time(NULL)-ROUTER_MAX_AGE_TO_PUBLISH;
  SMARTLIST_FOREACH(fps, const char *, fp, {
      switch (spool_src)
      {
        case DIR_SPOOL_EXTRA_BY_DIGEST:
          if (extrainfo_get_by_descriptor_digest(fp)) return 1;
          break;
        case DIR_SPOOL_SERVER_BY_DIGEST:
          if (router_get_by_descriptor_digest(fp)) return 1;
          break;
        case DIR_SPOOL_EXTRA_BY_FP:
        case DIR_SPOOL_SERVER_BY_FP:
          if (get_signed_descriptor_by_fp(fp,
                spool_src == DIR_SPOOL_EXTRA_BY_FP, publish_cutoff))
            return 1;
          break;
      }
  });
  return 0;
}

/** Return an approximate estimate of the number of bytes that will
 * be needed to transmit the server descriptors (if is_serverdescs --
 * they can be either d/ or fp/ queries) or networkstatus objects (if
 * !is_serverdescs) listed in <b>fps</b>.  If <b>compressed</b> is set,
 * we guess how large the data will be after compression.
 *
 * The return value is an estimate; it might be larger or smaller.
 **/
size_t
dirserv_estimate_data_size(smartlist_t *fps, int is_serverdescs,
                           int compressed)
{
  size_t result;
  tor_assert(fps);
  if (is_serverdescs) {
    int n = smartlist_len(fps);
    routerinfo_t *me = router_get_my_routerinfo();
    result = (me?me->cache_info.signed_descriptor_len:2048) * n;
    if (compressed)
      result /= 2; /* observed compressability is between 35 and 55%. */
  } else {
    result = 0;
    SMARTLIST_FOREACH(fps, const char *, d, {
        cached_dir_t *dir = digestmap_get(cached_v2_networkstatus, d);
        if (dir)
          result += compressed ? dir->dir_z_len : dir->dir_len;
      });
  }
  return result;
}

/** When we're spooling data onto our outbuf, add more whenever we dip
 * below this threshold. */
#define DIRSERV_BUFFER_MIN 16384

/** Spooling helper: called when we have no more data to spool to <b>conn</b>.
 * Flushes any remaining data to be (un)compressed, and changes the spool
 * source to NONE.  Returns 0 on success, negative on failure. */
static int
connection_dirserv_finish_spooling(dir_connection_t *conn)
{
  if (conn->zlib_state) {
    connection_write_to_buf_zlib("", 0, conn, 1);
    tor_zlib_free(conn->zlib_state);
    conn->zlib_state = NULL;
  }
  conn->dir_spool_src = DIR_SPOOL_NONE;
  return 0;
}

/** Spooling helper: called when we're sending a bunch of server descriptors,
 * and the outbuf has become too empty. Pulls some entries from
 * fingerprint_stack, and writes the corresponding servers onto outbuf.  If we
 * run out of entries, flushes the zlib state and sets the spool source to
 * NONE.  Returns 0 on success, negative on failure.
 */
static int
connection_dirserv_add_servers_to_outbuf(dir_connection_t *conn)
{
  int by_fp = (conn->dir_spool_src == DIR_SPOOL_SERVER_BY_FP ||
               conn->dir_spool_src == DIR_SPOOL_EXTRA_BY_FP);
  int extra = (conn->dir_spool_src == DIR_SPOOL_EXTRA_BY_FP ||
               conn->dir_spool_src == DIR_SPOOL_EXTRA_BY_DIGEST);
  time_t publish_cutoff = time(NULL)-ROUTER_MAX_AGE_TO_PUBLISH;

  while (smartlist_len(conn->fingerprint_stack) &&
         buf_datalen(conn->_base.outbuf) < DIRSERV_BUFFER_MIN) {
    const char *body;
    char *fp = smartlist_pop_last(conn->fingerprint_stack);
    signed_descriptor_t *sd = NULL;
    if (by_fp) {
      sd = get_signed_descriptor_by_fp(fp, extra, publish_cutoff);
    } else {
      sd = extra ? extrainfo_get_by_descriptor_digest(fp)
        : router_get_by_descriptor_digest(fp);
    }
    tor_free(fp);
    if (!sd)
      continue;
    body = signed_descriptor_get_body(sd);
    if (conn->zlib_state) {
      int last = ! smartlist_len(conn->fingerprint_stack);
      connection_write_to_buf_zlib(body, sd->signed_descriptor_len, conn,
                                   last);
      if (last) {
        tor_zlib_free(conn->zlib_state);
        conn->zlib_state = NULL;
      }
    } else {
      connection_write_to_buf(body,
                              sd->signed_descriptor_len,
                              TO_CONN(conn));
    }
  }

  if (!smartlist_len(conn->fingerprint_stack)) {
    /* We just wrote the last one; finish up. */
    conn->dir_spool_src = DIR_SPOOL_NONE;
    smartlist_free(conn->fingerprint_stack);
    conn->fingerprint_stack = NULL;
  }
  return 0;
}

/** Spooling helper: Called when we're sending a directory or networkstatus,
 * and the outbuf has become too empty.  Pulls some bytes from
 * <b>conn</b>-\>cached_dir-\>dir_z, uncompresses them if appropriate, and
 * puts them on the outbuf.  If we run out of entries, flushes the zlib state
 * and sets the spool source to NONE.  Returns 0 on success, negative on
 * failure. */
static int
connection_dirserv_add_dir_bytes_to_outbuf(dir_connection_t *conn)
{
  ssize_t bytes;
  int64_t remaining;

  bytes = DIRSERV_BUFFER_MIN - buf_datalen(conn->_base.outbuf);
  tor_assert(bytes > 0);
  tor_assert(conn->cached_dir);
  if (bytes < 8192)
    bytes = 8192;
  remaining = conn->cached_dir->dir_z_len - conn->cached_dir_offset;
  if (bytes > remaining)
    bytes = (ssize_t) remaining;

  if (conn->zlib_state) {
    connection_write_to_buf_zlib(
                             conn->cached_dir->dir_z + conn->cached_dir_offset,
                             bytes, conn, bytes == remaining);
  } else {
    connection_write_to_buf(conn->cached_dir->dir_z + conn->cached_dir_offset,
                            bytes, TO_CONN(conn));
  }
  conn->cached_dir_offset += bytes;
  if (conn->cached_dir_offset == (int)conn->cached_dir->dir_z_len) {
    /* We just wrote the last one; finish up. */
    connection_dirserv_finish_spooling(conn);
    cached_dir_decref(conn->cached_dir);
    conn->cached_dir = NULL;
  }
  return 0;
}

/** Spooling helper: Called when we're spooling networkstatus objects on
 * <b>conn</b>, and the outbuf has become too empty.  If the current
 * networkstatus object (in <b>conn</b>-\>cached_dir) has more data, pull data
 * from there.  Otherwise, pop the next fingerprint from fingerprint_stack,
 * and start spooling the next networkstatus.  (A digest of all 0 bytes is
 * treated as a request for the current consensus.) If we run out of entries,
 * flushes the zlib state and sets the spool source to NONE.  Returns 0 on
 * success, negative on failure. */
static int
connection_dirserv_add_networkstatus_bytes_to_outbuf(dir_connection_t *conn)
{

  while (buf_datalen(conn->_base.outbuf) < DIRSERV_BUFFER_MIN) {
    if (conn->cached_dir) {
      int uncompressing = (conn->zlib_state != NULL);
      int r = connection_dirserv_add_dir_bytes_to_outbuf(conn);
      if (conn->dir_spool_src == DIR_SPOOL_NONE) {
        /* add_dir_bytes thinks we're done with the cached_dir.  But we
         * may have more cached_dirs! */
        conn->dir_spool_src = DIR_SPOOL_NETWORKSTATUS;
        /* This bit is tricky.  If we were uncompressing the last
         * networkstatus, we may need to make a new zlib object to
         * uncompress the next one. */
        if (uncompressing && ! conn->zlib_state &&
            conn->fingerprint_stack &&
            smartlist_len(conn->fingerprint_stack)) {
          conn->zlib_state = tor_zlib_new(0, ZLIB_METHOD);
        }
      }
      if (r) return r;
    } else if (conn->fingerprint_stack &&
               smartlist_len(conn->fingerprint_stack)) {
      /* Add another networkstatus; start serving it. */
      char *fp = smartlist_pop_last(conn->fingerprint_stack);
      cached_dir_t *d;
      if (tor_digest_is_zero(fp))
        d = cached_v3_networkstatus;
      else if (router_digest_is_me(fp))
        d = the_v2_networkstatus;
      else
        d = digestmap_get(cached_v2_networkstatus, fp);
      tor_free(fp);
      if (d) {
        ++d->refcnt;
        conn->cached_dir = d;
        conn->cached_dir_offset = 0;
      }
    } else {
      connection_dirserv_finish_spooling(conn);
      if (conn->fingerprint_stack)
        smartlist_free(conn->fingerprint_stack);
      conn->fingerprint_stack = NULL;
      return 0;
    }
  }
  return 0;
}

/** Called whenever we have flushed some directory data in state
 * SERVER_WRITING. */
int
connection_dirserv_flushed_some(dir_connection_t *conn)
{
  tor_assert(conn->_base.state == DIR_CONN_STATE_SERVER_WRITING);

  if (buf_datalen(conn->_base.outbuf) >= DIRSERV_BUFFER_MIN)
    return 0;

  switch (conn->dir_spool_src) {
    case DIR_SPOOL_EXTRA_BY_DIGEST:
    case DIR_SPOOL_EXTRA_BY_FP:
    case DIR_SPOOL_SERVER_BY_DIGEST:
    case DIR_SPOOL_SERVER_BY_FP:
      return connection_dirserv_add_servers_to_outbuf(conn);
    case DIR_SPOOL_CACHED_DIR:
      return connection_dirserv_add_dir_bytes_to_outbuf(conn);
    case DIR_SPOOL_NETWORKSTATUS:
      return connection_dirserv_add_networkstatus_bytes_to_outbuf(conn);
    case DIR_SPOOL_NONE:
    default:
      return 0;
  }
}

/** Release all storage used by the directory server. */
void
dirserv_free_all(void)
{
  dirserv_free_fingerprint_list();

  cached_dir_decref(the_directory);
  clear_cached_dir(&the_runningrouters);
  cached_dir_decref(the_v2_networkstatus);
  cached_dir_decref(cached_directory);
  clear_cached_dir(&cached_runningrouters);
  if (cached_v2_networkstatus) {
    digestmap_free(cached_v2_networkstatus, _free_cached_dir);
    cached_v2_networkstatus = NULL;
  }
  cached_dir_decref(cached_v3_networkstatus);
}

