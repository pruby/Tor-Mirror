/* Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2008, The Tor Project, Inc. */
/* See LICENSE for licensing information */
/* $Id$ */
const char policies_c_id[] = \
  "$Id$";

/**
 * \file policies.c
 * \brief Code to parse and use address policies and exit policies.
 **/

#include "or.h"
#include "ht.h"

/** Policy that addresses for incoming SOCKS connections must match. */
static smartlist_t *socks_policy = NULL;
/** Policy that addresses for incoming directory connections must match. */
static smartlist_t *dir_policy = NULL;
/** Policy that addresses for incoming router descriptors must match in order
 * to be published by us. */
static smartlist_t *authdir_reject_policy = NULL;
/** Policy that addresses for incoming router descriptors must match in order
 * to be marked as valid in our networkstatus. */
static smartlist_t *authdir_invalid_policy = NULL;
/** Policy that addresses for incoming router descriptors must <b>not</b>
 * match in order to not be marked as BadDirectory. */
static smartlist_t *authdir_baddir_policy = NULL;
/** Policy that addresses for incoming router descriptors must <b>not</b>
 * match in order to not be marked as BadExit. */
static smartlist_t *authdir_badexit_policy = NULL;

/** Parsed addr_policy_t describing which addresses we believe we can start
 * circuits at. */
static smartlist_t *reachable_or_addr_policy = NULL;
/** Parsed addr_policy_t describing which addresses we believe we can connect
 * to directories at. */
static smartlist_t *reachable_dir_addr_policy = NULL;

/** Replace all "private" entries in *<b>policy</b> with their expanded
 * equivalents. */
void
policy_expand_private(smartlist_t **policy)
{
  static const char *private_nets[] = {
    "0.0.0.0/8", "169.254.0.0/16",
    "127.0.0.0/8", "192.168.0.0/16", "10.0.0.0/8", "172.16.0.0/12",
    // "fc00::/7", "fe80::/10", "fec0::/10", "::/127",
    NULL };
  uint16_t port_min, port_max;

  int i;
  smartlist_t *tmp;

  if (!*policy) /*XXXX021 disallow NULL policies */
    return;

  tmp = smartlist_create();

  SMARTLIST_FOREACH(*policy, addr_policy_t *, p,
  {
     if (! p->is_private) {
       smartlist_add(tmp, p);
       continue;
     }
     for (i = 0; private_nets[i]; ++i) {
       addr_policy_t policy;
       memcpy(&policy, p, sizeof(addr_policy_t));
       policy.is_private = 0;
       policy.is_canonical = 0;
       if (tor_addr_parse_mask_ports(private_nets[i], &policy.addr,
                                  &policy.maskbits, &port_min, &port_max)<0) {
         tor_assert(0);
       }
       smartlist_add(tmp, addr_policy_get_canonical_entry(&policy));
     }
     addr_policy_free(p);
  });

  smartlist_free(*policy);
  *policy = tmp;
}

/**
 * Given a linked list of config lines containing "allow" and "deny"
 * tokens, parse them and append the result to <b>dest</b>. Return -1
 * if any tokens are malformed (and don't append any), else return 0.
 */
static int
parse_addr_policy(config_line_t *cfg, smartlist_t **dest,
                  int assume_action)
{
  smartlist_t *result;
  smartlist_t *entries;
  addr_policy_t *item;
  int r = 0;

  if (!cfg)
    return 0;

  result = smartlist_create();
  entries = smartlist_create();
  for (; cfg; cfg = cfg->next) {
    smartlist_split_string(entries, cfg->value, ",",
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
    SMARTLIST_FOREACH(entries, const char *, ent,
    {
      log_debug(LD_CONFIG,"Adding new entry '%s'",ent);
      item = router_parse_addr_policy_item_from_string(ent, assume_action);
      if (item) {
        smartlist_add(result, item);
      } else {
        log_warn(LD_CONFIG,"Malformed policy '%s'.", ent);
        r = -1;
      }
    });
    SMARTLIST_FOREACH(entries, char *, ent, tor_free(ent));
    smartlist_clear(entries);
  }
  smartlist_free(entries);
  if (r == -1) {
    addr_policy_list_free(result);
  } else {
    policy_expand_private(&result);

    if (*dest) {
      smartlist_add_all(*dest, result);
      smartlist_free(result);
    } else {
      *dest = result;
    }
  }

  return r;
}

/** Helper: parse the Reachable(Dir|OR)?Addresses fields into
 * reachable_(or|dir)_addr_policy.  The options should already have
 * been validated by validate_addr_policies.
 */
static int
parse_reachable_addresses(void)
{
  or_options_t *options = get_options();
  int ret = 0;

  if (options->ReachableDirAddresses &&
      options->ReachableORAddresses &&
      options->ReachableAddresses) {
    log_warn(LD_CONFIG,
             "Both ReachableDirAddresses and ReachableORAddresses are set. "
             "ReachableAddresses setting will be ignored.");
  }
  addr_policy_list_free(reachable_or_addr_policy);
  reachable_or_addr_policy = NULL;
  if (!options->ReachableORAddresses && options->ReachableAddresses)
    log_info(LD_CONFIG,
             "Using ReachableAddresses as ReachableORAddresses.");
  if (parse_addr_policy(options->ReachableORAddresses ?
                          options->ReachableORAddresses :
                          options->ReachableAddresses,
                        &reachable_or_addr_policy, ADDR_POLICY_ACCEPT)) {
    log_warn(LD_CONFIG,
             "Error parsing Reachable%sAddresses entry; ignoring.",
             options->ReachableORAddresses ? "OR" : "");
    ret = -1;
  }

  addr_policy_list_free(reachable_dir_addr_policy);
  reachable_dir_addr_policy = NULL;
  if (!options->ReachableDirAddresses && options->ReachableAddresses)
    log_info(LD_CONFIG,
             "Using ReachableAddresses as ReachableDirAddresses");
  if (parse_addr_policy(options->ReachableDirAddresses ?
                          options->ReachableDirAddresses :
                          options->ReachableAddresses,
                        &reachable_dir_addr_policy, ADDR_POLICY_ACCEPT)) {
    if (options->ReachableDirAddresses)
      log_warn(LD_CONFIG,
               "Error parsing ReachableDirAddresses entry; ignoring.");
    ret = -1;
  }
  return ret;
}

/** Return true iff the firewall options might block any address:port
 * combination.
 */
int
firewall_is_fascist_or(void)
{
  return reachable_or_addr_policy != NULL;
}

/** Return true iff <b>policy</b> (possibly NULL) will allow a
 * connection to <b>addr</b>:<b>port</b>.
 */
static int
addr_policy_permits_tor_addr(const tor_addr_t *addr, uint16_t port,
                            smartlist_t *policy)
{
  addr_policy_result_t p;
  p = compare_tor_addr_to_addr_policy(addr, port, policy);
  switch (p) {
    case ADDR_POLICY_PROBABLY_ACCEPTED:
    case ADDR_POLICY_ACCEPTED:
      return 1;
    case ADDR_POLICY_PROBABLY_REJECTED:
    case ADDR_POLICY_REJECTED:
      return 0;
    default:
      log_warn(LD_BUG, "Unexpected result: %d", (int)p);
      return 0;
  }
}

/* DOCDOC XXXX021 deprecate? */
static int
addr_policy_permits_address(uint32_t addr, uint16_t port,
                            smartlist_t *policy)
{
  tor_addr_t a;
  tor_addr_from_ipv4h(&a, addr);
  return addr_policy_permits_tor_addr(&a, port, policy);
}

/** Return true iff we think our firewall will let us make an OR connection to
 * addr:port. */
int
fascist_firewall_allows_address_or(const tor_addr_t *addr, uint16_t port)
{
  return addr_policy_permits_tor_addr(addr, port,
                                     reachable_or_addr_policy);
}

/** DOCDOC */
int
fascist_firewall_allows_or(routerinfo_t *ri)
{
  /* XXXX021 proposal 118 */
  tor_addr_t addr;
  tor_addr_from_ipv4h(&addr, ri->addr);
  return fascist_firewall_allows_address_or(&addr, ri->or_port);
}

/** Return true iff we think our firewall will let us make a directory
 * connection to addr:port. */
int
fascist_firewall_allows_address_dir(const tor_addr_t *addr, uint16_t port)
{
  return addr_policy_permits_tor_addr(addr, port,
                                      reachable_dir_addr_policy);
}

/** Return 1 if <b>addr</b> is permitted to connect to our dir port,
 * based on <b>dir_policy</b>. Else return 0.
 */
int
dir_policy_permits_address(const tor_addr_t *addr)
{
  return addr_policy_permits_tor_addr(addr, 1, dir_policy);
}

/** Return 1 if <b>addr</b> is permitted to connect to our socks port,
 * based on <b>socks_policy</b>. Else return 0.
 */
int
socks_policy_permits_address(const tor_addr_t *addr)
{
  return addr_policy_permits_tor_addr(addr, 1, socks_policy);
}

/** Return 1 if <b>addr</b>:<b>port</b> is permitted to publish to our
 * directory, based on <b>authdir_reject_policy</b>. Else return 0.
 */
int
authdir_policy_permits_address(uint32_t addr, uint16_t port)
{
  return addr_policy_permits_address(addr, port, authdir_reject_policy);
}

/** Return 1 if <b>addr</b>:<b>port</b> is considered valid in our
 * directory, based on <b>authdir_invalid_policy</b>. Else return 0.
 */
int
authdir_policy_valid_address(uint32_t addr, uint16_t port)
{
  return addr_policy_permits_address(addr, port, authdir_invalid_policy);
}

/** Return 1 if <b>addr</b>:<b>port</b> should be marked as a bad dir,
 * based on <b>authdir_baddir_policy</b>. Else return 0.
 */
int
authdir_policy_baddir_address(uint32_t addr, uint16_t port)
{
  return ! addr_policy_permits_address(addr, port, authdir_baddir_policy);
}

/** Return 1 if <b>addr</b>:<b>port</b> should be marked as a bad exit,
 * based on <b>authdir_badexit_policy</b>. Else return 0.
 */
int
authdir_policy_badexit_address(uint32_t addr, uint16_t port)
{
  return ! addr_policy_permits_address(addr, port, authdir_badexit_policy);
}

#define REJECT(arg) \
  STMT_BEGIN *msg = tor_strdup(arg); goto err; STMT_END

/** Config helper: If there's any problem with the policy configuration
 * options in <b>options</b>, return -1 and set <b>msg</b> to a newly
 * allocated description of the error. Else return 0. */
int
validate_addr_policies(or_options_t *options, char **msg)
{
  /* XXXX Maybe merge this into parse_policies_from_options, to make sure
   * that the two can't go out of sync. */

  smartlist_t *addr_policy=NULL;
  *msg = NULL;

  if (policies_parse_exit_policy(options->ExitPolicy, &addr_policy,
                                 options->ExitPolicyRejectPrivate, NULL))
    REJECT("Error in ExitPolicy entry.");

  /* The rest of these calls *append* to addr_policy. So don't actually
   * use the results for anything other than checking if they parse! */
  if (parse_addr_policy(options->DirPolicy, &addr_policy, -1))
    REJECT("Error in DirPolicy entry.");
  if (parse_addr_policy(options->SocksPolicy, &addr_policy, -1))
    REJECT("Error in SocksPolicy entry.");
  if (parse_addr_policy(options->AuthDirReject, &addr_policy,
                        ADDR_POLICY_REJECT))
    REJECT("Error in AuthDirReject entry.");
  if (parse_addr_policy(options->AuthDirInvalid, &addr_policy,
                        ADDR_POLICY_REJECT))
    REJECT("Error in AuthDirInvalid entry.");
  if (parse_addr_policy(options->AuthDirBadDir, &addr_policy,
                        ADDR_POLICY_REJECT))
    REJECT("Error in AuthDirBadDir entry.");
  if (parse_addr_policy(options->AuthDirBadExit, &addr_policy,
                        ADDR_POLICY_REJECT))
    REJECT("Error in AuthDirBadExit entry.");

  if (parse_addr_policy(options->ReachableAddresses, &addr_policy,
                        ADDR_POLICY_ACCEPT))
    REJECT("Error in ReachableAddresses entry.");
  if (parse_addr_policy(options->ReachableORAddresses, &addr_policy,
                        ADDR_POLICY_ACCEPT))
    REJECT("Error in ReachableORAddresses entry.");
  if (parse_addr_policy(options->ReachableDirAddresses, &addr_policy,
                        ADDR_POLICY_ACCEPT))
    REJECT("Error in ReachableDirAddresses entry.");
  if (parse_addr_policy(options->AuthDirReject, &addr_policy,
                        ADDR_POLICY_REJECT))
    REJECT("Error in AuthDirReject entry.");
  if (parse_addr_policy(options->AuthDirInvalid, &addr_policy,
                        ADDR_POLICY_REJECT))
    REJECT("Error in AuthDirInvalid entry.");

err:
  addr_policy_list_free(addr_policy);
  return *msg ? -1 : 0;
#undef REJECT
}

/** Parse <b>string</b> in the same way that the exit policy
 * is parsed, and put the processed version in *<b>policy</b>.
 * Ignore port specifiers.
 */
static int
load_policy_from_option(config_line_t *config, smartlist_t **policy,
                        int assume_action)
{
  int r;
  addr_policy_list_free(*policy);
  *policy = NULL;
  r = parse_addr_policy(config, policy, assume_action);
  if (r < 0) {
    return -1;
  }
  if (*policy) {
    SMARTLIST_FOREACH(*policy, addr_policy_t *, n, {
        /* ports aren't used. */
        n->prt_min = 1;
        n->prt_max = 65535;
      });
  }
  return 0;
}

/** Set all policies based on <b>options</b>, which should have been validated
 * first by validate_addr_policies. */
int
policies_parse_from_options(or_options_t *options)
{
  int ret = 0;
  if (load_policy_from_option(options->SocksPolicy, &socks_policy, -1) < 0)
    ret = -1;
  if (load_policy_from_option(options->DirPolicy, &dir_policy, -1) < 0)
    ret = -1;
  if (load_policy_from_option(options->AuthDirReject,
                              &authdir_reject_policy, ADDR_POLICY_REJECT) < 0)
    ret = -1;
  if (load_policy_from_option(options->AuthDirInvalid,
                              &authdir_invalid_policy, ADDR_POLICY_REJECT) < 0)
    ret = -1;
  if (load_policy_from_option(options->AuthDirBadDir,
                              &authdir_baddir_policy, ADDR_POLICY_REJECT) < 0)
    ret = -1;
  if (load_policy_from_option(options->AuthDirBadExit,
                              &authdir_badexit_policy, ADDR_POLICY_REJECT) < 0)
    ret = -1;
  if (parse_reachable_addresses() < 0)
    ret = -1;
  return ret;
}

/** Compare two provided address policy items, and return -1, 0, or 1
 * if the first is less than, equal to, or greater than the second. */
static int
cmp_single_addr_policy(addr_policy_t *a, addr_policy_t *b)
{
  int r;
  if ((r=((int)a->policy_type - (int)b->policy_type)))
    return r;
  if ((r=((int)a->is_private - (int)b->is_private)))
    return r;
  if ((r=tor_addr_compare(&a->addr, &b->addr, CMP_EXACT)))
    return r;
  if ((r=((int)a->maskbits - (int)b->maskbits)))
    return r;
  if ((r=((int)a->prt_min - (int)b->prt_min)))
    return r;
  if ((r=((int)a->prt_max - (int)b->prt_max)))
    return r;
  return 0;
}

/** Like cmp_single_addr_policy() above, but looks at the
 * whole set of policies in each case. */
int
cmp_addr_policies(smartlist_t *a, smartlist_t *b)
{
  int r, i;
  int len_a = a ? smartlist_len(a) : 0;
  int len_b = b ? smartlist_len(b) : 0;

  for (i = 0; i < len_a && i < len_b; ++i) {
    if ((r = cmp_single_addr_policy(smartlist_get(a, i), smartlist_get(b, i))))
      return r;
  }
  if (i == len_a && i == len_b)
    return 0;
  if (i < len_a)
    return -1;
  else
    return 1;
}

/** Node in hashtable used to store address policy entries. */
typedef struct policy_map_ent_t {
  HT_ENTRY(policy_map_ent_t) node;
  addr_policy_t *policy;
} policy_map_ent_t;

static HT_HEAD(policy_map, policy_map_ent_t) policy_root = HT_INITIALIZER();

/** Return true iff a and b are equal. */
static INLINE int
policy_eq(policy_map_ent_t *a, policy_map_ent_t *b)
{
  return cmp_single_addr_policy(a->policy, b->policy) == 0;
}

/** Return a hashcode for <b>ent</b> */
static unsigned int
policy_hash(policy_map_ent_t *ent)
{
  addr_policy_t *a = ent->policy;
  unsigned int r;
  if (a->is_private)
    r = 0x1234abcd;
  else
    r = tor_addr_hash(&a->addr);
  r += a->prt_min << 8;
  r += a->prt_max << 16;
  r += a->maskbits;
  if (a->policy_type == ADDR_POLICY_REJECT)
    r ^= 0xffffffff;

  return r;
}

HT_PROTOTYPE(policy_map, policy_map_ent_t, node, policy_hash,
             policy_eq)
HT_GENERATE(policy_map, policy_map_ent_t, node, policy_hash,
            policy_eq, 0.6, malloc, realloc, free)

/** Given a pointer to an addr_policy_t, return a copy of the pointer to the
 * "canonical" copy of that addr_policy_t; the canonical copy is a single
 * reference-counted object. */
addr_policy_t *
addr_policy_get_canonical_entry(addr_policy_t *e)
{
  policy_map_ent_t search, *found;
  if (e->is_canonical)
    return e;

  search.policy = e;
  found = HT_FIND(policy_map, &policy_root, &search);
  if (!found) {
    found = tor_malloc_zero(sizeof(policy_map_ent_t));
    found->policy = tor_memdup(e, sizeof(addr_policy_t));
    found->policy->is_canonical = 1;
    found->policy->refcnt = 0;
    HT_INSERT(policy_map, &policy_root, found);
  }

  tor_assert(!cmp_single_addr_policy(found->policy, e));
  ++found->policy->refcnt;
  return found->policy;
}

/** DOCDOC */
addr_policy_result_t
compare_addr_to_addr_policy(uint32_t addr, uint16_t port, smartlist_t *policy)
{
  /*XXXX021 deprecate this function? */
  tor_addr_t a;
  tor_addr_from_ipv4h(&a, addr);
  return compare_tor_addr_to_addr_policy(&a, port, policy);
}

/** Decide whether a given addr:port is definitely accepted,
 * definitely rejected, probably accepted, or probably rejected by a
 * given policy.  If <b>addr</b> is 0, we don't know the IP of the
 * target address. If <b>port</b> is 0, we don't know the port of the
 * target address.
 *
 * For now, the algorithm is pretty simple: we look for definite and
 * uncertain matches.  The first definite match is what we guess; if
 * it was preceded by no uncertain matches of the opposite policy,
 * then the guess is definite; otherwise it is probable.  (If we
 * have a known addr and port, all matches are definite; if we have an
 * unknown addr/port, any address/port ranges other than "all" are
 * uncertain.)
 *
 * We could do better by assuming that some ranges never match typical
 * addresses (127.0.0.1, and so on).  But we'll try this for now.
 */
addr_policy_result_t
compare_tor_addr_to_addr_policy(const tor_addr_t *addr, uint16_t port,
                                smartlist_t *policy)
{
  int maybe_reject = 0;
  int maybe_accept = 0;
  int match = 0;
  int maybe = 0;
  int i, len;
  int addr_is_unknown;
  addr_is_unknown = tor_addr_is_null(addr);

  len = policy ? smartlist_len(policy) : 0;

  for (i = 0; i < len; ++i) {
    addr_policy_t *tmpe = smartlist_get(policy, i);
    maybe = 0;
    if (addr_is_unknown) {
      /* Address is unknown. */
      if ((port >= tmpe->prt_min && port <= tmpe->prt_max) ||
           (!port && tmpe->prt_min<=1 && tmpe->prt_max>=65535)) {
        /* The port definitely matches. */
        if (tmpe->maskbits == 0) {
          match = 1;
        } else {
          maybe = 1;
        }
      } else if (!port) {
        /* The port maybe matches. */
        maybe = 1;
      }
    } else {
      /* Address is known */
      if (!tor_addr_compare_masked(addr, &tmpe->addr, tmpe->maskbits,
                                   CMP_SEMANTIC)) {
        if (port >= tmpe->prt_min && port <= tmpe->prt_max) {
          /* Exact match for the policy */
          match = 1;
        } else if (!port) {
          maybe = 1;
        }
      }
    }
    if (maybe) {
      if (tmpe->policy_type == ADDR_POLICY_REJECT)
        maybe_reject = 1;
      else
        maybe_accept = 1;
    }
    if (match) {
      if (tmpe->policy_type == ADDR_POLICY_ACCEPT) {
        /* If we already hit a clause that might trigger a 'reject', than we
         * can't be sure of this certain 'accept'.*/
        return maybe_reject ? ADDR_POLICY_PROBABLY_ACCEPTED :
                              ADDR_POLICY_ACCEPTED;
      } else {
        return maybe_accept ? ADDR_POLICY_PROBABLY_REJECTED :
                              ADDR_POLICY_REJECTED;
      }
    }
  }

  /* accept all by default. */
  return maybe_reject ? ADDR_POLICY_PROBABLY_ACCEPTED : ADDR_POLICY_ACCEPTED;
}

/** Return true iff the address policy <b>a</b> covers every case that
 * would be covered by <b>b</b>, so that a,b is redundant. */
static int
addr_policy_covers(addr_policy_t *a, addr_policy_t *b)
{
  /* We can ignore accept/reject, since "accept *:80, reject *:80" reduces
   * to "accept *:80". */
  if (a->maskbits > b->maskbits) {
    /* a has more fixed bits than b; it can't possibly cover b. */
    return 0;
  }
  if (tor_addr_compare_masked(&a->addr, &b->addr, a->maskbits, CMP_SEMANTIC)) {
    /* There's a fixed bit in a that's set differently in b. */
    return 0;
  }
  return (a->prt_min <= b->prt_min && a->prt_max >= b->prt_max);
}

/** Return true iff the address policies <b>a</b> and <b>b</b> intersect,
 * that is, there exists an address/port that is covered by <b>a</b> that
 * is also covered by <b>b</b>.
 */
static int
addr_policy_intersects(addr_policy_t *a, addr_policy_t *b)
{
  maskbits_t minbits;
  /* All the bits we care about are those that are set in both
   * netmasks.  If they are equal in a and b's networkaddresses
   * then the networks intersect.  If there is a difference,
   * then they do not. */
  if (a->maskbits < b->maskbits)
    minbits = a->maskbits;
  else
    minbits = b->maskbits;
  if (tor_addr_compare_masked(&a->addr, &b->addr, minbits, CMP_SEMANTIC))
    return 0;
  if (a->prt_max < b->prt_min || b->prt_max < a->prt_min)
    return 0;
  return 1;
}

/** Add the exit policy described by <b>more</b> to <b>policy</b>.
 */
static void
append_exit_policy_string(smartlist_t **policy, const char *more)
{
  config_line_t tmp;

  tmp.key = NULL;
  tmp.value = (char*) more;
  tmp.next = NULL;
  if (parse_addr_policy(&tmp, policy, -1)<0) {
    log_warn(LD_BUG, "Unable to parse internally generated policy %s",more);
  }
}

/** Detect and excise "dead code" from the policy *<b>dest</b>. */
static void
exit_policy_remove_redundancies(smartlist_t *dest)
{
  addr_policy_t *ap, *tmp, *victim;
  int i, j;

  /* Step one: find a *:* entry and cut off everything after it. */
  for (i = 0; i < smartlist_len(dest); ++i) {
    ap = smartlist_get(dest, i);
    if (ap->maskbits == 0 && ap->prt_min <= 1 && ap->prt_max >= 65535) {
      /* This is a catch-all line -- later lines are unreachable. */
      while (i+1 < smartlist_len(dest)) {
        victim = smartlist_get(dest, i+1);
        smartlist_del(dest, i+1);
        addr_policy_free(victim);
      }
      break;
    }
  }

  /* Step two: for every entry, see if there's a redundant entry
   * later on, and remove it. */
  for (i = 0; i < smartlist_len(dest)-1; ++i) {
    ap = smartlist_get(dest, i);
    for (j = i+1; j < smartlist_len(dest); ++j) {
      tmp = smartlist_get(dest, j);
      tor_assert(j > i);
      if (addr_policy_covers(ap, tmp)) {
        char p1[POLICY_BUF_LEN], p2[POLICY_BUF_LEN];
        policy_write_item(p1, sizeof(p1), tmp, 0);
        policy_write_item(p2, sizeof(p2), ap, 0);
        log(LOG_DEBUG, LD_CONFIG, "Removing exit policy %s (%d).  It is made "
            "redundant by %s (%d).", p1, j, p2, i);
        smartlist_del_keeporder(dest, j--);
        addr_policy_free(tmp);
      }
    }
  }

  /* Step three: for every entry A, see if there's an entry B making this one
   * redundant later on.  This is the case if A and B are of the same type
   * (accept/reject), A is a subset of B, and there is no other entry of
   * different type in between those two that intersects with A.
   *
   * Anybody want to doublecheck the logic here? XXX
   */
  for (i = 0; i < smartlist_len(dest)-1; ++i) {
    ap = smartlist_get(dest, i);
    for (j = i+1; j < smartlist_len(dest); ++j) {
      // tor_assert(j > i); // j starts out at i+1; j only increases; i only
      //                    // decreases.
      tmp = smartlist_get(dest, j);
      if (ap->policy_type != tmp->policy_type) {
        if (addr_policy_intersects(ap, tmp))
          break;
      } else { /* policy_types are equal. */
        if (addr_policy_covers(tmp, ap)) {
          char p1[POLICY_BUF_LEN], p2[POLICY_BUF_LEN];
          policy_write_item(p1, sizeof(p1), ap, 0);
          policy_write_item(p2, sizeof(p2), tmp, 0);
          log(LOG_DEBUG, LD_CONFIG, "Removing exit policy %s.  It is already "
              "covered by %s.", p1, p2);
          smartlist_del_keeporder(dest, i--);
          addr_policy_free(ap);
          break;
        }
      }
    }
  }
}

#define DEFAULT_EXIT_POLICY                                         \
  "reject *:25,reject *:119,reject *:135-139,reject *:445,"         \
  "reject *:465,reject *:563,reject *:587,"                         \
  "reject *:1214,reject *:4661-4666,"                               \
  "reject *:6346-6429,reject *:6699,reject *:6881-6999,accept *:*"

/** Parse the exit policy <b>cfg</b> into the linked list *<b>dest</b>. If
 * cfg doesn't end in an absolute accept or reject, add the default exit
 * policy afterwards. If <b>rejectprivate</b> is true, prepend
 * "reject private:*" to the policy. Return -1 if we can't parse cfg,
 * else return 0.
 */
int
policies_parse_exit_policy(config_line_t *cfg, smartlist_t **dest,
                           int rejectprivate, const char *local_address)
{
  if (rejectprivate) {
    append_exit_policy_string(dest, "reject private:*");
    if (local_address) {
      char buf[POLICY_BUF_LEN];
      tor_snprintf(buf, sizeof(buf), "reject %s:*", local_address);
      append_exit_policy_string(dest, buf);
    }
  }
  if (parse_addr_policy(cfg, dest, -1))
    return -1;
  append_exit_policy_string(dest, DEFAULT_EXIT_POLICY);

  exit_policy_remove_redundancies(*dest);

  return 0;
}

/** Replace the exit policy of <b>r</b> with reject *:*. */
void
policies_set_router_exitpolicy_to_reject_all(routerinfo_t *r)
{
  addr_policy_t *item;
  addr_policy_list_free(r->exit_policy);
  r->exit_policy = smartlist_create();
  item = router_parse_addr_policy_item_from_string("reject *:*", -1);
  smartlist_add(r->exit_policy, item);
}

/** Return true iff <b>ri</b> is "useful as an exit node", meaning
 * it allows exit to at least one /8 address space for at least
 * two of ports 80, 443, and 6667. */
int
exit_policy_is_general_exit(smartlist_t *policy)
{
  static const int ports[] = { 80, 443, 6667 };
  int n_allowed = 0;
  int i;
  if (!policy) /*XXXX021 disallow NULL policies */
    return 0;

  for (i = 0; i < 3; ++i) {
    SMARTLIST_FOREACH(policy, addr_policy_t *, p, {
      if (p->prt_min > ports[i] || p->prt_max < ports[i])
        continue; /* Doesn't cover our port. */
      if (p->maskbits > 8)
        continue; /* Narrower than a /8. */
      if (tor_addr_is_loopback(&p->addr))
        continue; /* 127.x or ::1. */
      /* We have a match that is at least a /8. */
      if (p->policy_type == ADDR_POLICY_ACCEPT) {
        ++n_allowed;
        break; /* stop considering this port */
      }
    });
  }
  return n_allowed >= 2;
}

/** Return false if <b>policy</b> might permit access to some addr:port;
 * otherwise if we are certain it rejects everything, return true. */
int
policy_is_reject_star(smartlist_t *policy)
{
  if (!policy) /*XXXX021 disallow NULL policies */
    return 1;
  SMARTLIST_FOREACH(policy, addr_policy_t *, p, {
    if (p->policy_type == ADDR_POLICY_ACCEPT)
      return 0;
    else if (p->policy_type == ADDR_POLICY_REJECT &&
             p->prt_min <= 1 && p->prt_max == 65535 &&
             p->maskbits == 0)
      return 1;
  });
  return 1;
}

/** Write a single address policy to the buf_len byte buffer at buf.  Return
 * the number of characters written, or -1 on failure. */
int
policy_write_item(char *buf, size_t buflen, addr_policy_t *policy,
                  int format_for_desc)
{
  size_t written = 0;
  char addrbuf[TOR_ADDR_BUF_LEN];
  const char *addrpart;
  int result;
  const int is_accept = policy->policy_type == ADDR_POLICY_ACCEPT;
  const int is_ip6 = tor_addr_family(&policy->addr) == AF_INET6;

  tor_addr_to_str(addrbuf, &policy->addr, sizeof(addrbuf), 1);

  /* write accept/reject 1.2.3.4 */
  if (policy->is_private)
    addrpart = "private";
  else if (policy->maskbits == 0)
    addrpart = "*";
  else
    addrpart = addrbuf;

  result = tor_snprintf(buf, buflen, "%s%s%s %s",
                        (is_ip6&&format_for_desc)?"opt ":"",
                        is_accept ? "accept" : "reject",
                        (is_ip6&&format_for_desc)?"6":"",
                        addrpart);
  if (result < 0)
    return -1;
  written += strlen(buf);
  /* If the maskbits is 32 we don't need to give it.  If the mask is 0,
   * we already wrote "*". */
  if (policy->maskbits < 32 && policy->maskbits > 0) {
    if (tor_snprintf(buf+written, buflen-written, "/%d", policy->maskbits)<0)
      return -1;
    written += strlen(buf+written);
  }
  if (policy->prt_min <= 1 && policy->prt_max == 65535) {
    /* There is no port set; write ":*" */
    if (written+4 > buflen)
      return -1;
    strlcat(buf+written, ":*", buflen-written);
    written += 2;
  } else if (policy->prt_min == policy->prt_max) {
    /* There is only one port; write ":80". */
    result = tor_snprintf(buf+written, buflen-written, ":%d", policy->prt_min);
    if (result<0)
      return -1;
    written += result;
  } else {
    /* There is a range of ports; write ":79-80". */
    result = tor_snprintf(buf+written, buflen-written, ":%d-%d",
                          policy->prt_min, policy->prt_max);
    if (result<0)
      return -1;
    written += result;
  }
  if (written < buflen)
    buf[written] = '\0';
  else
    return -1;

  return (int)written;
}

/** Implementation for GETINFO control command: knows the answer for questions
 * about "exit-policy/..." */
int
getinfo_helper_policies(control_connection_t *conn,
                        const char *question, char **answer)
{
  (void) conn;
  if (!strcmp(question, "exit-policy/default")) {
    *answer = tor_strdup(DEFAULT_EXIT_POLICY);
  }
  return 0;
}

/** Release all storage held by <b>p</b>. */
void
addr_policy_list_free(smartlist_t *lst)
{
  if (!lst) return;
  SMARTLIST_FOREACH(lst, addr_policy_t *, policy, addr_policy_free(policy));
  smartlist_free(lst);
}

/** Release all storage held by <b>p</b>. */
void
addr_policy_free(addr_policy_t *p)
{
  if (p) {
    if (--p->refcnt <= 0) {
      if (p->is_canonical) {
        policy_map_ent_t search, *found;
        search.policy = p;
        found = HT_REMOVE(policy_map, &policy_root, &search);
        if (found) {
          tor_assert(p == found->policy);
          tor_free(found);
        }
      }
      tor_free(p);
    }
  }
}

/** Release all storage held by policy variables. */
void
policies_free_all(void)
{
  addr_policy_list_free(reachable_or_addr_policy);
  reachable_or_addr_policy = NULL;
  addr_policy_list_free(reachable_dir_addr_policy);
  reachable_dir_addr_policy = NULL;
  addr_policy_list_free(socks_policy);
  socks_policy = NULL;
  addr_policy_list_free(dir_policy);
  dir_policy = NULL;
  addr_policy_list_free(authdir_reject_policy);
  authdir_reject_policy = NULL;
  addr_policy_list_free(authdir_invalid_policy);
  authdir_invalid_policy = NULL;
  addr_policy_list_free(authdir_baddir_policy);
  authdir_baddir_policy = NULL;
  addr_policy_list_free(authdir_badexit_policy);
  authdir_badexit_policy = NULL;

  if (!HT_EMPTY(&policy_root))
    log_warn(LD_MM, "Still had some address policies cached at shutdown.");
  HT_CLEAR(policy_map, &policy_root);
}

