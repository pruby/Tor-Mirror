/* Copyright 2001-2004 Roger Dingledine.
 * Copyright 2004-2007 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char dirvote_c_id[] =
  "$Id$";

#include "or.h"

/**
 * \file dirvote.c
 **/

/** DOCDOC */
void
networkstatus_vote_free(networkstatus_vote_t *ns)
{
  int i;
  if (!ns)
    return;

  tor_free(ns->client_versions);
  tor_free(ns->server_versions);
  if (ns->known_flags) {
    for (i=0; ns->known_flags[i]; ++i)
      tor_free(ns->known_flags[i]);
    tor_free(ns->known_flags);
  }
  tor_free(ns->nickname);
  tor_free(ns->address);
  tor_free(ns->contact);
  if (ns->cert)
    authority_cert_free(ns->cert);

  if (ns->routerstatus_list) {
    SMARTLIST_FOREACH(ns->routerstatus_list, vote_routerstatus_t *, rs,
    {
      tor_free(rs->version);
      tor_free(rs);
    });

    smartlist_free(ns->routerstatus_list);
  }

  memset(ns, 11, sizeof(*ns));
  tor_free(ns);
}

/** DOCDOC */
static int
_compare_times(const void **_a, const void **_b)
{
  const time_t *a = *_a, *b = *_b;
  if (*a<*b)
    return -1;
  else if (*a>*b)
    return 1;
  else
    return 0;
}

/** DOCDOC */
static int
_compare_ints(const void **_a, const void **_b)
{
  const int *a = *_a, *b = *_b;
  if (*a<*b)
    return -1;
  else if (*a>*b)
    return 1;
  else
    return 0;
}

/** DOCDOC */
static time_t
median_time(smartlist_t *times)
{
  int idx;
  smartlist_sort(times, _compare_times);
  idx = (smartlist_len(times)-1)/2;
  return *(time_t*)smartlist_get(times, idx);
}

/** DOCDOC */
static int
median_int(smartlist_t *ints)
{
  int idx;
  smartlist_sort(ints, _compare_ints);
  idx = (smartlist_len(ints)-1)/2;
  return *(time_t*)smartlist_get(ints, idx);
}

/** DOCDOC */
static int
_compare_votes_by_authority_id(const void **_a, const void **_b)
{
  const networkstatus_vote_t *a = *_a, *b = *_b;
  return memcmp(a->identity_digest, b->identity_digest, DIGEST_LEN);
}

/** DOCDOC */
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

/** DOCDOC */
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

/** DOCDOC */
static int
compare_votes(const vote_routerstatus_t *a, const vote_routerstatus_t *b)
{
  int r;
  if ((r = memcmp(a->status.descriptor_digest, b->status.descriptor_digest,
                  DIGEST_LEN)))
    return r;
  if ((r = (b->status.published_on - a->status.published_on)))
    return r;
  if ((r = strcmp(b->status.nickname, a->status.nickname)))
    return r;
  if ((r = (((int)b->status.or_port) - ((int)a->status.or_port))))
    return r;
  if ((r = (((int)b->status.dir_port) - ((int)a->status.dir_port))))
    return r;
  return 0;
}

/** DOCDOC */
static int
_compare_votes(const void **_a, const void **_b)
{
  const vote_routerstatus_t *a = *_a, *b = *_b;
  return compare_votes(a,b);
}

/** DOCDOC */
static vote_routerstatus_t *
compute_routerstatus_consensus(smartlist_t *votes)
{
  vote_routerstatus_t *most = NULL, *cur = NULL;
  int most_n = 0, cur_n = 0;
  time_t most_published = 0;

  smartlist_sort(votes, _compare_votes);
  SMARTLIST_FOREACH(votes, vote_routerstatus_t *, rs,
  {
    if (cur && !compare_votes(cur, rs)) {
      ++cur_n;
    } else {
      if (cur_n > most_n ||
          (cur && cur_n == most_n && cur->status.published_on > most_published)) {
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

/** DOCDOC */
static void
hash_list_members(char *digest_out, smartlist_t *lst)
{
  crypto_digest_env_t *d = crypto_new_digest_env();
  SMARTLIST_FOREACH(lst, const char *, cp,
                    crypto_digest_add_bytes(d, cp, strlen(cp)));
  crypto_digest_get_digest(d, digest_out, DIGEST_LEN);
  crypto_free_digest_env(d);
}

/** DOCDOC */
char *
networkstatus_compute_consensus(smartlist_t *votes,
                                crypto_pk_env_t *identity_key,
                                crypto_pk_env_t *signing_key)
{
  smartlist_t *chunks;
  char *result = NULL;

  time_t valid_after, fresh_until, valid_until;
  int vote_seconds, dist_seconds;
  char *client_versions = NULL, *server_versions = NULL;
  smartlist_t *flags;
  int total_authorities = smartlist_len(votes); /*XXXX020 not right. */

  if (!smartlist_len(votes)) {
    log_warn(LD_DIR, "Can't compute a consensus from no votes.");
    return NULL;
  }
  /* XXXX020 somebody needs to check vote authority. It could be this
   * function, it could be somebody else. */

  flags = smartlist_create();

  /* Compute medians of time-related things, and figure out how many
   * routers we might need to talk about. */
  {
    smartlist_t *va_times = smartlist_create();
    smartlist_t *fu_times = smartlist_create();
    smartlist_t *vu_times = smartlist_create();
    smartlist_t *votesec_list = smartlist_create();
    smartlist_t *distsec_list = smartlist_create();
    int n_versioning_clients = 0, n_versioning_servers = 0;
    smartlist_t *combined_client_versions = smartlist_create();
    smartlist_t *combined_server_versions = smartlist_create();
    int j;
    SMARTLIST_FOREACH(votes, networkstatus_vote_t *, v,
    {
      smartlist_add(va_times, &v->valid_after);
      smartlist_add(fu_times, &v->fresh_until);
      smartlist_add(vu_times, &v->valid_until);
      smartlist_add(votesec_list, &v->vote_seconds);
      smartlist_add(distsec_list, &v->dist_seconds);
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
      for (j=0; v->known_flags[j]; ++j)
        smartlist_add(flags, tor_strdup(v->known_flags[j]));
    });
    valid_after = median_time(va_times);
    fresh_until = median_time(fu_times);
    valid_until = median_time(vu_times);
    vote_seconds = median_int(votesec_list);
    dist_seconds = median_int(distsec_list);

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

    smartlist_free(va_times);
    smartlist_free(fu_times);
    smartlist_free(vu_times);
    smartlist_free(votesec_list);
    smartlist_free(distsec_list);
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

    in.s_addr = htonl(v->addr);
    tor_inet_ntoa(&in, ip, sizeof(ip));
    base16_encode(fingerprint, sizeof(fingerprint), v->identity_digest,
                  DIGEST_LEN);
    base16_encode(votedigest, sizeof(votedigest), v->vote_digest, DIGEST_LEN);

    tor_snprintf(buf, sizeof(buf),
                 "dir-source %s %s %s %s %d %d\n"
                 "contact %s\n"
                 "vote-digest %s\n",
                 v->nickname, fingerprint, v->address, ip, v->dir_port,
                    v->or_port,
                 v->contact,
                 votedigest);
    smartlist_add(chunks, tor_strdup(buf));
  });

  /* Add the actual router entries. */
  {
    /* document these XXXX020 */
    int *index;
    int *size;
    int *flag_counts;
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
    int *named_flag;

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
      for (i = 0; v->known_flags[i]; ++i) {
        int p = smartlist_string_pos(flags, v->known_flags[i]);
        tor_assert(p >= 0);
        flag_map[v_sl_idx][i] = p;
        ++n_flag_voters[p];
        if (!strcmp(v->known_flags[i], "Named"))
          named_flag[v_sl_idx] = i;
        /* XXXX020 somebody needs to make sure that there are no duplicate
         * entries in anybody's flag list. */
      }
      tor_assert(!v->known_flags[i]);
      n_voter_flags[v_sl_idx] = i;
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
          if (chosen_name && strcmp(chosen_name, rs->status.nickname))
            naming_conflict = 1;
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
        /* XXXX020 fails on very long version string */
        tor_snprintf(buf, sizeof(buf), "\nv %s\n", chosen_version);
        smartlist_add(chunks, tor_strdup(buf));
      } else {
        smartlist_add(chunks, tor_strdup("\n"));
      }

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
    char hex_digest[HEX_DIGEST_LEN+1];
    char buf[4096];
    smartlist_add(chunks, tor_strdup("directory-signature "));

    /* Compute the hash of the chunks. */
    hash_list_members(digest, chunks);

    /* Get hex stuff as needed. */
    base16_encode(hex_digest, sizeof(hex_digest), digest, DIGEST_LEN);
    crypto_pk_get_fingerprint(identity_key, fingerprint, 0);

    /* add the junk that will go at the end of the line. */
    tor_snprintf(buf, sizeof(buf), "%s %s\n", hex_digest, fingerprint);
    /* And the signature. */
    /* XXXX020 check return */
    router_append_dirobj_signature(buf, sizeof(buf), digest, signing_key);
    smartlist_add(chunks, tor_strdup(buf));
  }

  result = smartlist_join_strings(chunks, "", 0, NULL);

  tor_free(client_versions);
  tor_free(server_versions);
  smartlist_free(flags);
  SMARTLIST_FOREACH(chunks, char *, cp, tor_free(cp));
  smartlist_free(chunks);

  return result;
}
