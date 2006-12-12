/* This software is Public Domain. To view a copy of the public domain dedication,
 * visit http://creativecommons.org/licenses/publicdomain/ or send a letter to
 * Creative Commons, 559 Nathan Abbott Way, Stanford, California 94305, USA.
 *
 * I ask and expect, but do not require, that all derivative works contain an
 * attribution similar to:
 * 	Parts developed by Adam Langley <agl@imperialviolet.org>
 *
 * You may wish to replace the word "Parts" with something else depending on
 * the amount of original code.
 *
 * (Derivative works does not include programs which link against, run or include
 * the source verbatim in their source distributions)
 */

#ifndef EVENTDNS_H
#define EVENTDNS_H

/* Error codes 0-5 are as described in RFC 1035. */
#define DNS_ERR_NONE 0
/* The name server was unable to interpret the query */
#define DNS_ERR_FORMAT 1
/* The name server was unable to process this query due to a problem with the
 * name server */
#define DNS_ERR_SERVERFAILED 2
/* The domain name does not exist */
#define DNS_ERR_NOTEXIST 3
/* The name server does not support the requested kind of query */
#define DNS_ERR_NOTIMPL 4
/* The name server refuses to reform the specified operation for policy
 * reasons */
#define DNS_ERR_REFUSED 5
/* The reply was truncated or ill-formated */
#define DNS_ERR_TRUNCATED 65
/* An unknown error occurred */
#define DNS_ERR_UNKNOWN 66
/* Communication with the server timed out */
#define DNS_ERR_TIMEOUT 67
/* The request was canceled because the DNS subsystem was shut down. */
#define DNS_ERR_SHUTDOWN 68

#define DNS_IPv4_A 1
#define DNS_PTR 2

#define DNS_QUERY_NO_SEARCH 1

#define DNS_OPTION_SEARCH 1
#define DNS_OPTION_NAMESERVERS 2
#define DNS_OPTION_MISC 4
#define DNS_OPTIONS_ALL 7

typedef void (*evdns_callback_type) (int result, char type, int count, int ttl, void *addresses, void *arg);

int evdns_init(void);
void evdns_shutdown(int fail_requests);
const char * evdns_err_to_string(int err);
int evdns_nameserver_add(unsigned long int address);
int evdns_count_nameservers(void);
int evdns_clear_nameservers_and_suspend(void);
int evdns_resume(void);
int evdns_nameserver_ip_add(const char *ip_as_string);
int evdns_resolve_ipv4(const char *name, int flags, evdns_callback_type callback, void *ptr);
struct in_addr;
int evdns_resolve_reverse(struct in_addr *addr, int flags, evdns_callback_type callback, void *ptr);
int evdns_resolv_conf_parse(int flags, const char *);
#ifdef MS_WINDOWS
int evdns_config_windows_nameservers(void);
#endif
void evdns_search_clear(void);
void evdns_search_add(const char *domain);
void evdns_search_ndots_set(const int ndots);

typedef void (*evdns_debug_log_fn_type)(int warn, const char *msg);
void evdns_set_log_fn(evdns_debug_log_fn_type fn);

#define DNS_NO_SEARCH 1

/* Structures and functions used to implement a DNS server. */

struct evdns_server_request {
	int flags;
	int nquestions;
	struct evdns_server_question **questions;
};
struct evdns_server_question {
	int type;
	int class;
	char name[1];
};
typedef void (*evdns_request_callback_fn_type)(struct evdns_server_request *, void *);
#define EVDNS_ANSWER_SECTION 0
#define EVDNS_AUTHORITY_SECTION 1
#define EVDNS_ADDITIONAL_SECTION 2

#define EVDNS_TYPE_A	   1
#define EVDNS_TYPE_NS	   2
#define EVDNS_TYPE_CNAME   5
#define EVDNS_TYPE_SOA     6
#define EVDNS_TYPE_PTR	  12
#define EVDNS_TYPE_MX	  15
#define EVDNS_TYPE_TXT	  16
#define EVDNS_TYPE_AAAA	  28

#define EVDNS_QTYPE_AXFR 252
#define EVDNS_QTYPE_ALL  255

#define EVDNS_CLASS_INET   1

struct evdns_server_port *evdns_add_server_port(int socket, int is_tcp, evdns_request_callback_fn_type callback, void *user_data);
void evdns_close_server_port(struct evdns_server_port *port);

int evdns_server_request_add_reply(struct evdns_server_request *req, int section, const char *name, int type, int class, int ttl, int datalen, int is_name, const char *data);
int evdns_server_request_add_a_reply(struct evdns_server_request *req, const char *name, int n, void *addrs, int ttl);
int evdns_server_request_add_aaaa_reply(struct evdns_server_request *req, const char *name, int n, void *addrs, int ttl);
int evdns_server_request_add_ptr_reply(struct evdns_server_request *req, struct in_addr *in, const char *inaddr_name, const char *hostname, int ttl);
int evdns_server_request_add_cname_reply(struct evdns_server_request *req, const char *name, const char *cname, int ttl);

int evdns_server_request_respond(struct evdns_server_request *req, int err);
int evdns_server_request_drop(struct evdns_server_request *req);

#endif  // !EVENTDNS_H
