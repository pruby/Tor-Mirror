/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2008, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file routerparse.c
 * \brief Code to parse and validate router descriptors and directories.
 **/

#include "or.h"
#include "memarea.h"

/****************************************************************************/

/** Enumeration of possible token types.  The ones starting with K_ correspond
 * to directory 'keywords'. _ERR is an error in the tokenizing process, _EOF
 * is an end-of-file marker, and _NIL is used to encode not-a-token.
 */
typedef enum {
  K_ACCEPT = 0,
  K_ACCEPT6,
  K_DIRECTORY_SIGNATURE,
  K_RECOMMENDED_SOFTWARE,
  K_REJECT,
  K_REJECT6,
  K_ROUTER,
  K_SIGNED_DIRECTORY,
  K_SIGNING_KEY,
  K_ONION_KEY,
  K_ROUTER_SIGNATURE,
  K_PUBLISHED,
  K_RUNNING_ROUTERS,
  K_ROUTER_STATUS,
  K_PLATFORM,
  K_OPT,
  K_BANDWIDTH,
  K_CONTACT,
  K_NETWORK_STATUS,
  K_UPTIME,
  K_DIR_SIGNING_KEY,
  K_FAMILY,
  K_FINGERPRINT,
  K_HIBERNATING,
  K_READ_HISTORY,
  K_WRITE_HISTORY,
  K_NETWORK_STATUS_VERSION,
  K_DIR_SOURCE,
  K_DIR_OPTIONS,
  K_CLIENT_VERSIONS,
  K_SERVER_VERSIONS,
  K_P,
  K_R,
  K_S,
  K_V,
  K_W,
  K_EVENTDNS,
  K_EXTRA_INFO,
  K_EXTRA_INFO_DIGEST,
  K_CACHES_EXTRA_INFO,
  K_HIDDEN_SERVICE_DIR,
  K_ALLOW_SINGLE_HOP_EXITS,

  K_DIR_KEY_CERTIFICATE_VERSION,
  K_DIR_IDENTITY_KEY,
  K_DIR_KEY_PUBLISHED,
  K_DIR_KEY_EXPIRES,
  K_DIR_KEY_CERTIFICATION,
  K_DIR_KEY_CROSSCERT,
  K_DIR_ADDRESS,

  K_VOTE_STATUS,
  K_VALID_AFTER,
  K_FRESH_UNTIL,
  K_VALID_UNTIL,
  K_VOTING_DELAY,

  K_KNOWN_FLAGS,
  K_VOTE_DIGEST,
  K_CONSENSUS_DIGEST,
  K_CONSENSUS_METHODS,
  K_CONSENSUS_METHOD,
  K_LEGACY_DIR_KEY,

  A_PURPOSE,
  _A_UNKNOWN,

  R_RENDEZVOUS_SERVICE_DESCRIPTOR,
  R_VERSION,
  R_PERMANENT_KEY,
  R_SECRET_ID_PART,
  R_PUBLICATION_TIME,
  R_PROTOCOL_VERSIONS,
  R_INTRODUCTION_POINTS,
  R_SIGNATURE,

  R_IPO_IDENTIFIER,
  R_IPO_IP_ADDRESS,
  R_IPO_ONION_PORT,
  R_IPO_ONION_KEY,
  R_IPO_SERVICE_KEY,

  C_CLIENT_NAME,
  C_DESCRIPTOR_COOKIE,
  C_CLIENT_KEY,

  _ERR,
  _EOF,
  _NIL
} directory_keyword;

#define MIN_ANNOTATION A_PURPOSE
#define MAX_ANNOTATION _A_UNKNOWN

/** Structure to hold a single directory token.
 *
 * We parse a directory by breaking it into "tokens", each consisting
 * of a keyword, a line full of arguments, and a binary object.  The
 * arguments and object are both optional, depending on the keyword
 * type.
 *
 * This structure is only allocated in memareas; do not allocate it on
 * the heap, or token_free() won't work.
 */
typedef struct directory_token_t {
  directory_keyword tp;        /**< Type of the token. */
  int n_args:30;               /**< Number of elements in args */
  char **args;                 /**< Array of arguments from keyword line. */

  char *object_type;           /**< -----BEGIN [object_type]-----*/
  size_t object_size;          /**< Bytes in object_body */
  char *object_body;           /**< Contents of object, base64-decoded. */

  crypto_pk_env_t *key;        /**< For public keys only.  Heap-allocated. */

  char *error;                 /**< For _ERR tokens only. */
} directory_token_t;

/* ********************************************************************** */

/** We use a table of rules to decide how to parse each token type. */

/** Rules for whether the keyword needs an object. */
typedef enum {
  NO_OBJ,        /**< No object, ever. */
  NEED_OBJ,      /**< Object is required. */
  NEED_SKEY_1024,/**< Object is required, and must be a 1024 bit private key */
  NEED_KEY_1024, /**< Object is required, and must be a 1024 bit public key */
  NEED_KEY,      /**< Object is required, and must be a public key. */
  OBJ_OK,        /**< Object is optional. */
} obj_syntax;

#define AT_START 1
#define AT_END 2

/** Determines the parsing rules for a single token type. */
typedef struct token_rule_t {
  /** The string value of the keyword identifying the type of item. */
  const char *t;
  /** The corresponding directory_keyword enum. */
  directory_keyword v;
  /** Minimum number of arguments for this item */
  int min_args;
  /** Maximum number of arguments for this item */
  int max_args;
  /** If true, we concatenate all arguments for this item into a single
   * string. */
  int concat_args;
  /** Requirments on object syntax for this item. */
  obj_syntax os;
  /** Lowest number of times this item may appear in a document. */
  int min_cnt;
  /** Highest number of times this item may appear in a document. */
  int max_cnt;
  /** One or more of AT_START/AT_END to limit where the item may appear in a
   * document. */
  int pos;
  /** True iff this token is an annotation. */
  int is_annotation;
} token_rule_t;

/*
 * Helper macros to define token tables.  's' is a string, 't' is a
 * directory_keyword, 'a' is a trio of argument multiplicities, and 'o' is an
 * object syntax.
 *
 */

/** Appears to indicate the end of a table. */
#define END_OF_TABLE { NULL, _NIL, 0,0,0, NO_OBJ, 0, INT_MAX, 0, 0 }
/** An item with no restrictions: used for obsolete document types */
#define T(s,t,a,o)    { s, t, a, o, 0, INT_MAX, 0, 0 }
/** An item with no restrictions on multiplicity or location. */
#define T0N(s,t,a,o)  { s, t, a, o, 0, INT_MAX, 0, 0 }
/** An item that must appear exactly once */
#define T1(s,t,a,o)   { s, t, a, o, 1, 1, 0, 0 }
/** An item that must appear exactly once, at the start of the document */
#define T1_START(s,t,a,o)   { s, t, a, o, 1, 1, AT_START, 0 }
/** An item that must appear exactly once, at the end of the document */
#define T1_END(s,t,a,o)   { s, t, a, o, 1, 1, AT_END, 0 }
/** An item that must appear one or more times */
#define T1N(s,t,a,o)  { s, t, a, o, 1, INT_MAX, 0, 0 }
/** An item that must appear no more than once */
#define T01(s,t,a,o)  { s, t, a, o, 0, 1, 0, 0 }
/** An annotation that must appear no more than once */
#define A01(s,t,a,o)  { s, t, a, o, 0, 1, 0, 1 }

/* Argument multiplicity: any number of arguments. */
#define ARGS        0,INT_MAX,0
/* Argument multiplicity: no arguments. */
#define NO_ARGS     0,0,0
/* Argument multiplicity: concatenate all arguments. */
#define CONCAT_ARGS 1,1,1
/* Argument multiplicity: at least <b>n</b> arguments. */
#define GE(n)       n,INT_MAX,0
/* Argument multiplicity: exactly <b>n</b> arguments. */
#define EQ(n)       n,n,0

/** List of tokens allowable in router derscriptors */
static token_rule_t routerdesc_token_table[] = {
  T0N("reject",              K_REJECT,              ARGS,    NO_OBJ ),
  T0N("accept",              K_ACCEPT,              ARGS,    NO_OBJ ),
  T0N("reject6",             K_REJECT6,             ARGS,    NO_OBJ ),
  T0N("accept6",             K_ACCEPT6,             ARGS,    NO_OBJ ),
  T1_START( "router",        K_ROUTER,              GE(5),   NO_OBJ ),
  T1( "signing-key",         K_SIGNING_KEY,         NO_ARGS, NEED_KEY_1024 ),
  T1( "onion-key",           K_ONION_KEY,           NO_ARGS, NEED_KEY_1024 ),
  T1_END( "router-signature",    K_ROUTER_SIGNATURE,    NO_ARGS, NEED_OBJ ),
  T1( "published",           K_PUBLISHED,       CONCAT_ARGS, NO_OBJ ),
  T01("uptime",              K_UPTIME,              GE(1),   NO_OBJ ),
  T01("fingerprint",         K_FINGERPRINT,     CONCAT_ARGS, NO_OBJ ),
  T01("hibernating",         K_HIBERNATING,         GE(1),   NO_OBJ ),
  T01("platform",            K_PLATFORM,        CONCAT_ARGS, NO_OBJ ),
  T01("contact",             K_CONTACT,         CONCAT_ARGS, NO_OBJ ),
  T01("read-history",        K_READ_HISTORY,        ARGS,    NO_OBJ ),
  T01("write-history",       K_WRITE_HISTORY,       ARGS,    NO_OBJ ),
  T01("extra-info-digest",   K_EXTRA_INFO_DIGEST,   GE(1),   NO_OBJ ),
  T01("hidden-service-dir",  K_HIDDEN_SERVICE_DIR,  NO_ARGS, NO_OBJ ),
  T01("allow-single-hop-exits",K_ALLOW_SINGLE_HOP_EXITS,    NO_ARGS, NO_OBJ ),

  T01("family",              K_FAMILY,              ARGS,    NO_OBJ ),
  T01("caches-extra-info",   K_CACHES_EXTRA_INFO,   NO_ARGS, NO_OBJ ),
  T01("eventdns",            K_EVENTDNS,            ARGS,    NO_OBJ ),

  T0N("opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),
  T1( "bandwidth",           K_BANDWIDTH,           GE(3),   NO_OBJ ),
  A01("@purpose",            A_PURPOSE,             GE(1),   NO_OBJ ),

  END_OF_TABLE
};

/** List of tokens allowable in extra-info documents. */
static token_rule_t extrainfo_token_table[] = {
  T1_END( "router-signature",    K_ROUTER_SIGNATURE,    NO_ARGS, NEED_OBJ ),
  T1( "published",           K_PUBLISHED,       CONCAT_ARGS, NO_OBJ ),
  T0N("opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),
  T01("read-history",        K_READ_HISTORY,        ARGS,    NO_OBJ ),
  T01("write-history",       K_WRITE_HISTORY,       ARGS,    NO_OBJ ),
  T1_START( "extra-info",          K_EXTRA_INFO,          GE(2),   NO_OBJ ),

  END_OF_TABLE
};

/** List of tokens allowable in the body part of v2 and v3 networkstatus
 * documents. */
static token_rule_t rtrstatus_token_table[] = {
  T01("p",                   K_P,               CONCAT_ARGS, NO_OBJ ),
  T1( "r",                   K_R,                   GE(8),   NO_OBJ ),
  T1( "s",                   K_S,                   ARGS,    NO_OBJ ),
  T01("v",                   K_V,               CONCAT_ARGS, NO_OBJ ),
  T01("w",                   K_W,                   ARGS,    NO_OBJ ),
  T0N("opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),
  END_OF_TABLE
};

/** List of tokens allowable in the header part of v2 networkstatus documents.
 */
static token_rule_t netstatus_token_table[] = {
  T1( "published",           K_PUBLISHED,       CONCAT_ARGS, NO_OBJ ),
  T0N("opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),
  T1( "contact",             K_CONTACT,         CONCAT_ARGS, NO_OBJ ),
  T1( "dir-signing-key",     K_DIR_SIGNING_KEY,  NO_ARGS,    NEED_KEY_1024 ),
  T1( "fingerprint",         K_FINGERPRINT,     CONCAT_ARGS, NO_OBJ ),
  T1_START("network-status-version", K_NETWORK_STATUS_VERSION,
                                                    GE(1),   NO_OBJ ),
  T1( "dir-source",          K_DIR_SOURCE,          GE(3),   NO_OBJ ),
  T01("dir-options",         K_DIR_OPTIONS,         ARGS,    NO_OBJ ),
  T01("client-versions",     K_CLIENT_VERSIONS, CONCAT_ARGS, NO_OBJ ),
  T01("server-versions",     K_SERVER_VERSIONS, CONCAT_ARGS, NO_OBJ ),

  END_OF_TABLE
};

/** List of tokens allowable in the footer of v1/v2 directory/networkstatus
 * footers. */
static token_rule_t dir_footer_token_table[] = {
  T1("directory-signature", K_DIRECTORY_SIGNATURE, EQ(1), NEED_OBJ ),
  END_OF_TABLE
};

/** List of tokens allowable in v1 directory headers/footers. */
static token_rule_t dir_token_table[] = {
  /* don't enforce counts; this is obsolete. */
  T( "network-status",      K_NETWORK_STATUS,      NO_ARGS, NO_OBJ ),
  T( "directory-signature", K_DIRECTORY_SIGNATURE, ARGS,    NEED_OBJ ),
  T( "recommended-software",K_RECOMMENDED_SOFTWARE,CONCAT_ARGS, NO_OBJ ),
  T( "signed-directory",    K_SIGNED_DIRECTORY,    NO_ARGS, NO_OBJ ),

  T( "running-routers",     K_RUNNING_ROUTERS,     ARGS,    NO_OBJ ),
  T( "router-status",       K_ROUTER_STATUS,       ARGS,    NO_OBJ ),
  T( "published",           K_PUBLISHED,       CONCAT_ARGS, NO_OBJ ),
  T( "opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),
  T( "contact",             K_CONTACT,         CONCAT_ARGS, NO_OBJ ),
  T( "dir-signing-key",     K_DIR_SIGNING_KEY,     ARGS,    OBJ_OK ),
  T( "fingerprint",         K_FINGERPRINT,     CONCAT_ARGS, NO_OBJ ),

  END_OF_TABLE
};

/** List of tokens common to V3 authority certificates and V3 consensuses. */
#define CERTIFICATE_MEMBERS                                                  \
  T1("dir-key-certificate-version", K_DIR_KEY_CERTIFICATE_VERSION,           \
                                                     GE(1),       NO_OBJ ),  \
  T1("dir-identity-key", K_DIR_IDENTITY_KEY,         NO_ARGS,     NEED_KEY ),\
  T1("dir-key-published",K_DIR_KEY_PUBLISHED,        CONCAT_ARGS, NO_OBJ),   \
  T1("dir-key-expires",  K_DIR_KEY_EXPIRES,          CONCAT_ARGS, NO_OBJ),   \
  T1("dir-signing-key",  K_DIR_SIGNING_KEY,          NO_ARGS,     NEED_KEY ),\
  T01("dir-key-crosscert", K_DIR_KEY_CROSSCERT,       NO_ARGS,    NEED_OBJ ),\
  T1("dir-key-certification", K_DIR_KEY_CERTIFICATION,                       \
                                                     NO_ARGS,     NEED_OBJ), \
  T01("dir-address",     K_DIR_ADDRESS,              GE(1),       NO_OBJ),

/** List of tokens allowable in V3 authority certificates. */
static token_rule_t dir_key_certificate_table[] = {
  CERTIFICATE_MEMBERS
  T1("fingerprint",      K_FINGERPRINT,              CONCAT_ARGS, NO_OBJ ),
  END_OF_TABLE
};

/** List of tokens allowable in rendezvous service descriptors */
static token_rule_t desc_token_table[] = {
  T1_START("rendezvous-service-descriptor", R_RENDEZVOUS_SERVICE_DESCRIPTOR,
           EQ(1), NO_OBJ),
  T1("version", R_VERSION, EQ(1), NO_OBJ),
  T1("permanent-key", R_PERMANENT_KEY, NO_ARGS, NEED_KEY_1024),
  T1("secret-id-part", R_SECRET_ID_PART, EQ(1), NO_OBJ),
  T1("publication-time", R_PUBLICATION_TIME, CONCAT_ARGS, NO_OBJ),
  T1("protocol-versions", R_PROTOCOL_VERSIONS, EQ(1), NO_OBJ),
  T01("introduction-points", R_INTRODUCTION_POINTS, NO_ARGS, NEED_OBJ),
  T1_END("signature", R_SIGNATURE, NO_ARGS, NEED_OBJ),
  END_OF_TABLE
};

/** List of tokens allowed in the (encrypted) list of introduction points of
 * rendezvous service descriptors */
static token_rule_t ipo_token_table[] = {
  T1_START("introduction-point", R_IPO_IDENTIFIER, EQ(1), NO_OBJ),
  T1("ip-address", R_IPO_IP_ADDRESS, EQ(1), NO_OBJ),
  T1("onion-port", R_IPO_ONION_PORT, EQ(1), NO_OBJ),
  T1("onion-key", R_IPO_ONION_KEY, NO_ARGS, NEED_KEY_1024),
  T1("service-key", R_IPO_SERVICE_KEY, NO_ARGS, NEED_KEY_1024),
  END_OF_TABLE
};

/** List of tokens allowed in the (possibly encrypted) list of introduction
 * points of rendezvous service descriptors */
static token_rule_t client_keys_token_table[] = {
  T1_START("client-name", C_CLIENT_NAME, CONCAT_ARGS, NO_OBJ),
  T1("descriptor-cookie", C_DESCRIPTOR_COOKIE, EQ(1), NO_OBJ),
  T01("client-key", C_CLIENT_KEY, NO_ARGS, NEED_SKEY_1024),
  END_OF_TABLE
};

/** List of tokens allowed in V3 networkstatus votes. */
static token_rule_t networkstatus_token_table[] = {
  T1("network-status-version", K_NETWORK_STATUS_VERSION,
                                                   GE(1),       NO_OBJ ),
  T1("vote-status",            K_VOTE_STATUS,      GE(1),       NO_OBJ ),
  T1("published",              K_PUBLISHED,        CONCAT_ARGS, NO_OBJ ),
  T1("valid-after",            K_VALID_AFTER,      CONCAT_ARGS, NO_OBJ ),
  T1("fresh-until",            K_FRESH_UNTIL,      CONCAT_ARGS, NO_OBJ ),
  T1("valid-until",            K_VALID_UNTIL,      CONCAT_ARGS, NO_OBJ ),
  T1("voting-delay",           K_VOTING_DELAY,     GE(2),       NO_OBJ ),
  T1("known-flags",            K_KNOWN_FLAGS,      ARGS,        NO_OBJ ),
  T( "fingerprint",            K_FINGERPRINT,      CONCAT_ARGS, NO_OBJ ),

  CERTIFICATE_MEMBERS

  T0N("opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),
  T1( "contact",             K_CONTACT,         CONCAT_ARGS, NO_OBJ ),
  T1( "dir-source",          K_DIR_SOURCE,      GE(6),       NO_OBJ ),
  T01("legacy-dir-key",      K_LEGACY_DIR_KEY,  GE(1),       NO_OBJ ),
  T1( "known-flags",         K_KNOWN_FLAGS,     CONCAT_ARGS, NO_OBJ ),
  T01("client-versions",     K_CLIENT_VERSIONS, CONCAT_ARGS, NO_OBJ ),
  T01("server-versions",     K_SERVER_VERSIONS, CONCAT_ARGS, NO_OBJ ),
  T1( "consensus-methods",   K_CONSENSUS_METHODS, GE(1),     NO_OBJ ),

  END_OF_TABLE
};

/** List of tokens allowed in V3 networkstatus consensuses. */
static token_rule_t networkstatus_consensus_token_table[] = {
  T1("network-status-version", K_NETWORK_STATUS_VERSION,
                                                   GE(1),       NO_OBJ ),
  T1("vote-status",            K_VOTE_STATUS,      GE(1),       NO_OBJ ),
  T1("valid-after",            K_VALID_AFTER,      CONCAT_ARGS, NO_OBJ ),
  T1("fresh-until",            K_FRESH_UNTIL,      CONCAT_ARGS, NO_OBJ ),
  T1("valid-until",            K_VALID_UNTIL,      CONCAT_ARGS, NO_OBJ ),
  T1("voting-delay",           K_VOTING_DELAY,     GE(2),       NO_OBJ ),

  T0N("opt",                 K_OPT,             CONCAT_ARGS, OBJ_OK ),

  T1N("dir-source",          K_DIR_SOURCE,          GE(3),   NO_OBJ ),
  T1N("contact",             K_CONTACT,         CONCAT_ARGS, NO_OBJ ),
  T1N("vote-digest",         K_VOTE_DIGEST,         GE(1),   NO_OBJ ),

  T1( "known-flags",         K_KNOWN_FLAGS,     CONCAT_ARGS, NO_OBJ ),

  T01("client-versions",     K_CLIENT_VERSIONS, CONCAT_ARGS, NO_OBJ ),
  T01("server-versions",     K_SERVER_VERSIONS, CONCAT_ARGS, NO_OBJ ),
  T01("consensus-method",    K_CONSENSUS_METHOD,    EQ(1),   NO_OBJ),

  END_OF_TABLE
};

/** List of tokens allowable in the footer of v1/v2 directory/networkstatus
 * footers. */
static token_rule_t networkstatus_vote_footer_token_table[] = {
  T(  "directory-signature", K_DIRECTORY_SIGNATURE, GE(2),   NEED_OBJ ),
  END_OF_TABLE
};

/** List of tokens allowable in detached networkstatus signature documents. */
static token_rule_t networkstatus_detached_signature_token_table[] = {
  T1_START("consensus-digest", K_CONSENSUS_DIGEST, GE(1),       NO_OBJ ),
  T1("valid-after",            K_VALID_AFTER,      CONCAT_ARGS, NO_OBJ ),
  T1("fresh-until",            K_FRESH_UNTIL,      CONCAT_ARGS, NO_OBJ ),
  T1("valid-until",            K_VALID_UNTIL,      CONCAT_ARGS, NO_OBJ ),
  T1N("directory-signature", K_DIRECTORY_SIGNATURE, GE(2),   NEED_OBJ ),
  END_OF_TABLE
};

#undef T

/* static function prototypes */
static int router_add_exit_policy(routerinfo_t *router,directory_token_t *tok);
static addr_policy_t *router_parse_addr_policy(directory_token_t *tok);
static addr_policy_t *router_parse_addr_policy_private(directory_token_t *tok);

static int router_get_hash_impl(const char *s, char *digest,
                                const char *start_str, const char *end_str,
                                char end_char);
static void token_free(directory_token_t *tok);
static smartlist_t *find_all_exitpolicy(smartlist_t *s);
static directory_token_t *_find_by_keyword(smartlist_t *s,
                                           directory_keyword keyword,
                                           const char *keyword_str);
#define find_by_keyword(s, keyword) _find_by_keyword((s), (keyword), #keyword)
static directory_token_t *find_opt_by_keyword(smartlist_t *s,
                                              directory_keyword keyword);

#define TS_ANNOTATIONS_OK 1
#define TS_NOCHECK 2
#define TS_NO_NEW_ANNOTATIONS 4
static int tokenize_string(memarea_t *area,
                           const char *start, const char *end,
                           smartlist_t *out,
                           token_rule_t *table,
                           int flags);
static directory_token_t *get_next_token(memarea_t *area,
                                         const char **s,
                                         const char *eos,
                                         token_rule_t *table);
#define CST_CHECK_AUTHORITY   (1<<0)
#define CST_NO_CHECK_OBJTYPE  (1<<1)
static int check_signature_token(const char *digest,
                                 directory_token_t *tok,
                                 crypto_pk_env_t *pkey,
                                 int flags,
                                 const char *doctype);
static crypto_pk_env_t *find_dir_signing_key(const char *str, const char *eos);
static int tor_version_same_series(tor_version_t *a, tor_version_t *b);

#undef DEBUG_AREA_ALLOC

#ifdef DEBUG_AREA_ALLOC
#define DUMP_AREA(a,name) STMT_BEGIN                              \
  size_t alloc=0, used=0;                                         \
  memarea_get_stats((a),&alloc,&used);                            \
  log_debug(LD_MM, "Area for %s has %lu allocated; using %lu.",   \
            name, (unsigned long)alloc, (unsigned long)used);     \
  STMT_END
#else
#define DUMP_AREA(a,name) STMT_NIL
#endif

/** Set <b>digest</b> to the SHA-1 digest of the hash of the directory in
 * <b>s</b>.  Return 0 on success, -1 on failure.
 */
int
router_get_dir_hash(const char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "signed-directory","\ndirectory-signature",'\n');
}

/** Set <b>digest</b> to the SHA-1 digest of the hash of the first router in
 * <b>s</b>. Return 0 on success, -1 on failure.
 */
int
router_get_router_hash(const char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "router ","\nrouter-signature", '\n');
}

/** Set <b>digest</b> to the SHA-1 digest of the hash of the running-routers
 * string in <b>s</b>. Return 0 on success, -1 on failure.
 */
int
router_get_runningrouters_hash(const char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "network-status","\ndirectory-signature", '\n');
}

/** Set <b>digest</b> to the SHA-1 digest of the hash of the network-status
 * string in <b>s</b>.  Return 0 on success, -1 on failure. */
int
router_get_networkstatus_v2_hash(const char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "network-status-version","\ndirectory-signature",
                              '\n');
}

/** Set <b>digest</b> to the SHA-1 digest of the hash of the network-status
 * string in <b>s</b>.  Return 0 on success, -1 on failure. */
int
router_get_networkstatus_v3_hash(const char *s, char *digest)
{
  return router_get_hash_impl(s,digest,
                              "network-status-version","\ndirectory-signature",
                              ' ');
}

/** Set <b>digest</b> to the SHA-1 digest of the hash of the extrainfo
 * string in <b>s</b>.  Return 0 on success, -1 on failure. */
int
router_get_extrainfo_hash(const char *s, char *digest)
{
  return router_get_hash_impl(s,digest,"extra-info","\nrouter-signature",'\n');
}

/** Helper: used to generate signatures for routers, directories and
 * network-status objects.  Given a digest in <b>digest</b> and a secret
 * <b>private_key</b>, generate an PKCS1-padded signature, BASE64-encode it,
 * surround it with -----BEGIN/END----- pairs, and write it to the
 * <b>buf_len</b>-byte buffer at <b>buf</b>.  Return 0 on success, -1 on
 * failure.
 */
int
router_append_dirobj_signature(char *buf, size_t buf_len, const char *digest,
                               crypto_pk_env_t *private_key)
{
  char *signature;
  size_t i;

  signature = tor_malloc(crypto_pk_keysize(private_key));
  if (crypto_pk_private_sign(private_key, signature, digest, DIGEST_LEN) < 0) {

    log_warn(LD_BUG,"Couldn't sign digest.");
    goto err;
  }
  if (strlcat(buf, "-----BEGIN SIGNATURE-----\n", buf_len) >= buf_len)
    goto truncated;

  i = strlen(buf);
  if (base64_encode(buf+i, buf_len-i, signature, 128) < 0) {
    log_warn(LD_BUG,"couldn't base64-encode signature");
    goto err;
  }

  if (strlcat(buf, "-----END SIGNATURE-----\n", buf_len) >= buf_len)
    goto truncated;

  tor_free(signature);
  return 0;

 truncated:
  log_warn(LD_BUG,"tried to exceed string length.");
 err:
  tor_free(signature);
  return -1;
}

/** Return VS_RECOMMENDED if <b>myversion</b> is contained in
 * <b>versionlist</b>.  Else, return VS_EMPTY if versionlist has no
 * entries. Else, return VS_OLD if every member of
 * <b>versionlist</b> is newer than <b>myversion</b>.  Else, return
 * VS_NEW_IN_SERIES if there is at least one member of <b>versionlist</b> in
 * the same series (major.minor.micro) as <b>myversion</b>, but no such member
 * is newer than <b>myversion.</b>.  Else, return VS_NEW if every memeber of
 * <b>versionlist</b> is older than <b>myversion</b>.  Else, return
 * VS_UNRECOMMENDED.
 *
 * (versionlist is a comma-separated list of version strings,
 * optionally prefixed with "Tor".  Versions that can't be parsed are
 * ignored.)
 */
version_status_t
tor_version_is_obsolete(const char *myversion, const char *versionlist)
{
  tor_version_t mine, other;
  int found_newer = 0, found_older = 0, found_newer_in_series = 0,
    found_any_in_series = 0, r, same;
  version_status_t ret = VS_UNRECOMMENDED;
  smartlist_t *version_sl;

  log_debug(LD_CONFIG,"Checking whether version '%s' is in '%s'",
            myversion, versionlist);

  if (tor_version_parse(myversion, &mine)) {
    log_err(LD_BUG,"I couldn't parse my own version (%s)", myversion);
    tor_assert(0);
  }
  version_sl = smartlist_create();
  smartlist_split_string(version_sl, versionlist, ",", SPLIT_SKIP_SPACE, 0);

  if (!strlen(versionlist)) { /* no authorities cared or agreed */
    ret = VS_EMPTY;
    goto done;
  }

  SMARTLIST_FOREACH(version_sl, const char *, cp, {
    if (!strcmpstart(cp, "Tor "))
      cp += 4;

    if (tor_version_parse(cp, &other)) {
      /* Couldn't parse other; it can't be a match. */
    } else {
      same = tor_version_same_series(&mine, &other);
      if (same)
        found_any_in_series = 1;
      r = tor_version_compare(&mine, &other);
      if (r==0) {
        ret = VS_RECOMMENDED;
        goto done;
      } else if (r<0) {
        found_newer = 1;
        if (same)
          found_newer_in_series = 1;
      } else if (r>0) {
        found_older = 1;
      }
    }
  });

  /* We didn't find the listed version. Is it new or old? */
  if (found_any_in_series && !found_newer_in_series && found_newer) {
    ret = VS_NEW_IN_SERIES;
  } else if (found_newer && !found_older) {
    ret = VS_OLD;
  } else if (found_older && !found_newer) {
    ret = VS_NEW;
  } else {
    ret = VS_UNRECOMMENDED;
  }

 done:
  SMARTLIST_FOREACH(version_sl, char *, version, tor_free(version));
  smartlist_free(version_sl);
  return ret;
}

/** Read a signed directory from <b>str</b>.  If it's well-formed, return 0.
 * Otherwise, return -1.  If we're a directory cache, cache it.
 */
int
router_parse_directory(const char *str)
{
  directory_token_t *tok;
  char digest[DIGEST_LEN];
  time_t published_on;
  int r;
  const char *end, *cp;
  smartlist_t *tokens = NULL;
  crypto_pk_env_t *declared_key = NULL;
  memarea_t *area = memarea_new();

  /* XXXX This could be simplified a lot, but it will all go away
   * once pre-0.1.1.8 is obsolete, and for now it's better not to
   * touch it. */

  if (router_get_dir_hash(str, digest)) {
    log_warn(LD_DIR, "Unable to compute digest of directory");
    goto err;
  }
  log_debug(LD_DIR,"Received directory hashes to %s",hex_str(digest,4));

  /* Check signature first, before we try to tokenize. */
  cp = str;
  while (cp && (end = strstr(cp+1, "\ndirectory-signature")))
    cp = end;
  if (cp == str || !cp) {
    log_warn(LD_DIR, "No signature found on directory."); goto err;
  }
  ++cp;
  tokens = smartlist_create();
  if (tokenize_string(area,cp,strchr(cp,'\0'),tokens,dir_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing directory signature"); goto err;
  }
  if (smartlist_len(tokens) != 1) {
    log_warn(LD_DIR, "Unexpected number of tokens in signature"); goto err;
  }
  tok=smartlist_get(tokens,0);
  if (tok->tp != K_DIRECTORY_SIGNATURE) {
    log_warn(LD_DIR,"Expected a single directory signature"); goto err;
  }
  declared_key = find_dir_signing_key(str, str+strlen(str));
  note_crypto_pk_op(VERIFY_DIR);
  if (check_signature_token(digest, tok, declared_key,
                            CST_CHECK_AUTHORITY, "directory")<0)
    goto err;

  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_clear(tokens);
  memarea_clear(area);

  /* Now try to parse the first part of the directory. */
  if ((end = strstr(str,"\nrouter "))) {
    ++end;
  } else if ((end = strstr(str, "\ndirectory-signature"))) {
    ++end;
  } else {
    end = str + strlen(str);
  }

  if (tokenize_string(area,str,end,tokens,dir_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing directory"); goto err;
  }

  tok = find_by_keyword(tokens, K_PUBLISHED);
  tor_assert(tok->n_args == 1);

  if (parse_iso_time(tok->args[0], &published_on) < 0) {
     goto err;
  }

  /* Now that we know the signature is okay, and we have a
   * publication time, cache the directory. */
  if (directory_caches_v1_dir_info(get_options()) &&
      !authdir_mode_v1(get_options()))
    dirserv_set_cached_directory(str, published_on, 0);

  r = 0;
  goto done;
 err:
  r = -1;
 done:
  if (declared_key) crypto_free_pk_env(declared_key);
  if (tokens) {
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_free(tokens);
  }
  if (area) {
    DUMP_AREA(area, "v1 directory");
    memarea_drop_all(area);
  }
  return r;
}

/** Read a signed router status statement from <b>str</b>.  If it's
 * well-formed, return 0.  Otherwise, return -1.  If we're a directory cache,
 * cache it.*/
int
router_parse_runningrouters(const char *str)
{
  char digest[DIGEST_LEN];
  directory_token_t *tok;
  time_t published_on;
  int r = -1;
  crypto_pk_env_t *declared_key = NULL;
  smartlist_t *tokens = NULL;
  const char *eos = str + strlen(str);
  memarea_t *area = NULL;

  if (router_get_runningrouters_hash(str, digest)) {
    log_warn(LD_DIR, "Unable to compute digest of running-routers");
    goto err;
  }
  area = memarea_new();
  tokens = smartlist_create();
  if (tokenize_string(area,str,eos,tokens,dir_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing running-routers"); goto err;
  }
  tok = smartlist_get(tokens,0);
  if (tok->tp != K_NETWORK_STATUS) {
    log_warn(LD_DIR, "Network-status starts with wrong token");
    goto err;
  }

  tok = find_by_keyword(tokens, K_PUBLISHED);
  tor_assert(tok->n_args == 1);
  if (parse_iso_time(tok->args[0], &published_on) < 0) {
     goto err;
  }
  if (!(tok = find_opt_by_keyword(tokens, K_DIRECTORY_SIGNATURE))) {
    log_warn(LD_DIR, "Missing signature on running-routers");
    goto err;
  }
  declared_key = find_dir_signing_key(str, eos);
  note_crypto_pk_op(VERIFY_DIR);
  if (check_signature_token(digest, tok, declared_key,
                            CST_CHECK_AUTHORITY, "running-routers")
      < 0)
    goto err;

  /* Now that we know the signature is okay, and we have a
   * publication time, cache the list. */
  if (get_options()->DirPort && !authdir_mode_v1(get_options()))
    dirserv_set_cached_directory(str, published_on, 1);

  r = 0;
 err:
  if (declared_key) crypto_free_pk_env(declared_key);
  if (tokens) {
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_free(tokens);
  }
  if (area) {
    DUMP_AREA(area, "v1 running-routers");
    memarea_drop_all(area);
  }
  return r;
}

/** Given a directory or running-routers string in <b>str</b>, try to
 * find the its dir-signing-key token (if any).  If this token is
 * present, extract and return the key.  Return NULL on failure. */
static crypto_pk_env_t *
find_dir_signing_key(const char *str, const char *eos)
{
  const char *cp;
  directory_token_t *tok;
  crypto_pk_env_t *key = NULL;
  memarea_t *area = NULL;
  tor_assert(str);
  tor_assert(eos);

  /* Is there a dir-signing-key in the directory? */
  cp = tor_memstr(str, eos-str, "\nopt dir-signing-key");
  if (!cp)
    cp = tor_memstr(str, eos-str, "\ndir-signing-key");
  if (!cp)
    return NULL;
  ++cp; /* Now cp points to the start of the token. */

  area = memarea_new();
  tok = get_next_token(area, &cp, eos, dir_token_table);
  if (!tok) {
    log_warn(LD_DIR, "Unparseable dir-signing-key token");
    goto done;
  }
  if (tok->tp != K_DIR_SIGNING_KEY) {
    log_warn(LD_DIR, "Dir-signing-key token did not parse as expected");
    goto done;
  }

  if (tok->key) {
    key = tok->key;
    tok->key = NULL; /* steal reference. */
  } else {
    log_warn(LD_DIR, "Dir-signing-key token contained no key");
  }

 done:
  if (tok) token_free(tok);
  if (area) {
    DUMP_AREA(area, "dir-signing-key token");
    memarea_drop_all(area);
  }
  return key;
}

/** Return true iff <b>key</b> is allowed to sign directories.
 */
static int
dir_signing_key_is_trusted(crypto_pk_env_t *key)
{
  char digest[DIGEST_LEN];
  if (!key) return 0;
  if (crypto_pk_get_digest(key, digest) < 0) {
    log_warn(LD_DIR, "Error computing dir-signing-key digest");
    return 0;
  }
  if (!router_digest_is_trusted_dir(digest)) {
    log_warn(LD_DIR, "Listed dir-signing-key is not trusted");
    return 0;
  }
  return 1;
}

/** Check whether the object body of the token in <b>tok</b> has a good
 * signature for <b>digest</b> using key <b>pkey</b>.  If
 * <b>CST_CHECK_AUTHORITY</b> is set, make sure that <b>pkey</b> is the key of
 * a directory authority.  If <b>CST_NO_CHECK_OBJTYPE</b> is set, do not check
 * the object type of the signature object. Use <b>doctype</b> as the type of
 * the document when generating log messages.  Return 0 on success, negative
 * on failure.
 */
static int
check_signature_token(const char *digest,
                      directory_token_t *tok,
                      crypto_pk_env_t *pkey,
                      int flags,
                      const char *doctype)
{
  char *signed_digest;
  const int check_authority = (flags & CST_CHECK_AUTHORITY);
  const int check_objtype = ! (flags & CST_NO_CHECK_OBJTYPE);

  tor_assert(pkey);
  tor_assert(tok);
  tor_assert(digest);
  tor_assert(doctype);

  if (check_authority && !dir_signing_key_is_trusted(pkey)) {
    log_warn(LD_DIR, "Key on %s did not come from an authority; rejecting",
             doctype);
    return -1;
  }

  if (check_objtype) {
    if (strcmp(tok->object_type, "SIGNATURE")) {
      log_warn(LD_DIR, "Bad object type on %s signature", doctype);
      return -1;
    }
  }

  signed_digest = tor_malloc(tok->object_size);
  if (crypto_pk_public_checksig(pkey, signed_digest, tok->object_body,
                                tok->object_size)
      != DIGEST_LEN) {
    log_warn(LD_DIR, "Error reading %s: invalid signature.", doctype);
    tor_free(signed_digest);
    return -1;
  }
//  log_debug(LD_DIR,"Signed %s hash starts %s", doctype,
//            hex_str(signed_digest,4));
  if (memcmp(digest, signed_digest, DIGEST_LEN)) {
    log_warn(LD_DIR, "Error reading %s: signature does not match.", doctype);
    tor_free(signed_digest);
    return -1;
  }
  tor_free(signed_digest);
  return 0;
}

/** Helper: move *<b>s_ptr</b> ahead to the next router, the next extra-info,
 * or to the first of the annotations proceeding the next router or
 * extra-info---whichever comes first.  Set <b>is_extrainfo_out</b> to true if
 * we found an extrainfo, or false if found a router. Do not scan beyond
 * <b>eos</b>.  Return -1 if we found nothing; 0 if we found something. */
static int
find_start_of_next_router_or_extrainfo(const char **s_ptr,
                                       const char *eos,
                                       int *is_extrainfo_out)
{
  const char *annotations = NULL;
  const char *s = *s_ptr;

  s = eat_whitespace_eos(s, eos);

  while (s < eos-32) {  /* 32 gives enough room for a the first keyword. */
    /* We're at the start of a line. */
    tor_assert(*s != '\n');

    if (*s == '@' && !annotations) {
      annotations = s;
    } else if (*s == 'r' && !strcmpstart(s, "router ")) {
      *s_ptr = annotations ? annotations : s;
      *is_extrainfo_out = 0;
      return 0;
    } else if (*s == 'e' && !strcmpstart(s, "extra-info ")) {
      *s_ptr = annotations ? annotations : s;
      *is_extrainfo_out = 1;
      return 0;
    }

    if (!(s = memchr(s+1, '\n', eos-(s+1))))
      break;
    s = eat_whitespace_eos(s, eos);
  }
  return -1;
}

/** Given a string *<b>s</b> containing a concatenated sequence of router
 * descriptors (or extra-info documents if <b>is_extrainfo</b> is set), parses
 * them and stores the result in <b>dest</b>.  All routers are marked running
 * and valid.  Advances *s to a point immediately following the last router
 * entry.  Ignore any trailing router entries that are not complete.
 *
 * If <b>saved_location</b> isn't SAVED_IN_CACHE, make a local copy of each
 * descriptor in the signed_descriptor_body field of each routerinfo_t.  If it
 * isn't SAVED_NOWHERE, remember the offset of each descriptor.
 *
 * Returns 0 on success and -1 on failure.
 */
int
router_parse_list_from_string(const char **s, const char *eos,
                              smartlist_t *dest,
                              saved_location_t saved_location,
                              int want_extrainfo,
                              int allow_annotations,
                              const char *prepend_annotations)
{
  routerinfo_t *router;
  extrainfo_t *extrainfo;
  signed_descriptor_t *signed_desc;
  void *elt;
  const char *end, *start;
  int have_extrainfo;

  tor_assert(s);
  tor_assert(*s);
  tor_assert(dest);

  start = *s;
  if (!eos)
    eos = *s + strlen(*s);

  tor_assert(eos >= *s);

  while (1) {
    if (find_start_of_next_router_or_extrainfo(s, eos, &have_extrainfo) < 0)
      break;

    end = tor_memstr(*s, eos-*s, "\nrouter-signature");
    if (end)
      end = tor_memstr(end, eos-end, "\n-----END SIGNATURE-----\n");
    if (end)
      end += strlen("\n-----END SIGNATURE-----\n");

    if (!end)
      break;

    elt = NULL;

    if (have_extrainfo && want_extrainfo) {
      routerlist_t *rl = router_get_routerlist();
      extrainfo = extrainfo_parse_entry_from_string(*s, end,
                                       saved_location != SAVED_IN_CACHE,
                                       rl->identity_map);
      if (extrainfo) {
        signed_desc = &extrainfo->cache_info;
        elt = extrainfo;
      }
    } else if (!have_extrainfo && !want_extrainfo) {
      router = router_parse_entry_from_string(*s, end,
                                              saved_location != SAVED_IN_CACHE,
                                              allow_annotations,
                                              prepend_annotations);
      if (router) {
        log_debug(LD_DIR, "Read router '%s', purpose '%s'",
                  router->nickname, router_purpose_to_string(router->purpose));
        signed_desc = &router->cache_info;
        elt = router;
      }
    }
    if (!elt) {
      *s = end;
      continue;
    }
    if (saved_location != SAVED_NOWHERE) {
      signed_desc->saved_location = saved_location;
      signed_desc->saved_offset = *s - start;
    }
    *s = end;
    smartlist_add(dest, elt);
  }

  return 0;
}

/* For debugging: define to count every descriptor digest we've seen so we
 * know if we need to try harder to avoid duplicate verifies. */
#undef COUNT_DISTINCT_DIGESTS

#ifdef COUNT_DISTINCT_DIGESTS
static digestmap_t *verified_digests = NULL;
#endif

/** Log the total count of the number of distinct router digests we've ever
 * verified.  When compared to the number of times we've verified routerdesc
 * signatures <i>in toto</i>, this will tell us if we're doing too much
 * multiple-verification. */
void
dump_distinct_digest_count(int severity)
{
#ifdef COUNT_DISTINCT_DIGESTS
  if (!verified_digests)
    verified_digests = digestmap_new();
  log(severity, LD_GENERAL, "%d *distinct* router digests verified",
      digestmap_size(verified_digests));
#else
  (void)severity; /* suppress "unused parameter" warning */
#endif
}

/** Helper function: reads a single router entry from *<b>s</b> ...
 * *<b>end</b>.  Mallocs a new router and returns it if all goes well, else
 * returns NULL.  If <b>cache_copy</b> is true, duplicate the contents of
 * s through end into the signed_descriptor_body of the resulting
 * routerinfo_t.
 *
 * If <b>allow_annotations</b>, it's okay to encounter annotations in <b>s</b>
 * before the router; if it's false, reject the router if it's annotated.  If
 * <b>prepend_annotations</b> is set, it should contain some annotations:
 * append them to the front of the router before parsing it, and keep them
 * around when caching the router.
 *
 * Only one of allow_annotations and prepend_annotations may be set.
 */
routerinfo_t *
router_parse_entry_from_string(const char *s, const char *end,
                               int cache_copy, int allow_annotations,
                               const char *prepend_annotations)
{
  routerinfo_t *router = NULL;
  char digest[128];
  smartlist_t *tokens = NULL, *exit_policy_tokens = NULL;
  directory_token_t *tok;
  struct in_addr in;
  const char *start_of_annotations, *cp;
  size_t prepend_len = prepend_annotations ? strlen(prepend_annotations) : 0;
  int ok = 1;
  memarea_t *area = NULL;

  tor_assert(!allow_annotations || !prepend_annotations);

  if (!end) {
    end = s + strlen(s);
  }

  /* point 'end' to a point immediately after the final newline. */
  while (end > s+2 && *(end-1) == '\n' && *(end-2) == '\n')
    --end;

  area = memarea_new();
  tokens = smartlist_create();
  if (prepend_annotations) {
    if (tokenize_string(area,prepend_annotations,NULL,tokens,
                        routerdesc_token_table,TS_NOCHECK)) {
      log_warn(LD_DIR, "Error tokenizing router descriptor (annotations).");
      goto err;
    }
  }

  start_of_annotations = s;
  cp = tor_memstr(s, end-s, "\nrouter ");
  if (!cp) {
    if (end-s < 7 || strcmpstart(s, "router ")) {
      log_warn(LD_DIR, "No router keyword found.");
      goto err;
    }
  } else {
    s = cp+1;
  }

  if (allow_annotations && start_of_annotations != s) {
    if (tokenize_string(area,start_of_annotations,s,tokens,
                        routerdesc_token_table,TS_NOCHECK)) {
      log_warn(LD_DIR, "Error tokenizing router descriptor (annotations).");
      goto err;
    }
  }

  if (router_get_router_hash(s, digest) < 0) {
    log_warn(LD_DIR, "Couldn't compute router hash.");
    goto err;
  }
  {
    int flags = 0;
    if (allow_annotations)
      flags |= TS_ANNOTATIONS_OK;
    if (prepend_annotations)
      flags |= TS_ANNOTATIONS_OK|TS_NO_NEW_ANNOTATIONS;

    if (tokenize_string(area,s,end,tokens,routerdesc_token_table, flags)) {
      log_warn(LD_DIR, "Error tokenizing router descriptor.");
      goto err;
    }
  }

  if (smartlist_len(tokens) < 2) {
    log_warn(LD_DIR, "Impossibly short router descriptor.");
    goto err;
  }

  tok = find_by_keyword(tokens, K_ROUTER);
  tor_assert(tok->n_args >= 5);

  router = tor_malloc_zero(sizeof(routerinfo_t));
  router->country = -1;
  router->cache_info.routerlist_index = -1;
  router->cache_info.annotations_len = s-start_of_annotations + prepend_len;
  router->cache_info.signed_descriptor_len = end-s;
  if (cache_copy) {
    size_t len = router->cache_info.signed_descriptor_len +
                 router->cache_info.annotations_len;
    char *cp =
      router->cache_info.signed_descriptor_body = tor_malloc(len+1);
    if (prepend_annotations) {
      memcpy(cp, prepend_annotations, prepend_len);
      cp += prepend_len;
    }
    /* This assertion will always succeed.
     * len == signed_desc_len + annotations_len
     *     == end-s + s-start_of_annotations + prepend_len
     *     == end-start_of_annotations + prepend_len
     * We already wrote prepend_len bytes into the buffer; now we're
     * writing end-start_of_annotations -NM. */
    tor_assert(cp+(end-start_of_annotations) ==
               router->cache_info.signed_descriptor_body+len);
    memcpy(cp, start_of_annotations, end-start_of_annotations);
    router->cache_info.signed_descriptor_body[len] = '\0';
    tor_assert(strlen(router->cache_info.signed_descriptor_body) == len);
  }
  memcpy(router->cache_info.signed_descriptor_digest, digest, DIGEST_LEN);

  router->nickname = tor_strdup(tok->args[0]);
  if (!is_legal_nickname(router->nickname)) {
    log_warn(LD_DIR,"Router nickname is invalid");
    goto err;
  }
  router->address = tor_strdup(tok->args[1]);
  if (!tor_inet_aton(router->address, &in)) {
    log_warn(LD_DIR,"Router address is not an IP address.");
    goto err;
  }
  router->addr = ntohl(in.s_addr);

  router->or_port =
    (uint16_t) tor_parse_long(tok->args[2],10,0,65535,&ok,NULL);
  if (!ok) {
    log_warn(LD_DIR,"Invalid OR port %s", escaped(tok->args[2]));
    goto err;
  }
  router->dir_port =
    (uint16_t) tor_parse_long(tok->args[4],10,0,65535,&ok,NULL);
  if (!ok) {
    log_warn(LD_DIR,"Invalid dir port %s", escaped(tok->args[4]));
    goto err;
  }

  tok = find_by_keyword(tokens, K_BANDWIDTH);
  tor_assert(tok->n_args >= 3);
  router->bandwidthrate = (int)
    tor_parse_long(tok->args[0],10,1,INT_MAX,&ok,NULL);

  if (!ok) {
    log_warn(LD_DIR, "bandwidthrate %s unreadable or 0. Failing.",
             escaped(tok->args[0]));
    goto err;
  }
  router->bandwidthburst =
    (int) tor_parse_long(tok->args[1],10,0,INT_MAX,&ok,NULL);
  if (!ok) {
    log_warn(LD_DIR, "Invalid bandwidthburst %s", escaped(tok->args[1]));
    goto err;
  }
  router->bandwidthcapacity = (int)
    tor_parse_long(tok->args[2],10,0,INT_MAX,&ok,NULL);
  if (!ok) {
    log_warn(LD_DIR, "Invalid bandwidthcapacity %s", escaped(tok->args[1]));
    goto err;
  }

  if ((tok = find_opt_by_keyword(tokens, A_PURPOSE))) {
    tor_assert(tok->n_args);
    router->purpose = router_purpose_from_string(tok->args[0]);
  } else {
    router->purpose = ROUTER_PURPOSE_GENERAL;
  }
  router->cache_info.send_unencrypted =
    (router->purpose == ROUTER_PURPOSE_GENERAL) ? 1 : 0;

  if ((tok = find_opt_by_keyword(tokens, K_UPTIME))) {
    tor_assert(tok->n_args >= 1);
    router->uptime = tor_parse_long(tok->args[0],10,0,LONG_MAX,&ok,NULL);
    if (!ok) {
      log_warn(LD_DIR, "Invalid uptime %s", escaped(tok->args[0]));
      goto err;
    }
  }

  if ((tok = find_opt_by_keyword(tokens, K_HIBERNATING))) {
    tor_assert(tok->n_args >= 1);
    router->is_hibernating
      = (tor_parse_long(tok->args[0],10,0,LONG_MAX,NULL,NULL) != 0);
  }

  tok = find_by_keyword(tokens, K_PUBLISHED);
  tor_assert(tok->n_args == 1);
  if (parse_iso_time(tok->args[0], &router->cache_info.published_on) < 0)
    goto err;

  tok = find_by_keyword(tokens, K_ONION_KEY);
  router->onion_pkey = tok->key;
  tok->key = NULL; /* Prevent free */

  tok = find_by_keyword(tokens, K_SIGNING_KEY);
  router->identity_pkey = tok->key;
  tok->key = NULL; /* Prevent free */
  if (crypto_pk_get_digest(router->identity_pkey,
                           router->cache_info.identity_digest)) {
    log_warn(LD_DIR, "Couldn't calculate key digest"); goto err;
  }

  if ((tok = find_opt_by_keyword(tokens, K_FINGERPRINT))) {
    /* If there's a fingerprint line, it must match the identity digest. */
    char d[DIGEST_LEN];
    tor_assert(tok->n_args == 1);
    tor_strstrip(tok->args[0], " ");
    if (base16_decode(d, DIGEST_LEN, tok->args[0], strlen(tok->args[0]))) {
      log_warn(LD_DIR, "Couldn't decode router fingerprint %s",
               escaped(tok->args[0]));
      goto err;
    }
    if (memcmp(d,router->cache_info.identity_digest, DIGEST_LEN)!=0) {
      log_warn(LD_DIR, "Fingerprint '%s' does not match identity digest.",
               tok->args[0]);
      goto err;
    }
  }

  if ((tok = find_opt_by_keyword(tokens, K_PLATFORM))) {
    router->platform = tor_strdup(tok->args[0]);
  }

  if ((tok = find_opt_by_keyword(tokens, K_CONTACT))) {
    router->contact_info = tor_strdup(tok->args[0]);
  }

  if ((tok = find_opt_by_keyword(tokens, K_EVENTDNS))) {
    router->has_old_dnsworkers = tok->n_args && !strcmp(tok->args[0], "0");
  } else if (router->platform) {
    if (! tor_version_as_new_as(router->platform, "0.1.2.2-alpha"))
      router->has_old_dnsworkers = 1;
  }

  exit_policy_tokens = find_all_exitpolicy(tokens);
  if (!smartlist_len(exit_policy_tokens)) {
    log_warn(LD_DIR, "No exit policy tokens in descriptor.");
    goto err;
  }
  SMARTLIST_FOREACH(exit_policy_tokens, directory_token_t *, t,
                    if (router_add_exit_policy(router,t)<0) {
                      log_warn(LD_DIR,"Error in exit policy");
                      goto err;
                    });
  policy_expand_private(&router->exit_policy);
  if (policy_is_reject_star(router->exit_policy))
    router->policy_is_reject_star = 1;

  if ((tok = find_opt_by_keyword(tokens, K_FAMILY)) && tok->n_args) {
    int i;
    router->declared_family = smartlist_create();
    for (i=0;i<tok->n_args;++i) {
      if (!is_legal_nickname_or_hexdigest(tok->args[i])) {
        log_warn(LD_DIR, "Illegal nickname %s in family line",
                 escaped(tok->args[i]));
        goto err;
      }
      smartlist_add(router->declared_family, tor_strdup(tok->args[i]));
    }
  }

  if ((tok = find_opt_by_keyword(tokens, K_CACHES_EXTRA_INFO)))
    router->caches_extra_info = 1;

  if ((tok = find_opt_by_keyword(tokens, K_ALLOW_SINGLE_HOP_EXITS)))
    router->allow_single_hop_exits = 1;

  if ((tok = find_opt_by_keyword(tokens, K_EXTRA_INFO_DIGEST))) {
    tor_assert(tok->n_args >= 1);
    if (strlen(tok->args[0]) == HEX_DIGEST_LEN) {
      base16_decode(router->cache_info.extra_info_digest,
                    DIGEST_LEN, tok->args[0], HEX_DIGEST_LEN);
    } else {
      log_warn(LD_DIR, "Invalid extra info digest %s", escaped(tok->args[0]));
    }
  }

  if ((tok = find_opt_by_keyword(tokens, K_HIDDEN_SERVICE_DIR))) {
    router->wants_to_be_hs_dir = 1;
  }

  tok = find_by_keyword(tokens, K_ROUTER_SIGNATURE);
  note_crypto_pk_op(VERIFY_RTR);
#ifdef COUNT_DISTINCT_DIGESTS
  if (!verified_digests)
    verified_digests = digestmap_new();
  digestmap_set(verified_digests, signed_digest, (void*)(uintptr_t)1);
#endif
  if (check_signature_token(digest, tok, router->identity_pkey, 0,
                            "router descriptor") < 0)
    goto err;

  routerinfo_set_country(router);

  if (!router->or_port) {
    log_warn(LD_DIR,"or_port unreadable or 0. Failing.");
    goto err;
  }

  if (!router->platform) {
    router->platform = tor_strdup("<unknown>");
  }

  goto done;

 err:
  routerinfo_free(router);
  router = NULL;
 done:
  if (tokens) {
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_free(tokens);
  }
  if (exit_policy_tokens) {
    smartlist_free(exit_policy_tokens);
  }
  if (area) {
    DUMP_AREA(area, "routerinfo");
    memarea_drop_all(area);
  }
  return router;
}

/** Parse a single extrainfo entry from the string <b>s</b>, ending at
 * <b>end</b>.  (If <b>end</b> is NULL, parse up to the end of <b>s</b>.)  If
 * <b>cache_copy</b> is true, make a copy of the extra-info document in the
 * cache_info fields of the result.  If <b>routermap</b> is provided, use it
 * as a map from router identity to routerinfo_t when looking up signing keys.
 */
extrainfo_t *
extrainfo_parse_entry_from_string(const char *s, const char *end,
                           int cache_copy, struct digest_ri_map_t *routermap)
{
  extrainfo_t *extrainfo = NULL;
  char digest[128];
  smartlist_t *tokens = NULL;
  directory_token_t *tok;
  crypto_pk_env_t *key = NULL;
  routerinfo_t *router = NULL;
  memarea_t *area = NULL;

  if (!end) {
    end = s + strlen(s);
  }

  /* point 'end' to a point immediately after the final newline. */
  while (end > s+2 && *(end-1) == '\n' && *(end-2) == '\n')
    --end;

  if (router_get_extrainfo_hash(s, digest) < 0) {
    log_warn(LD_DIR, "Couldn't compute router hash.");
    goto err;
  }
  tokens = smartlist_create();
  area = memarea_new();
  if (tokenize_string(area,s,end,tokens,extrainfo_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing extra-info document.");
    goto err;
  }

  if (smartlist_len(tokens) < 2) {
    log_warn(LD_DIR, "Impossibly short extra-info document.");
    goto err;
  }

  tok = smartlist_get(tokens,0);
  if (tok->tp != K_EXTRA_INFO) {
    log_warn(LD_DIR,"Entry does not start with \"extra-info\"");
    goto err;
  }

  extrainfo = tor_malloc_zero(sizeof(extrainfo_t));
  extrainfo->cache_info.is_extrainfo = 1;
  if (cache_copy)
    extrainfo->cache_info.signed_descriptor_body = tor_strndup(s, end-s);
  extrainfo->cache_info.signed_descriptor_len = end-s;
  memcpy(extrainfo->cache_info.signed_descriptor_digest, digest, DIGEST_LEN);

  tor_assert(tok->n_args >= 2);
  if (!is_legal_nickname(tok->args[0])) {
    log_warn(LD_DIR,"Bad nickname %s on \"extra-info\"",escaped(tok->args[0]));
    goto err;
  }
  strlcpy(extrainfo->nickname, tok->args[0], sizeof(extrainfo->nickname));
  if (strlen(tok->args[1]) != HEX_DIGEST_LEN ||
      base16_decode(extrainfo->cache_info.identity_digest, DIGEST_LEN,
                    tok->args[1], HEX_DIGEST_LEN)) {
    log_warn(LD_DIR,"Invalid fingerprint %s on \"extra-info\"",
             escaped(tok->args[1]));
    goto err;
  }

  tok = find_by_keyword(tokens, K_PUBLISHED);
  if (parse_iso_time(tok->args[0], &extrainfo->cache_info.published_on)) {
    log_warn(LD_DIR,"Invalid published time %s on \"extra-info\"",
             escaped(tok->args[0]));
    goto err;
  }

  if (routermap &&
      (router = digestmap_get((digestmap_t*)routermap,
                              extrainfo->cache_info.identity_digest))) {
    key = router->identity_pkey;
  }

  tok = find_by_keyword(tokens, K_ROUTER_SIGNATURE);
  if (strcmp(tok->object_type, "SIGNATURE") ||
      tok->object_size < 128 || tok->object_size > 512) {
    log_warn(LD_DIR, "Bad object type or length on extra-info signature");
    goto err;
  }

  if (key) {
    note_crypto_pk_op(VERIFY_RTR);
    if (check_signature_token(digest, tok, key, 0, "extra-info") < 0)
      goto err;

    if (router)
      extrainfo->cache_info.send_unencrypted =
        router->cache_info.send_unencrypted;
  } else {
    extrainfo->pending_sig = tor_memdup(tok->object_body,
                                        tok->object_size);
    extrainfo->pending_sig_len = tok->object_size;
  }

  goto done;
 err:
  if (extrainfo)
    extrainfo_free(extrainfo);
  extrainfo = NULL;
 done:
  if (tokens) {
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_free(tokens);
  }
  if (area) {
    DUMP_AREA(area, "extrainfo");
    memarea_drop_all(area);
  }
  return extrainfo;
}

/** Parse a key certificate from <b>s</b>; point <b>end-of-string</b> to
 * the first character after the certificate. */
authority_cert_t *
authority_cert_parse_from_string(const char *s, const char **end_of_string)
{
  authority_cert_t *cert = NULL, *old_cert;
  smartlist_t *tokens = NULL;
  char digest[DIGEST_LEN];
  directory_token_t *tok;
  char fp_declared[DIGEST_LEN];
  char *eos;
  size_t len;
  int found;
  memarea_t *area = NULL;

  s = eat_whitespace(s);
  eos = strstr(s, "\ndir-key-certification");
  if (! eos) {
    log_warn(LD_DIR, "No signature found on key certificate");
    return NULL;
  }
  eos = strstr(eos, "\n-----END SIGNATURE-----\n");
  if (! eos) {
    log_warn(LD_DIR, "No end-of-signature found on key certificate");
    return NULL;
  }
  eos = strchr(eos+2, '\n');
  tor_assert(eos);
  ++eos;
  len = eos - s;

  tokens = smartlist_create();
  area = memarea_new();
  if (tokenize_string(area,s, eos, tokens, dir_key_certificate_table, 0) < 0) {
    log_warn(LD_DIR, "Error tokenizing key certificate");
    goto err;
  }
  if (router_get_hash_impl(s, digest, "dir-key-certificate-version",
                           "\ndir-key-certification", '\n') < 0)
    goto err;
  tok = smartlist_get(tokens, 0);
  if (tok->tp != K_DIR_KEY_CERTIFICATE_VERSION || strcmp(tok->args[0], "3")) {
    log_warn(LD_DIR,
             "Key certificate does not begin with a recognized version (3).");
    goto err;
  }

  cert = tor_malloc_zero(sizeof(authority_cert_t));
  memcpy(cert->cache_info.signed_descriptor_digest, digest, DIGEST_LEN);

  tok = find_by_keyword(tokens, K_DIR_SIGNING_KEY);
  tor_assert(tok->key);
  cert->signing_key = tok->key;
  tok->key = NULL;
  if (crypto_pk_get_digest(cert->signing_key, cert->signing_key_digest))
    goto err;

  tok = find_by_keyword(tokens, K_DIR_IDENTITY_KEY);
  tor_assert(tok->key);
  cert->identity_key = tok->key;
  tok->key = NULL;

  tok = find_by_keyword(tokens, K_FINGERPRINT);
  tor_assert(tok->n_args);
  if (base16_decode(fp_declared, DIGEST_LEN, tok->args[0],
                    strlen(tok->args[0]))) {
    log_warn(LD_DIR, "Couldn't decode key certificate fingerprint %s",
             escaped(tok->args[0]));
    goto err;
  }

  if (crypto_pk_get_digest(cert->identity_key,
                           cert->cache_info.identity_digest))
    goto err;

  if (memcmp(cert->cache_info.identity_digest, fp_declared, DIGEST_LEN)) {
    log_warn(LD_DIR, "Digest of certificate key didn't match declared "
             "fingerprint");
    goto err;
  }

  tok = find_opt_by_keyword(tokens, K_DIR_ADDRESS);
  if (tok) {
    tor_assert(tok->n_args);
    if (parse_addr_port(LOG_WARN, tok->args[0], NULL, &cert->addr,
                        &cert->dir_port)<0) {
      log_warn(LD_DIR, "Couldn't parse dir-address in certificate");
      goto err;
    }
  }

  tok = find_by_keyword(tokens, K_DIR_KEY_PUBLISHED);
  if (parse_iso_time(tok->args[0], &cert->cache_info.published_on) < 0) {
     goto err;
  }
  tok = find_by_keyword(tokens, K_DIR_KEY_EXPIRES);
  if (parse_iso_time(tok->args[0], &cert->expires) < 0) {
     goto err;
  }

  tok = smartlist_get(tokens, smartlist_len(tokens)-1);
  if (tok->tp != K_DIR_KEY_CERTIFICATION) {
    log_warn(LD_DIR, "Certificate didn't end with dir-key-certification.");
    goto err;
  }

  /* If we already have this cert, don't bother checking the signature. */
  old_cert = authority_cert_get_by_digests(
                                     cert->cache_info.identity_digest,
                                     cert->signing_key_digest);
  found = 0;
  if (old_cert) {
    /* XXXX We could just compare signed_descriptor_digest, but that wouldn't
     * buy us much. */
    if (old_cert->cache_info.signed_descriptor_len == len &&
        old_cert->cache_info.signed_descriptor_body &&
        !memcmp(s, old_cert->cache_info.signed_descriptor_body, len)) {
      log_debug(LD_DIR, "We already checked the signature on this "
                "certificate; no need to do so again.");
      found = 1;
      cert->is_cross_certified = old_cert->is_cross_certified;
    }
  }
  if (!found) {
    if (check_signature_token(digest, tok, cert->identity_key, 0,
                              "key certificate")) {
      goto err;
    }

    if ((tok = find_opt_by_keyword(tokens, K_DIR_KEY_CROSSCERT))) {
      /* XXXX Once all authorities generate cross-certified certificates,
       * make this field mandatory. */
      if (check_signature_token(cert->cache_info.identity_digest,
                                tok,
                                cert->signing_key,
                                CST_NO_CHECK_OBJTYPE,
                                "key cross-certification")) {
        goto err;
      }
      cert->is_cross_certified = 1;
    }
  }

  cert->cache_info.signed_descriptor_len = len;
  cert->cache_info.signed_descriptor_body = tor_malloc(len+1);
  memcpy(cert->cache_info.signed_descriptor_body, s, len);
  cert->cache_info.signed_descriptor_body[len] = 0;
  cert->cache_info.saved_location = SAVED_NOWHERE;

  if (end_of_string) {
    *end_of_string = eat_whitespace(eos);
  }
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_free(tokens);
  if (area) {
    DUMP_AREA(area, "authority cert");
    memarea_drop_all(area);
  }
  return cert;
 err:
  authority_cert_free(cert);
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_free(tokens);
  if (area) {
    DUMP_AREA(area, "authority cert");
    memarea_drop_all(area);
  }
  return NULL;
}

/** Helper: given a string <b>s</b>, return the start of the next router-status
 * object (starting with "r " at the start of a line).  If none is found,
 * return the start of the next directory signature.  If none is found, return
 * the end of the string. */
static INLINE const char *
find_start_of_next_routerstatus(const char *s)
{
  const char *eos = strstr(s, "\nr ");
  if (eos) {
    const char *eos2 = tor_memstr(s, eos-s, "\ndirectory-signature");
    if (eos2 && eos2 < eos)
      return eos2;
    else
      return eos+1;
  } else {
    if ((eos = strstr(s, "\ndirectory-signature")))
      return eos+1;
    return s + strlen(s);
  }
}

/** Given a string at *<b>s</b>, containing a routerstatus object, and an
 * empty smartlist at <b>tokens</b>, parse and return the first router status
 * object in the string, and advance *<b>s</b> to just after the end of the
 * router status.  Return NULL and advance *<b>s</b> on error.
 *
 * If <b>vote</b> and <b>vote_rs</b> are provided, don't allocate a fresh
 * routerstatus but use <b>vote_rs</b> instead.
 *
 * If <b>consensus_method</b> is nonzero, this routerstatus is part of a
 * consensus, and we should parse it according to the method used to
 * make that consensus.
 **/
static routerstatus_t *
routerstatus_parse_entry_from_string(memarea_t *area,
                                     const char **s, smartlist_t *tokens,
                                     networkstatus_t *vote,
                                     vote_routerstatus_t *vote_rs,
                                     int consensus_method)
{
  const char *eos;
  routerstatus_t *rs = NULL;
  directory_token_t *tok;
  char timebuf[ISO_TIME_LEN+1];
  struct in_addr in;
  tor_assert(tokens);
  tor_assert(bool_eq(vote, vote_rs));

  eos = find_start_of_next_routerstatus(*s);

  if (tokenize_string(area,*s, eos, tokens, rtrstatus_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing router status");
    goto err;
  }
  if (smartlist_len(tokens) < 1) {
    log_warn(LD_DIR, "Impossibly short router status");
    goto err;
  }
  tok = find_by_keyword(tokens, K_R);
  tor_assert(tok->n_args >= 8);
  if (vote_rs) {
    rs = &vote_rs->status;
  } else {
    rs = tor_malloc_zero(sizeof(routerstatus_t));
  }

  if (!is_legal_nickname(tok->args[0])) {
    log_warn(LD_DIR,
             "Invalid nickname %s in router status; skipping.",
             escaped(tok->args[0]));
    goto err;
  }
  strlcpy(rs->nickname, tok->args[0], sizeof(rs->nickname));

  if (digest_from_base64(rs->identity_digest, tok->args[1])) {
    log_warn(LD_DIR, "Error decoding identity digest %s",
             escaped(tok->args[1]));
    goto err;
  }

  if (digest_from_base64(rs->descriptor_digest, tok->args[2])) {
    log_warn(LD_DIR, "Error decoding descriptor digest %s",
             escaped(tok->args[2]));
    goto err;
  }

  if (tor_snprintf(timebuf, sizeof(timebuf), "%s %s",
                   tok->args[3], tok->args[4]) < 0 ||
      parse_iso_time(timebuf, &rs->published_on)<0) {
    log_warn(LD_DIR, "Error parsing time '%s %s'",
             tok->args[3], tok->args[4]);
    goto err;
  }

  if (tor_inet_aton(tok->args[5], &in) == 0) {
    log_warn(LD_DIR, "Error parsing router address in network-status %s",
             escaped(tok->args[5]));
    goto err;
  }
  rs->addr = ntohl(in.s_addr);

  rs->or_port =(uint16_t) tor_parse_long(tok->args[6],10,0,65535,NULL,NULL);
  rs->dir_port = (uint16_t) tor_parse_long(tok->args[7],10,0,65535,NULL,NULL);

  tok = find_opt_by_keyword(tokens, K_S);
  if (tok && vote) {
    int i;
    vote_rs->flags = 0;
    for (i=0; i < tok->n_args; ++i) {
      int p = smartlist_string_pos(vote->known_flags, tok->args[i]);
      if (p >= 0) {
        vote_rs->flags |= (1<<p);
      } else {
        log_warn(LD_DIR, "Flags line had a flag %s not listed in known_flags.",
                 escaped(tok->args[i]));
        goto err;
      }
    }
  } else if (tok) {
    int i;
    for (i=0; i < tok->n_args; ++i) {
      if (!strcmp(tok->args[i], "Exit"))
        rs->is_exit = 1;
      else if (!strcmp(tok->args[i], "Stable"))
        rs->is_stable = 1;
      else if (!strcmp(tok->args[i], "Fast"))
        rs->is_fast = 1;
      else if (!strcmp(tok->args[i], "Running"))
        rs->is_running = 1;
      else if (!strcmp(tok->args[i], "Named"))
        rs->is_named = 1;
      else if (!strcmp(tok->args[i], "Valid"))
        rs->is_valid = 1;
      else if (!strcmp(tok->args[i], "V2Dir"))
        rs->is_v2_dir = 1;
      else if (!strcmp(tok->args[i], "Guard"))
        rs->is_possible_guard = 1;
      else if (!strcmp(tok->args[i], "BadExit"))
        rs->is_bad_exit = 1;
      else if (!strcmp(tok->args[i], "BadDirectory"))
        rs->is_bad_directory = 1;
      else if (!strcmp(tok->args[i], "Authority"))
        rs->is_authority = 1;
      else if (!strcmp(tok->args[i], "Unnamed") &&
               consensus_method >= 2) {
        /* Unnamed is computed right by consensus method 2 and later. */
        rs->is_unnamed = 1;
      } else if (!strcmp(tok->args[i], "HSDir")) {
        rs->is_hs_dir = 1;
      }
    }
  }
  if ((tok = find_opt_by_keyword(tokens, K_V))) {
    tor_assert(tok->n_args == 1);
    rs->version_known = 1;
    if (strcmpstart(tok->args[0], "Tor ")) {
      rs->version_supports_begindir = 1;
      rs->version_supports_extrainfo_upload = 1;
      rs->version_supports_conditional_consensus = 1;
    } else {
      rs->version_supports_begindir =
        tor_version_as_new_as(tok->args[0], "0.2.0.1-alpha");
      rs->version_supports_extrainfo_upload =
        tor_version_as_new_as(tok->args[0], "0.2.0.0-alpha-dev (r10070)");
      rs->version_supports_v3_dir =
        tor_version_as_new_as(tok->args[0], "0.2.0.8-alpha");
      rs->version_supports_conditional_consensus =
        tor_version_as_new_as(tok->args[0], "0.2.1.1-alpha");
    }
    if (vote_rs) {
      vote_rs->version = tor_strdup(tok->args[0]);
    }
  }

  /* handle weighting/bandwidth info */
  if ((tok = find_opt_by_keyword(tokens, K_W))) {
    int i;
    for (i=0; i < tok->n_args; ++i) {
      if (!strcmpstart(tok->args[i], "Bandwidth=")) {
        int ok;
        rs->bandwidth = tor_parse_ulong(strchr(tok->args[i], '=')+1, 10,
                                        0, UINT32_MAX, &ok, NULL);
        if (!ok) {
          log_warn(LD_DIR, "Invalid Bandwidth %s", escaped(tok->args[i]));
          goto err;
        }
        rs->has_bandwidth = 1;
      }
    }
  }

  /* parse exit policy summaries */
  if ((tok = find_opt_by_keyword(tokens, K_P))) {
    tor_assert(tok->n_args == 1);
    if (strcmpstart(tok->args[0], "accept ") &&
        strcmpstart(tok->args[0], "reject ")) {
      log_err(LD_DIR, "Unknown exit policy summary type %s.",
               escaped(tok->args[0]));
      goto err;
    }
    /* XXX weasel: parse this into ports and represent them somehow smart,
     * maybe not here but somewhere on if we need it for the client.
     * we should still parse it here to check it's valid tho.
     */
    rs->exitsummary = tor_strdup(tok->args[0]);
    rs->has_exitsummary = 1;
  }

  if (!strcasecmp(rs->nickname, UNNAMED_ROUTER_NICKNAME))
    rs->is_named = 0;

  goto done;
 err:
  if (rs && !vote_rs)
    routerstatus_free(rs);
  rs = NULL;
 done:
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_clear(tokens);
  if (area) {
    DUMP_AREA(area, "routerstatus entry");
    memarea_clear(area);
  }
  *s = eos;

  return rs;
}

/** Helper to sort a smartlist of pointers to routerstatus_t */
static int
_compare_routerstatus_entries(const void **_a, const void **_b)
{
  const routerstatus_t *a = *_a, *b = *_b;
  return memcmp(a->identity_digest, b->identity_digest, DIGEST_LEN);
}

/** Helper: used in call to _smartlist_uniq to clear out duplicate entries. */
static void
_free_duplicate_routerstatus_entry(void *e)
{
  log_warn(LD_DIR,
           "Network-status has two entries for the same router. "
           "Dropping one.");
  routerstatus_free(e);
}

/** Given a v2 network-status object in <b>s</b>, try to
 * parse it and return the result.  Return NULL on failure.  Check the
 * signature of the network status, but do not (yet) check the signing key for
 * authority.
 */
networkstatus_v2_t *
networkstatus_v2_parse_from_string(const char *s)
{
  const char *eos;
  smartlist_t *tokens = smartlist_create();
  smartlist_t *footer_tokens = smartlist_create();
  networkstatus_v2_t *ns = NULL;
  char ns_digest[DIGEST_LEN];
  char tmp_digest[DIGEST_LEN];
  struct in_addr in;
  directory_token_t *tok;
  int i;
  memarea_t *area = NULL;

  if (router_get_networkstatus_v2_hash(s, ns_digest)) {
    log_warn(LD_DIR, "Unable to compute digest of network-status");
    goto err;
  }

  area = memarea_new();
  eos = find_start_of_next_routerstatus(s);
  if (tokenize_string(area, s, eos, tokens, netstatus_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing network-status header.");
    goto err;
  }
  ns = tor_malloc_zero(sizeof(networkstatus_v2_t));
  memcpy(ns->networkstatus_digest, ns_digest, DIGEST_LEN);

  tok = find_by_keyword(tokens, K_NETWORK_STATUS_VERSION);
  tor_assert(tok->n_args >= 1);
  if (strcmp(tok->args[0], "2")) {
    log_warn(LD_BUG, "Got a non-v2 networkstatus. Version was "
             "%s", escaped(tok->args[0]));
    goto err;
  }

  tok = find_by_keyword(tokens, K_DIR_SOURCE);
  tor_assert(tok->n_args >= 3);
  ns->source_address = tor_strdup(tok->args[0]);
  if (tor_inet_aton(tok->args[1], &in) == 0) {
    log_warn(LD_DIR, "Error parsing network-status source address %s",
             escaped(tok->args[1]));
    goto err;
  }
  ns->source_addr = ntohl(in.s_addr);
  ns->source_dirport =
    (uint16_t) tor_parse_long(tok->args[2],10,0,65535,NULL,NULL);
  if (ns->source_dirport == 0) {
    log_warn(LD_DIR, "Directory source without dirport; skipping.");
    goto err;
  }

  tok = find_by_keyword(tokens, K_FINGERPRINT);
  tor_assert(tok->n_args);
  if (base16_decode(ns->identity_digest, DIGEST_LEN, tok->args[0],
                    strlen(tok->args[0]))) {
    log_warn(LD_DIR, "Couldn't decode networkstatus fingerprint %s",
             escaped(tok->args[0]));
    goto err;
  }

  if ((tok = find_opt_by_keyword(tokens, K_CONTACT))) {
    tor_assert(tok->n_args);
    ns->contact = tor_strdup(tok->args[0]);
  }

  tok = find_by_keyword(tokens, K_DIR_SIGNING_KEY);
  tor_assert(tok->key);
  ns->signing_key = tok->key;
  tok->key = NULL;

  if (crypto_pk_get_digest(ns->signing_key, tmp_digest)<0) {
    log_warn(LD_DIR, "Couldn't compute signing key digest");
    goto err;
  }
  if (memcmp(tmp_digest, ns->identity_digest, DIGEST_LEN)) {
    log_warn(LD_DIR,
             "network-status fingerprint did not match dir-signing-key");
    goto err;
  }

  if ((tok = find_opt_by_keyword(tokens, K_DIR_OPTIONS))) {
    for (i=0; i < tok->n_args; ++i) {
      if (!strcmp(tok->args[i], "Names"))
        ns->binds_names = 1;
      if (!strcmp(tok->args[i], "Versions"))
        ns->recommends_versions = 1;
      if (!strcmp(tok->args[i], "BadExits"))
        ns->lists_bad_exits = 1;
      if (!strcmp(tok->args[i], "BadDirectories"))
        ns->lists_bad_directories = 1;
    }
  }

  if (ns->recommends_versions) {
    if (!(tok = find_opt_by_keyword(tokens, K_CLIENT_VERSIONS))) {
      log_warn(LD_DIR, "Missing client-versions on versioning directory");
      goto err;
    }
    ns->client_versions = tor_strdup(tok->args[0]);

    if (!(tok = find_opt_by_keyword(tokens, K_SERVER_VERSIONS)) ||
        tok->n_args<1) {
      log_warn(LD_DIR, "Missing server-versions on versioning directory");
      goto err;
    }
    ns->server_versions = tor_strdup(tok->args[0]);
  }

  tok = find_by_keyword(tokens, K_PUBLISHED);
  tor_assert(tok->n_args == 1);
  if (parse_iso_time(tok->args[0], &ns->published_on) < 0) {
     goto err;
  }

  ns->entries = smartlist_create();
  s = eos;
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_clear(tokens);
  memarea_clear(area);
  while (!strcmpstart(s, "r ")) {
    routerstatus_t *rs;
    if ((rs = routerstatus_parse_entry_from_string(area, &s, tokens,
                                                   NULL, NULL, 0)))
      smartlist_add(ns->entries, rs);
  }
  smartlist_sort(ns->entries, _compare_routerstatus_entries);
  smartlist_uniq(ns->entries, _compare_routerstatus_entries,
                 _free_duplicate_routerstatus_entry);

  if (tokenize_string(area,s, NULL, footer_tokens, dir_footer_token_table,0)) {
    log_warn(LD_DIR, "Error tokenizing network-status footer.");
    goto err;
  }
  if (smartlist_len(footer_tokens) < 1) {
    log_warn(LD_DIR, "Too few items in network-status footer.");
    goto err;
  }
  tok = smartlist_get(footer_tokens, smartlist_len(footer_tokens)-1);
  if (tok->tp != K_DIRECTORY_SIGNATURE) {
    log_warn(LD_DIR,
             "Expected network-status footer to end with a signature.");
    goto err;
  }

  note_crypto_pk_op(VERIFY_DIR);
  if (check_signature_token(ns_digest, tok, ns->signing_key, 0,
                            "network-status") < 0)
    goto err;

  goto done;
 err:
  if (ns)
    networkstatus_v2_free(ns);
  ns = NULL;
 done:
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_free(tokens);
  SMARTLIST_FOREACH(footer_tokens, directory_token_t *, t, token_free(t));
  smartlist_free(footer_tokens);
  if (area) {
    DUMP_AREA(area, "v2 networkstatus");
    memarea_drop_all(area);
  }
  return ns;
}

/** Parse a v3 networkstatus vote, opinion, or consensus (depending on
 * ns_type), from <b>s</b>, and return the result.  Return NULL on failure. */
networkstatus_t *
networkstatus_parse_vote_from_string(const char *s, const char **eos_out,
                                     networkstatus_type_t ns_type)
{
  smartlist_t *tokens = smartlist_create();
  smartlist_t *rs_tokens = NULL, *footer_tokens = NULL;
  networkstatus_voter_info_t *voter = NULL;
  networkstatus_t *ns = NULL;
  char ns_digest[DIGEST_LEN];
  const char *cert, *end_of_header, *end_of_footer;
  directory_token_t *tok;
  int ok;
  struct in_addr in;
  int i, inorder, n_signatures = 0;
  memarea_t *area = NULL, *rs_area = NULL;
  tor_assert(s);

  if (router_get_networkstatus_v3_hash(s, ns_digest)) {
    log_warn(LD_DIR, "Unable to compute digest of network-status");
    goto err;
  }

  area = memarea_new();
  end_of_header = find_start_of_next_routerstatus(s);
  if (tokenize_string(area, s, end_of_header, tokens,
                      (ns_type == NS_TYPE_CONSENSUS) ?
                      networkstatus_consensus_token_table :
                      networkstatus_token_table, 0)) {
    log_warn(LD_DIR, "Error tokenizing network-status vote header");
    goto err;
  }

  ns = tor_malloc_zero(sizeof(networkstatus_t));
  memcpy(ns->networkstatus_digest, ns_digest, DIGEST_LEN);

  if (ns_type != NS_TYPE_CONSENSUS) {
    const char *end_of_cert = NULL;
    if (!(cert = strstr(s, "\ndir-key-certificate-version")))
      goto err;
    ++cert;
    ns->cert = authority_cert_parse_from_string(cert, &end_of_cert);
    if (!ns->cert || !end_of_cert || end_of_cert > end_of_header)
      goto err;
  }

  tok = find_by_keyword(tokens, K_VOTE_STATUS);
  tor_assert(tok->n_args);
  if (!strcmp(tok->args[0], "vote")) {
    ns->type = NS_TYPE_VOTE;
  } else if (!strcmp(tok->args[0], "consensus")) {
    ns->type = NS_TYPE_CONSENSUS;
  } else if (!strcmp(tok->args[0], "opinion")) {
    ns->type = NS_TYPE_OPINION;
  } else {
    log_warn(LD_DIR, "Unrecognized vote status %s in network-status",
             escaped(tok->args[0]));
    goto err;
  }
  if (ns_type != ns->type) {
    log_warn(LD_DIR, "Got the wrong kind of v3 networkstatus.");
    goto err;
  }

  if (ns->type == NS_TYPE_VOTE || ns->type == NS_TYPE_OPINION) {
    tok = find_by_keyword(tokens, K_PUBLISHED);
    if (parse_iso_time(tok->args[0], &ns->published))
      goto err;

    ns->supported_methods = smartlist_create();
    tok = find_opt_by_keyword(tokens, K_CONSENSUS_METHODS);
    if (tok) {
      for (i=0; i < tok->n_args; ++i)
        smartlist_add(ns->supported_methods, tor_strdup(tok->args[i]));
    } else {
      smartlist_add(ns->supported_methods, tor_strdup("1"));
    }
  } else {
    tok = find_opt_by_keyword(tokens, K_CONSENSUS_METHOD);
    if (tok) {
      ns->consensus_method = (int)tor_parse_long(tok->args[0], 10, 1, INT_MAX,
                                                 &ok, NULL);
      if (!ok)
        goto err;
    } else {
      ns->consensus_method = 1;
    }
  }

  tok = find_by_keyword(tokens, K_VALID_AFTER);
  if (parse_iso_time(tok->args[0], &ns->valid_after))
    goto err;

  tok = find_by_keyword(tokens, K_FRESH_UNTIL);
  if (parse_iso_time(tok->args[0], &ns->fresh_until))
    goto err;

  tok = find_by_keyword(tokens, K_VALID_UNTIL);
  if (parse_iso_time(tok->args[0], &ns->valid_until))
    goto err;

  tok = find_by_keyword(tokens, K_VOTING_DELAY);
  tor_assert(tok->n_args >= 2);
  ns->vote_seconds =
    (int) tor_parse_long(tok->args[0], 10, 0, INT_MAX, &ok, NULL);
  if (!ok)
    goto err;
  ns->dist_seconds =
    (int) tor_parse_long(tok->args[1], 10, 0, INT_MAX, &ok, NULL);
  if (!ok)
    goto err;
  if (ns->valid_after + MIN_VOTE_INTERVAL > ns->fresh_until) {
    log_warn(LD_DIR, "Vote/consensus freshness interval is too short");
    goto err;
  }
  if (ns->valid_after + MIN_VOTE_INTERVAL*2 > ns->valid_until) {
    log_warn(LD_DIR, "Vote/consensus liveness interval is too short");
    goto err;
  }
  if (ns->vote_seconds < MIN_VOTE_SECONDS) {
    log_warn(LD_DIR, "Vote seconds is too short");
    goto err;
  }
  if (ns->dist_seconds < MIN_DIST_SECONDS) {
    log_warn(LD_DIR, "Dist seconds is too short");
    goto err;
  }

  if ((tok = find_opt_by_keyword(tokens, K_CLIENT_VERSIONS))) {
    ns->client_versions = tor_strdup(tok->args[0]);
  }
  if ((tok = find_opt_by_keyword(tokens, K_SERVER_VERSIONS))) {
    ns->server_versions = tor_strdup(tok->args[0]);
  }

  tok = find_by_keyword(tokens, K_KNOWN_FLAGS);
  ns->known_flags = smartlist_create();
  inorder = 1;
  for (i = 0; i < tok->n_args; ++i) {
    smartlist_add(ns->known_flags, tor_strdup(tok->args[i]));
    if (i>0 && strcmp(tok->args[i-1], tok->args[i])>= 0) {
      log_warn(LD_DIR, "%s >= %s", tok->args[i-1], tok->args[i]);
      inorder = 0;
    }
  }
  if (!inorder) {
    log_warn(LD_DIR, "known-flags not in order");
    goto err;
  }

  ns->voters = smartlist_create();

  SMARTLIST_FOREACH(tokens, directory_token_t *, _tok,
  {
    tok = _tok;
    if (tok->tp == K_DIR_SOURCE) {
      tor_assert(tok->n_args >= 6);

      if (voter)
        smartlist_add(ns->voters, voter);
      voter = tor_malloc_zero(sizeof(networkstatus_voter_info_t));
      if (ns->type != NS_TYPE_CONSENSUS)
        memcpy(voter->vote_digest, ns_digest, DIGEST_LEN);

      voter->nickname = tor_strdup(tok->args[0]);
      if (strlen(tok->args[1]) != HEX_DIGEST_LEN ||
          base16_decode(voter->identity_digest, sizeof(voter->identity_digest),
                        tok->args[1], HEX_DIGEST_LEN) < 0) {
        log_warn(LD_DIR, "Error decoding identity digest %s in "
                 "network-status vote.", escaped(tok->args[1]));
        goto err;
      }
      if (ns->type != NS_TYPE_CONSENSUS &&
          memcmp(ns->cert->cache_info.identity_digest,
                 voter->identity_digest, DIGEST_LEN)) {
        log_warn(LD_DIR,"Mismatch between identities in certificate and vote");
        goto err;
      }
      voter->address = tor_strdup(tok->args[2]);
      if (!tor_inet_aton(tok->args[3], &in)) {
        log_warn(LD_DIR, "Error decoding IP address %s in network-status.",
                 escaped(tok->args[3]));
        goto err;
      }
      voter->addr = ntohl(in.s_addr);
      voter->dir_port = (uint16_t)
        tor_parse_long(tok->args[4], 10, 0, 65535, &ok, NULL);
      if (!ok)
        goto err;
      voter->or_port = (uint16_t)
        tor_parse_long(tok->args[5], 10, 0, 65535, &ok, NULL);
      if (!ok)
        goto err;
    } else if (tok->tp == K_CONTACT) {
      if (!voter || voter->contact) {
        log_warn(LD_DIR, "contact element is out of place.");
        goto err;
      }
      voter->contact = tor_strdup(tok->args[0]);
    } else if (tok->tp == K_VOTE_DIGEST) {
      tor_assert(ns->type == NS_TYPE_CONSENSUS);
      tor_assert(tok->n_args >= 1);
      if (!voter || ! tor_digest_is_zero(voter->vote_digest)) {
        log_warn(LD_DIR, "vote-digest element is out of place.");
        goto err;
      }
      if (strlen(tok->args[0]) != HEX_DIGEST_LEN ||
        base16_decode(voter->vote_digest, sizeof(voter->vote_digest),
                      tok->args[0], HEX_DIGEST_LEN) < 0) {
        log_warn(LD_DIR, "Error decoding vote digest %s in "
                 "network-status consensus.", escaped(tok->args[1]));
        goto err;
      }
    }
  });
  if (voter) {
    smartlist_add(ns->voters, voter);
    voter = NULL;
  }
  if (smartlist_len(ns->voters) == 0) {
    log_warn(LD_DIR, "Missing dir-source elements in a vote networkstatus.");
    goto err;
  } else if (ns->type != NS_TYPE_CONSENSUS && smartlist_len(ns->voters) != 1) {
    log_warn(LD_DIR, "Too many dir-source elements in a vote networkstatus.");
    goto err;
  }

  if (ns->type != NS_TYPE_CONSENSUS &&
      (tok = find_opt_by_keyword(tokens, K_LEGACY_DIR_KEY))) {
    int bad = 1;
    if (strlen(tok->args[0]) == HEX_DIGEST_LEN) {
      networkstatus_voter_info_t *voter = smartlist_get(ns->voters, 0);
      if (base16_decode(voter->legacy_id_digest, DIGEST_LEN,
                        tok->args[0], HEX_DIGEST_LEN)<0)
        bad = 1;
      else
        bad = 0;
    }
    if (bad) {
      log_warn(LD_DIR, "Invalid legacy key digest %s on vote.",
               escaped(tok->args[0]));
    }
  }

  /* Parse routerstatus lines. */
  rs_tokens = smartlist_create();
  rs_area = memarea_new();
  s = end_of_header;
  ns->routerstatus_list = smartlist_create();

  while (!strcmpstart(s, "r ")) {
    if (ns->type != NS_TYPE_CONSENSUS) {
      vote_routerstatus_t *rs = tor_malloc_zero(sizeof(vote_routerstatus_t));
      if (routerstatus_parse_entry_from_string(rs_area, &s, rs_tokens, ns,
                                               rs, 0))
        smartlist_add(ns->routerstatus_list, rs);
      else {
        tor_free(rs->version);
        tor_free(rs);
      }
    } else {
      routerstatus_t *rs;
      if ((rs = routerstatus_parse_entry_from_string(rs_area, &s, rs_tokens,
                                                     NULL, NULL,
                                                     ns->consensus_method)))
        smartlist_add(ns->routerstatus_list, rs);
    }
  }
  for (i = 1; i < smartlist_len(ns->routerstatus_list); ++i) {
    routerstatus_t *rs1, *rs2;
    if (ns->type != NS_TYPE_CONSENSUS) {
      vote_routerstatus_t *a = smartlist_get(ns->routerstatus_list, i-1);
      vote_routerstatus_t *b = smartlist_get(ns->routerstatus_list, i);
      rs1 = &a->status; rs2 = &b->status;
    } else {
      rs1 = smartlist_get(ns->routerstatus_list, i-1);
      rs2 = smartlist_get(ns->routerstatus_list, i);
    }
    if (memcmp(rs1->identity_digest, rs2->identity_digest, DIGEST_LEN) >= 0) {
      log_warn(LD_DIR, "Vote networkstatus entries not sorted by identity "
               "digest");
      goto err;
    }
  }

  /* Parse footer; check signature. */
  footer_tokens = smartlist_create();
  if ((end_of_footer = strstr(s, "\nnetwork-status-version ")))
    ++end_of_footer;
  else
    end_of_footer = s + strlen(s);
  if (tokenize_string(area,s, end_of_footer, footer_tokens,
                      networkstatus_vote_footer_token_table, 0)) {
    log_warn(LD_DIR, "Error tokenizing network-status vote footer.");
    goto err;
  }

  SMARTLIST_FOREACH(footer_tokens, directory_token_t *, _tok,
  {
    char declared_identity[DIGEST_LEN];
    networkstatus_voter_info_t *v;
    tok = _tok;
    if (tok->tp != K_DIRECTORY_SIGNATURE)
      continue;
    tor_assert(tok->n_args >= 2);

    if (!tok->object_type ||
        strcmp(tok->object_type, "SIGNATURE") ||
        tok->object_size < 128 || tok->object_size > 512) {
      log_warn(LD_DIR, "Bad object type or length on directory-signature");
      goto err;
    }

    if (strlen(tok->args[0]) != HEX_DIGEST_LEN ||
        base16_decode(declared_identity, sizeof(declared_identity),
                      tok->args[0], HEX_DIGEST_LEN) < 0) {
      log_warn(LD_DIR, "Error decoding declared identity %s in "
               "network-status vote.", escaped(tok->args[0]));
      goto err;
    }
    if (!(v = networkstatus_get_voter_by_id(ns, declared_identity))) {
      log_warn(LD_DIR, "ID on signature on network-status vote does not match "
               "any declared directory source.");
      goto err;
    }
    if (strlen(tok->args[1]) != HEX_DIGEST_LEN ||
        base16_decode(v->signing_key_digest, sizeof(v->signing_key_digest),
                      tok->args[1], HEX_DIGEST_LEN) < 0) {
      log_warn(LD_DIR, "Error decoding declared digest %s in "
               "network-status vote.", escaped(tok->args[1]));
      goto err;
    }

    if (ns->type != NS_TYPE_CONSENSUS) {
      if (memcmp(declared_identity, ns->cert->cache_info.identity_digest,
                 DIGEST_LEN)) {
        log_warn(LD_DIR, "Digest mismatch between declared and actual on "
                 "network-status vote.");
        goto err;
      }
    }

    if (ns->type != NS_TYPE_CONSENSUS) {
      if (check_signature_token(ns_digest, tok, ns->cert->signing_key, 0,
                                "network-status vote"))
        goto err;
      v->good_signature = 1;
    } else {
      if (tok->object_size >= INT_MAX)
        goto err;
      v->signature = tor_memdup(tok->object_body, tok->object_size);
      v->signature_len = (int) tok->object_size;
    }
    ++n_signatures;
  });

  if (! n_signatures) {
    log_warn(LD_DIR, "No signatures on networkstatus vote.");
    goto err;
  }

  if (eos_out)
    *eos_out = end_of_footer;

  goto done;
 err:
  if (ns)
    networkstatus_vote_free(ns);
  ns = NULL;
 done:
  if (tokens) {
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_free(tokens);
  }
  if (voter) {
    tor_free(voter->nickname);
    tor_free(voter->address);
    tor_free(voter->contact);
    tor_free(voter->signature);
    tor_free(voter);
  }
  if (rs_tokens) {
    SMARTLIST_FOREACH(rs_tokens, directory_token_t *, t, token_free(t));
    smartlist_free(rs_tokens);
  }
  if (footer_tokens) {
    SMARTLIST_FOREACH(footer_tokens, directory_token_t *, t, token_free(t));
    smartlist_free(footer_tokens);
  }
  if (area) {
    DUMP_AREA(area, "v3 networkstatus");
    memarea_drop_all(area);
  }
  if (rs_area)
    memarea_drop_all(rs_area);

  return ns;
}

/** Parse a detached v3 networkstatus signature document between <b>s</b> and
 * <b>eos</b> and return the result.  Return -1 on failure. */
ns_detached_signatures_t *
networkstatus_parse_detached_signatures(const char *s, const char *eos)
{
  /* XXXX there is too much duplicate shared between this function and
   * networkstatus_parse_vote_from_string(). */
  directory_token_t *tok;
  memarea_t *area = NULL;

  smartlist_t *tokens = smartlist_create();
  ns_detached_signatures_t *sigs =
    tor_malloc_zero(sizeof(ns_detached_signatures_t));

  if (!eos)
    eos = s + strlen(s);

  area = memarea_new();
  if (tokenize_string(area,s, eos, tokens,
                      networkstatus_detached_signature_token_table, 0)) {
    log_warn(LD_DIR, "Error tokenizing detached networkstatus signatures");
    goto err;
  }

  tok = find_by_keyword(tokens, K_CONSENSUS_DIGEST);
  if (strlen(tok->args[0]) != HEX_DIGEST_LEN) {
    log_warn(LD_DIR, "Wrong length on consensus-digest in detached "
             "networkstatus signatures");
    goto err;
  }
  if (base16_decode(sigs->networkstatus_digest, DIGEST_LEN,
                    tok->args[0], strlen(tok->args[0])) < 0) {
    log_warn(LD_DIR, "Bad encoding on on consensus-digest in detached "
             "networkstatus signatures");
    goto err;
  }

  tok = find_by_keyword(tokens, K_VALID_AFTER);
  if (parse_iso_time(tok->args[0], &sigs->valid_after)) {
    log_warn(LD_DIR, "Bad valid-after in detached networkstatus signatures");
    goto err;
  }

  tok = find_by_keyword(tokens, K_FRESH_UNTIL);
  if (parse_iso_time(tok->args[0], &sigs->fresh_until)) {
    log_warn(LD_DIR, "Bad fresh-until in detached networkstatus signatures");
    goto err;
  }

  tok = find_by_keyword(tokens, K_VALID_UNTIL);
  if (parse_iso_time(tok->args[0], &sigs->valid_until)) {
    log_warn(LD_DIR, "Bad valid-until in detached networkstatus signatures");
    goto err;
  }

  sigs->signatures = smartlist_create();
  SMARTLIST_FOREACH(tokens, directory_token_t *, _tok,
    {
      char id_digest[DIGEST_LEN];
      char sk_digest[DIGEST_LEN];
      networkstatus_voter_info_t *voter;

      tok = _tok;
      if (tok->tp != K_DIRECTORY_SIGNATURE)
        continue;
      tor_assert(tok->n_args >= 2);

      if (!tok->object_type ||
          strcmp(tok->object_type, "SIGNATURE") ||
          tok->object_size < 128 || tok->object_size > 512) {
        log_warn(LD_DIR, "Bad object type or length on directory-signature");
        goto err;
      }

      if (strlen(tok->args[0]) != HEX_DIGEST_LEN ||
          base16_decode(id_digest, sizeof(id_digest),
                        tok->args[0], HEX_DIGEST_LEN) < 0) {
        log_warn(LD_DIR, "Error decoding declared identity %s in "
                 "network-status vote.", escaped(tok->args[0]));
        goto err;
      }
      if (strlen(tok->args[1]) != HEX_DIGEST_LEN ||
          base16_decode(sk_digest, sizeof(sk_digest),
                        tok->args[1], HEX_DIGEST_LEN) < 0) {
        log_warn(LD_DIR, "Error decoding declared digest %s in "
                 "network-status vote.", escaped(tok->args[1]));
        goto err;
      }

      voter = tor_malloc_zero(sizeof(networkstatus_voter_info_t));
      memcpy(voter->identity_digest, id_digest, DIGEST_LEN);
      memcpy(voter->signing_key_digest, sk_digest, DIGEST_LEN);
      if (tok->object_size >= INT_MAX)
        goto err;
      voter->signature = tor_memdup(tok->object_body, tok->object_size);
      voter->signature_len = (int) tok->object_size;

      smartlist_add(sigs->signatures, voter);
    });

  goto done;
 err:
  ns_detached_signatures_free(sigs);
  sigs = NULL;
 done:
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_free(tokens);
  if (area) {
    DUMP_AREA(area, "detached signatures");
    memarea_drop_all(area);
  }
  return sigs;
}

/** Parse the addr policy in the string <b>s</b> and return it.  If
 * assume_action is nonnegative, then insert its action (ADDR_POLICY_ACCEPT or
 * ADDR_POLICY_REJECT) for items that specify no action.
 */
addr_policy_t *
router_parse_addr_policy_item_from_string(const char *s, int assume_action)
{
  directory_token_t *tok = NULL;
  const char *cp, *eos;
  /* Longest possible policy is "accept ffff:ffff:..255/ffff:...255:0-65535".
   * But note that there can be an arbitrary amount of space between the
   * accept and the address:mask/port element. */
  char line[TOR_ADDR_BUF_LEN*2 + 32];
  addr_policy_t *r;
  memarea_t *area = NULL;

  s = eat_whitespace(s);
  if ((*s == '*' || TOR_ISDIGIT(*s)) && assume_action >= 0) {
    if (tor_snprintf(line, sizeof(line), "%s %s",
               assume_action == ADDR_POLICY_ACCEPT?"accept":"reject", s)<0) {
      log_warn(LD_DIR, "Policy %s is too long.", escaped(s));
      return NULL;
    }
    cp = line;
    tor_strlower(line);
  } else { /* assume an already well-formed address policy line */
    cp = s;
  }

  eos = cp + strlen(cp);
  area = memarea_new();
  tok = get_next_token(area, &cp, eos, routerdesc_token_table);
  if (tok->tp == _ERR) {
    log_warn(LD_DIR, "Error reading address policy: %s", tok->error);
    goto err;
  }
  if (tok->tp != K_ACCEPT && tok->tp != K_ACCEPT6 &&
      tok->tp != K_REJECT && tok->tp != K_REJECT6) {
    log_warn(LD_DIR, "Expected 'accept' or 'reject'.");
    goto err;
  }

  r = router_parse_addr_policy(tok);
  goto done;
 err:
  r = NULL;
 done:
  token_free(tok);
  if (area) {
    DUMP_AREA(area, "policy item");
    memarea_drop_all(area);
  }
  return r;
}

/** Add an exit policy stored in the token <b>tok</b> to the router info in
 * <b>router</b>.  Return 0 on success, -1 on failure. */
static int
router_add_exit_policy(routerinfo_t *router, directory_token_t *tok)
{
  addr_policy_t *newe;
  newe = router_parse_addr_policy(tok);
  if (!newe)
    return -1;
  if (! router->exit_policy)
    router->exit_policy = smartlist_create();

  if (((tok->tp == K_ACCEPT6 || tok->tp == K_REJECT6) &&
       tor_addr_family(&newe->addr) == AF_INET)
      ||
      ((tok->tp == K_ACCEPT || tok->tp == K_REJECT) &&
       tor_addr_family(&newe->addr) == AF_INET6)) {
    log_warn(LD_DIR, "Mismatch between field type and address type in exit "
             "policy");
    addr_policy_free(newe);
    return -1;
  }

  smartlist_add(router->exit_policy, newe);

  return 0;
}

/** Given a K_ACCEPT or K_REJECT token and a router, create and return
 * a new exit_policy_t corresponding to the token. */
static addr_policy_t *
router_parse_addr_policy(directory_token_t *tok)
{
  addr_policy_t newe;
  char *arg;

  tor_assert(tok->tp == K_REJECT || tok->tp == K_REJECT6 ||
             tok->tp == K_ACCEPT || tok->tp == K_ACCEPT6);

  if (tok->n_args != 1)
    return NULL;
  arg = tok->args[0];

  if (!strcmpstart(arg,"private"))
    return router_parse_addr_policy_private(tok);

  memset(&newe, 0, sizeof(newe));

  if (tok->tp == K_REJECT || tok->tp == K_REJECT6)
    newe.policy_type = ADDR_POLICY_REJECT;
  else
    newe.policy_type = ADDR_POLICY_ACCEPT;

  if (tor_addr_parse_mask_ports(arg, &newe.addr, &newe.maskbits,
                                &newe.prt_min, &newe.prt_max) < 0) {
    log_warn(LD_DIR,"Couldn't parse line %s. Dropping", escaped(arg));
    return NULL;
  }

  return addr_policy_get_canonical_entry(&newe);
}

/** Parse an exit policy line of the format "accept/reject private:...".
 * This didn't exist until Tor 0.1.1.15, so nobody should generate it in
 * router descriptors until earlier versions are obsolete.
 */
static addr_policy_t *
router_parse_addr_policy_private(directory_token_t *tok)
{
  const char *arg;
  uint16_t port_min, port_max;
  addr_policy_t result;

  arg = tok->args[0];
  if (strcmpstart(arg, "private"))
    return NULL;

  arg += strlen("private");
  arg = (char*) eat_whitespace(arg);
  if (!arg || *arg != ':')
    return NULL;

  if (parse_port_range(arg+1, &port_min, &port_max)<0)
    return NULL;

  memset(&result, 0, sizeof(result));
  if (tok->tp == K_REJECT || tok->tp == K_REJECT6)
    result.policy_type = ADDR_POLICY_REJECT;
  else
    result.policy_type = ADDR_POLICY_ACCEPT;
  result.is_private = 1;
  result.prt_min = port_min;
  result.prt_max = port_max;

  return addr_policy_get_canonical_entry(&result);
}

/** Log and exit if <b>t</b> is malformed */
void
assert_addr_policy_ok(smartlist_t *lst)
{
  if (!lst) return;
  SMARTLIST_FOREACH(lst, addr_policy_t *, t, {
    tor_assert(t->policy_type == ADDR_POLICY_REJECT ||
               t->policy_type == ADDR_POLICY_ACCEPT);
    tor_assert(t->prt_min <= t->prt_max);
  });
}

/*
 * Low-level tokenizer for router descriptors and directories.
 */

/** Free all resources allocated for <b>tok</b> */
static void
token_free(directory_token_t *tok)
{
  tor_assert(tok);
  if (tok->key)
    crypto_free_pk_env(tok->key);
}

#define ALLOC_ZERO(sz) memarea_alloc_zero(area,sz)
#define ALLOC(sz) memarea_alloc(area,sz)
#define STRDUP(str) memarea_strdup(area,str)
#define STRNDUP(str,n) memarea_strndup(area,(str),(n))

#define RET_ERR(msg)                                               \
  STMT_BEGIN                                                       \
    if (tok) token_free(tok);                                      \
    tok = ALLOC_ZERO(sizeof(directory_token_t));                   \
    tok->tp = _ERR;                                                \
    tok->error = STRDUP(msg);                                      \
    goto done_tokenizing;                                          \
  STMT_END

/** Helper: make sure that the token <b>tok</b> with keyword <b>kwd</b> obeys
 * the object syntax of <b>o_syn</b>.  Allocate all storage in <b>area</b>.
 * Return <b>tok</b> on success, or a new _ERR token if the token didn't
 * conform to the syntax we wanted.
 **/
static INLINE directory_token_t *
token_check_object(memarea_t *area, const char *kwd,
                   directory_token_t *tok, obj_syntax o_syn)
{
  char ebuf[128];
  switch (o_syn) {
    case NO_OBJ:
      /* No object is allowed for this token. */
      if (tok->object_body) {
        tor_snprintf(ebuf, sizeof(ebuf), "Unexpected object for %s", kwd);
        RET_ERR(ebuf);
      }
      if (tok->key) {
        tor_snprintf(ebuf, sizeof(ebuf), "Unexpected public key for %s", kwd);
        RET_ERR(ebuf);
      }
      break;
    case NEED_OBJ:
      /* There must be a (non-key) object. */
      if (!tok->object_body) {
        tor_snprintf(ebuf, sizeof(ebuf), "Missing object for %s", kwd);
        RET_ERR(ebuf);
      }
      break;
    case NEED_KEY_1024: /* There must be a 1024-bit public key. */
    case NEED_SKEY_1024: /* There must be a 1024-bit private key. */
      if (tok->key && crypto_pk_keysize(tok->key) != PK_BYTES) {
        tor_snprintf(ebuf, sizeof(ebuf), "Wrong size on key for %s: %d bits",
                     kwd, (int)crypto_pk_keysize(tok->key));
        RET_ERR(ebuf);
      }
      /* fall through */
    case NEED_KEY: /* There must be some kind of key. */
      if (!tok->key) {
        tor_snprintf(ebuf, sizeof(ebuf), "Missing public key for %s", kwd);
      }
      if (o_syn != NEED_SKEY_1024) {
        if (crypto_pk_key_is_private(tok->key)) {
          tor_snprintf(ebuf, sizeof(ebuf),
               "Private key given for %s, which wants a public key", kwd);
          RET_ERR(ebuf);
        }
      } else { /* o_syn == NEED_SKEY_1024 */
        if (!crypto_pk_key_is_private(tok->key)) {
          tor_snprintf(ebuf, sizeof(ebuf),
               "Public key given for %s, which wants a private key", kwd);
          RET_ERR(ebuf);
        }
      }
      break;
    case OBJ_OK:
      /* Anything goes with this token. */
      break;
  }

 done_tokenizing:
  return tok;
}

/** Helper: parse space-separated arguments from the string <b>s</b> ending at
 * <b>eol</b>, and store them in the args field of <b>tok</b>.  Store the
 * number of parsed elements into the n_args field of <b>tok</b>.  Allocate
 * all storage in <b>area</b>.  Return the number of arguments parsed, or
 * return -1 if there was an insanely high number of arguments. */
static INLINE int
get_token_arguments(memarea_t *area, directory_token_t *tok,
                    const char *s, const char *eol)
{
/** Largest number of arguments we'll accept to any token, ever. */
#define MAX_ARGS 512
  char *mem = memarea_strndup(area, s, eol-s);
  char *cp = mem;
  int j = 0;
  char *args[MAX_ARGS];
  while (*cp) {
    if (j == MAX_ARGS)
      return -1;
    args[j++] = cp;
    cp = (char*)find_whitespace(cp);
    if (!cp || !*cp)
      break; /* End of the line. */
    *cp++ = '\0';
    cp = (char*)eat_whitespace(cp);
  }
  tok->n_args = j;
  tok->args = memarea_memdup(area, args, j*sizeof(char*));
  return j;
#undef MAX_ARGS
}

/** Helper function: read the next token from *s, advance *s to the end of the
 * token, and return the parsed token.  Parse *<b>s</b> according to the list
 * of tokens in <b>table</b>.
 */
static directory_token_t *
get_next_token(memarea_t *area,
               const char **s, const char *eos, token_rule_t *table)
{
  const char *next, *eol, *obstart;
  size_t obname_len;
  int i;
  directory_token_t *tok;
  obj_syntax o_syn = NO_OBJ;
  char ebuf[128];
  const char *kwd = "";

  tor_assert(area);
  tok = ALLOC_ZERO(sizeof(directory_token_t));
  tok->tp = _ERR;

  /* Set *s to first token, eol to end-of-line, next to after first token */
  *s = eat_whitespace_eos(*s, eos); /* eat multi-line whitespace */
  tor_assert(eos >= *s);
  eol = memchr(*s, '\n', eos-*s);
  if (!eol)
    eol = eos;
  next = find_whitespace_eos(*s, eol);

  if (!strcmp_len(*s, "opt", next-*s)) {
    /* Skip past an "opt" at the start of the line. */
    *s = eat_whitespace_eos_no_nl(next, eol);
    next = find_whitespace_eos(*s, eol);
  } else if (*s == eos) {  /* If no "opt", and end-of-line, line is invalid */
    RET_ERR("Unexpected EOF");
  }

  /* Search the table for the appropriate entry.  (I tried a binary search
   * instead, but it wasn't any faster.) */
  for (i = 0; table[i].t ; ++i) {
    if (!strcmp_len(*s, table[i].t, next-*s)) {
      /* We've found the keyword. */
      kwd = table[i].t;
      tok->tp = table[i].v;
      o_syn = table[i].os;
      *s = eat_whitespace_eos_no_nl(next, eol);
      /* We go ahead whether there are arguments or not, so that tok->args is
       * always set if we want arguments. */
      if (table[i].concat_args) {
        /* The keyword takes the line as a single argument */
        tok->args = ALLOC(sizeof(char*));
        tok->args[0] = STRNDUP(*s,eol-*s); /* Grab everything on line */
        tok->n_args = 1;
      } else {
        /* This keyword takes multiple arguments. */
        if (get_token_arguments(area, tok, *s, eol)<0) {
          tor_snprintf(ebuf, sizeof(ebuf),"Far too many arguments to %s", kwd);
          RET_ERR(ebuf);
        }
        *s = eol;
      }
      if (tok->n_args < table[i].min_args) {
        tor_snprintf(ebuf, sizeof(ebuf), "Too few arguments to %s", kwd);
        RET_ERR(ebuf);
      } else if (tok->n_args > table[i].max_args) {
        tor_snprintf(ebuf, sizeof(ebuf), "Too many arguments to %s", kwd);
        RET_ERR(ebuf);
      }
      break;
    }
  }

  if (tok->tp == _ERR) {
    /* No keyword matched; call it an "K_opt" or "A_unrecognized" */
    if (**s == '@')
      tok->tp = _A_UNKNOWN;
    else
      tok->tp = K_OPT;
    tok->args = ALLOC(sizeof(char*));
    tok->args[0] = STRNDUP(*s, eol-*s);
    tok->n_args = 1;
    o_syn = OBJ_OK;
  }

  /* Check whether there's an object present */
  *s = eat_whitespace_eos(eol, eos);  /* Scan from end of first line */
  tor_assert(eos >= *s);
  eol = memchr(*s, '\n', eos-*s);
  if (!eol || eol-*s<11 || strcmpstart(*s, "-----BEGIN ")) /* No object. */
    goto check_object;

  obstart = *s; /* Set obstart to start of object spec */
  tor_assert(eol >= (*s+16));
  if (*s+11 >= eol-5 || memchr(*s+11,'\0',eol-*s-16) || /* no short lines, */
      strcmp_len(eol-5, "-----", 5)) {          /* nuls or invalid endings */
    RET_ERR("Malformed object: bad begin line");
  }
  tok->object_type = STRNDUP(*s+11, eol-*s-16);
  obname_len = eol-*s-16; /* store objname length here to avoid a strlen() */
  *s = eol+1;    /* Set *s to possible start of object data (could be eos) */

  /* Go to the end of the object */
  next = tor_memstr(*s, eos-*s, "-----END ");
  if (!next) {
    RET_ERR("Malformed object: missing object end line");
  }
  tor_assert(eos >= next);
  eol = memchr(next, '\n', eos-next);
  if (!eol)  /* end-of-line marker, or eos if there's no '\n' */
    eol = eos;
  /* Validate the ending tag, which should be 9 + NAME + 5 + eol */
  if ((size_t)(eol-next) != 9+obname_len+5 ||
      strcmp_len(next+9, tok->object_type, obname_len) ||
      strcmp_len(eol-5, "-----", 5)) {
    snprintf(ebuf, sizeof(ebuf), "Malformed object: mismatched end tag %s",
             tok->object_type);
    ebuf[sizeof(ebuf)-1] = '\0';
    RET_ERR(ebuf);
  }
  if (!strcmp(tok->object_type, "RSA PUBLIC KEY")) { /* If it's a public key */
    tok->key = crypto_new_pk_env();
    if (crypto_pk_read_public_key_from_string(tok->key, obstart, eol-obstart))
      RET_ERR("Couldn't parse public key.");
  } else if (!strcmp(tok->object_type, "RSA PRIVATE KEY")) { /* private key */
    tok->key = crypto_new_pk_env();
    if (crypto_pk_read_private_key_from_string(tok->key, obstart))
      RET_ERR("Couldn't parse private key.");
  } else { /* If it's something else, try to base64-decode it */
    int r;
    tok->object_body = ALLOC(next-*s); /* really, this is too much RAM. */
    r = base64_decode(tok->object_body, next-*s, *s, next-*s);
    if (r<0)
      RET_ERR("Malformed object: bad base64-encoded data");
    tok->object_size = r;
  }
  *s = eol;

 check_object:
  tok = token_check_object(area, kwd, tok, o_syn);

 done_tokenizing:
  return tok;

#undef RET_ERR
#undef ALLOC
#undef ALLOC_ZERO
#undef STRDUP
#undef STRNDUP
}

/** Read all tokens from a string between <b>start</b> and <b>end</b>, and add
 * them to <b>out</b>.  Parse according to the token rules in <b>table</b>.
 * Caller must free tokens in <b>out</b>.  If <b>end</b> is NULL, use the
 * entire string.
 */
static int
tokenize_string(memarea_t *area,
                const char *start, const char *end, smartlist_t *out,
                token_rule_t *table, int flags)
{
  const char **s;
  directory_token_t *tok = NULL;
  int counts[_NIL];
  int i;
  int first_nonannotation;
  int prev_len = smartlist_len(out);
  tor_assert(area);

  s = &start;
  if (!end)
    end = start+strlen(start);
  for (i = 0; i < _NIL; ++i)
    counts[i] = 0;
  while (*s < end && (!tok || tok->tp != _EOF)) {
    tok = get_next_token(area, s, end, table);
    if (tok->tp == _ERR) {
      log_warn(LD_DIR, "parse error: %s", tok->error);
      token_free(tok);
      return -1;
    }
    ++counts[tok->tp];
    smartlist_add(out, tok);
    *s = eat_whitespace_eos(*s, end);
  }

  if (flags & TS_NOCHECK)
    return 0;

  if ((flags & TS_ANNOTATIONS_OK)) {
    first_nonannotation = -1;
    for (i = 0; i < smartlist_len(out); ++i) {
      tok = smartlist_get(out, i);
      if (tok->tp < MIN_ANNOTATION || tok->tp > MAX_ANNOTATION) {
        first_nonannotation = i;
        break;
      }
    }
    if (first_nonannotation < 0) {
      log_warn(LD_DIR, "parse error: item contains only annotations");
      return -1;
    }
    for (i=first_nonannotation;  i < smartlist_len(out); ++i) {
      tok = smartlist_get(out, i);
      if (tok->tp >= MIN_ANNOTATION && tok->tp <= MAX_ANNOTATION) {
        log_warn(LD_DIR, "parse error: Annotations mixed with keywords");
        return -1;
      }
    }
    if ((flags & TS_NO_NEW_ANNOTATIONS)) {
      if (first_nonannotation != prev_len) {
        log_warn(LD_DIR, "parse error: Unexpectd annotations.");
        return -1;
      }
    }
  } else {
    for (i=0;  i < smartlist_len(out); ++i) {
      tok = smartlist_get(out, i);
      if (tok->tp >= MIN_ANNOTATION && tok->tp <= MAX_ANNOTATION) {
        log_warn(LD_DIR, "parse error: no annotations allowed.");
        return -1;
      }
    }
    first_nonannotation = 0;
  }
  for (i = 0; table[i].t; ++i) {
    if (counts[table[i].v] < table[i].min_cnt) {
      log_warn(LD_DIR, "Parse error: missing %s element.", table[i].t);
      return -1;
    }
    if (counts[table[i].v] > table[i].max_cnt) {
      log_warn(LD_DIR, "Parse error: too many %s elements.", table[i].t);
      return -1;
    }
    if (table[i].pos & AT_START) {
      if (smartlist_len(out) < 1 ||
          (tok = smartlist_get(out, first_nonannotation))->tp != table[i].v) {
        log_warn(LD_DIR, "Parse error: first item is not %s.", table[i].t);
        return -1;
      }
    }
    if (table[i].pos & AT_END) {
      if (smartlist_len(out) < 1 ||
          (tok = smartlist_get(out, smartlist_len(out)-1))->tp != table[i].v) {
        log_warn(LD_DIR, "Parse error: last item is not %s.", table[i].t);
        return -1;
      }
    }
  }
  return 0;
}

/** Find the first token in <b>s</b> whose keyword is <b>keyword</b>; return
 * NULL if no such keyword is found.
 */
static directory_token_t *
find_opt_by_keyword(smartlist_t *s, directory_keyword keyword)
{
  SMARTLIST_FOREACH(s, directory_token_t *, t, if (t->tp == keyword) return t);
  return NULL;
}

/** Find the first token in <b>s</b> whose keyword is <b>keyword</b>; fail
 * with an assert if no such keyword is found.
 */
static directory_token_t *
_find_by_keyword(smartlist_t *s, directory_keyword keyword,
                 const char *keyword_as_string)
{
  directory_token_t *tok = find_opt_by_keyword(s, keyword);
  if (PREDICT_UNLIKELY(!tok)) {
    log_err(LD_BUG, "Missing %s [%d] in directory object that should have "
         "been validated. Internal error.", keyword_as_string, (int)keyword);
    tor_assert(tok);
  }
  return tok;
}

/** Return a newly allocated smartlist of all accept or reject tokens in
 * <b>s</b>.
 */
static smartlist_t *
find_all_exitpolicy(smartlist_t *s)
{
  smartlist_t *out = smartlist_create();
  SMARTLIST_FOREACH(s, directory_token_t *, t,
      if (t->tp == K_ACCEPT || t->tp == K_ACCEPT6 ||
          t->tp == K_REJECT || t->tp == K_REJECT6)
        smartlist_add(out,t));
  return out;
}

/** Compute the SHA-1 digest of the substring of <b>s</b> taken from the first
 * occurrence of <b>start_str</b> through the first instance of c after the
 * first subsequent occurrence of <b>end_str</b>; store the 20-byte result in
 * <b>digest</b>; return 0 on success.
 *
 * If no such substring exists, return -1.
 */
static int
router_get_hash_impl(const char *s, char *digest,
                     const char *start_str,
                     const char *end_str, char end_c)
{
  char *start, *end;
  start = strstr(s, start_str);
  if (!start) {
    log_warn(LD_DIR,"couldn't find start of hashed material \"%s\"",start_str);
    return -1;
  }
  if (start != s && *(start-1) != '\n') {
    log_warn(LD_DIR,
             "first occurrence of \"%s\" is not at the start of a line",
             start_str);
    return -1;
  }
  end = strstr(start+strlen(start_str), end_str);
  if (!end) {
    log_warn(LD_DIR,"couldn't find end of hashed material \"%s\"",end_str);
    return -1;
  }
  end = strchr(end+strlen(end_str), end_c);
  if (!end) {
    log_warn(LD_DIR,"couldn't find EOL");
    return -1;
  }
  ++end;

  if (crypto_digest(digest, start, end-start)) {
    log_warn(LD_BUG,"couldn't compute digest");
    return -1;
  }

  return 0;
}

/** Parse the Tor version of the platform string <b>platform</b>,
 * and compare it to the version in <b>cutoff</b>. Return 1 if
 * the router is at least as new as the cutoff, else return 0.
 */
int
tor_version_as_new_as(const char *platform, const char *cutoff)
{
  tor_version_t cutoff_version, router_version;
  char *s, *s2, *start;
  char tmp[128];

  tor_assert(platform);

  if (tor_version_parse(cutoff, &cutoff_version)<0) {
    log_warn(LD_BUG,"cutoff version '%s' unparseable.",cutoff);
    return 0;
  }
  if (strcmpstart(platform,"Tor ")) /* nonstandard Tor; be safe and say yes */
    return 1;

  start = (char *)eat_whitespace(platform+3);
  if (!*start) return 0;
  s = (char *)find_whitespace(start); /* also finds '\0', which is fine */
  s2 = (char*)eat_whitespace(s);
  if (!strcmpstart(s2, "(r"))
    s = (char*)find_whitespace(s2);

  if ((size_t)(s-start+1) >= sizeof(tmp)) /* too big, no */
    return 0;
  strlcpy(tmp, start, s-start+1);

  if (tor_version_parse(tmp, &router_version)<0) {
    log_info(LD_DIR,"Router version '%s' unparseable.",tmp);
    return 1; /* be safe and say yes */
  }

  /* Here's why we don't need to do any special handling for svn revisions:
   * - If neither has an svn revision, we're fine.
   * - If the router doesn't have an svn revision, we can't assume that it
   *   is "at least" any svn revision, so we need to return 0.
   * - If the target version doesn't have an svn revision, any svn revision
   *   (or none at all) is good enough, so return 1.
   * - If both target and router have an svn revision, we compare them.
   */

  return tor_version_compare(&router_version, &cutoff_version) >= 0;
}

/** Parse a tor version from <b>s</b>, and store the result in <b>out</b>.
 * Return 0 on success, -1 on failure. */
int
tor_version_parse(const char *s, tor_version_t *out)
{
  char *eos=NULL;
  const char *cp=NULL;
  /* Format is:
   *   "Tor " ? NUM dot NUM dot NUM [ ( pre | rc | dot ) NUM [ - tag ] ]
   */
  tor_assert(s);
  tor_assert(out);

  memset(out, 0, sizeof(tor_version_t));

  if (!strcasecmpstart(s, "Tor "))
    s += 4;

  /* Get major. */
  out->major = (int)strtol(s,&eos,10);
  if (!eos || eos==s || *eos != '.') return -1;
  cp = eos+1;

  /* Get minor */
  out->minor = (int) strtol(cp,&eos,10);
  if (!eos || eos==cp || *eos != '.') return -1;
  cp = eos+1;

  /* Get micro */
  out->micro = (int) strtol(cp,&eos,10);
  if (!eos || eos==cp) return -1;
  if (!*eos) {
    out->status = VER_RELEASE;
    out->patchlevel = 0;
    return 0;
  }
  cp = eos;

  /* Get status */
  if (*cp == '.') {
    out->status = VER_RELEASE;
    ++cp;
  } else if (0==strncmp(cp, "pre", 3)) {
    out->status = VER_PRE;
    cp += 3;
  } else if (0==strncmp(cp, "rc", 2)) {
    out->status = VER_RC;
    cp += 2;
  } else {
    return -1;
  }

  /* Get patchlevel */
  out->patchlevel = (int) strtol(cp,&eos,10);
  if (!eos || eos==cp) return -1;
  cp = eos;

  /* Get status tag. */
  if (*cp == '-' || *cp == '.')
    ++cp;
  eos = (char*) find_whitespace(cp);
  if (eos-cp >= (int)sizeof(out->status_tag))
    strlcpy(out->status_tag, cp, sizeof(out->status_tag));
  else {
    memcpy(out->status_tag, cp, eos-cp);
    out->status_tag[eos-cp] = 0;
  }
  cp = eat_whitespace(eos);

  if (!strcmpstart(cp, "(r")) {
    cp += 2;
    out->svn_revision = (int) strtol(cp,&eos,10);
  }

  return 0;
}

/** Compare two tor versions; Return <0 if a < b; 0 if a ==b, >0 if a >
 * b. */
int
tor_version_compare(tor_version_t *a, tor_version_t *b)
{
  int i;
  tor_assert(a);
  tor_assert(b);
  if ((i = a->major - b->major))
    return i;
  else if ((i = a->minor - b->minor))
    return i;
  else if ((i = a->micro - b->micro))
    return i;
  else if ((i = a->status - b->status))
    return i;
  else if ((i = a->patchlevel - b->patchlevel))
    return i;
  else if ((i = strcmp(a->status_tag, b->status_tag)))
    return i;
  else
    return a->svn_revision - b->svn_revision;
}

/** Return true iff versions <b>a</b> and <b>b</b> belong to the same series.
 */
static int
tor_version_same_series(tor_version_t *a, tor_version_t *b)
{
  tor_assert(a);
  tor_assert(b);
  return ((a->major == b->major) &&
          (a->minor == b->minor) &&
          (a->micro == b->micro));
}

/** Helper: Given pointers to two strings describing tor versions, return -1
 * if _a precedes _b, 1 if _b preceeds _a, and 0 if they are equivalent.
 * Used to sort a list of versions. */
static int
_compare_tor_version_str_ptr(const void **_a, const void **_b)
{
  const char *a = *_a, *b = *_b;
  int ca, cb;
  tor_version_t va, vb;
  ca = tor_version_parse(a, &va);
  cb = tor_version_parse(b, &vb);
  /* If they both parse, compare them. */
  if (!ca && !cb)
    return tor_version_compare(&va,&vb);
  /* If one parses, it comes first. */
  if (!ca && cb)
    return -1;
  if (ca && !cb)
    return 1;
  /* If neither parses, compare strings.  Also, the directory server admin
  ** needs to be smacked upside the head.  But Tor is tolerant and gentle. */
  return strcmp(a,b);
}

/** Sort a list of string-representations of versions in ascending order. */
void
sort_version_list(smartlist_t *versions, int remove_duplicates)
{
  smartlist_sort(versions, _compare_tor_version_str_ptr);

  if (remove_duplicates)
    smartlist_uniq(versions, _compare_tor_version_str_ptr, _tor_free);
}

/** Parse and validate the ASCII-encoded v2 descriptor in <b>desc</b>,
 * write the parsed descriptor to the newly allocated *<b>parsed_out</b>, the
 * binary descriptor ID of length DIGEST_LEN to <b>desc_id_out</b>, the
 * encrypted introduction points to the newly allocated
 * *<b>intro_points_encrypted_out</b>, their encrypted size to
 * *<b>intro_points_encrypted_size_out</b>, the size of the encoded descriptor
 * to *<b>encoded_size_out</b>, and a pointer to the possibly next
 * descriptor to *<b>next_out</b>; return 0 for success (including validation)
 * and -1 for failure.
 */
int
rend_parse_v2_service_descriptor(rend_service_descriptor_t **parsed_out,
                                 char *desc_id_out,
                                 char **intro_points_encrypted_out,
                                 size_t *intro_points_encrypted_size_out,
                                 size_t *encoded_size_out,
                                 const char **next_out, const char *desc)
{
  rend_service_descriptor_t *result =
                            tor_malloc_zero(sizeof(rend_service_descriptor_t));
  char desc_hash[DIGEST_LEN];
  const char *eos;
  smartlist_t *tokens = smartlist_create();
  directory_token_t *tok;
  char secret_id_part[DIGEST_LEN];
  int i, version, num_ok=1;
  smartlist_t *versions;
  char public_key_hash[DIGEST_LEN];
  char test_desc_id[DIGEST_LEN];
  memarea_t *area = NULL;
  tor_assert(desc);
  /* Check if desc starts correctly. */
  if (strncmp(desc, "rendezvous-service-descriptor ",
              strlen("rendezvous-service-descriptor "))) {
    log_info(LD_REND, "Descriptor does not start correctly.");
    goto err;
  }
  /* Compute descriptor hash for later validation. */
  if (router_get_hash_impl(desc, desc_hash,
                           "rendezvous-service-descriptor ",
                           "\nsignature", '\n') < 0) {
    log_warn(LD_REND, "Couldn't compute descriptor hash.");
    goto err;
  }
  /* Determine end of string. */
  eos = strstr(desc, "\nrendezvous-service-descriptor ");
  if (!eos)
    eos = desc + strlen(desc);
  else
    eos = eos + 1;
  /* Check length. */
  if (strlen(desc) > REND_DESC_MAX_SIZE) {
    log_warn(LD_REND, "Descriptor length is %i which exceeds "
             "maximum rendezvous descriptor size of %i kilobytes.",
             (int)strlen(desc), REND_DESC_MAX_SIZE);
    goto err;
  }
  /* Tokenize descriptor. */
  area = memarea_new();
  if (tokenize_string(area, desc, eos, tokens, desc_token_table, 0)) {
    log_warn(LD_REND, "Error tokenizing descriptor.");
    goto err;
  }
  /* Set next to next descriptor, if available. */
  *next_out = eos;
  /* Set length of encoded descriptor. */
  *encoded_size_out = eos - desc;
  /* Check min allowed length of token list. */
  if (smartlist_len(tokens) < 7) {
    log_warn(LD_REND, "Impossibly short descriptor.");
    goto err;
  }
  /* Parse base32-encoded descriptor ID. */
  tok = find_by_keyword(tokens, R_RENDEZVOUS_SERVICE_DESCRIPTOR);
  tor_assert(tok == smartlist_get(tokens, 0));
  tor_assert(tok->n_args == 1);
  if (strlen(tok->args[0]) != REND_DESC_ID_V2_LEN_BASE32 ||
      strspn(tok->args[0], BASE32_CHARS) != REND_DESC_ID_V2_LEN_BASE32) {
    log_warn(LD_REND, "Invalid descriptor ID: '%s'", tok->args[0]);
    goto err;
  }
  if (base32_decode(desc_id_out, DIGEST_LEN,
                    tok->args[0], REND_DESC_ID_V2_LEN_BASE32) < 0) {
    log_warn(LD_REND, "Descriptor ID contains illegal characters: %s",
             tok->args[0]);
    goto err;
  }
  /* Parse descriptor version. */
  tok = find_by_keyword(tokens, R_VERSION);
  tor_assert(tok->n_args == 1);
  result->version =
    (int) tor_parse_long(tok->args[0], 10, 0, INT_MAX, &num_ok, NULL);
  if (result->version != 2 || !num_ok) {
    /* If it's <2, it shouldn't be under this format.  If the number
     * is greater than 2, we bumped it because we broke backward
     * compatibility.  See how version numbers in our other formats
     * work. */
    log_warn(LD_REND, "Unrecognized descriptor version: %s",
             escaped(tok->args[0]));
    goto err;
  }
  /* Parse public key. */
  tok = find_by_keyword(tokens, R_PERMANENT_KEY);
  result->pk = tok->key;
  tok->key = NULL; /* Prevent free */
  /* Parse secret ID part. */
  tok = find_by_keyword(tokens, R_SECRET_ID_PART);
  tor_assert(tok->n_args == 1);
  if (strlen(tok->args[0]) != REND_SECRET_ID_PART_LEN_BASE32 ||
      strspn(tok->args[0], BASE32_CHARS) != REND_SECRET_ID_PART_LEN_BASE32) {
    log_warn(LD_REND, "Invalid secret ID part: '%s'", tok->args[0]);
    goto err;
  }
  if (base32_decode(secret_id_part, DIGEST_LEN, tok->args[0], 32) < 0) {
    log_warn(LD_REND, "Secret ID part contains illegal characters: %s",
             tok->args[0]);
    goto err;
  }
  /* Parse publication time -- up-to-date check is done when storing the
   * descriptor. */
  tok = find_by_keyword(tokens, R_PUBLICATION_TIME);
  tor_assert(tok->n_args == 1);
  if (parse_iso_time(tok->args[0], &result->timestamp) < 0) {
    log_warn(LD_REND, "Invalid publication time: '%s'", tok->args[0]);
    goto err;
  }
  /* Parse protocol versions. */
  tok = find_by_keyword(tokens, R_PROTOCOL_VERSIONS);
  tor_assert(tok->n_args == 1);
  versions = smartlist_create();
  smartlist_split_string(versions, tok->args[0], ",",
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  for (i = 0; i < smartlist_len(versions); i++) {
    version = (int) tor_parse_long(smartlist_get(versions, i),
                                   10, 0, INT_MAX, &num_ok, NULL);
    if (!num_ok) /* It's a string; let's ignore it. */
      continue;
    result->protocols |= 1 << version;
  }
  SMARTLIST_FOREACH(versions, char *, cp, tor_free(cp));
  smartlist_free(versions);
  /* Parse encrypted introduction points. Don't verify. */
  tok = find_opt_by_keyword(tokens, R_INTRODUCTION_POINTS);
  if (tok) {
    if (strcmp(tok->object_type, "MESSAGE")) {
      log_warn(LD_DIR, "Bad object type: introduction points should be of "
               "type MESSAGE");
      goto err;
    }
    *intro_points_encrypted_out = tor_memdup(tok->object_body,
                                             tok->object_size);
    *intro_points_encrypted_size_out = tok->object_size;
  } else {
    *intro_points_encrypted_out = NULL;
    *intro_points_encrypted_size_out = 0;
  }
  /* Parse and verify signature. */
  tok = find_by_keyword(tokens, R_SIGNATURE);
  note_crypto_pk_op(VERIFY_RTR);
  if (check_signature_token(desc_hash, tok, result->pk, 0,
                            "v2 rendezvous service descriptor") < 0)
    goto err;
  /* Verify that descriptor ID belongs to public key and secret ID part. */
  crypto_pk_get_digest(result->pk, public_key_hash);
  rend_get_descriptor_id_bytes(test_desc_id, public_key_hash,
                               secret_id_part);
  if (memcmp(desc_id_out, test_desc_id, DIGEST_LEN)) {
    log_warn(LD_REND, "Parsed descriptor ID does not match "
             "computed descriptor ID.");
    goto err;
  }
  goto done;
 err:
  if (result)
    rend_service_descriptor_free(result);
  result = NULL;
 done:
  if (tokens) {
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_free(tokens);
  }
  if (area)
    memarea_drop_all(area);
  *parsed_out = result;
  if (result)
    return 0;
  return -1;
}

/** Decrypt the encrypted introduction points in <b>ipos_encrypted</b> of
 * length <b>ipos_encrypted_size</b> using <b>descriptor_cookie</b> and
 * write the result to a newly allocated string that is pointed to by
 * <b>ipos_decrypted</b> and its length to <b>ipos_decrypted_size</b>.
 * Return 0 if decryption was successful and -1 otherwise. */
int
rend_decrypt_introduction_points(char **ipos_decrypted,
                                 size_t *ipos_decrypted_size,
                                 const char *descriptor_cookie,
                                 const char *ipos_encrypted,
                                 size_t ipos_encrypted_size)
{
  tor_assert(ipos_encrypted);
  tor_assert(descriptor_cookie);
  if (ipos_encrypted_size < 2) {
    log_warn(LD_REND, "Size of encrypted introduction points is too "
                      "small.");
    return -1;
  }
  if (ipos_encrypted[0] == (int)REND_BASIC_AUTH) {
    char iv[CIPHER_IV_LEN], client_id[REND_BASIC_AUTH_CLIENT_ID_LEN],
         session_key[CIPHER_KEY_LEN], *dec;
    int declen, client_blocks;
    size_t pos = 0, len, client_entries_len;
    crypto_digest_env_t *digest;
    crypto_cipher_env_t *cipher;
    client_blocks = (int) ipos_encrypted[1];
    client_entries_len = client_blocks * REND_BASIC_AUTH_CLIENT_MULTIPLE *
                         REND_BASIC_AUTH_CLIENT_ENTRY_LEN;
    if (ipos_encrypted_size < 2 + client_entries_len + CIPHER_IV_LEN + 1) {
      log_warn(LD_REND, "Size of encrypted introduction points is too "
                        "small.");
      return -1;
    }
    memcpy(iv, ipos_encrypted + 2 + client_entries_len, CIPHER_IV_LEN);
    digest = crypto_new_digest_env();
    crypto_digest_add_bytes(digest, descriptor_cookie, REND_DESC_COOKIE_LEN);
    crypto_digest_add_bytes(digest, iv, CIPHER_IV_LEN);
    crypto_digest_get_digest(digest, client_id,
                             REND_BASIC_AUTH_CLIENT_ID_LEN);
    crypto_free_digest_env(digest);
    for (pos = 2; pos < 2 + client_entries_len;
         pos += REND_BASIC_AUTH_CLIENT_ENTRY_LEN) {
      if (!memcmp(ipos_encrypted + pos, client_id,
                  REND_BASIC_AUTH_CLIENT_ID_LEN)) {
        /* Attempt to decrypt introduction points. */
        cipher = crypto_create_init_cipher(descriptor_cookie, 0);
        if (crypto_cipher_decrypt(cipher, session_key, ipos_encrypted
                                  + pos + REND_BASIC_AUTH_CLIENT_ID_LEN,
                                  CIPHER_KEY_LEN) < 0) {
          log_warn(LD_REND, "Could not decrypt session key for client.");
          crypto_free_cipher_env(cipher);
          return -1;
        }
        crypto_free_cipher_env(cipher);
        cipher = crypto_create_init_cipher(session_key, 0);
        len = ipos_encrypted_size - 2 - client_entries_len - CIPHER_IV_LEN;
        dec = tor_malloc(len);
        declen = crypto_cipher_decrypt_with_iv(cipher, dec, len,
            ipos_encrypted + 2 + client_entries_len,
            ipos_encrypted_size - 2 - client_entries_len);
        crypto_free_cipher_env(cipher);
        if (declen < 0) {
          log_warn(LD_REND, "Could not decrypt introduction point string.");
          tor_free(dec);
          return -1;
        }
        if (memcmpstart(dec, declen, "introduction-point ")) {
          log_warn(LD_REND, "Decrypted introduction points don't "
                            "look like we could parse them.");
          tor_free(dec);
          continue;
        }
        *ipos_decrypted = dec;
        *ipos_decrypted_size = declen;
        return 0;
      }
    }
    log_warn(LD_REND, "Could not decrypt introduction points. Please "
             "check your authorization for this service!");
    return -1;
  } else if (ipos_encrypted[0] == (int)REND_STEALTH_AUTH) {
    crypto_cipher_env_t *cipher;
    char *dec;
    int declen;
    dec = tor_malloc_zero(ipos_encrypted_size - CIPHER_IV_LEN - 1);
    cipher = crypto_create_init_cipher(descriptor_cookie, 0);
    declen = crypto_cipher_decrypt_with_iv(cipher, dec,
                                           ipos_encrypted_size -
                                               CIPHER_IV_LEN - 1,
                                           ipos_encrypted + 1,
                                           ipos_encrypted_size - 1);
    crypto_free_cipher_env(cipher);
    if (declen < 0) {
      log_warn(LD_REND, "Decrypting introduction points failed!");
      tor_free(dec);
      return -1;
    }
    *ipos_decrypted = dec;
    *ipos_decrypted_size = declen;
    return 0;
  } else {
    log_warn(LD_REND, "Unknown authorization type number: %d",
             ipos_encrypted[0]);
    return -1;
  }
}

/** Parse the encoded introduction points in <b>intro_points_encoded</b> of
 * length <b>intro_points_encoded_size</b> and write the result to the
 * descriptor in <b>parsed</b>; return the number of successfully parsed
 * introduction points or -1 in case of a failure. */
int
rend_parse_introduction_points(rend_service_descriptor_t *parsed,
                               const char *intro_points_encoded,
                               size_t intro_points_encoded_size)
{
  const char *current_ipo, *end_of_intro_points;
  smartlist_t *tokens;
  directory_token_t *tok;
  rend_intro_point_t *intro;
  extend_info_t *info;
  int result, num_ok=1;
  memarea_t *area = NULL;
  tor_assert(parsed);
  /** Function may only be invoked once. */
  tor_assert(!parsed->intro_nodes);
  tor_assert(intro_points_encoded);
  tor_assert(intro_points_encoded_size > 0);
  /* Consider one intro point after the other. */
  current_ipo = intro_points_encoded;
  end_of_intro_points = intro_points_encoded + intro_points_encoded_size;
  tokens = smartlist_create();
  parsed->intro_nodes = smartlist_create();
  area = memarea_new();

  while (!memcmpstart(current_ipo, end_of_intro_points-current_ipo,
                      "introduction-point ")) {
    /* Determine end of string. */
    const char *eos = tor_memstr(current_ipo, end_of_intro_points-current_ipo,
                                 "\nintroduction-point ");
    if (!eos)
      eos = end_of_intro_points;
    else
      eos = eos+1;
    tor_assert(eos <= intro_points_encoded+intro_points_encoded_size);
    /* Free tokens and clear token list. */
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_clear(tokens);
    memarea_clear(area);
    /* Tokenize string. */
    if (tokenize_string(area, current_ipo, eos, tokens, ipo_token_table, 0)) {
      log_warn(LD_REND, "Error tokenizing introduction point");
      goto err;
    }
    /* Advance to next introduction point, if available. */
    current_ipo = eos;
    /* Check minimum allowed length of introduction point. */
    if (smartlist_len(tokens) < 5) {
      log_warn(LD_REND, "Impossibly short introduction point.");
      goto err;
    }
    /* Allocate new intro point and extend info. */
    intro = tor_malloc_zero(sizeof(rend_intro_point_t));
    info = intro->extend_info = tor_malloc_zero(sizeof(extend_info_t));
    /* Parse identifier. */
    tok = find_by_keyword(tokens, R_IPO_IDENTIFIER);
    if (base32_decode(info->identity_digest, DIGEST_LEN,
                      tok->args[0], REND_INTRO_POINT_ID_LEN_BASE32) < 0) {
      log_warn(LD_REND, "Identity digest contains illegal characters: %s",
               tok->args[0]);
      rend_intro_point_free(intro);
      goto err;
    }
    /* Write identifier to nickname. */
    info->nickname[0] = '$';
    base16_encode(info->nickname + 1, sizeof(info->nickname) - 1,
                  info->identity_digest, DIGEST_LEN);
    /* Parse IP address. */
    tok = find_by_keyword(tokens, R_IPO_IP_ADDRESS);
    if (tor_addr_from_str(&info->addr, tok->args[0])<0) {
      log_warn(LD_REND, "Could not parse introduction point address.");
      rend_intro_point_free(intro);
      goto err;
    }
    if (tor_addr_family(&info->addr) != AF_INET) {
      log_warn(LD_REND, "Introduction point address was not ipv4.");
      rend_intro_point_free(intro);
      goto err;
    }

    /* Parse onion port. */
    tok = find_by_keyword(tokens, R_IPO_ONION_PORT);
    info->port = (uint16_t) tor_parse_long(tok->args[0],10,1,65535,
                                           &num_ok,NULL);
    if (!info->port || !num_ok) {
      log_warn(LD_REND, "Introduction point onion port %s is invalid",
               escaped(tok->args[0]));
      rend_intro_point_free(intro);
      goto err;
    }
    /* Parse onion key. */
    tok = find_by_keyword(tokens, R_IPO_ONION_KEY);
    info->onion_key = tok->key;
    tok->key = NULL; /* Prevent free */
    /* Parse service key. */
    tok = find_by_keyword(tokens, R_IPO_SERVICE_KEY);
    intro->intro_key = tok->key;
    tok->key = NULL; /* Prevent free */
    /* Add extend info to list of introduction points. */
    smartlist_add(parsed->intro_nodes, intro);
  }
  result = smartlist_len(parsed->intro_nodes);
  goto done;

 err:
  result = -1;

 done:
  /* Free tokens and clear token list. */
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_free(tokens);
  if (area)
    memarea_drop_all(area);

  return result;
}

/** Parse the content of a client_key file in <b>ckstr</b> and add
 * rend_authorized_client_t's for each parsed client to
 * <b>parsed_clients</b>. Return the number of parsed clients as result
 * or -1 for failure. */
int
rend_parse_client_keys(strmap_t *parsed_clients, const char *ckstr)
{
  int result = -1;
  smartlist_t *tokens;
  directory_token_t *tok;
  const char *current_entry = NULL;
  memarea_t *area = NULL;
  if (!ckstr || strlen(ckstr) == 0)
    return -1;
  tokens = smartlist_create();
  /* Begin parsing with first entry, skipping comments or whitespace at the
   * beginning. */
  area = memarea_new();
  current_entry = eat_whitespace(ckstr);
  while (!strcmpstart(current_entry, "client-name ")) {
    rend_authorized_client_t *parsed_entry;
    size_t len;
    char descriptor_cookie_base64[REND_DESC_COOKIE_LEN_BASE64+2+1];
    char descriptor_cookie_tmp[REND_DESC_COOKIE_LEN+2];
    /* Determine end of string. */
    const char *eos = strstr(current_entry, "\nclient-name ");
    if (!eos)
      eos = current_entry + strlen(current_entry);
    else
      eos = eos + 1;
    /* Free tokens and clear token list. */
    SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
    smartlist_clear(tokens);
    memarea_clear(area);
    /* Tokenize string. */
    if (tokenize_string(area, current_entry, eos, tokens,
                        client_keys_token_table, 0)) {
      log_warn(LD_REND, "Error tokenizing client keys file.");
      goto err;
    }
    /* Advance to next entry, if available. */
    current_entry = eos;
    /* Check minimum allowed length of token list. */
    if (smartlist_len(tokens) < 2) {
      log_warn(LD_REND, "Impossibly short client key entry.");
      goto err;
    }
    /* Parse client name. */
    tok = find_by_keyword(tokens, C_CLIENT_NAME);
    tor_assert(tok == smartlist_get(tokens, 0));
    tor_assert(tok->n_args == 1);

    len = strlen(tok->args[0]);
    if (len < 1 || len > 19 ||
      strspn(tok->args[0], REND_LEGAL_CLIENTNAME_CHARACTERS) != len) {
      log_warn(LD_CONFIG, "Illegal client name: %s. (Length must be "
               "between 1 and 19, and valid characters are "
               "[A-Za-z0-9+-_].)", tok->args[0]);
      goto err;
    }
    /* Check if client name is duplicate. */
    if (strmap_get(parsed_clients, tok->args[0])) {
      log_warn(LD_CONFIG, "HiddenServiceAuthorizeClient contains a "
               "duplicate client name: '%s'. Ignoring.", tok->args[0]);
      goto err;
    }
    parsed_entry = tor_malloc_zero(sizeof(rend_authorized_client_t));
    parsed_entry->client_name = tor_strdup(tok->args[0]);
    strmap_set(parsed_clients, parsed_entry->client_name, parsed_entry);
    /* Parse client key. */
    tok = find_opt_by_keyword(tokens, C_CLIENT_KEY);
    if (tok) {
      parsed_entry->client_key = tok->key;
      tok->key = NULL; /* Prevent free */
    }

    /* Parse descriptor cookie. */
    tok = find_by_keyword(tokens, C_DESCRIPTOR_COOKIE);
    tor_assert(tok->n_args == 1);
    if (strlen(tok->args[0]) != REND_DESC_COOKIE_LEN_BASE64 + 2) {
      log_warn(LD_REND, "Descriptor cookie has illegal length: %s",
               escaped(tok->args[0]));
      goto err;
    }
    /* The size of descriptor_cookie_tmp needs to be REND_DESC_COOKIE_LEN+2,
     * because a base64 encoding of length 24 does not fit into 16 bytes in all
     * cases. */
    if ((base64_decode(descriptor_cookie_tmp, REND_DESC_COOKIE_LEN+2,
                       tok->args[0], REND_DESC_COOKIE_LEN_BASE64+2+1)
           != REND_DESC_COOKIE_LEN)) {
      log_warn(LD_REND, "Descriptor cookie contains illegal characters: "
                        "%s", descriptor_cookie_base64);
      goto err;
    }
    memcpy(parsed_entry->descriptor_cookie, descriptor_cookie_tmp,
           REND_DESC_COOKIE_LEN);
  }
  result = strmap_size(parsed_clients);
  goto done;
 err:
  result = -1;
 done:
  /* Free tokens and clear token list. */
  SMARTLIST_FOREACH(tokens, directory_token_t *, t, token_free(t));
  smartlist_free(tokens);
  if (area)
    memarea_drop_all(area);
  return result;
}

