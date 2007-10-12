/* Copyright 2001-2004 Roger Dingledine.
 * Copyright 2004-2007 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char dirvote_c_id[] =
  "$Id$";

#define DIRVOTE_PRIVATE
#include "or.h"

/**
 * \file dirvote.c
 * \brief Functions to compute directory consensus, and schedule voting.
 **/

static int dirvote_add_signatures_to_pending_consensus(
                       const char *detached_signatures_body,
                       const char **msg_out);
static char *list_v3_auth_ids(void);
static void dirvote_fetch_missing_votes(void);
static void dirvote_fetch_missing_signatures(void);
static void dirvote_perform_vote(void);
static void dirvote_clear_votes(int all_votes);
static int dirvote_compute_consensus(void);
static int dirvote_publish_consensus(void);

/* =====
 * Voting and consensus generation
 * ===== */

/** Clear all storage held in <b>ns</b>. */
void
networkstatus_vote_free(networkstatus_vote_t *ns)
{
  if (!ns)
    return;

  tor_free(ns->client_versions);
  tor_free(ns->server_versions);
  if (ns->known_flags) {
    SMARTLIST_FOREACH(ns->known_flags, char *, c, tor_free(c));
    smartlist_free(ns->known_flags);
  }
  if (ns->voters) {
    SMARTLIST_FOREACH(ns->voters, networkstatus_voter_info_t *, voter,
    {
      tor_free(voter->nickname);
      tor_free(voter->address);
      tor_free(voter->contact);
    });
    smartlist_free(ns->voters);
  }
  if (ns->cert)
    authority_cert_free(ns->cert);

  if (ns->routerstatus_list) {
    if (ns->is_vote) {
      SMARTLIST_FOREACH(ns->routerstatus_list, vote_routerstatus_t *, rs,
      {
        tor_free(rs->version);
        tor_free(rs);
      });
    } else {
      SMARTLIST_FOREACH(ns->routerstatus_list, routerstatus_t *, rs,
                        tor_free(rs));
    }

    smartlist_free(ns->routerstatus_list);
  }

  memset(ns, 11, sizeof(*ns));
  tor_free(ns);
}

/** Return the voter info from <b>vote</b> for the voter whose identity digest
 * is <b>identity</b>, or NULL if no such voter is associated with
 * <b>vote</b>. */
networkstatus_voter_info_t *
networkstatus_get_voter_by_id(networkstatus_vote_t *vote,
                              const char *identity)
{
  if (!vote || !vote->voters)
    return NULL;
  SMARTLIST_FOREACH(vote->voters, networkstatus_voter_info_t *, voter,
    if (!memcmp(voter->identity_digest, identity, DIGEST_LEN))
      return voter);
  return NULL;
}

/** Given a vote <b>vote</b> (not a consensus!), return its associated
 * networkstatus_voter_info_t.*/
static networkstatus_voter_info_t *
get_voter(const networkstatus_vote_t *vote)
{
  tor_assert(vote);
  tor_assert(vote->is_vote);
  tor_assert(vote->voters);
  tor_assert(smartlist_len(vote->voters) == 1);
  return smartlist_get(vote->voters, 0);
}

/** Helper for sorting networkstatus_vote_t votes (not consensuses) by the
 * hash of their voters' identity digests. */
static int
_compare_votes_by_authority_id(const void **_a, const void **_b)
{
  const networkstatus_vote_t *a = *_a, *b = *_b;
  return memcmp(get_voter(a)->identity_digest,
                get_voter(b)->identity_digest, DIGEST_LEN);
}

/** Given a sorted list of strings <b>in</b>, add every member to <b>out</b>
 * that occurs more than <b>min</b> times. */
static void
get_frequent_members(smartlist_t *out, smartlist_t *in, int min)
{
  char *cur = NULL;
  int count = 0;
  SMARTLIST_FOREACH(in, char *, cp,
  {
    if (cur && !strcmp(cp, cur)) {
      ++count;
    } else {
      if (count > min)
        smartlist_add(out, cur);
      cur = cp;
      count = 1;
    }
  });
  if (count > min)
    smartlist_add(out, cur);
}

/** Given a sorted list of strings <b>lst</b>, return the member that appears
 * most.  Break ties in favor of later-occurring members. */
static const char *
get_most_frequent_member(smartlist_t *lst)
{
  const char *most_frequent = NULL;
  int most_frequent_count = 0;

  const char *cur = NULL;
  int count = 0;

  SMARTLIST_FOREACH(lst, const char *, s,
  {
    if (cur && !strcmp(s, cur)) {
      ++count;
    } else {
      if (count >= most_frequent_count) {
        most_frequent = cur;
        most_frequent_count = count;
      }
      cur = s;
      count = 1;
    }
  });
  if (count >= most_frequent_count) {
    most_frequent = cur;
    most_frequent_count = count;
  }
  return most_frequent;
}

/** Return 0 if and only if <b>a</b> and <b>b</b> are routerstatuses
 * that come from the same routerinfo, with the same derived elements.
 */
static int
compare_vote_rs(const vote_routerstatus_t *a, const vote_routerstatus_t *b)
{
  int r;
  if ((r = memcmp(a->status.identity_digest, b->status.identity_digest,
                  DIGEST_LEN)))
    return r;
  if ((r = memcmp(a->status.descriptor_digest, b->status.descriptor_digest,
                  DIGEST_LEN)))
    return r;
  if ((r = (b->status.published_on - a->status.published_on)))
    return r;
  if ((r = strcmp(b->status.nickname, a->status.nickname)))
    return r;
  if ((r = (((int)b->status.addr) - ((int)a->status.addr))))
    return r;
  if ((r = (((int)b->status.or_port) - ((int)a->status.or_port))))
    return r;
  if ((r = (((int)b->status.dir_port) - ((int)a->status.dir_port))))
    return r;
  return 0;
}

/** Helper for sorting routerlists based on compare_vote_rs. */
static int
_compare_vote_rs(const void **_a, const void **_b)
{
  const vote_routerstatus_t *a = *_a, *b = *_b;
  return compare_vote_rs(a,b);
}

/** Given a list of vote_routerstatus_t, all for the same router identity,
 * return whichever is most frequent, breaking ties in favor of more
 * recently published vote_routerstatus_t.
 */
static vote_routerstatus_t *
compute_routerstatus_consensus(smartlist_t *votes)
{
  vote_routerstatus_t *most = NULL, *cur = NULL;
  int most_n = 0, cur_n = 0;
  time_t most_published = 0;

  smartlist_sort(votes, _compare_vote_rs);
  SMARTLIST_FOREACH(votes, vote_routerstatus_t *, rs,
  {
    if (cur && !compare_vote_rs(cur, rs)) {
      ++cur_n;
    } else {
      if (cur_n > most_n ||
          (cur && cur_n == most_n &&
           cur->status.published_on > most_published)) {
        most = cur;
        most_n = cur_n;
        most_published = cur->status.published_on;
      }
      cur_n = 1;
      cur = rs;
    }
  });

  if (cur_n > most_n ||
      (cur && cur_n == most_n && cur->status.published_on > most_published)) {
    most = cur;
    most_n = cur_n;
    most_published = cur->status.published_on;
  }

  tor_assert(most);
  return most;
}

/** Given a list of strings in <b>lst</b>, set the DIGEST_LEN-byte digest at
 * <b>digest_out</b> to the hash of the concatenation of those strings. */
static void
hash_list_members(char *digest_out, smartlist_t *lst)
{
  crypto_digest_env_t *d = crypto_new_digest_env();
  SMARTLIST_FOREACH(lst, const char *, cp,
                    crypto_digest_add_bytes(d, cp, strlen(cp)));
  crypto_digest_get_digest(d, digest_out, DIGEST_LEN);
  crypto_free_digest_env(d);
}

/** Given a list of vote networkstatus_vote_t in <b>votes</b>, our public
 * authority <b>identity_key</b>, our private authority <b>signing_key</b>,
 * and the number of <b>total_authorities</b> that we believe exist in our
 * voting quorum, generate the text of a new v3 consensus vote, and return the
 * value in a newly allocated string.
 *
 * Note: this function DOES NOT check whether the votes are from
 * recognized authorities.   (dirvote_add_vote does that.) */
char *
networkstatus_compute_consensus(smartlist_t *votes,
                                int total_authorities,
                                crypto_pk_env_t *identity_key,
                                crypto_pk_env_t *signing_key)
{
  smartlist_t *chunks;
  char *result = NULL;

  time_t valid_after, fresh_until, valid_until;
  int vote_seconds, dist_seconds;
  char *client_versions = NULL, *server_versions = NULL;
  smartlist_t *flags;
  tor_assert(total_authorities >= smartlist_len(votes));

  if (!smartlist_len(votes)) {
    log_warn(LD_DIR, "Can't compute a consensus from no votes.");
    return NULL;
  }
  flags = smartlist_create();

  /* Compute medians of time-related things, and figure out how many
   * routers we might need to talk about. */
  {
    int n_votes = smartlist_len(votes);
    time_t *va_times = tor_malloc(n_votes * sizeof(time_t));
    time_t *fu_times = tor_malloc(n_votes * sizeof(time_t));
    time_t *vu_times = tor_malloc(n_votes * sizeof(time_t));
    int *votesec_list = tor_malloc(n_votes * sizeof(int));
    int *distsec_list = tor_malloc(n_votes * sizeof(int));
    int n_versioning_clients = 0, n_versioning_servers = 0;
    smartlist_t *combined_client_versions = smartlist_create();
    smartlist_t *combined_server_versions = smartlist_create();
    int j;
    SMARTLIST_FOREACH(votes, networkstatus_vote_t *, v,
    {
      tor_assert(v->is_vote);
      va_times[v_sl_idx] = v->valid_after;
      fu_times[v_sl_idx] = v->fresh_until;
      vu_times[v_sl_idx] = v->valid_until;
      votesec_list[v_sl_idx] = v->vote_seconds;
      distsec_list[v_sl_idx] = v->dist_seconds;
      if (v->client_versions) {
        smartlist_t *cv = smartlist_create();
        ++n_versioning_clients;
        smartlist_split_string(cv, v->client_versions, ",",
                               SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
        sort_version_list(cv, 1);
        smartlist_add_all(combined_client_versions, cv);
        smartlist_free(cv); /* elements get freed later. */
      }
      if (v->server_versions) {
        smartlist_t *sv = smartlist_create();
        ++n_versioning_servers;
        smartlist_split_string(sv, v->server_versions, ",",
                               SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
        sort_version_list(sv, 1);
        smartlist_add_all(combined_server_versions, sv);
        smartlist_free(sv); /* elements get freed later. */
      }
      SMARTLIST_FOREACH(v->known_flags, const char *, cp,
                        smartlist_add(flags, tor_strdup(cp)));
    });
    valid_after = median_time(va_times, n_votes);
    fresh_until = median_time(fu_times, n_votes);
    valid_until = median_time(vu_times, n_votes);
    vote_seconds = median_int(votesec_list, n_votes);
    dist_seconds = median_int(distsec_list, n_votes);

    tor_assert(valid_after+MIN_VOTE_INTERVAL <= fresh_until);
    tor_assert(fresh_until+MIN_VOTE_INTERVAL <= valid_until);
    tor_assert(vote_seconds >= MIN_VOTE_SECONDS);
    tor_assert(dist_seconds >= MIN_DIST_SECONDS);

    for (j = 0; j < 2; ++j) {
      smartlist_t *lst =
        j ? combined_server_versions : combined_client_versions;
      int min = (j ? n_versioning_servers : n_versioning_clients) / 2;
      smartlist_t *good = smartlist_create();
      char *res;
      sort_version_list(lst, 0);
      get_frequent_members(good, lst, min);
      res = smartlist_join_strings(good, ",", 0, NULL);
      if (j)
        server_versions = res;
      else
        client_versions = res;
      SMARTLIST_FOREACH(lst, char *, cp, tor_free(cp));
      smartlist_free(good);
      smartlist_free(lst);
    }

    smartlist_sort_strings(flags);
    smartlist_uniq_strings(flags);

    tor_free(va_times);
    tor_free(fu_times);
    tor_free(vu_times);
    tor_free(votesec_list);
    tor_free(distsec_list);
  }

  chunks = smartlist_create();

  {
    char buf[1024];
    char va_buf[ISO_TIME_LEN+1], fu_buf[ISO_TIME_LEN+1],
      vu_buf[ISO_TIME_LEN+1];
    char *flaglist;
    format_iso_time(va_buf, valid_after);
    format_iso_time(fu_buf, fresh_until);
    format_iso_time(vu_buf, valid_until);
    flaglist = smartlist_join_strings(flags, " ", 0, NULL);

    tor_snprintf(buf, sizeof(buf),
                 "network-status-version 3\n"
                 "vote-status consensus\n"
                 "valid-after %s\n"
                 "fresh-until %s\n"
                 "valid-until %s\n"
                 "voting-delay %d %d\n"
                 "client-versions %s\n"
                 "server-versions %s\n"
                 "known-flags %s\n",
                 va_buf, fu_buf, vu_buf,
                 vote_seconds, dist_seconds,
                 client_versions, server_versions, flaglist);
    smartlist_add(chunks, tor_strdup(buf));

    tor_free(flaglist);
  }

  /* Sort the votes. */
  smartlist_sort(votes, _compare_votes_by_authority_id);
  /* Add the authority sections. */
  SMARTLIST_FOREACH(votes, networkstatus_vote_t *, v,
  {
    char buf[1024];
    struct in_addr in;
    char ip[INET_NTOA_BUF_LEN];
    char fingerprint[HEX_DIGEST_LEN+1];
    char votedigest[HEX_DIGEST_LEN+1];
    networkstatus_voter_info_t *voter = get_voter(v);

    in.s_addr = htonl(voter->addr);
    tor_inet_ntoa(&in, ip, sizeof(ip));
    base16_encode(fingerprint, sizeof(fingerprint), voter->identity_digest,
                  DIGEST_LEN);
    base16_encode(votedigest, sizeof(votedigest), voter->vote_digest,
                  DIGEST_LEN);

    tor_snprintf(buf, sizeof(buf),
                 "dir-source %s %s %s %s %d %d\n"
                 "contact %s\n"
                 "vote-digest %s\n",
                 voter->nickname, fingerprint, voter->address, ip,
                    voter->dir_port,
                    voter->or_port,
                 voter->contact,
                 votedigest);
    smartlist_add(chunks, tor_strdup(buf));
  });

  /* Add the actual router entries. */
  {
    int *index; /* index[j] is the current index into votes[j]. */
    int *size; /* size[j] is the number of routerstatuses in votes[j]. */
    int *flag_counts; /* The number of voters that list flag[j] for the
                       * currently considered router. */
    int i;
    smartlist_t *matching_descs = smartlist_create();
    smartlist_t *chosen_flags = smartlist_create();
    smartlist_t *versions = smartlist_create();

    int *n_voter_flags; /* n_voter_flags[j] is the number of flags that
                         * votes[j] knows about. */
    int *n_flag_voters; /* n_flag_voters[f] is the number of votes that care
                         * about flags[f]. */
    int **flag_map; /* flag_map[j][b] is an index f such that flag_map[f]
                     * is the same flag as votes[j]->known_flags[b]. */
    int *named_flag; /* Index of the flag "Named" for votes[j] */

    index = tor_malloc_zero(sizeof(int)*smartlist_len(votes));
    size = tor_malloc_zero(sizeof(int)*smartlist_len(votes));
    n_voter_flags = tor_malloc_zero(sizeof(int) * smartlist_len(votes));
    n_flag_voters = tor_malloc_zero(sizeof(int) * smartlist_len(flags));
    flag_map = tor_malloc_zero(sizeof(int*) * smartlist_len(votes));
    named_flag = tor_malloc_zero(sizeof(int*) * smartlist_len(votes));
    for (i = 0; i < smartlist_len(votes); ++i)
      named_flag[i] = -1;
    SMARTLIST_FOREACH(votes, networkstatus_vote_t *, v,
    {
      flag_map[v_sl_idx] = tor_malloc_zero(
                           sizeof(int)*smartlist_len(v->known_flags));
      SMARTLIST_FOREACH(v->known_flags, const char *, fl,
      {
        int p = smartlist_string_pos(flags, fl);
        tor_assert(p >= 0);
        flag_map[v_sl_idx][fl_sl_idx] = p;
        ++n_flag_voters[p];
        if (!strcmp(fl, "Named"))
          named_flag[v_sl_idx] = fl_sl_idx;
      });
      n_voter_flags[v_sl_idx] = smartlist_len(v->known_flags);
      size[v_sl_idx] = smartlist_len(v->routerstatus_list);
    });

    /* Now go through all the votes */
    flag_counts = tor_malloc(sizeof(int) * smartlist_len(flags));
    while (1) {
      vote_routerstatus_t *rs;
      routerstatus_t rs_out;
      const char *lowest_id = NULL;
      const char *chosen_version;
      const char *chosen_name = NULL;
      int naming_conflict = 0;
      int n_listing = 0;
      int i;
      char buf[256];

      /* Of the next-to-be-considered digest in each voter, which is first? */
      SMARTLIST_FOREACH(votes, networkstatus_vote_t *, v, {
        if (index[v_sl_idx] < size[v_sl_idx]) {
          rs = smartlist_get(v->routerstatus_list, index[v_sl_idx]);
          if (!lowest_id ||
              memcmp(rs->status.identity_digest, lowest_id, DIGEST_LEN) < 0)
            lowest_id = rs->status.identity_digest;
        }
      });
      if (!lowest_id) /* we're out of routers. */
        break;

      memset(flag_counts, 0, sizeof(int)*smartlist_len(flags));
      smartlist_clear(matching_descs);
      smartlist_clear(chosen_flags);
      smartlist_clear(versions);

      /* Okay, go through all the entries for this digest. */
      SMARTLIST_FOREACH(votes, networkstatus_vote_t *, v, {
        if (index[v_sl_idx] >= size[v_sl_idx])
          continue; /* out of entries. */
        rs = smartlist_get(v->routerstatus_list, index[v_sl_idx]);
        if (memcmp(rs->status.identity_digest, lowest_id, DIGEST_LEN))
          continue; /* doesn't include this router. */
        /* At this point, we know that we're looking at a routersatus with
         * identity "lowest".
         */
        ++index[v_sl_idx];
        ++n_listing;

        smartlist_add(matching_descs, rs);
        if (rs->version && rs->version[0])
          smartlist_add(versions, rs->version);

        /* Tally up all the flags. */
        for (i = 0; i < n_voter_flags[v_sl_idx]; ++i) {
          if (rs->flags & (U64_LITERAL(1) << i))
            ++flag_counts[flag_map[v_sl_idx][i]];
        }
        if (rs->flags & (U64_LITERAL(1) << named_flag[v_sl_idx])) {
          if (chosen_name && strcmp(chosen_name, rs->status.nickname)) {
            log_notice(LD_DIR, "Conflict on naming for router: %s vs %s",
                       chosen_name, rs->status.nickname);
            naming_conflict = 1;
          }
          chosen_name = rs->status.nickname;
        }

      });

      /* We don't include this router at all unless more than half of
       * the authorities we believe in list it. */
      if (n_listing <= total_authorities/2)
        continue;

      /* Figure out the most popular opinion of what the most recent
       * routerinfo and its contents are. */
      rs = compute_routerstatus_consensus(matching_descs);
      /* Copy bits of that into rs_out. */
      tor_assert(!memcmp(lowest_id, rs->status.identity_digest, DIGEST_LEN));
      memcpy(rs_out.identity_digest, lowest_id, DIGEST_LEN);
      memcpy(rs_out.descriptor_digest, rs->status.descriptor_digest,
             DIGEST_LEN);
      rs_out.addr = rs->status.addr;
      rs_out.published_on = rs->status.published_on;
      rs_out.dir_port = rs->status.dir_port;
      rs_out.or_port = rs->status.or_port;

      if (chosen_name && !naming_conflict) {
        strlcpy(rs_out.nickname, chosen_name, sizeof(rs_out.nickname));
      } else {
        strlcpy(rs_out.nickname, rs->status.nickname, sizeof(rs_out.nickname));
      }

      /* Set the flags. */
      smartlist_add(chosen_flags, (char*)"s"); /* for the start of the line. */
      SMARTLIST_FOREACH(flags, const char *, fl,
      {
        if (strcmp(fl, "Named")) {
          if (flag_counts[fl_sl_idx] > n_flag_voters[fl_sl_idx]/2)
            smartlist_add(chosen_flags, (char*)fl);
        } else {
          if (!naming_conflict && flag_counts[fl_sl_idx])
            smartlist_add(chosen_flags, (char*)"Named");
        }
      });

      /* Pick the version. */
      if (smartlist_len(versions)) {
        sort_version_list(versions, 0);
        chosen_version = get_most_frequent_member(versions);
      } else {
        chosen_version = NULL;
      }

      /* Okay!! Now we can write the descriptor... */
      /*     First line goes into "buf". */
      routerstatus_format_entry(buf, sizeof(buf), &rs_out, NULL, 1);
      smartlist_add(chunks, tor_strdup(buf));
      /*     Second line is all flags.  The "\n" is missing. */
      smartlist_add(chunks,
                    smartlist_join_strings(chosen_flags, " ", 0, NULL));
      /*     Now the version line. */
      if (chosen_version) {
        smartlist_add(chunks, tor_strdup("\nv "));
        smartlist_add(chunks, tor_strdup(chosen_version));
      }
      smartlist_add(chunks, tor_strdup("\n"));

      /* And the loop is over and we move on to the next router */
    }

    tor_free(index);
    tor_free(size);
    tor_free(n_voter_flags);
    tor_free(n_flag_voters);
    for (i = 0; i < smartlist_len(votes); ++i)
      tor_free(flag_map[i]);
    tor_free(flag_map);
    tor_free(flag_counts);
    smartlist_free(matching_descs);
    smartlist_free(chosen_flags);
    smartlist_free(versions);
  }

  /* Add a signature. */
  {
    char digest[DIGEST_LEN];
    char fingerprint[HEX_DIGEST_LEN+1];
    char signing_key_fingerprint[HEX_DIGEST_LEN+1];

    char buf[4096];
    smartlist_add(chunks, tor_strdup("directory-signature "));

    /* Compute the hash of the chunks. */
    hash_list_members(digest, chunks);

    /* Get the fingerprints */
    crypto_pk_get_fingerprint(identity_key, fingerprint, 0);
    crypto_pk_get_fingerprint(signing_key, signing_key_fingerprint, 0);

    /* add the junk that will go at the end of the line. */
    tor_snprintf(buf, sizeof(buf), "%s %s\n", fingerprint,
                 signing_key_fingerprint);
    /* And the signature. */
    if (router_append_dirobj_signature(buf, sizeof(buf), digest,
                                       signing_key)) {
      log_warn(LD_BUG, "Couldn't sign consensus networkstatus.");
      return NULL; /* This leaks, but it should never happen. */
    }
    smartlist_add(chunks, tor_strdup(buf));
  }

  result = smartlist_join_strings(chunks, "", 0, NULL);

  tor_free(client_versions);
  tor_free(server_versions);
  smartlist_free(flags);
  SMARTLIST_FOREACH(chunks, char *, cp, tor_free(cp));
  smartlist_free(chunks);

  {
    networkstatus_vote_t *c;
    if (!(c = networkstatus_parse_vote_from_string(result, NULL, 0))) {
      log_err(LD_BUG,"Generated a networkstatus consensus we couldn't "
              "parse.");
      tor_free(result);
      return NULL;
    }
    networkstatus_vote_free(c);
  }

  return result;
}

/** Check whether the signature on <b>voter</b> is correctly signed by
 * the signing key of <b>cert</b>. Return -1 if <b>cert</b> doesn't match the
 * signing key; otherwise set the good_signature or bad_signature flag on
 * <b>voter</b>, and return 0. */
/* (private; exposed for testing.) */
int
networkstatus_check_voter_signature(networkstatus_vote_t *consensus,
                                    networkstatus_voter_info_t *voter,
                                    authority_cert_t *cert)
{
  char d[DIGEST_LEN];
  char *signed_digest;
  size_t signed_digest_len;
  if (crypto_pk_get_digest(cert->signing_key, d)<0)
    return -1;
  if (memcmp(voter->signing_key_digest, d, DIGEST_LEN))
    return -1;
  signed_digest_len = crypto_pk_keysize(cert->signing_key);
  signed_digest = tor_malloc(signed_digest_len);
  if (crypto_pk_public_checksig(cert->signing_key,
                                signed_digest,
                                voter->signature,
                                voter->signature_len) != DIGEST_LEN ||
      memcmp(signed_digest, consensus->networkstatus_digest, DIGEST_LEN)) {
    log_warn(LD_DIR, "Got a bad signature on a networkstatus vote");
    voter->bad_signature = 1;
  } else {
    voter->good_signature = 1;
  }
  return 0;
}

/** Given a v3 networkstatus consensus in <b>consensus</b>, check every
 * as-yet-unchecked signature on <b>consensus</b>.  Return 1 if there is a
 * signature from every recognized authority on it, 0 if there are
 * enough good signatures from recognized authorities on it, -1 if we might
 * get enough good signatures by fetching missing certificates, and -2
 * otherwise.  Log messages at INFO or WARN: if <b>warn</b> is over 1, warn
 * about every problem; if warn is at least 1, warn only if we can't get
 * enough signatures; if warn is negative, log nothing at all. */
int
networkstatus_check_consensus_signature(networkstatus_vote_t *consensus,
                                        int warn)
{
  int n_good = 0;
  int n_missing_key = 0;
  int n_bad = 0;
  int n_unknown = 0;
  int n_no_signature = 0;
  int n_v3_authorities = get_n_authorities(V3_AUTHORITY);
  int n_required = n_v3_authorities/2 + 1;
  smartlist_t *need_certs_from = smartlist_create();
  smartlist_t *unrecognized = smartlist_create();
  smartlist_t *missing_authorities = smartlist_create();
  int severity;

  tor_assert(! consensus->is_vote);

  SMARTLIST_FOREACH(consensus->voters, networkstatus_voter_info_t *, voter,
  {
    if (!voter->good_signature && !voter->bad_signature && voter->signature) {
      /* we can try to check the signature. */
      authority_cert_t *cert =
        authority_cert_get_by_digests(voter->identity_digest,
                                      voter->signing_key_digest);
      if (! cert) {
        if (!trusteddirserver_get_by_v3_auth_digest(voter->identity_digest)) {
          smartlist_add(unrecognized, voter);
          ++n_unknown;
        } else {
          smartlist_add(need_certs_from, voter);
          ++n_missing_key;
        }
        continue;
      }
      if (networkstatus_check_voter_signature(consensus, voter, cert) < 0) {
        smartlist_add(need_certs_from, voter);
        ++n_missing_key;
        continue;
      }
    }
    if (voter->good_signature)
      ++n_good;
    else if (voter->bad_signature)
      ++n_bad;
    else
      ++n_no_signature;
  });

  /* Now see whether we're missing any voters entirely. */
  SMARTLIST_FOREACH(router_get_trusted_dir_servers(),
                    trusted_dir_server_t *, ds,
    {
      if ((ds->type & V3_AUTHORITY) &&
          !networkstatus_get_voter_by_id(consensus, ds->v3_identity_digest))
        smartlist_add(missing_authorities, ds);
    });

  if (warn > 1 || (warn >= 0 && n_good < n_required))
    severity = LOG_WARN;
  else
    severity = LOG_INFO;

  if (warn >= 0) {
    SMARTLIST_FOREACH(unrecognized, networkstatus_voter_info_t *, voter,
      {
        log(severity, LD_DIR, "Consensus includes unrecognized authority '%s' "
            "at %s:%d (contact %s; identity %s)",
            voter->nickname, voter->address, (int)voter->dir_port,
            voter->contact?voter->contact:"n/a",
            hex_str(voter->identity_digest, DIGEST_LEN));
      });
    SMARTLIST_FOREACH(need_certs_from, networkstatus_voter_info_t *, voter,
      {
        log_info(LD_DIR, "Looks like we need to download a new certificate "
                 "from authority '%s' at %s:%d (contact %s; identity %s)",
                 voter->nickname, voter->address, (int)voter->dir_port,
                 voter->contact?voter->contact:"n/a",
                 hex_str(voter->identity_digest, DIGEST_LEN));
      });
    SMARTLIST_FOREACH(missing_authorities, trusted_dir_server_t *, ds,
      {
        log(severity, LD_DIR, "Consensus does not include configured "
            "authority '%s' at %s:%d (identity %s)",
            ds->nickname, ds->address, (int)ds->dir_port,
            hex_str(ds->v3_identity_digest, DIGEST_LEN));
      });
    log(severity, LD_DIR,
        "%d unknown, %d missing key, %d good, %d bad, %d no signature, "
        "%d required", n_unknown, n_missing_key, n_good, n_bad,
        n_no_signature, n_required);
  }

  smartlist_free(unrecognized);
  smartlist_free(need_certs_from);
  smartlist_free(missing_authorities);

  if (n_good == n_v3_authorities)
    return 1;
  else if (n_good >= n_required)
    return 0;
  else if (n_good + n_missing_key >= n_required)
    return -1;
  else
    return -2;
}

/** Given a consensus vote <b>target</b> and a set of detached signatures in
 * <b>sigs</b> that correspond to the same consensus, check whether there are
 * any new signatures in <b>src_voter_list</b> that should be added to
 * <b>target.  (A signature should be added if we have no signature for that
 * voter in <b>target</b> yet, or if we have no verifiable signature and the
 * new signature is verifiable.)  Return the number of signatures added or
 * changed, or -1 if the document signed by <b>sigs</b> isn't the same
 * document as <b>target</b>. */
int
networkstatus_add_detached_signatures(networkstatus_vote_t *target,
                                      ns_detached_signatures_t *sigs)
{
  int r = 0;
  tor_assert(sigs);
  tor_assert(target);
  tor_assert(!target->is_vote);

  /* Are they the same consensus? */
  if (memcmp(target->networkstatus_digest, sigs->networkstatus_digest,
             DIGEST_LEN))
    return -1;

  /* For each voter in src... */
  SMARTLIST_FOREACH(sigs->signatures, networkstatus_voter_info_t *, src_voter,
    {
      networkstatus_voter_info_t *target_voter =
        networkstatus_get_voter_by_id(target, src_voter->identity_digest);
      authority_cert_t *cert;
      /* If the target doesn't know about this voter, then forget it. */
      if (!target_voter)
        continue;

      /* If the target already has a good signature from this voter, then skip
       * this one. */
      if (target_voter->good_signature)
        continue;

      /* Try checking the signature if we haven't already. */
      if (!src_voter->good_signature && !src_voter->bad_signature) {
        cert = authority_cert_get_by_digests(src_voter->identity_digest,
                                             src_voter->signing_key_digest);
        if (cert) {
          networkstatus_check_voter_signature(target, src_voter, cert);
        }
      }
      /* If this signature is good, or we don't have any signature yet,
       * then add it. */
      if (src_voter->good_signature || !target_voter->signature) {
        ++r;
        tor_free(target_voter->signature);
        target_voter->signature =
          tor_memdup(src_voter->signature, src_voter->signature_len);
        memcpy(target_voter->signing_key_digest, src_voter->signing_key_digest,
               DIGEST_LEN);
        target_voter->signature_len = src_voter->signature_len;
        target_voter->good_signature = 1;
        target_voter->bad_signature = 0;
      }
    });

  return r;
}

/** Return a newly allocated string holding the detached-signatures document
 * corresponding to the signatures on <b>consensus</b>. */
char *
networkstatus_get_detached_signatures(networkstatus_vote_t *consensus)
{
  smartlist_t *elements;
  char buf[4096];
  char *result = NULL;
  int n_sigs = 0;
  tor_assert(consensus);
  tor_assert(! consensus->is_vote);

  elements = smartlist_create();

  {
    char va_buf[ISO_TIME_LEN+1], fu_buf[ISO_TIME_LEN+1],
      vu_buf[ISO_TIME_LEN+1];
    char d[HEX_DIGEST_LEN+1];

    base16_encode(d, sizeof(d), consensus->networkstatus_digest, DIGEST_LEN);
    format_iso_time(va_buf, consensus->valid_after);
    format_iso_time(fu_buf, consensus->fresh_until);
    format_iso_time(vu_buf, consensus->valid_until);

    tor_snprintf(buf, sizeof(buf),
                 "consensus-digest %s\n"
                 "valid-after %s\n"
                 "fresh-until %s\n"
                 "valid-until %s\n", d, va_buf, fu_buf, vu_buf);
    smartlist_add(elements, tor_strdup(buf));
  }

  SMARTLIST_FOREACH(consensus->voters, networkstatus_voter_info_t *, v,
    {
      char sk[HEX_DIGEST_LEN+1];
      char id[HEX_DIGEST_LEN+1];
      if (!v->signature || v->bad_signature)
        continue;
      ++n_sigs;
      base16_encode(sk, sizeof(sk), v->signing_key_digest, DIGEST_LEN);
      base16_encode(id, sizeof(id), v->identity_digest, DIGEST_LEN);
      tor_snprintf(buf, sizeof(buf),
                   "directory-signature %s %s\n-----BEGIN SIGNATURE-----\n",
                   id, sk);
      smartlist_add(elements, tor_strdup(buf));
      base64_encode(buf, sizeof(buf), v->signature, v->signature_len);
      strlcat(buf, "-----END SIGNATURE-----\n", sizeof(buf));
      smartlist_add(elements, tor_strdup(buf));
    });

  result = smartlist_join_strings(elements, "", 0, NULL);

  SMARTLIST_FOREACH(elements, char *, cp, tor_free(cp));
  smartlist_free(elements);
  if (!n_sigs)
    tor_free(result);
  return result;
}

/** Release all storage held in <b>s</b>. */
void
ns_detached_signatures_free(ns_detached_signatures_t *s)
{
  if (s->signatures) {
    SMARTLIST_FOREACH(s->signatures, networkstatus_voter_info_t *, v,
      {
        tor_free(v->signature);
        tor_free(v);
      });
    smartlist_free(s->signatures);
  }
  tor_free(s);
}

/* =====
 * Certificate functions
 * ===== */

/** Free storage held in <b>cert</b>. */
void
authority_cert_free(authority_cert_t *cert)
{
  if (!cert)
    return;

  tor_free(cert->cache_info.signed_descriptor_body);
  if (cert->signing_key)
    crypto_free_pk_env(cert->signing_key);
  if (cert->identity_key)
    crypto_free_pk_env(cert->identity_key);

  tor_free(cert);
}

/** Allocate and return a new authority_cert_t with the same contents as
 * <b>cert</b>. */
authority_cert_t *
authority_cert_dup(authority_cert_t *cert)
{
  authority_cert_t *out = tor_malloc(sizeof(authority_cert_t));
  tor_assert(cert);

  memcpy(out, cert, sizeof(authority_cert_t));
  /* Now copy pointed-to things. */
  out->cache_info.signed_descriptor_body =
    tor_strndup(cert->cache_info.signed_descriptor_body,
                cert->cache_info.signed_descriptor_len);
  out->cache_info.saved_location = SAVED_NOWHERE;
  out->identity_key = crypto_pk_dup_key(cert->identity_key);
  out->signing_key = crypto_pk_dup_key(cert->signing_key);

  return out;
}

/* =====
 * Vote scheduling
 * ===== */

/** Set *<b>timing_out</b> to the intervals at which we would like to vote.
 * Note that these aren't the intervals we'll use to vote; they're the ones
 * that we'll vote to use. */
void
dirvote_get_preferred_voting_intervals(vote_timing_t *timing_out)
{
  or_options_t *options = get_options();

  tor_assert(timing_out);

  timing_out->vote_interval = options->V3AuthVotingInterval;
  timing_out->n_intervals_valid = options->V3AuthNIntervalsValid;
  timing_out->vote_delay = options->V3AuthVoteDelay;
  timing_out->dist_delay = options->V3AuthDistDelay;
}

/** Return the start of the next interval of size <b>interval</b> (in seconds)
 * after <b>now</b>.  Midnight always starts a fresh interval, and if the last
 * interval of a day would be truncated to less than half its size, it is
 * rolled into the previous interval. */
time_t
dirvote_get_start_of_next_interval(time_t now, int interval)
{
  struct tm tm;
  time_t midnight_today;
  time_t midnight_tomorrow;
  time_t next;

  tor_gmtime_r(&now, &tm);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;

  midnight_today = tor_timegm(&tm);
  midnight_tomorrow = midnight_today + (24*60*60);

  next = midnight_today + ((now-midnight_today)/interval + 1)*interval;

  /* Intervals never cross midnight. */
  if (next > midnight_tomorrow)
    next = midnight_tomorrow;

  /* If the interval would only last half as long as it's supposed to, then
   * skip over to the next day. */
  if (next + interval/2 > midnight_tomorrow)
    next = midnight_tomorrow;

  return next;
}

/** Scheduling information for a voting interval. */
static struct {
  /** When do we generate and distribute our vote for this interval? */
  time_t voting_starts;
  /** When do we send an HTTP request for any votes that we haven't
   * been posted yet?*/
  time_t fetch_missing_votes;
  /** When do we give up on getting more votes and generate a consensus? */
  time_t voting_ends;
  /** When do we send an HTTP request for any signatures we're expecting to
   * see on the consensus? */
  time_t fetch_missing_signatures;
  /** When do we publish the consensus? */
  time_t interval_starts;
  /** When do we discard old votes and pending detached signatures? */
  time_t discard_old_votes;

  /* True iff we have generated and distributed our vote. */
  int have_voted;
  /* True iff we've requested missing votes. */
  int have_fetched_missing_votes;
  /* True iff we have built a consensus and sent the signatures around. */
  int have_built_consensus;
  /* True iff we've fetched missing signatures. */
  int have_fetched_missing_signatures;
  /* True iff we have published our consensus. */
  int have_published_consensus;
} voting_schedule = {0,0,0,0,0,0,0,0,0,0,0};

/** Set voting_schedule to hold the timing for the next vote we should be
 * doing. */
void
dirvote_recalculate_timing(time_t now)
{
  int interval, vote_delay, dist_delay;
  time_t start;
  time_t end;
  networkstatus_vote_t *consensus = networkstatus_get_live_consensus(now);

  memset(&voting_schedule, 0, sizeof(voting_schedule));

  if (consensus) {
    interval = consensus->fresh_until - consensus->valid_after;
    vote_delay = consensus->vote_seconds;
    dist_delay = consensus->dist_seconds;
  } else {
    interval = 30*60;
    vote_delay = dist_delay = 300;
  }

  tor_assert(interval > 0);

  if (vote_delay + dist_delay > interval/2)
    vote_delay = dist_delay = interval / 4;

  start = voting_schedule.interval_starts =
    dirvote_get_start_of_next_interval(now,interval);
  end = dirvote_get_start_of_next_interval(start+1, interval);

  tor_assert(end > start);

  voting_schedule.fetch_missing_signatures = start - (dist_delay/2);
  voting_schedule.voting_ends = start - dist_delay;
  voting_schedule.fetch_missing_votes = start - dist_delay - (vote_delay/2);
  voting_schedule.voting_starts = start - dist_delay - vote_delay;

  voting_schedule.discard_old_votes = start;
}

/** Entry point: Take whatever voting actions are pending as of <b>now</b>. */
void
dirvote_act(time_t now)
{
  if (!voting_schedule.voting_starts) {
    char *keys = list_v3_auth_ids();
    authority_cert_t *c = get_my_v3_authority_cert();
    log_notice(LD_DIR, "Scheduling voting.  Known authority IDs are %s. "
               "Mine is %s.",
               keys, hex_str(c->cache_info.identity_digest, DIGEST_LEN));
    tor_free(keys);
    dirvote_recalculate_timing(now);
  }
  if (voting_schedule.voting_starts < now && !voting_schedule.have_voted) {
    log_notice(LD_DIR, "Time to vote.");
    dirvote_perform_vote();
    voting_schedule.have_voted = 1;
  }
  if (voting_schedule.fetch_missing_votes < now &&
      !voting_schedule.have_fetched_missing_votes) {
    log_notice(LD_DIR, "Time to fetch any votes that we're missing.");
    dirvote_fetch_missing_votes();
    voting_schedule.have_fetched_missing_votes = 1;
  }
  if (voting_schedule.voting_ends < now &&
      !voting_schedule.have_built_consensus) {
    log_notice(LD_DIR, "Time to compute a consensus.");
    dirvote_compute_consensus();
    /* XXXX020 we will want to try again later if we haven't got enough
     * votes yet. */
    voting_schedule.have_built_consensus = 1;
  }
  if (voting_schedule.fetch_missing_signatures < now &&
      !voting_schedule.have_fetched_missing_signatures) {
    log_notice(LD_DIR, "Time to fetch any signatures that we're missing.");
    dirvote_fetch_missing_signatures();
    voting_schedule.have_fetched_missing_signatures = 1;
  }
  if (voting_schedule.interval_starts < now &&
      !voting_schedule.have_published_consensus) {
    log_notice(LD_DIR, "Time to publish the consensus.");
    dirvote_publish_consensus();
    /* XXXX020 we will want to try again later if we haven't got enough
     * signatures yet. */
    voting_schedule.have_published_consensus = 1;
  }
  if (voting_schedule.discard_old_votes < now) {
    log_notice(LD_DIR, "Time to discard old votes.");
    dirvote_clear_votes(0);
    dirvote_recalculate_timing(now);
  }
}

/** A vote networkstatus_vote_t and its unparsed body: held around so we can
 * use it to generate a consensus (at voting_ends) and so we can serve it to
 * other authorities that might want it. */
typedef struct pending_vote_t {
  cached_dir_t *vote_body;
  networkstatus_vote_t *vote;
} pending_vote_t;

/** List of pending_vote_t for the current vote.  Before we've used them to
 * build a consensus, the votes go here. */
static smartlist_t *pending_vote_list = NULL;
/** List of pending_vote_t for the previous vote.  After we've used them to
 * build a consensus, the votes go here for the next period. */
static smartlist_t *previous_vote_list = NULL;
/** The body of the consensus that we're currently building.  Once we
 * have it built, it goes into dirserv.c */
static char *pending_consensus_body = NULL;
/** The detached signatures for the consensus that we're currently
 * building. */
static char *pending_consensus_signatures = NULL;
/** The parsed in-progress consensus document. */
static networkstatus_vote_t *pending_consensus = NULL;
/** List of ns_detached_signatures_t: hold signatures that get posted to us
 * before we have generated the consensus on our own. */
static smartlist_t *pending_consensus_signature_list = NULL;

/** Generate a networkstatus vote and post it to all the v3 authorities.
 * (V3 Authority only) */
static void
dirvote_perform_vote(void)
{
  cached_dir_t *new_vote = generate_v3_networkstatus();
  pending_vote_t *pending_vote;
  int status;
  const char *msg = "";

  if (!new_vote)
    return;

  if (!(pending_vote = dirvote_add_vote(new_vote->dir, &msg, &status))) {
    log_warn(LD_DIR, "Couldn't store my own vote! (I told myself, '%s'.)",
             msg);
    return;
  }

  directory_post_to_dirservers(DIR_PURPOSE_UPLOAD_VOTE,
                               ROUTER_PURPOSE_GENERAL,
                               V3_AUTHORITY,
                               pending_vote->vote_body->dir,
                               pending_vote->vote_body->dir_len, 0);
  log_notice(LD_DIR, "Vote posted.");
}

/** Send an HTTP request to every other v3 authority, for the votes of every
 * authority for which we haven't received a vote yet in this period. (V3
 * authority only) */
static void
dirvote_fetch_missing_votes(void)
{
  smartlist_t *missing_fps = smartlist_create();
  char *resource;

  SMARTLIST_FOREACH(router_get_trusted_dir_servers(),
                    trusted_dir_server_t *, ds,
    {
      if (!(ds->type & V3_AUTHORITY))
        continue;
      if (!dirvote_get_vote(ds->v3_identity_digest,
                            DGV_BY_ID|DGV_INCLUDE_PENDING)) {
        char *cp = tor_malloc(HEX_DIGEST_LEN+1);
        base16_encode(cp, HEX_DIGEST_LEN+1, ds->v3_identity_digest,
                      DIGEST_LEN);
        smartlist_add(missing_fps, cp);
      }
    });

  if (!smartlist_len(missing_fps)) {
    smartlist_free(missing_fps);
    return;
  }
  log_notice(LOG_NOTICE, "We're missing votes from %d authorities. Asking "
             "every other authority for a copy.", smartlist_len(missing_fps));
  resource = smartlist_join_strings(missing_fps, "+", 0, NULL);
  directory_get_from_all_authorities(DIR_PURPOSE_FETCH_STATUS_VOTE,
                                     0, resource);
  tor_free(resource);
  SMARTLIST_FOREACH(missing_fps, char *, cp, tor_free(cp));
  smartlist_free(missing_fps);
}

/** Send a request to every other authority for its detached signatures,
 * unless we have signatures from all other v3 authorities already. */
static void
dirvote_fetch_missing_signatures(void)
{
  if (!pending_consensus)
    return;
  if (networkstatus_check_consensus_signature(pending_consensus, -1) == 1)
    return; /* we have a signature from everybody. */

  directory_get_from_all_authorities(DIR_PURPOSE_FETCH_DETACHED_SIGNATURES,
                                     0, NULL);
}

/** Drop all currently pending votes, consensus, and detached signatures. */
static void
dirvote_clear_votes(int all_votes)
{
  if (!previous_vote_list)
    previous_vote_list = smartlist_create();
  if (!pending_vote_list)
    pending_vote_list = smartlist_create();

  /* All "previous" votes are now junk. */
  SMARTLIST_FOREACH(previous_vote_list, pending_vote_t *, v, {
      cached_dir_decref(v->vote_body);
      v->vote_body = NULL;
      networkstatus_vote_free(v->vote);
      tor_free(v);
    });
  smartlist_clear(previous_vote_list);

  if (all_votes) {
    /* If we're dumping all the votes, we delete the pending ones. */
    SMARTLIST_FOREACH(pending_vote_list, pending_vote_t *, v, {
        cached_dir_decref(v->vote_body);
        v->vote_body = NULL;
        networkstatus_vote_free(v->vote);
        tor_free(v);
      });
  } else {
    /* Otherwise, we move them into "previous". */
    smartlist_add_all(previous_vote_list, pending_vote_list);
  }
  smartlist_clear(pending_vote_list);

  if (pending_consensus_signature_list) {
    SMARTLIST_FOREACH(pending_consensus_signature_list, char *, cp,
                      tor_free(cp));
    smartlist_clear(pending_consensus_signature_list);
  }
  tor_free(pending_consensus_body);
  tor_free(pending_consensus_signatures);
  if (pending_consensus) {
    networkstatus_vote_free(pending_consensus);
    pending_consensus = NULL;
  }
}

/** Return a newly allocated string containing the hex-encoded v3 authority
    identity digest of every recognized v3 authority. */
static char *
list_v3_auth_ids(void)
{
  smartlist_t *known_v3_keys = smartlist_create();
  char *keys;
  SMARTLIST_FOREACH(router_get_trusted_dir_servers(),
                    trusted_dir_server_t *, ds,
    if ((ds->type & V3_AUTHORITY) &&
        !tor_digest_is_zero(ds->v3_identity_digest))
      smartlist_add(known_v3_keys,
                    tor_strdup(hex_str(ds->v3_identity_digest, DIGEST_LEN))));
  keys = smartlist_join_strings(known_v3_keys, ", ", 0, NULL);
  SMARTLIST_FOREACH(known_v3_keys, char *, cp, tor_free(cp));
  smartlist_free(known_v3_keys);
  return keys;
}

/** Called when we have received a networkstatus vote in <b>vote_body</b>.
 * Parse and validate it, and on success store it as a pending vote (which we
 * then return).  Return NULL on failure.  Sets *<b>msg_out</b> and
 * *<b>status_out</b> to an HTTP response and status code.  (V3 authority
 * only) */
pending_vote_t *
dirvote_add_vote(const char *vote_body, const char **msg_out, int *status_out)
{
  networkstatus_vote_t *vote;
  networkstatus_voter_info_t *vi;
  trusted_dir_server_t *ds;
  pending_vote_t *pending_vote = NULL;
  const char *end_of_vote = NULL;
  int any_failed = 0;
  tor_assert(vote_body);
  tor_assert(msg_out);
  tor_assert(status_out);

  if (!pending_vote_list)
    pending_vote_list = smartlist_create();
  *status_out = 0;
  *msg_out = NULL;

 again:
  vote = networkstatus_parse_vote_from_string(vote_body, &end_of_vote, 1);
  if (!end_of_vote)
    end_of_vote = vote_body + strlen(vote_body);
  if (!vote) {
    log_warn(LD_DIR, "Couldn't parse vote: length was %d",
             (int)strlen(vote_body));
    *msg_out = "Unable to parse vote";
    goto err;
  }
  tor_assert(smartlist_len(vote->voters) == 1);
  vi = get_voter(vote);
  tor_assert(vi->good_signature == 1);
  ds = trusteddirserver_get_by_v3_auth_digest(vi->identity_digest);
  if (!ds || !(ds->type & V3_AUTHORITY)) {
    char *keys = list_v3_auth_ids();
    log_warn(LD_DIR, "Got a vote from an authority with authority key ID %s. "
             "This authority %s.  Known v3 key IDs are: %s",
             hex_str(vi->identity_digest, DIGEST_LEN),
             ds?"is not recognized":"is recognized, but is not listed as v3",
             keys);
    tor_free(keys);

    *msg_out = "Vote not from a recognized v3 authority";
    goto err;
  }
  tor_assert(vote->cert);
  if (!authority_cert_get_by_digests(vote->cert->cache_info.identity_digest,
                                     vote->cert->signing_key_digest)) {
    /* Hey, it's a new cert! */
    trusted_dirs_load_certs_from_string(
                               vote->cert->cache_info.signed_descriptor_body,
                               0 /* from_store */);
    if (!authority_cert_get_by_digests(vote->cert->cache_info.identity_digest,
                                       vote->cert->signing_key_digest)) {
      log_warn(LD_BUG, "We added a cert, but still couldn't find it.");
    }
  }

  /* Is it for the right period? */
  if (vote->valid_after != voting_schedule.interval_starts) {
    char tbuf1[ISO_TIME_LEN+1], tbuf2[ISO_TIME_LEN+1];
    format_iso_time(tbuf1, vote->valid_after);
    format_iso_time(tbuf2, voting_schedule.interval_starts);
    log_warn(LD_DIR, "Rejecting vote with valid-after time of %s; we were "
             "expecting %s", tbuf1, tbuf2);
    *msg_out = "Bad valid-after time";
    goto err;
  }

  /* Now see whether we already h<ave a vote from this authority.*/
  SMARTLIST_FOREACH(pending_vote_list, pending_vote_t *, v, {
      if (! memcmp(v->vote->cert->cache_info.identity_digest,
                   vote->cert->cache_info.identity_digest,
                   DIGEST_LEN)) {
        networkstatus_voter_info_t *vi_old = get_voter(v->vote);
        if (!memcmp(vi_old->vote_digest, vi->vote_digest, DIGEST_LEN)) {
          /* Ah, it's the same vote. Not a problem. */
          log_info(LD_DIR, "Discarding a vote we already have.");
          if (*status_out < 200)
            *status_out = 200;
          if (!*msg_out)
            *msg_out = "OK";
          goto discard;
        } else if (v->vote->published < vote->published) {
          log_notice(LD_DIR, "Replacing an older pending vote from this "
                     "directory.");
          cached_dir_decref(v->vote_body);
          networkstatus_vote_free(v->vote);
          v->vote_body = new_cached_dir(tor_strndup(vote_body,
                                                    end_of_vote-vote_body),
                                        vote->published);
          v->vote = vote;
          if (end_of_vote &&
              !strcmpstart(end_of_vote, "network-status-version"))
            goto again;

          if (*status_out < 200)
            *status_out = 200;
          if (!*msg_out)
            *msg_out = "OK";
          return v;
        } else {
          *msg_out = "Already have a newer pending vote";
          goto err;
        }
      }
    });

  pending_vote = tor_malloc_zero(sizeof(pending_vote_t));
  pending_vote->vote_body = new_cached_dir(tor_strndup(vote_body,
                                                       end_of_vote-vote_body),
                                           vote->published);
  pending_vote->vote = vote;
  smartlist_add(pending_vote_list, pending_vote);

  if (!strcmpstart(end_of_vote, "network-status-version ")) {
    vote_body = end_of_vote;
    goto again;
  }

  goto done;

 err:
  any_failed = 1;
  if (!*msg_out)
    *msg_out = "Error adding vote";
  if (*status_out < 400)
    *status_out = 400;

 discard:
  if (vote)
    networkstatus_vote_free(vote);

  if (end_of_vote && !strcmpstart(end_of_vote, "network-status-version ")) {
    vote_body = end_of_vote;
    goto again;
  }

 done:

  if (*status_out < 200)
    *status_out = 200;
  if (!*msg_out)
    *msg_out = "ok";

  return any_failed ? NULL : pending_vote;
}

/** Try to compute a v3 networkstatus consensus from the currently pending
 * votes.  Return 0 on success, -1 on failure.  Store the consensus in
 * pending_consensus: it won't be ready to be published until we have
 * everybody else's signatures collected too. (V3 Authoritity only) */
static int
dirvote_compute_consensus(void)
{
  /* Have we got enough votes to try? */
  int n_votes, n_voters;
  smartlist_t *votes = NULL;
  char *consensus_body = NULL, *signatures = NULL;
  networkstatus_vote_t *consensus = NULL;
  authority_cert_t *my_cert;

  if (!pending_vote_list)
    pending_vote_list = smartlist_create();

  n_voters = get_n_authorities(V3_AUTHORITY);
  n_votes = smartlist_len(pending_vote_list);
  if (n_votes <= n_voters/2) {
    log_warn(LD_DIR, "We don't have enough votes to generate a consensus.");
    goto err;
  }

  if (!(my_cert = get_my_v3_authority_cert())) {
    log_warn(LD_DIR, "Can't generate consensus without a certificate.");
    goto err;
  }

  votes = smartlist_create();
  SMARTLIST_FOREACH(pending_vote_list, pending_vote_t *, v,
                    smartlist_add(votes, v->vote));

  consensus_body = networkstatus_compute_consensus(
        votes, n_voters,
        my_cert->identity_key,
        get_my_v3_authority_signing_key());
  if (!consensus_body) {
    log_warn(LD_DIR, "Couldn't generate a consensus at all!");
    goto err;
  }
  consensus = networkstatus_parse_vote_from_string(consensus_body, NULL, 0);
  if (!consensus) {
    log_warn(LD_DIR, "Couldn't parse consensus we generated!");
    goto err;
  }
  /* 'Check' our own signature, to mark it valid. */
  networkstatus_check_consensus_signature(consensus, -1);

  signatures = networkstatus_get_detached_signatures(consensus);
  if (!signatures) {
    log_warn(LD_DIR, "Couldn't extract signatures.");
    goto err;
  }

  tor_free(pending_consensus_body);
  pending_consensus_body = consensus_body;
  tor_free(pending_consensus_signatures);
  pending_consensus_signatures = signatures;

  if (pending_consensus)
    networkstatus_vote_free(pending_consensus);
  pending_consensus = consensus;

  if (pending_consensus_signature_list) {
    int n_sigs = 0;
    /* we may have gotten signatures for this consensus before we built
     * it ourself.  Add them now. */
    SMARTLIST_FOREACH(pending_consensus_signature_list, char *, sig,
      {
        const char *msg = NULL;
        n_sigs += dirvote_add_signatures_to_pending_consensus(sig, &msg);
        tor_free(sig);
      });
    if (n_sigs)
      log_notice(LD_DIR, "Added %d pending signatures while building "
                 "consensus.", n_sigs);
    smartlist_clear(pending_consensus_signature_list);
  }

  log_notice(LD_DIR, "Consensus computed; uploading signature(s)");

  directory_post_to_dirservers(DIR_PURPOSE_UPLOAD_SIGNATURES,
                               ROUTER_PURPOSE_GENERAL,
                               V3_AUTHORITY,
                               pending_consensus_signatures,
                               strlen(pending_consensus_signatures), 0);
  log_notice(LD_DIR, "Signature(s) posted.");

  return 0;
 err:
  if (votes)
    smartlist_free(votes);
  tor_free(consensus_body);
  tor_free(signatures);
  networkstatus_vote_free(consensus);

  return -1;
}

/** Helper: we just got the <b>deteached_signatures_body</b> sent to us as
 * signatures on the currently pending consensus.  Add them to the consensus
 * as appropriate.  Return the number of signatures added. (?) */
static int
dirvote_add_signatures_to_pending_consensus(
                       const char *detached_signatures_body,
                       const char **msg_out)
{
  ns_detached_signatures_t *sigs = NULL;
  int r = -1;

  tor_assert(detached_signatures_body);
  tor_assert(msg_out);

  /* Only call if we have a pending consensus right now. */
  tor_assert(pending_consensus);
  tor_assert(pending_consensus_body);
  tor_assert(pending_consensus_signatures);

  *msg_out = NULL;

  if (!(sigs = networkstatus_parse_detached_signatures(
                               detached_signatures_body, NULL))) {
    *msg_out = "Couldn't parse detached signatures.";
    goto err;
  }

  r = networkstatus_add_detached_signatures(pending_consensus,
                                            sigs);

  if (r >= 0) {
    char *new_detached =
      networkstatus_get_detached_signatures(pending_consensus);
    const char *src;
    char *dst;
    size_t new_consensus_len =
      strlen(pending_consensus_body) + strlen(new_detached) + 1;
    pending_consensus_body = tor_realloc(pending_consensus_body,
                                         new_consensus_len);
    dst = strstr(pending_consensus_body, "directory-signature ");
    tor_assert(dst);
    src = strstr(new_detached, "directory-signature ");
    tor_assert(src);
    strlcpy(dst, src, new_consensus_len - (dst-pending_consensus_body));

    /* XXXX020 remove this block once it has failed to crash for a while. */
    {
      ns_detached_signatures_t *sigs =
        networkstatus_parse_detached_signatures(new_detached, NULL);
      networkstatus_vote_t *v = networkstatus_parse_vote_from_string(
                                             pending_consensus_body, NULL, 0);
      tor_assert(sigs);
      ns_detached_signatures_free(sigs);
      tor_assert(v);
      networkstatus_vote_free(v);
    }
    tor_free(pending_consensus_signatures);
    pending_consensus_signatures = new_detached;
    *msg_out = "Signatures added";
  } else {
    *msg_out = "Digest mismatch when adding detached signatures";
  }

  goto done;
 err:
  if (!msg_out)
    *msg_out = "Unrecognized error while adding detached signatures.";
 done:
  if (sigs)
    ns_detached_signatures_free(sigs);
  return r;
}

/** Helper: we just got the <b>deteached_signatures_body</b> sent to us as
 * signatures on the currently pending consensus.  Add them to the pending
 * consensus (if we have one); otherwise queue them until we have a
 * consensus.  Return negative on failure, nonnegative on success. */
int
dirvote_add_signatures(const char *detached_signatures_body,
                       const char **msg)
{
  if (pending_consensus) {
    log_notice(LD_DIR, "Got a signature. Adding it to the pending consensus.");
    return dirvote_add_signatures_to_pending_consensus(
                                     detached_signatures_body, msg);
  } else {
    log_notice(LD_DIR, "Got a signature. Queueing it for the next consensus.");
    if (!pending_consensus_signature_list)
      pending_consensus_signature_list = smartlist_create();
    smartlist_add(pending_consensus_signature_list,
                  tor_strdup(detached_signatures_body));
    *msg = "Signature queued";
    return 0;
  }
}

/** Replace the consensus that we're currently serving with the one that we've
 * been building. (V3 Authority only) */
static int
dirvote_publish_consensus(void)
{
  /* Can we actually publish it yet? */
  if (!pending_consensus ||
      networkstatus_check_consensus_signature(pending_consensus, 1)<0) {
    log_warn(LD_DIR, "Not enough info to publish pending consensus");
    return -1;
  }

  if (networkstatus_set_current_consensus(pending_consensus_body, 0, 0))
    log_warn(LD_DIR, "Error publishing consensus");
  else
    log_notice(LD_DIR, "Consensus published.");

  return 0;
}

/** Release all static storage held in dirvote.c */
void
dirvote_free_all(void)
{
  dirvote_clear_votes(1);
  /* now empty as a result of clear_pending_votes. */
  smartlist_free(pending_vote_list);
  pending_vote_list = NULL;
  smartlist_free(previous_vote_list);
  previous_vote_list = NULL;

  tor_free(pending_consensus_body);
  tor_free(pending_consensus_signatures);
  if (pending_consensus) {
    networkstatus_vote_free(pending_consensus);
    pending_consensus = NULL;
  }
  if (pending_consensus_signature_list) {
    /* now empty as a result of clear_pending_votes. */
    smartlist_free(pending_consensus_signature_list);
    pending_consensus_signature_list = NULL;
  }
}

/* ====
 * Access to pending items.
 * ==== */

/** Return the body of the consensus that we're currently trying to build. */
const char *
dirvote_get_pending_consensus(void)
{
  return pending_consensus_body;
}

/** Return the signatures that we know for the consensus that we're currently
 * trying to build */
const char *
dirvote_get_pending_detached_signatures(void)
{
  return pending_consensus_signatures;
}

/** Return a given vote specified by <b>fp</b>.  If <b>by_id</b>, return the
 * vote for the authority with the v3 authority identity key digest <b>fp</b>;
 * if <b>by_id</b> is false, return the vote whose digest is <b>fp</b>.  If
 * <b>fp</b> is NULL, return our own vote.  If <b>include_previous</b> is
 * false, do not consider any votes for a consensus that's already been built.
 * If <b>include_pending</b> is false, do not consider any votes for the
 * consensus that's in progress.  May return NULL if we have no vote for the
 * authority in question. */
const cached_dir_t *
dirvote_get_vote(const char *fp, int flags)
{
  int by_id = flags & DGV_BY_ID;
  const int include_pending = flags & DGV_INCLUDE_PENDING;
  const int include_previous = flags & DGV_INCLUDE_PREVIOUS;

  if (!pending_vote_list && !previous_vote_list)
    return NULL;
  if (fp == NULL) {
    authority_cert_t *c = get_my_v3_authority_cert();
    if (c) {
      fp = c->cache_info.identity_digest;
      by_id = 1;
    } else
      return NULL;
  }
  if (by_id) {
    if (pending_vote_list && include_pending) {
      SMARTLIST_FOREACH(pending_vote_list, pending_vote_t *, pv,
        if (!memcmp(get_voter(pv->vote)->identity_digest, fp, DIGEST_LEN))
          return pv->vote_body);
    }
    if (previous_vote_list && include_previous) {
      SMARTLIST_FOREACH(previous_vote_list, pending_vote_t *, pv,
        if (!memcmp(get_voter(pv->vote)->identity_digest, fp, DIGEST_LEN))
          return pv->vote_body);
    }
  } else {
    if (pending_vote_list && include_pending) {
      SMARTLIST_FOREACH(pending_vote_list, pending_vote_t *, pv,
        if (!memcmp(pv->vote->networkstatus_digest, fp, DIGEST_LEN))
          return pv->vote_body);
    }
    if (previous_vote_list && include_previous) {
      SMARTLIST_FOREACH(previous_vote_list, pending_vote_t *, pv,
        if (!memcmp(pv->vote->networkstatus_digest, fp, DIGEST_LEN))
          return pv->vote_body);
    }
  }
  return NULL;
}

