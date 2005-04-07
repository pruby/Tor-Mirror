/* Copyright 2001 Matej Pfajfar.
 * Copyright 2001-2004 Roger Dingledine.
 * Copyright 2004-2005 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char connection_c_id[] = "$Id$";

/**
 * \file connection.c
 * \brief General high-level functions to handle reading and writing
 * on connections.
 **/

#include "or.h"

static int connection_create_listener(const char *bindaddress,
                                      uint16_t bindport, int type);
static int connection_init_accepted_conn(connection_t *conn);
static int connection_handle_listener_read(connection_t *conn, int new_type);
static int connection_receiver_bucket_should_increase(connection_t *conn);
static int connection_finished_flushing(connection_t *conn);
static int connection_finished_connecting(connection_t *conn);
static int connection_reached_eof(connection_t *conn);
static int connection_read_to_buf(connection_t *conn, int *max_to_read);
static int connection_process_inbuf(connection_t *conn, int package_partial);
static int connection_bucket_read_limit(connection_t *conn);

/**************************************************************/

const char *
conn_type_to_string(int type)
{
  static char buf[64];
  switch (type) {
    case CONN_TYPE_OR_LISTENER: return "OR listener";
    case CONN_TYPE_OR: return "OR";
    case CONN_TYPE_EXIT: return "Exit";
    case CONN_TYPE_AP_LISTENER: return "App listener";
    case CONN_TYPE_AP: return "App";
    case CONN_TYPE_DIR_LISTENER: return "Dir listener";
    case CONN_TYPE_DIR: return "Dir";
    case CONN_TYPE_DNSWORKER: return "DNS worker";
    case CONN_TYPE_CPUWORKER: return "CPU worker";
    case CONN_TYPE_CONTROL_LISTENER: return "Control listener";
    case CONN_TYPE_CONTROL: return "Control";
    default:
      log_fn(LOG_WARN, "Bug: unknown connection type %d", type);
      tor_snprintf(buf, sizeof(buf), "unknown [%d]", type);
      return buf;
  }
}

const char *
conn_state_to_string(int type, int state) {
  static char buf[96];
  switch (type) {
    case CONN_TYPE_OR_LISTENER:
    case CONN_TYPE_AP_LISTENER:
    case CONN_TYPE_DIR_LISTENER:
    case CONN_TYPE_CONTROL_LISTENER:
      if (state == LISTENER_STATE_READY)
        return "ready";
      break;
    case CONN_TYPE_OR:
      switch (state) {
        case OR_CONN_STATE_CONNECTING: return "connect()ing";
        case OR_CONN_STATE_PROXY_FLUSHING: return "proxy flushing";
        case OR_CONN_STATE_PROXY_READING: return "proxy reading";
        case OR_CONN_STATE_HANDSHAKING: return "proxy reading";
        case OR_CONN_STATE_OPEN: return "open";
      }
      break;
    case CONN_TYPE_EXIT:
      switch (state) {
        case EXIT_CONN_STATE_RESOLVING: return "waiting for dest info";
        case EXIT_CONN_STATE_CONNECTING: return "connecting";
        case EXIT_CONN_STATE_OPEN: return "open";
        case EXIT_CONN_STATE_RESOLVEFAILED: return "resolve failed";
      }
      break;
    case CONN_TYPE_AP:
      switch (state) {
        case AP_CONN_STATE_SOCKS_WAIT: return "waiting for dest info";
        case AP_CONN_STATE_RENDDESC_WAIT: return "waiting for rendezvous desc";
        case AP_CONN_STATE_CONTROLLER_WAIT: return "waiting for controller";
        case AP_CONN_STATE_CIRCUIT_WAIT: return "waiting for safe circuit";
        case AP_CONN_STATE_CONNECT_WAIT: return "waiting for connect";
        case AP_CONN_STATE_RESOLVE_WAIT: return "waiting for resolve";
        case AP_CONN_STATE_OPEN: return "open";
      }
      break;
    case CONN_TYPE_DIR:
      switch (state) {
        case DIR_CONN_STATE_CONNECTING: return "connecting";
        case DIR_CONN_STATE_CLIENT_SENDING: return "client sending";
        case DIR_CONN_STATE_CLIENT_READING: return "cleint reading";
        case DIR_CONN_STATE_SERVER_COMMAND_WAIT: return "waiting for command";
        case DIR_CONN_STATE_SERVER_WRITING: return "writing";
      }
      break;
    case CONN_TYPE_DNSWORKER:
      switch (state) {
        case DNSWORKER_STATE_IDLE: return "idle";
        case DNSWORKER_STATE_BUSY: return "busy";
      }
      break;
    case CONN_TYPE_CPUWORKER:
      switch (state) {
        case CPUWORKER_STATE_IDLE: return "idle";
        case CPUWORKER_STATE_BUSY_ONION: return "busy with onion";
      }
      break;
    case CONN_TYPE_CONTROL:
      switch (state) {
        case CONTROL_CONN_STATE_OPEN: return "open";
        case CONTROL_CONN_STATE_NEEDAUTH: return "waiting for authentication";
      }
      break;
  }

  log_fn(LOG_WARN, "Bug: unknown connection state %d (type %d)", state, type);
  tor_snprintf(buf, sizeof(buf),
               "unknown state [%d] on unknown [%s] connection",
               state, conn_type_to_string(type));
  return buf;
}


/** Allocate space for a new connection_t. This function just initializes
 * conn; you must call connection_add() to link it into the main array.
 *
 * Set conn-\>type to <b>type</b>. Set conn-\>s and conn-\>poll_index to
 * -1 to signify they are not yet assigned.
 *
 * If conn is not a listener type, allocate buffers for it. If it's
 * an AP type, allocate space to store the socks_request.
 *
 * Assign a pseudorandom next_circ_id between 0 and 2**15.
 *
 * Initialize conn's timestamps to now.
 */
connection_t *connection_new(int type) {
  static uint32_t n_connections_allocated = 0;
  connection_t *conn;
  time_t now = time(NULL);

  conn = tor_malloc_zero(sizeof(connection_t));
  conn->magic = CONNECTION_MAGIC;
  conn->s = -1; /* give it a default of 'not used' */
  conn->poll_index = -1; /* also default to 'not used' */
  conn->global_identifier = n_connections_allocated++;

  conn->type = type;
  if (!connection_is_listener(conn)) { /* listeners never use their buf */
    conn->inbuf = buf_new();
    conn->outbuf = buf_new();
  }
  if (type == CONN_TYPE_AP) {
    conn->socks_request = tor_malloc_zero(sizeof(socks_request_t));
  }

  conn->next_circ_id = crypto_pseudo_rand_int(1<<15);

  conn->timestamp_created = now;
  conn->timestamp_lastread = now;
  conn->timestamp_lastwritten = now;

  return conn;
}

/** Tell libevent that we don't care about <b>conn</b> any more. */
void
connection_unregister(connection_t *conn)
{
  if (conn->read_event) {
    if (event_del(conn->read_event))
      log_fn(LOG_WARN, "Error removing read event for %d", (int)conn->s);
    tor_free(conn->read_event);
  }
  if (conn->write_event) {
    if (event_del(conn->write_event))
      log_fn(LOG_WARN, "Error removing write event for %d", (int)conn->s);
    tor_free(conn->write_event);
  }
}

/** Deallocate memory used by <b>conn</b>. Deallocate its buffers if necessary,
 * close its socket if necessary, and mark the directory as dirty if <b>conn</b>
 * is an OR or OP connection.
 */
static void
_connection_free(connection_t *conn) {
  tor_assert(conn->magic == CONNECTION_MAGIC);

  if (!connection_is_listener(conn)) {
    buf_free(conn->inbuf);
    buf_free(conn->outbuf);
  }
  tor_free(conn->address);
  tor_free(conn->chosen_exit_name);

  if (connection_speaks_cells(conn)) {
    if (conn->tls) {
      tor_tls_free(conn->tls);
      conn->tls = NULL;
    }
  }

  if (conn->identity_pkey)
    crypto_free_pk_env(conn->identity_pkey);
  tor_free(conn->nickname);
  tor_free(conn->socks_request);
  tor_free(conn->read_event); /* Probably already freed by connection_free. */
  tor_free(conn->write_event); /* Probably already freed by connection_free. */

  if (conn->s >= 0) {
    log_fn(LOG_INFO,"closing fd %d.",conn->s);
    tor_close_socket(conn->s);
  }

  memset(conn, 0xAA, sizeof(connection_t)); /* poison memory */
  tor_free(conn);
}

/** Make sure <b>conn</b> isn't in any of the global conn lists; then free it.
 */
void connection_free(connection_t *conn) {
  tor_assert(conn);
  tor_assert(!connection_is_on_closeable_list(conn));
  tor_assert(!connection_in_array(conn));
  if (connection_speaks_cells(conn)) {
    if (conn->state == OR_CONN_STATE_OPEN)
      directory_set_dirty();
  }
  connection_unregister(conn);
  _connection_free(conn);
}

/** Call _connection_free() on every connection in our array.
 * This is used by cpuworkers and dnsworkers when they fork,
 * so they don't keep resources held open (especially sockets).
 *
 * Don't do the checks in connection_free(), because they will
 * fail.
 */
void connection_free_all(void) {
  int i, n;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++)
    _connection_free(carray[i]);
}

/** Do any cleanup needed:
 *   - Directory conns that failed to fetch a rendezvous descriptor
 *     need to inform pending rendezvous streams.
 *   - OR conns need to call rep_hist_note_*() to record status.
 *   - AP conns need to send a socks reject if necessary.
 *   - Exit conns need to call connection_dns_remove() if necessary.
 *   - AP and Exit conns need to send an end cell if they can.
 *   - DNS conns need to fail any resolves that are pending on them.
 */
void connection_about_to_close_connection(connection_t *conn)
{
  circuit_t *circ;

  assert(conn->marked_for_close);

  if (CONN_IS_EDGE(conn)) {
    if (!conn->has_sent_end) {
      log_fn(LOG_WARN,"Harmless bug: Edge connection (marked at %s:%d) hasn't sent end yet?", conn->marked_for_close_file, conn->marked_for_close);
#ifdef TOR_FRAGILE
      tor_assert(0);
#endif
    }
  }

  switch (conn->type) {
    case CONN_TYPE_DIR:
      if (conn->state == DIR_CONN_STATE_CONNECTING) {
        /* it's a directory server and connecting failed: forget about
           this router */
        connection_dir_connect_failed(conn);
      }
      if (conn->purpose == DIR_PURPOSE_FETCH_RENDDESC)
        rend_client_desc_here(conn->rend_query); /* give it a try */
      break;
    case CONN_TYPE_OR:
      /* Remember why we're closing this connection. */
      if (conn->state != OR_CONN_STATE_OPEN) {
        if (connection_or_nonopen_was_started_here(conn)) {
          rep_hist_note_connect_failed(conn->identity_digest, time(NULL));
          control_event_or_conn_status(conn, OR_CONN_EVENT_FAILED);
        }
      } else if (conn->hold_open_until_flushed) {
        /* XXXX009 We used to have an arg that told us whether we closed the
         * connection on purpose or not.  Can we use hold_open_until_flushed
         * instead?  We only set it when we are intentionally closing a
         * connection. -NM
         *
         * (Of course, now things we set to close which expire rather than
         * flushing still get noted as dead, not disconnected.  But this is an
         * improvement. -NM
         */
        rep_hist_note_disconnect(conn->identity_digest, time(NULL));
        control_event_or_conn_status(conn, OR_CONN_EVENT_CLOSED);
      } else if (conn->identity_digest) {
        rep_hist_note_connection_died(conn->identity_digest, time(NULL));
        control_event_or_conn_status(conn, OR_CONN_EVENT_CLOSED);
      }
      break;
    case CONN_TYPE_AP:
      if (conn->socks_request->has_finished == 0) {
        /* since conn gets removed right after this function finishes,
         * there's no point trying to send back a reply at this point. */
        log_fn(LOG_WARN,"Bug: Closing stream (marked at %s:%d) without sending back a socks reply.",
               conn->marked_for_close_file, conn->marked_for_close);
      } else {
        control_event_stream_status(conn, STREAM_EVENT_CLOSED);
      }
      break;
    case CONN_TYPE_EXIT:
      if (conn->state == EXIT_CONN_STATE_RESOLVING) {
        circ = circuit_get_by_edge_conn(conn);
        if (circ)
          circuit_detach_stream(circ, conn);
        connection_dns_remove(conn);
      }
      break;
    case CONN_TYPE_DNSWORKER:
      if (conn->state == DNSWORKER_STATE_BUSY) {
        dns_cancel_pending_resolve(conn->address);
      }
      break;
  }
}

/** Close the underlying socket for <b>conn</b>, so we don't try to
 * flush it. Must be used in conjunction with (right before)
 * connection_mark_for_close().
 */
void connection_close_immediate(connection_t *conn)
{
  assert_connection_ok(conn,0);
  if (conn->s < 0) {
    log_fn(LOG_WARN,"Bug: Attempt to close already-closed connection.");
#ifdef TOR_FRAGILE
    tor_assert(0);
#endif
    return;
  }
  if (conn->outbuf_flushlen) {
    log_fn(LOG_INFO,"fd %d, type %s, state %s, %d bytes on outbuf.",
           conn->s, conn_type_to_string(conn->type),
           conn_state_to_string(conn->type, conn->state),
           (int)conn->outbuf_flushlen);
  }

  connection_unregister(conn);

  tor_close_socket(conn->s);
  conn->s = -1;
  if (!connection_is_listener(conn)) {
    buf_clear(conn->outbuf);
    conn->outbuf_flushlen = 0;
  }
}

/** Mark <b>conn</b> to be closed next time we loop through
 * conn_close_if_marked() in main.c. */
void
_connection_mark_for_close(connection_t *conn, int line, const char *file)
{
  assert_connection_ok(conn,0);
  tor_assert(line);
  tor_assert(file);

  if (conn->marked_for_close) {
    log(LOG_WARN, "Duplicate call to connection_mark_for_close at %s:%d"
        " (first at %s:%d)", file, line, conn->marked_for_close_file,
        conn->marked_for_close);
#ifdef TOR_FRAGILE
    tor_assert(0);
#endif
    return;
  }

  conn->marked_for_close = line;
  conn->marked_for_close_file = file;
  add_connection_to_closeable_list(conn);

  /* in case we're going to be held-open-til-flushed, reset
   * the number of seconds since last successful write, so
   * we get our whole 15 seconds */
  conn->timestamp_lastwritten = time(NULL);
}

/** Find each connection that has hold_open_until_flushed set to
 * 1 but hasn't written in the past 15 seconds, and set
 * hold_open_until_flushed to 0. This means it will get cleaned
 * up in the next loop through close_if_marked() in main.c.
 */
void connection_expire_held_open(void)
{
  connection_t **carray, *conn;
  int n, i;
  time_t now;

  now = time(NULL);

  get_connection_array(&carray, &n);
  for (i = 0; i < n; ++i) {
    conn = carray[i];
    /* If we've been holding the connection open, but we haven't written
     * for 15 seconds...
     */
    if (conn->hold_open_until_flushed) {
      tor_assert(conn->marked_for_close);
      if (now - conn->timestamp_lastwritten >= 15) {
        log_fn(LOG_NOTICE,"Giving up on marked_for_close conn that's been flushing for 15s (fd %d, type %s, state %s).",
               conn->s, conn_type_to_string(conn->type),
               conn_state_to_string(conn->type, conn->state));
        conn->hold_open_until_flushed = 0;
      }
    }
  }
}

/** Bind a new non-blocking socket listening to
 * <b>bindaddress</b>:<b>bindport</b>, and add this new connection
 * (of type <b>type</b>) to the connection array.
 *
 * If <b>bindaddress</b> includes a port, we bind on that port; otherwise, we
 * use bindport.
 */
static int connection_create_listener(const char *bindaddress, uint16_t bindport, int type) {
  struct sockaddr_in bindaddr; /* where to bind */
  connection_t *conn;
  uint16_t usePort;
  uint32_t addr;
  int s; /* the socket we're going to make */
#ifndef MS_WINDOWS
  int one=1;
#endif

  memset(&bindaddr,0,sizeof(struct sockaddr_in));
  if (parse_addr_port(bindaddress, NULL, &addr, &usePort)<0) {
    log_fn(LOG_WARN, "Error parsing/resolving BindAddress %s",bindaddress);
    return -1;
  }

  if (usePort==0)
    usePort = bindport;
  bindaddr.sin_addr.s_addr = htonl(addr);
  bindaddr.sin_family = AF_INET;
  bindaddr.sin_port = htons((uint16_t) usePort);

  s = socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0) {
    log_fn(LOG_WARN,"Socket creation failed.");
    return -1;
  } else if (!SOCKET_IS_POLLABLE(s)) {
    log_fn(LOG_WARN,"Too many connections; can't create pollable listener.");
    tor_close_socket(s);
    return -1;
  }

#ifndef MS_WINDOWS
  /* REUSEADDR on normal places means you can rebind to the port
   * right after somebody else has let it go. But REUSEADDR on win32
   * means you can bind to the port _even when somebody else
   * already has it bound_. So, don't do that on Win32. */
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*) &one, sizeof(one));
#endif

  if (bind(s,(struct sockaddr *)&bindaddr,sizeof(bindaddr)) < 0) {
    log_fn(LOG_WARN,"Could not bind to port %u: %s",usePort,
           tor_socket_strerror(tor_socket_errno(s)));
    return -1;
  }

  if (listen(s,SOMAXCONN) < 0) {
    log_fn(LOG_WARN,"Could not listen on port %u: %s",usePort,
           tor_socket_strerror(tor_socket_errno(s)));
    return -1;
  }

  set_socket_nonblocking(s);

  conn = connection_new(type);
  conn->s = s;

  if (connection_add(conn) < 0) { /* no space, forget it */
    log_fn(LOG_WARN,"connection_add failed. Giving up.");
    connection_free(conn);
    return -1;
  }

  log_fn(LOG_DEBUG,"%s listening on port %u.",conn_type_to_string(type), usePort);

  conn->state = LISTENER_STATE_READY;
  connection_start_reading(conn);

  return 0;
}

/** Do basic sanity checking on a newly received socket. Return 0
 * if it looks ok, else return -1. */
static int
check_sockaddr_in(struct sockaddr *sa, int len, int level)
{
  int ok = 1;
  struct sockaddr_in *sin=(struct sockaddr_in*)sa;

  if (len != sizeof(struct sockaddr_in)) {
    log_fn(level, "Length of address not as expected: %d vs %d",
           len,(int)sizeof(struct sockaddr_in));
    ok = 0;
  }
  if (sa->sa_family != AF_INET) {
    log_fn(level, "Family of address not as expected: %d vs %d",
           sa->sa_family, AF_INET);
    ok = 0;
  }
  if (sin->sin_addr.s_addr == 0 || sin->sin_port == 0) {
    log_fn(level, "Address for new connection has address/port equal to zero.");
    ok = 0;
  }
  return ok ? 0 : -1;
}

/** The listener connection <b>conn</b> told poll() it wanted to read.
 * Call accept() on conn-\>s, and add the new connection if necessary.
 */
static int connection_handle_listener_read(connection_t *conn, int new_type) {
  int news; /* the new socket */
  connection_t *newconn;
  /* information about the remote peer when connecting to other routers */
  struct sockaddr_in remote;
  char addrbuf[256];
  /* length of the remote address. Must be an int, since accept() needs that. */
  int remotelen = 256;
  char tmpbuf[INET_NTOA_BUF_LEN];
  tor_assert(remotelen >= sizeof(struct sockaddr_in));
  memset(addrbuf, 0, sizeof(addrbuf));

  news = accept(conn->s,(struct sockaddr *)&addrbuf,&remotelen);
  if (!SOCKET_IS_POLLABLE(news)) {
    /* accept() error, or too many conns to poll */
    int e;
    if (news>=0) {
      /* Too many conns to poll. */
      log_fn(LOG_WARN,"Too many connections; couldn't accept connection.");
      tor_close_socket(news);
      return 0;
    }
    e = tor_socket_errno(conn->s);
    if (ERRNO_IS_ACCEPT_EAGAIN(e)) {
      return 0; /* he hung up before we could accept(). that's fine. */
    } else if (ERRNO_IS_ACCEPT_RESOURCE_LIMIT(e)) {
      log_fn(LOG_NOTICE,"accept failed: %s. Dropping incoming connection.",
             tor_socket_strerror(e));
      return 0;
    }
    /* else there was a real error. */
    log_fn(LOG_WARN,"accept() failed: %s. Closing listener.",
           tor_socket_strerror(e));
    connection_mark_for_close(conn);
    return -1;
  }
  log(LOG_INFO,"Connection accepted on socket %d (child of fd %d).",news, conn->s);

  set_socket_nonblocking(news);

  if (check_sockaddr_in((struct sockaddr*)addrbuf, remotelen, LOG_INFO)<0) {
    log_fn(LOG_INFO, "accept() returned a strange address; trying getsockname().");
    remotelen=256;
    memset(addrbuf, 0, sizeof(addrbuf));
    if (getsockname(news, (struct sockaddr*)addrbuf, &remotelen)<0) {
      log_fn(LOG_WARN, "getsockname() failed.");
    } else {
      if (check_sockaddr_in((struct sockaddr*)addrbuf, remotelen, LOG_WARN)<0) {
        log_fn(LOG_WARN,"Something's wrong with this conn. Closing it.");
        tor_close_socket(news);
        return 0;
      }
    }
  }
  memcpy(&remote, addrbuf, sizeof(struct sockaddr_in));

  /* process entrance policies here, before we even create the connection */
  if (new_type == CONN_TYPE_AP) {
    /* check sockspolicy to see if we should accept it */
    if (socks_policy_permits_address(ntohl(remote.sin_addr.s_addr)) == 0) {
      tor_inet_ntoa(&remote.sin_addr, tmpbuf, sizeof(tmpbuf));
      log_fn(LOG_NOTICE,"Denying socks connection from untrusted address %s.",
             tmpbuf);
      tor_close_socket(news);
      return 0;
    }
  }
  if (new_type == CONN_TYPE_DIR) {
    /* check dirpolicy to see if we should accept it */
    if (dir_policy_permits_address(ntohl(remote.sin_addr.s_addr)) == 0) {
      tor_inet_ntoa(&remote.sin_addr, tmpbuf, sizeof(tmpbuf));
      log_fn(LOG_NOTICE,"Denying dir connection from address %s.",
             tmpbuf);
      tor_close_socket(news);
      return 0;
    }
  }

  newconn = connection_new(new_type);
  newconn->s = news;

  /* remember the remote address */
  newconn->address = tor_malloc(INET_NTOA_BUF_LEN);
  tor_inet_ntoa(&remote.sin_addr, newconn->address, INET_NTOA_BUF_LEN);
  newconn->addr = ntohl(remote.sin_addr.s_addr);
  newconn->port = ntohs(remote.sin_port);

  if (connection_add(newconn) < 0) { /* no space, forget it */
    connection_free(newconn);
    return 0; /* no need to tear down the parent */
  }

  if (connection_init_accepted_conn(newconn) < 0) {
    connection_mark_for_close(newconn);
    return 0;
  }
  return 0;
}

/** Initialize states for newly accepted connection <b>conn</b>.
 * If conn is an OR, start the tls handshake.
 */
static int connection_init_accepted_conn(connection_t *conn) {

  connection_start_reading(conn);

  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_tls_start_handshake(conn, 1);
    case CONN_TYPE_AP:
      conn->state = AP_CONN_STATE_SOCKS_WAIT;
      break;
    case CONN_TYPE_DIR:
      conn->purpose = DIR_PURPOSE_SERVER;
      conn->state = DIR_CONN_STATE_SERVER_COMMAND_WAIT;
      break;
    case CONN_TYPE_CONTROL:
      conn->state = CONTROL_CONN_STATE_NEEDAUTH;
      break;
  }
  return 0;
}

/** Take conn, make a nonblocking socket; try to connect to
 * addr:port (they arrive in *host order*). If fail, return -1. Else
 * assign s to conn->\s: if connected return 1, if EAGAIN return 0.
 *
 * address is used to make the logs useful.
 *
 * On success, add conn to the list of polled connections.
 */
int connection_connect(connection_t *conn, char *address, uint32_t addr, uint16_t port) {
  int s;
  struct sockaddr_in dest_addr;
  or_options_t *options = get_options();

  s = socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0) {
    log_fn(LOG_WARN,"Error creating network socket: %s",
           tor_socket_strerror(tor_socket_errno(-1)));
    return -1;
  } else if (!SOCKET_IS_POLLABLE(s)) {
    log_fn(LOG_WARN,
      "Too many connections; can't create pollable connection to %s", address);
    tor_close_socket(s);
    return -1;
  }

  if (options->OutboundBindAddress) {
    struct sockaddr_in ext_addr;

    memset(&ext_addr, 0, sizeof(ext_addr));
    ext_addr.sin_family = AF_INET;
    ext_addr.sin_port = 0;
    if (!tor_inet_aton(options->OutboundBindAddress, &ext_addr.sin_addr)) {
      log_fn(LOG_WARN,"Outbound bind address '%s' didn't parse. Ignoring.",
             options->OutboundBindAddress);
    } else {
      if (bind(s, (struct sockaddr*)&ext_addr, sizeof(ext_addr)) < 0) {
        log_fn(LOG_WARN,"Error binding network socket: %s",
               tor_socket_strerror(tor_socket_errno(s)));
        return -1;
      }
    }
  }

  set_socket_nonblocking(s);

  memset(&dest_addr,0,sizeof(dest_addr));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(port);
  dest_addr.sin_addr.s_addr = htonl(addr);

  log_fn(LOG_DEBUG,"Connecting to %s:%u.",address,port);

  if (connect(s,(struct sockaddr *)&dest_addr,sizeof(dest_addr)) < 0) {
    int e = tor_socket_errno(s);
    if (!ERRNO_IS_CONN_EINPROGRESS(e)) {
      /* yuck. kill it. */
      log_fn(LOG_INFO,"Connect() to %s:%u failed: %s",address,port,
             tor_socket_strerror(e));
      tor_close_socket(s);
      return -1;
    } else {
      /* it's in progress. set state appropriately and return. */
      conn->s = s;
      if (connection_add(conn) < 0) /* no space, forget it */
        return -1;
      log_fn(LOG_DEBUG,"connect in progress, socket %d.",s);
      return 0;
    }
  }

  /* it succeeded. we're connected. */
  log_fn(LOG_INFO,"Connection to %s:%u established.",address,port);
  conn->s = s;
  if (connection_add(conn) < 0) /* no space, forget it */
    return -1;
  return 1;
}

/** If there exist any listeners of type <b>type</b> in the connection
 * array, mark them for close.
 */
static void listener_close_if_present(int type) {
  connection_t *conn;
  connection_t **carray;
  int i,n;
  tor_assert(type == CONN_TYPE_OR_LISTENER ||
             type == CONN_TYPE_AP_LISTENER ||
             type == CONN_TYPE_DIR_LISTENER ||
             type == CONN_TYPE_CONTROL_LISTENER);
  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == type && !conn->marked_for_close) {
      connection_close_immediate(conn);
      connection_mark_for_close(conn);
    }
  }
}

/**
 * Launch any configured listener connections of type <b>type</b>.  (A
 * listener is configured if <b>port_option</b> is non-zero.  If any
 * BindAddress configuration options are given in <b>cfg</b>, create a
 * connection binding to each one.  Otherwise, create a single
 * connection binding to the address <b>default_addr</b>.)
 *
 * If <b>force</b> is true, close and re-open all listener connections.
 * Otherwise, only relaunch the listeners of this type if the number of
 * existing connections is not as configured (e.g., because one died).
 */
static int retry_listeners(int type, struct config_line_t *cfg,
                           int port_option, const char *default_addr,
                           int force)
{
  if (!force) {
    int want, have, n_conn, i;
    struct config_line_t *c;
    connection_t *conn;
    connection_t **carray;
    /* How many should there be? */
    if (cfg && port_option) {
      want = 0;
      for (c = cfg; c; c = c->next)
        ++want;
    } else if (port_option) {
      want = 1;
    } else {
      want = 0;
    }

    /* How many are there actually? */
    have = 0;
    get_connection_array(&carray,&n_conn);
    for (i=0;i<n_conn;i++) {
      conn = carray[i];
      if (conn->type == type && !conn->marked_for_close)
        ++have;
    }

    /* If we have the right number of listeners, do nothing. */
    if (have == want)
      return 0;

    /* Otherwise, warn the user and relaunch. */
    log_fn(LOG_NOTICE,"We have %d %s(s) open, but we want %d; relaunching.",
           have, conn_type_to_string(type), want);
  }

  listener_close_if_present(type);
  if (port_option) {
    if (!cfg) {
           if (connection_create_listener(default_addr, (uint16_t) port_option,
                                     type)<0)
        return -1;
    } else {
      for ( ; cfg; cfg = cfg->next) {
        if (connection_create_listener(cfg->value, (uint16_t) port_option,
                                       type)<0)
          return -1;
      }
    }
  }
  return 0;
}

/** (Re)launch listeners for each port you should have open.  If
 * <b>force</b> is true, close and relaunch all listeners. If <b>force</b>
 * is false, then only relaunch listeners when we have the wrong number of
 * connections for a given type.
 */
int retry_all_listeners(int force) {
  or_options_t *options = get_options();

  if (retry_listeners(CONN_TYPE_OR_LISTENER, options->ORBindAddress,
                      options->ORPort, "0.0.0.0", force)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_DIR_LISTENER, options->DirBindAddress,
                      options->DirPort, "0.0.0.0", force)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_AP_LISTENER, options->SocksBindAddress,
                      options->SocksPort, "127.0.0.1", force)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_CONTROL_LISTENER, NULL,
                      options->ControlPort, "127.0.0.1", force)<0)
    return -1;

  return 0;
}

extern int global_read_bucket, global_write_bucket;

/** How many bytes at most can we read onto this connection? */
static int connection_bucket_read_limit(connection_t *conn) {
  int at_most;

  /* do a rudimentary round-robin so one circuit can't hog a connection */
  if (connection_speaks_cells(conn)) {
    at_most = 32*(CELL_NETWORK_SIZE);
  } else {
    at_most = 32*(RELAY_PAYLOAD_SIZE);
  }

  if (at_most > global_read_bucket)
    at_most = global_read_bucket;

  if (connection_speaks_cells(conn) && conn->state == OR_CONN_STATE_OPEN)
    if (at_most > conn->receiver_bucket)
      at_most = conn->receiver_bucket;

  if (at_most < 0)
    return 0;
  return at_most;
}

/** We just read num_read onto conn. Decrement buckets appropriately. */
static void connection_read_bucket_decrement(connection_t *conn, int num_read) {
  global_read_bucket -= num_read; //tor_assert(global_read_bucket >= 0);
  if (connection_speaks_cells(conn) && conn->state == OR_CONN_STATE_OPEN) {
    conn->receiver_bucket -= num_read; //tor_assert(conn->receiver_bucket >= 0);
  }
}

static void connection_consider_empty_buckets(connection_t *conn) {
  if (global_read_bucket <= 0) {
    log_fn(LOG_DEBUG,"global bucket exhausted. Pausing.");
    conn->wants_to_read = 1;
    connection_stop_reading(conn);
    return;
  }
  if (connection_speaks_cells(conn) &&
      conn->state == OR_CONN_STATE_OPEN &&
      conn->receiver_bucket <= 0) {
    log_fn(LOG_DEBUG,"receiver bucket exhausted. Pausing.");
    conn->wants_to_read = 1;
    connection_stop_reading(conn);
  }
}

/** Initialize the global read bucket to options->BandwidthBurst,
 * and current_time to the current time. */
void connection_bucket_init(void) {
  or_options_t *options = get_options();
  global_read_bucket = (int)options->BandwidthBurst; /* start it at max traffic */
  global_write_bucket = (int)options->BandwidthBurst; /* start it at max traffic */
}

/** A second has rolled over; increment buckets appropriately. */
void connection_bucket_refill(struct timeval *now) {
  int i, n;
  connection_t *conn;
  connection_t **carray;
  or_options_t *options = get_options();

  /* refill the global buckets */
  if (global_read_bucket < (int)options->BandwidthBurst) {
    global_read_bucket += (int)options->BandwidthRate;
    log_fn(LOG_DEBUG,"global_read_bucket now %d.", global_read_bucket);
  }
  if (global_write_bucket < (int)options->BandwidthBurst) {
    global_write_bucket += (int)options->BandwidthRate;
    log_fn(LOG_DEBUG,"global_write_bucket now %d.", global_write_bucket);
  }

  /* refill the per-connection buckets */
  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];

    if (connection_receiver_bucket_should_increase(conn)) {
      conn->receiver_bucket = conn->bandwidth;
      //log_fn(LOG_DEBUG,"Receiver bucket %d now %d.", i, conn->receiver_bucket);
    }

    if (conn->wants_to_read == 1 /* it's marked to turn reading back on now */
        && global_read_bucket > 0 /* and we're allowed to read */
        && global_write_bucket > 0 /* and we're allowed to write (XXXX,
                                    * not the best place to check this.) */
        && (!connection_speaks_cells(conn) ||
            conn->state != OR_CONN_STATE_OPEN ||
            conn->receiver_bucket > 0)) {
      /* and either a non-cell conn or a cell conn with non-empty bucket */
      log_fn(LOG_DEBUG,"waking up conn (fd %d)",conn->s);
      conn->wants_to_read = 0;
      connection_start_reading(conn);
      if (conn->wants_to_write == 1) {
        conn->wants_to_write = 0;
        connection_start_writing(conn);
      }
    }
  }
}

/** Is the receiver bucket for connection <b>conn</b> low enough that we
 * should add another pile of tokens to it?
 */
static int connection_receiver_bucket_should_increase(connection_t *conn) {
  tor_assert(conn);

  if (!connection_speaks_cells(conn))
    return 0; /* edge connections don't use receiver_buckets */
  if (conn->state != OR_CONN_STATE_OPEN)
    return 0; /* only open connections play the rate limiting game */

  if (conn->receiver_bucket >= conn->bandwidth)
    return 0;

  return 1;
}

/** Read bytes from conn->\s and process them.
 *
 * This function gets called from conn_read() in main.c, either
 * when poll() has declared that conn wants to read, or (for OR conns)
 * when there are pending TLS bytes.
 *
 * It calls connection_read_to_buf() to bring in any new bytes,
 * and then calls connection_process_inbuf() to process them.
 *
 * Mark the connection and return -1 if you want to close it, else
 * return 0.
 */
int connection_handle_read(connection_t *conn) {
  int max_to_read=-1, try_to_read;

  conn->timestamp_lastread = time(NULL);

  switch (conn->type) {
    case CONN_TYPE_OR_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_OR);
    case CONN_TYPE_AP_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_AP);
    case CONN_TYPE_DIR_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_DIR);
    case CONN_TYPE_CONTROL_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_CONTROL);
  }

loop_again:
  try_to_read = max_to_read;
  tor_assert(!conn->marked_for_close);
  if (connection_read_to_buf(conn, &max_to_read) < 0) {
    /* There's a read error; kill the connection.*/
    connection_close_immediate(conn); /* Don't flush; connection is dead. */
    if (CONN_IS_EDGE(conn)) {
      connection_edge_end_errno(conn, conn->cpath_layer);
    }
    connection_mark_for_close(conn);
    return -1;
  }
  if (CONN_IS_EDGE(conn) &&
      try_to_read != max_to_read) {
    /* instruct it not to try to package partial cells. */
    if (connection_process_inbuf(conn, 0) < 0) {
      return -1;
    }
    if (!conn->marked_for_close &&
        connection_is_reading(conn) &&
        !conn->inbuf_reached_eof &&
        max_to_read > 0)
      goto loop_again; /* try reading again, in case more is here now */
  }
  /* one last try, packaging partial cells and all. */
  if (!conn->marked_for_close &&
      connection_process_inbuf(conn, 1) < 0) {
    return -1;
  }
  if (!conn->marked_for_close &&
      conn->inbuf_reached_eof &&
      connection_reached_eof(conn) < 0) {
    return -1;
  }
  return 0;
}

/** Pull in new bytes from conn-\>s onto conn-\>inbuf, either
 * directly or via TLS. Reduce the token buckets by the number of
 * bytes read.
 *
 * If *max_to_read is -1, then decide it ourselves, else go with the
 * value passed to us. When returning, if it's changed, subtract the
 * number of bytes we read from *max_to_read.
 *
 * Return -1 if we want to break conn, else return 0.
 */
static int connection_read_to_buf(connection_t *conn, int *max_to_read) {
  int result, at_most = *max_to_read;

  if (at_most == -1) { /* we need to initialize it */
    /* how many bytes are we allowed to read? */
    at_most = connection_bucket_read_limit(conn);
  }

  if (connection_speaks_cells(conn) && conn->state > OR_CONN_STATE_PROXY_READING) {
    int pending;
    if (conn->state == OR_CONN_STATE_HANDSHAKING) {
      /* continue handshaking even if global token bucket is empty */
      return connection_tls_continue_handshake(conn);
    }

    log_fn(LOG_DEBUG,"%d: starting, inbuf_datalen %d (%d pending in tls object). at_most %d.",
           conn->s,(int)buf_datalen(conn->inbuf),tor_tls_get_pending_bytes(conn->tls), at_most);

    /* else open, or closing */
    result = read_to_buf_tls(conn->tls, at_most, conn->inbuf);

    switch (result) {
      case TOR_TLS_CLOSE:
        log_fn(LOG_INFO,"TLS connection closed on read. Closing. (Nickname %s, address %s",
               conn->nickname ? conn->nickname : "not set", conn->address);
        return -1;
      case TOR_TLS_ERROR:
        log_fn(LOG_INFO,"tls error. breaking (nickname %s, address %s).",
               conn->nickname ? conn->nickname : "not set", conn->address);
        return -1;
      case TOR_TLS_WANTWRITE:
        connection_start_writing(conn);
        return 0;
      case TOR_TLS_WANTREAD: /* we're already reading */
      case TOR_TLS_DONE: /* no data read, so nothing to process */
        result = 0;
        break; /* so we call bucket_decrement below */
      default:
        break;
    }
    pending = tor_tls_get_pending_bytes(conn->tls);
    if (pending) {
      /* XXXX If we have any pending bytes, read them now.  This *can*
       * take us over our read alotment, but really we shouldn't be
       * believing that SSL bytes are the same as TCP bytes anyway. */
      int r2 = read_to_buf_tls(conn->tls, pending, conn->inbuf);
      if (r2<0) {
        log_fn(LOG_WARN, "Bug: apparently, reading pending bytes can fail.");
        return -1;
      } else {
        result += r2;
      }
    }

  } else {
    result = read_to_buf(conn->s, at_most, conn->inbuf,
                         &conn->inbuf_reached_eof);

//  log_fn(LOG_DEBUG,"read_to_buf returned %d.",read_result);

    if (result < 0)
      return -1;
  }

  if (result > 0) { /* change *max_to_read */
    *max_to_read = at_most - result;
  }

  if (result > 0 && !is_local_IP(conn->addr)) { /* remember it */
    rep_hist_note_bytes_read(result, time(NULL));
    connection_read_bucket_decrement(conn, result);
  }

  /* Call even if result is 0, since the global read bucket may
   * have reached 0 on a different conn, and this guy needs to
   * know to stop reading. */
  connection_consider_empty_buckets(conn);

  return 0;
}

/** A pass-through to fetch_from_buf. */
int connection_fetch_from_buf(char *string, size_t len, connection_t *conn) {
  return fetch_from_buf(string, len, conn->inbuf);
}

/** Return conn-\>outbuf_flushlen: how many bytes conn wants to flush
 * from its outbuf. */
int connection_wants_to_flush(connection_t *conn) {
  return conn->outbuf_flushlen;
}

/** Are there too many bytes on edge connection <b>conn</b>'s outbuf to
 * send back a relay-level sendme yet? Return 1 if so, 0 if not. Used by
 * connection_edge_consider_sending_sendme().
 */
int connection_outbuf_too_full(connection_t *conn) {
  return (conn->outbuf_flushlen > 10*CELL_PAYLOAD_SIZE);
}

/** Try to flush more bytes onto conn-\>s.
 *
 * This function gets called either from conn_write() in main.c
 * when poll() has declared that conn wants to write, or below
 * from connection_write_to_buf() when an entire TLS record is ready.
 *
 * Update conn-\>timestamp_lastwritten to now, and call flush_buf
 * or flush_buf_tls appropriately. If it succeeds and there no more
 * more bytes on conn->outbuf, then call connection_finished_flushing
 * on it too.
 *
 * Mark the connection and return -1 if you want to close it, else
 * return 0.
 */
int connection_handle_write(connection_t *conn) {
  int e, len=sizeof(e);
  int result;
  time_t now = time(NULL);

  tor_assert(!connection_is_listener(conn));

  conn->timestamp_lastwritten = now;

  /* Sometimes, "writable" means "connected". */
  if (connection_state_is_connecting(conn)) {
    if (getsockopt(conn->s, SOL_SOCKET, SO_ERROR, (void*)&e, &len) < 0) {
      log_fn(LOG_WARN,"getsockopt() syscall failed?! Please report to tor-ops.");
      if (CONN_IS_EDGE(conn))
        connection_edge_end_errno(conn, conn->cpath_layer);
      connection_mark_for_close(conn);
      return -1;
    }
    if (e) {
      /* some sort of error, but maybe just inprogress still */
      if (!ERRNO_IS_CONN_EINPROGRESS(e)) {
        log_fn(LOG_INFO,"in-progress connect failed. Removing.");
        if (CONN_IS_EDGE(conn))
          connection_edge_end_errno(conn, conn->cpath_layer);

        connection_close_immediate(conn);
        connection_mark_for_close(conn);
        /* it's safe to pass OPs to router_mark_as_down(), since it just
         * ignores unrecognized routers
         */
        if (conn->type == CONN_TYPE_OR && !get_options()->HttpsProxy)
          router_mark_as_down(conn->identity_digest);
        return -1;
      } else {
        return 0; /* no change, see if next time is better */
      }
    }
    /* The connection is successful. */
    if (connection_finished_connecting(conn)<0)
      return -1;
  }

  if (connection_speaks_cells(conn) && conn->state > OR_CONN_STATE_PROXY_READING) {
    if (conn->state == OR_CONN_STATE_HANDSHAKING) {
      connection_stop_writing(conn);
      if (connection_tls_continue_handshake(conn) < 0) {
        connection_close_immediate(conn); /* Don't flush; connection is dead. */
        connection_mark_for_close(conn);
        return -1;
      }
      return 0;
    }

    /* else open, or closing */
    result = flush_buf_tls(conn->tls, conn->outbuf, &conn->outbuf_flushlen);
    switch (result) {
      case TOR_TLS_ERROR:
      case TOR_TLS_CLOSE:
        log_fn(LOG_INFO,result==TOR_TLS_ERROR?
               "tls error. breaking.":"TLS connection closed on flush");
        connection_close_immediate(conn); /* Don't flush; connection is dead. */
        connection_mark_for_close(conn);
        return -1;
      case TOR_TLS_WANTWRITE:
        log_fn(LOG_DEBUG,"wanted write.");
        /* we're already writing */
        return 0;
      case TOR_TLS_WANTREAD:
        /* Make sure to avoid a loop if the receive buckets are empty. */
        log_fn(LOG_DEBUG,"wanted read.");
        if (!connection_is_reading(conn)) {
          connection_stop_writing(conn);
          conn->wants_to_write = 1;
          /* we'll start reading again when the next second arrives,
           * and then also start writing again.
           */
        }
        /* else no problem, we're already reading */
        return 0;
      /* case TOR_TLS_DONE:
       * for TOR_TLS_DONE, fall through to check if the flushlen
       * is empty, so we can stop writing.
       */
    }
  } else {
    result = flush_buf(conn->s, conn->outbuf, &conn->outbuf_flushlen);
    if (result < 0) {
      if (CONN_IS_EDGE(conn))
        connection_edge_end_errno(conn, conn->cpath_layer);

      connection_close_immediate(conn); /* Don't flush; connection is dead. */
      connection_mark_for_close(conn);
      return -1;
    }
  }

  if (result > 0 && !is_local_IP(conn->addr)) { /* remember it */
    rep_hist_note_bytes_written(result, now);
    global_write_bucket -= result;
  }

  if (!connection_wants_to_flush(conn)) { /* it's done flushing */
    if (connection_finished_flushing(conn) < 0) {
      /* already marked */
      return -1;
    }
  }

  return 0;
}

/** Append <b>len</b> bytes of <b>string</b> onto <b>conn</b>'s
 * outbuf, and ask it to start writing.
 */
void connection_write_to_buf(const char *string, size_t len, connection_t *conn) {

  if (!len)
    return;
  /* if it's marked for close, only allow write if we mean to flush it */
  if (conn->marked_for_close && !conn->hold_open_until_flushed)
    return;

  if (write_to_buf(string, len, conn->outbuf) < 0) {
    if (CONN_IS_EDGE(conn)) {
      /* if it failed, it means we have our package/delivery windows set
         wrong compared to our max outbuf size. close the whole circuit. */
      log_fn(LOG_WARN,"write_to_buf failed. Closing circuit (fd %d).", conn->s);
      circuit_mark_for_close(circuit_get_by_edge_conn(conn));
    } else {
      log_fn(LOG_WARN,"write_to_buf failed. Closing connection (fd %d).", conn->s);
      connection_mark_for_close(conn);
    }
    return;
  }

  connection_start_writing(conn);
  conn->outbuf_flushlen += len;
}

/** Return the conn to addr/port that has the most recent
 * timestamp_created, or NULL if no such conn exists. */
connection_t *connection_or_exact_get_by_addr_port(uint32_t addr, uint16_t port) {
  int i, n;
  connection_t *conn, *best=NULL;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == CONN_TYPE_OR &&
        conn->addr == addr &&
        conn->port == port &&
        !conn->marked_for_close &&
        (!best || best->timestamp_created < conn->timestamp_created))
      best = conn;
  }
  return best;
}

connection_t *connection_get_by_identity_digest(const char *digest, int type)
{
  int i, n;
  connection_t *conn, *best=NULL;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type != type)
      continue;
    if (!memcmp(conn->identity_digest, digest, DIGEST_LEN) &&
        !conn->marked_for_close &&
        (!best || best->timestamp_created < conn->timestamp_created))
      best = conn;
  }
  return best;
}

/** Return the connection with id <b>id</b> if it is not already
 * marked for close.
 */
connection_t *
connection_get_by_global_id(uint32_t id) {
  int i, n;
  connection_t *conn;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->global_identifier == id) {
      if (!conn->marked_for_close)
        return conn;
      else
        return NULL;
    }
  }
  return NULL;
}

/** Return a connection of type <b>type</b> that is not marked for
 * close.
 */
connection_t *connection_get_by_type(int type) {
  int i, n;
  connection_t *conn;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == type && !conn->marked_for_close)
      return conn;
  }
  return NULL;
}

/** Return a connection of type <b>type</b> that is in state <b>state</b>,
 * and that is not marked for close.
 */
connection_t *connection_get_by_type_state(int type, int state) {
  int i, n;
  connection_t *conn;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == type && conn->state == state && !conn->marked_for_close)
      return conn;
  }
  return NULL;
}

/** Return a connection of type <b>type</b> that has purpose <b>purpose</b>,
 * and that is not marked for close.
 */
connection_t *connection_get_by_type_purpose(int type, int purpose) {
  int i, n;
  connection_t *conn;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == type && conn->purpose == purpose && !conn->marked_for_close)
      return conn;
  }
  return NULL;
}

/** Return the connection of type <b>type</b> that is in state
 * <b>state</b>, that was written to least recently, and that is not
 * marked for close.
 */
connection_t *connection_get_by_type_state_lastwritten(int type, int state) {
  int i, n;
  connection_t *conn, *best=NULL;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == type && conn->state == state && !conn->marked_for_close)
      if (!best || conn->timestamp_lastwritten < best->timestamp_lastwritten)
        best = conn;
  }
  return best;
}

/** Return a connection of type <b>type</b> that has rendquery equal
 * to <b>rendquery</b>, and that is not marked for close. If state
 * is non-zero, conn must be of that state too.
 */
connection_t *
connection_get_by_type_state_rendquery(int type, int state, const char *rendquery) {
  int i, n;
  connection_t *conn;
  connection_t **carray;

  get_connection_array(&carray,&n);
  for (i=0;i<n;i++) {
    conn = carray[i];
    if (conn->type == type &&
        !conn->marked_for_close &&
        (!state || state == conn->state) &&
        !rend_cmp_service_ids(rendquery, conn->rend_query))
      return conn;
  }
  return NULL;
}

/** Return 1 if <b>conn</b> is a listener conn, else return 0. */
int connection_is_listener(connection_t *conn) {
  if (conn->type == CONN_TYPE_OR_LISTENER ||
      conn->type == CONN_TYPE_AP_LISTENER ||
      conn->type == CONN_TYPE_DIR_LISTENER ||
      conn->type == CONN_TYPE_CONTROL_LISTENER)
    return 1;
  return 0;
}

/** Return 1 if <b>conn</b> is in state "open" and is not marked
 * for close, else return 0.
 */
int connection_state_is_open(connection_t *conn) {
  tor_assert(conn);

  if (conn->marked_for_close)
    return 0;

  if ((conn->type == CONN_TYPE_OR && conn->state == OR_CONN_STATE_OPEN) ||
      (conn->type == CONN_TYPE_AP && conn->state == AP_CONN_STATE_OPEN) ||
      (conn->type == CONN_TYPE_EXIT && conn->state == EXIT_CONN_STATE_OPEN) ||
      (conn->type == CONN_TYPE_CONTROL && conn->state ==CONTROL_CONN_STATE_OPEN))
    return 1;

  return 0;
}

/** Return 1 if conn is in 'connecting' state, else return 0. */
int connection_state_is_connecting(connection_t *conn) {
  tor_assert(conn);

  if (conn->marked_for_close)
    return 0;
  switch (conn->type)
    {
    case CONN_TYPE_OR:
      return conn->state == OR_CONN_STATE_CONNECTING;
    case CONN_TYPE_EXIT:
      return conn->state == EXIT_CONN_STATE_CONNECTING;
    case CONN_TYPE_DIR:
      return conn->state == DIR_CONN_STATE_CONNECTING;
    }

  return 0;
}

/** Write a destroy cell with circ ID <b>circ_id</b> onto OR connection
 * <b>conn</b>.
 *
 * Return 0.
 */
int connection_send_destroy(uint16_t circ_id, connection_t *conn) {
  cell_t cell;

  tor_assert(conn);
  tor_assert(connection_speaks_cells(conn));

  memset(&cell, 0, sizeof(cell_t));
  cell.circ_id = circ_id;
  cell.command = CELL_DESTROY;
  log_fn(LOG_INFO,"Sending destroy (circID %d).", circ_id);
  connection_or_write_cell_to_buf(&cell, conn);
  return 0;
}

/** Process new bytes that have arrived on conn-\>inbuf.
 *
 * This function just passes conn to the connection-specific
 * connection_*_process_inbuf() function. It also passes in
 * package_partial if wanted.
 */
static int connection_process_inbuf(connection_t *conn, int package_partial) {

  tor_assert(conn);

  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_or_process_inbuf(conn);
    case CONN_TYPE_EXIT:
    case CONN_TYPE_AP:
      return connection_edge_process_inbuf(conn, package_partial);
    case CONN_TYPE_DIR:
      return connection_dir_process_inbuf(conn);
    case CONN_TYPE_DNSWORKER:
      return connection_dns_process_inbuf(conn);
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_process_inbuf(conn);
    case CONN_TYPE_CONTROL:
      return connection_control_process_inbuf(conn);
    default:
      log_fn(LOG_WARN,"Bug: got unexpected conn type %d.", conn->type);
#ifdef TOR_FRAGILE
      tor_assert(0);
#endif
      return -1;
  }
}

/** We just finished flushing bytes from conn-\>outbuf, and there
 * are no more bytes remaining.
 *
 * This function just passes conn to the connection-specific
 * connection_*_finished_flushing() function.
 */
static int connection_finished_flushing(connection_t *conn) {

  tor_assert(conn);

//  log_fn(LOG_DEBUG,"entered. Socket %u.", conn->s);

  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_or_finished_flushing(conn);
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      return connection_edge_finished_flushing(conn);
    case CONN_TYPE_DIR:
      return connection_dir_finished_flushing(conn);
    case CONN_TYPE_DNSWORKER:
      return connection_dns_finished_flushing(conn);
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_finished_flushing(conn);
    case CONN_TYPE_CONTROL:
      return connection_control_finished_flushing(conn);
    default:
      log_fn(LOG_WARN,"Bug: got unexpected conn type %d.", conn->type);
#ifdef TOR_FRAGILE
      tor_assert(0);
#endif
      return -1;
  }
}

/** Called when our attempt to connect() to another server has just
 * succeeded.
 *
 * This function just passes conn to the connection-specific
 * connection_*_finished_connecting() function.
 */
static int connection_finished_connecting(connection_t *conn)
{
  tor_assert(conn);
  switch (conn->type)
    {
    case CONN_TYPE_OR:
      return connection_or_finished_connecting(conn);
    case CONN_TYPE_EXIT:
      return connection_edge_finished_connecting(conn);
    case CONN_TYPE_DIR:
      return connection_dir_finished_connecting(conn);
    default:
      log_fn(LOG_WARN,"Bug: got unexpected conn type %d.", conn->type);
#ifdef TOR_FRAGILE
      tor_assert(0);
#endif
      return -1;
  }
}

static int connection_reached_eof(connection_t *conn)
{
  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_or_reached_eof(conn);
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      return connection_edge_reached_eof(conn);
    case CONN_TYPE_DIR:
      return connection_dir_reached_eof(conn);
    case CONN_TYPE_DNSWORKER:
      return connection_dns_reached_eof(conn);
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_reached_eof(conn);
    case CONN_TYPE_CONTROL:
      return connection_control_reached_eof(conn);
    default:
      log_fn(LOG_WARN,"Bug: got unexpected conn type %d.", conn->type);
#ifdef TOR_FRAGILE
      tor_assert(0);
#endif
      return -1;
  }
}

/** Verify that connection <b>conn</b> has all of its invariants
 * correct. Trigger an assert if anything is invalid.
 */
void assert_connection_ok(connection_t *conn, time_t now)
{
  tor_assert(conn);
  tor_assert(conn->magic == CONNECTION_MAGIC);
  tor_assert(conn->type >= _CONN_TYPE_MIN);
  tor_assert(conn->type <= _CONN_TYPE_MAX);

  if (conn->outbuf_flushlen > 0) {
    tor_assert(connection_is_writing(conn) || conn->wants_to_write);
  }

  if (conn->hold_open_until_flushed)
    tor_assert(conn->marked_for_close);

  /* XXX check: wants_to_read, wants_to_write, s, poll_index,
   * marked_for_close. */

  /* buffers */
  if (!connection_is_listener(conn)) {
    assert_buf_ok(conn->inbuf);
    assert_buf_ok(conn->outbuf);
  }

#if 0 /* computers often go back in time; no way to know */
  tor_assert(!now || conn->timestamp_lastread <= now);
  tor_assert(!now || conn->timestamp_lastwritten <= now);
  tor_assert(conn->timestamp_created <= conn->timestamp_lastread);
  tor_assert(conn->timestamp_created <= conn->timestamp_lastwritten);
#endif

  /* XXX Fix this; no longer so.*/
#if 0
  if (conn->type != CONN_TYPE_OR && conn->type != CONN_TYPE_DIR)
    tor_assert(!conn->pkey);
  /* pkey is set if we're a dir client, or if we're an OR in state OPEN
   * connected to another OR.
   */
#endif

  if (conn->type != CONN_TYPE_OR) {
    tor_assert(!conn->tls);
  } else {
    if (conn->state == OR_CONN_STATE_OPEN) {
      /* tor_assert(conn->bandwidth > 0); */
      /* the above isn't necessarily true: if we just did a TLS
       * handshake but we didn't recognize the other peer, or it
       * gave a bad cert/etc, then we won't have assigned bandwidth,
       * yet it will be open. -RD
       */
//      tor_assert(conn->receiver_bucket >= 0);
    }
//    tor_assert(conn->addr && conn->port);
    tor_assert(conn->address);
    if (conn->state > OR_CONN_STATE_PROXY_READING)
      tor_assert(conn->tls);
  }

  if (! CONN_IS_EDGE(conn)) {
    tor_assert(!conn->stream_id);
    tor_assert(!conn->next_stream);
    tor_assert(!conn->cpath_layer);
    tor_assert(!conn->package_window);
    tor_assert(!conn->deliver_window);
    tor_assert(!conn->done_sending);
    tor_assert(!conn->done_receiving);
  } else {
    /* XXX unchecked: package window, deliver window. */
  }
  if (conn->type == CONN_TYPE_AP) {
    tor_assert(conn->socks_request);
    if (conn->state == AP_CONN_STATE_OPEN) {
      tor_assert(conn->socks_request->has_finished);
      if (!conn->marked_for_close) {
        tor_assert(conn->cpath_layer);
        assert_cpath_layer_ok(conn->cpath_layer);
      }
    }
  } else {
    tor_assert(!conn->socks_request);
  }
  if (conn->type == CONN_TYPE_EXIT) {
    tor_assert(conn->purpose == EXIT_PURPOSE_CONNECT ||
               conn->purpose == EXIT_PURPOSE_RESOLVE);
  } else if (conn->type != CONN_TYPE_DIR) {
    tor_assert(!conn->purpose); /* only used for dir types currently */
  }

  switch (conn->type)
    {
    case CONN_TYPE_OR_LISTENER:
    case CONN_TYPE_AP_LISTENER:
    case CONN_TYPE_DIR_LISTENER:
    case CONN_TYPE_CONTROL_LISTENER:
      tor_assert(conn->state == LISTENER_STATE_READY);
      break;
    case CONN_TYPE_OR:
      tor_assert(conn->state >= _OR_CONN_STATE_MIN);
      tor_assert(conn->state <= _OR_CONN_STATE_MAX);
      break;
    case CONN_TYPE_EXIT:
      tor_assert(conn->state >= _EXIT_CONN_STATE_MIN);
      tor_assert(conn->state <= _EXIT_CONN_STATE_MAX);
      break;
    case CONN_TYPE_AP:
      tor_assert(conn->state >= _AP_CONN_STATE_MIN);
      tor_assert(conn->state <= _AP_CONN_STATE_MAX);
      tor_assert(conn->socks_request);
      break;
    case CONN_TYPE_DIR:
      tor_assert(conn->state >= _DIR_CONN_STATE_MIN);
      tor_assert(conn->state <= _DIR_CONN_STATE_MAX);
      tor_assert(conn->purpose >= _DIR_PURPOSE_MIN);
      tor_assert(conn->purpose <= _DIR_PURPOSE_MAX);
      break;
    case CONN_TYPE_DNSWORKER:
      tor_assert(conn->state == DNSWORKER_STATE_IDLE ||
                 conn->state == DNSWORKER_STATE_BUSY);
      break;
    case CONN_TYPE_CPUWORKER:
      tor_assert(conn->state >= _CPUWORKER_STATE_MIN);
      tor_assert(conn->state <= _CPUWORKER_STATE_MAX);
      break;
    case CONN_TYPE_CONTROL:
      tor_assert(conn->state >= _CONTROL_CONN_STATE_MIN);
      tor_assert(conn->state <= _CONTROL_CONN_STATE_MAX);
      break;
    default:
      tor_assert(0);
  }
}
