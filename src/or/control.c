/* Copyright 2004 Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char control_c_id[] = "$Id$";

/**
 * \file control.c
 *
 * \brief Implementation for Tor's control-socket interface.
 */

#include "or.h"

/* Protocol outline: a bidirectional stream, over which each side
 * sends a series of messages.  Each message has a two-byte length field,
 * a two-byte typecode, and a variable-length body whose length is
 * given in the length field.
 *
 * By default, the server only sends messages in response to client messages.
 * Every client message gets a message in response.  The client may, however,
 * _request_ that other messages be delivered asynchronously.
 *
 *
 * Every message type is either client-only or server-only, and every
 * server message type is either synchronous-only (only occurs in
 * response to a client request) or asynchronous-only (never is an
 * answer to a client request.
 *
 * See control-spec.txt for full details.
 */

/* Recognized message type codes. */
#define CONTROL_CMD_ERROR        0x0000
#define CONTROL_CMD_DONE         0x0001
#define CONTROL_CMD_SETCONF      0x0002
#define CONTROL_CMD_GETCONF      0x0003
#define CONTROL_CMD_CONFVALUE    0x0004
#define CONTROL_CMD_SETEVENTS    0x0005
#define CONTROL_CMD_EVENT        0x0006
#define CONTROL_CMD_AUTHENTICATE 0x0007
#define CONTROL_CMD_SAVECONF     0x0008
#define CONTROL_CMD_SIGNAL       0x0009
#define _CONTROL_CMD_MAX_RECOGNIZED 0x0009

/* Recognized error codes. */
#define ERR_UNSPECIFIED             0x0000
#define ERR_INTERNAL                0x0001
#define ERR_UNRECOGNIZED_TYPE       0x0002
#define ERR_SYNTAX                  0x0003
#define ERR_UNRECOGNIZED_CONFIG_KEY 0x0004
#define ERR_INVALID_CONFIG_VALUE    0x0005
#define ERR_UNRECOGNIZED_EVENT_CODE 0x0006
#define ERR_UNAUTHORIZED            0x0007
#define ERR_REJECTED_AUTHENTICATION 0x0008

/* Recognized asynchronous event types. */
#define _EVENT_MIN            0x0001
#define EVENT_CIRCUIT_STATUS  0x0001
#define EVENT_STREAM_STATUS   0x0002
#define EVENT_OR_CONN_STATUS  0x0003
#define EVENT_BANDWIDTH_USED  0x0004
#define EVENT_WARNING         0x0005
#define _EVENT_MAX            0x0005

/** Array mapping from message type codes to human-readable message
 * type names.  */
static const char * CONTROL_COMMANDS[] = {
  "error",
  "done",
  "setconf",
  "getconf",
  "confvalue",
  "setevents",
  "events",
  "authenticate",
  "saveconf",
};

/** Bitfield: The bit 1&lt;&lt;e is set if <b>any</b> open control
 * connection is interested in events of type <b>e</b>.  We use this
 * so that we can decide to skip generating event messages that nobody
 * has interest in without having to walk over the global connection
 * list to find out.
 **/
static uint32_t global_event_mask = 0;

/** Macro: true if any control connection is interested in events of type
 * <b>e</b>. */
#define EVENT_IS_INTERESTING(e) (global_event_mask & (1<<(e)))

/** If we're using cookie-type authentication, how long should our cookies be?
 */
#define AUTHENTICATION_COOKIE_LEN 32

/** If true, we've set authentication_cookie to a secret code and
 * stored it to disk. */
static int authentication_cookie_is_set = 0;
static char authentication_cookie[AUTHENTICATION_COOKIE_LEN];

static void update_global_event_mask(void);
static void send_control_message(connection_t *conn, uint16_t type,
                                 uint16_t len, const char *body);
static void send_control_done(connection_t *conn);
static void send_control_error(connection_t *conn, uint16_t error,
                               const char *message);
static void send_control_event(uint16_t event, uint16_t len, const char *body);
static int handle_control_setconf(connection_t *conn, uint16_t len,
                                  char *body);
static int handle_control_getconf(connection_t *conn, uint16_t len,
                                  const char *body);
static int handle_control_setevents(connection_t *conn, uint16_t len,
                                    const char *body);
static int handle_control_authenticate(connection_t *conn, uint16_t len,
                                       const char *body);
static int handle_control_saveconf(connection_t *conn, uint16_t len,
                                   const char *body);
static int handle_control_signal(connection_t *conn, uint16_t len,
                                 const char *body);

/** Given a possibly invalid message type code <b>cmd</b>, return a
 * human-readable string equivalent. */
static INLINE const char *
control_cmd_to_string(uint16_t cmd)
{
  return (cmd<=_CONTROL_CMD_MAX_RECOGNIZED) ? CONTROL_COMMANDS[cmd] : "Unknown";
}

/** Set <b>global_event_mask</b> to the bitwise OR of each live control
 * connection's event_mask field. */
static void update_global_event_mask(void)
{
  connection_t **conns;
  int n_conns, i;

  global_event_mask = 0;
  get_connection_array(&conns, &n_conns);
  for (i = 0; i < n_conns; ++i) {
    if (conns[i]->type == CONN_TYPE_CONTROL &&
        conns[i]->state == CONTROL_CONN_STATE_OPEN) {
      global_event_mask |= conns[i]->event_mask;
    }
  }
}

/** Send a message of type <b>type</b> containing <b>len</b> bytes
 * from <b>body</b> along the control connection <b>conn</b> */
static void
send_control_message(connection_t *conn, uint16_t type, uint16_t len,
                     const char *body)
{
  char buf[4];
  tor_assert(conn);
  tor_assert(len || !body);
  tor_assert(type <= _CONTROL_CMD_MAX_RECOGNIZED);
  set_uint16(buf, htons(len));
  set_uint16(buf+2, htons(type));
  connection_write_to_buf(buf, 4, conn);
  if (len)
    connection_write_to_buf(body, len, conn);
}

/** Send a "DONE" message down the control connection <b>conn</b> */
static void
send_control_done(connection_t *conn)
{
  send_control_message(conn, CONTROL_CMD_DONE, 0, NULL);
}

/** Send an error message with error code <b>error</b> and body
 * <b>message</b> down the connection <b>conn</b> */
static void
send_control_error(connection_t *conn, uint16_t error, const char *message)
{
  char buf[256];
  size_t len;
  set_uint16(buf, htons(error));
  len = strlen(message);
  tor_assert(len < (256-2));
  memcpy(buf+2, message, len);
  send_control_message(conn, CONTROL_CMD_ERROR, (uint16_t)(len+2), buf);
}

/** Send an 'event' message of event type <b>event</b>, containing
 * <b>len</b> bytes in <b>body</b> to every control connection that
 * is interested in it. */
static void
send_control_event(uint16_t event, uint16_t len, const char *body)
{
  connection_t **conns;
  int n_conns, i;
  size_t buflen;
  char *buf;

  buflen = len + 2;
  buf = tor_malloc_zero(buflen);
  set_uint16(buf, htons(event));
  memcpy(buf+2, body, len);

  get_connection_array(&conns, &n_conns);
  for (i = 0; i < n_conns; ++i) {
    if (conns[i]->type == CONN_TYPE_CONTROL &&
        conns[i]->state == CONTROL_CONN_STATE_OPEN &&
        conns[i]->event_mask & (1<<event)) {
      send_control_message(conns[i], CONTROL_CMD_EVENT, (uint16_t)(buflen), buf);
    }
  }

  tor_free(buf);
}

/** Called when we receive a SETCONF message: parse the body and try
 * to update our configuration.  Reply with a DONE or ERROR message. */
static int
handle_control_setconf(connection_t *conn, uint16_t len, char *body)
{
  int r;
  struct config_line_t *lines=NULL;

  if (config_get_lines(body, &lines) < 0) {
    log_fn(LOG_WARN,"Controller gave us config lines we can't parse.");
    send_control_error(conn, ERR_SYNTAX, "Couldn't parse configuration");
    return 0;
  }

  if ((r=config_trial_assign(lines, 1)) < 0) {
    log_fn(LOG_WARN,"Controller gave us config lines that didn't validate.");
    if (r==-1) {
      send_control_error(conn, ERR_UNRECOGNIZED_CONFIG_KEY,
                         "Unrecognized option");
    } else {
      send_control_error(conn, ERR_INVALID_CONFIG_VALUE,"Invalid option value");
    }
    config_free_lines(lines);
    return 0;
  }

  config_free_lines(lines);
  if (options_act() < 0) { /* acting on them failed. die. */
    log_fn(LOG_ERR,"Acting on config options left us in a broken state. Dying.");
    exit(1);
  }
  send_control_done(conn);
  return 0;
}

/** Called when we receive a GETCONF message.  Parse the request, and
 * reply with a CONFVALUE or an ERROR message */
static int
handle_control_getconf(connection_t *conn, uint16_t body_len, const char *body)
{
  smartlist_t *questions = NULL;
  smartlist_t *answers = NULL;
  char *msg = NULL;
  size_t msg_len;
  or_options_t *options = get_options();

  questions = smartlist_create();
  smartlist_split_string(questions, body, "\n",
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  answers = smartlist_create();
  SMARTLIST_FOREACH(questions, const char *, q,
  {
    int recognized = config_option_is_recognized(q);
    if (!recognized) {
      send_control_error(conn, ERR_UNRECOGNIZED_CONFIG_KEY, body);
      goto done;
    } else {
      struct config_line_t *answer = config_get_assigned_option(options,q);

      while (answer) {
        struct config_line_t *next;
        size_t alen = strlen(answer->key)+strlen(answer->value)+3;
        char *astr = tor_malloc(alen);
        tor_snprintf(astr, alen, "%s %s\n", answer->key, answer->value);
        smartlist_add(answers, astr);

        next = answer->next;
        tor_free(answer->key);
        tor_free(answer->value);
        tor_free(answer);
        answer = next;
      }
    }
  });

  msg = smartlist_join_strings(answers, "", 0, &msg_len);
  send_control_message(conn, CONTROL_CMD_CONFVALUE,
                       (uint16_t)msg_len, msg_len?msg:NULL);

 done:
  if (answers) SMARTLIST_FOREACH(answers, char *, cp, tor_free(cp));
  if (questions) SMARTLIST_FOREACH(questions, char *, cp, tor_free(cp));
  smartlist_free(answers);
  smartlist_free(questions);
  tor_free(msg);

  return 0;
}

/** Called when we get a SETEVENTS message: update conn->event_mask,
 * and reply with DONE or ERROR. */
static int
handle_control_setevents(connection_t *conn, uint16_t len, const char *body)
{
  uint16_t event_code;
  uint32_t event_mask = 0;
  if (len % 2) {
    send_control_error(conn, ERR_SYNTAX,
                       "Odd number of bytes in setevents message");
    return 0;
  }

  for (; len; len -= 2, body += 2) {
    event_code = ntohs(get_uint16(body));
    if (event_code < _EVENT_MIN || event_code > _EVENT_MAX) {
      send_control_error(conn, ERR_UNRECOGNIZED_EVENT_CODE,
                         "Unrecognized event code");
      return 0;
    }
    event_mask |= (1 << event_code);
  }

  conn->event_mask = event_mask;

  update_global_event_mask();
  send_control_done(conn);
  return 0;
}

/** Decode the hashed, base64'd password stored in <b>hashed</b>.  If
 * <b>buf</b> is provided, store the hashed password in the first
 * S2K_SPECIFIER_LEN+DIGEST_LEN bytes of <b>buf</b>.  Return 0 on
 * success, -1 on failure.
 */
int
decode_hashed_password(char *buf, const char *hashed)
{
  char decoded[64];
  if (base64_decode(decoded, sizeof(decoded), hashed, strlen(hashed))
      != S2K_SPECIFIER_LEN+DIGEST_LEN) {
    return -1;
  }
  if (buf)
    memcpy(buf, decoded, sizeof(decoded));
  return 0;
}

/** Called when we get an AUTHENTICATE message.  Check whether the
 * authentication is valid, and if so, update the connection's state to
 * OPEN.  Reply with DONE or ERROR.
 */
static int
handle_control_authenticate(connection_t *conn, uint16_t len, const char *body)
{
  or_options_t *options = get_options();
  if (options->CookieAuthentication) {
    if (len == AUTHENTICATION_COOKIE_LEN &&
        !memcmp(authentication_cookie, body, len)) {
      goto ok;
    }
  } else if (options->HashedControlPassword) {
    char expected[S2K_SPECIFIER_LEN+DIGEST_LEN];
    char received[DIGEST_LEN];
    if (decode_hashed_password(expected, options->HashedControlPassword)<0) {
      log_fn(LOG_WARN,"Couldn't decode HashedControlPassword: invalid base64");
      goto err;
    }
    secret_to_key(received,DIGEST_LEN,body,len,expected);
    if (!memcmp(expected+S2K_SPECIFIER_LEN, received, DIGEST_LEN))
      goto ok;
    goto err;
  } else {
    if (len == 0) {
      /* if Tor doesn't demand any stronger authentication, then
       * the controller can get in with a blank auth line. */
      goto ok;
    }
    goto err;
  }

 err:
  send_control_error(conn, ERR_REJECTED_AUTHENTICATION,"Authentication failed");
  return 0;
 ok:
  log_fn(LOG_INFO, "Authenticated control connection (%d)", conn->s);
  send_control_done(conn);
  conn->state = CONTROL_CONN_STATE_OPEN;
  return 0;
}

static int
handle_control_saveconf(connection_t *conn, uint16_t len,
                        const char *body)
{
  if (save_current_config()<0) {
    send_control_error(conn, ERR_INTERNAL,
                       "Unable to write configuration to disk.");
  } else {
    send_control_done(conn);
  }
  return 0;
}

static int
handle_control_signal(connection_t *conn, uint16_t len,
                      const char *body)
{
  if (len != 1) {
    send_control_error(conn, ERR_SYNTAX,
                       "Body of SIGNAL command too long or too short.");
  } else if (control_signal_act((uint8_t)body[0]) < 0) {
    send_control_error(conn, ERR_SYNTAX, "Unrecognized signal number.");
  } else {
    send_control_done(conn);
  }
  return 0;
}

/** Called when <b>conn</b> has no more bytes left on its outbuf. */
int
connection_control_finished_flushing(connection_t *conn) {
  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_CONTROL);

  connection_stop_writing(conn);
  return 0;
}

/** Called when <b>conn</b> has gotten its socket closed. */
int connection_control_reached_eof(connection_t *conn) {
  log_fn(LOG_INFO,"Control connection reached EOF. Closing.");
  connection_mark_for_close(conn);
  return 0;
}

/** Called when <b>conn</b> has received more bytes on its inbuf.
 */
int
connection_control_process_inbuf(connection_t *conn) {
  uint16_t body_len, command_type;
  char *body;

  tor_assert(conn);
  tor_assert(conn->type == CONN_TYPE_CONTROL);

 again:
  /* Try to suck a control message from the buffer. */
  switch (fetch_from_buf_control(conn->inbuf, &body_len, &command_type, &body))
    {
    case -1:
      tor_free(body);
      log_fn(LOG_WARN, "Error in control command. Failing.");
      return -1;
    case 0:
      /* Control command not all here yet. Wait. */
      return 0;
    case 1:
      /* We got a command. Process it. */
      break;
    default:
      tor_assert(0);
    }

  /* We got a command.  If we need authentication, only authentication
   * commands will be considered. */
  if (conn->state == CONTROL_CONN_STATE_NEEDAUTH &&
      command_type != CONTROL_CMD_AUTHENTICATE) {
    log_fn(LOG_WARN, "Rejecting '%s' command; authentication needed.",
           control_cmd_to_string(command_type));
    send_control_error(conn, ERR_UNAUTHORIZED, "Authentication required");
    tor_free(body);
    goto again;
  }

  /* Okay, we're willing to process the command. */
  switch (command_type)
    {
    case CONTROL_CMD_SETCONF:
      if (handle_control_setconf(conn, body_len, body))
        return -1;
      break;
    case CONTROL_CMD_GETCONF:
      if (handle_control_getconf(conn, body_len, body))
        return -1;
      break;
    case CONTROL_CMD_SETEVENTS:
      if (handle_control_setevents(conn, body_len, body))
        return -1;
      break;
    case CONTROL_CMD_AUTHENTICATE:
      if (handle_control_authenticate(conn, body_len, body))
        return -1;
      break;
    case CONTROL_CMD_SAVECONF:
      if (handle_control_saveconf(conn, body_len, body))
        return -1;
      break;
    case CONTROL_CMD_SIGNAL:
      if (handle_control_signal(conn, body_len, body))
        return -1;
      break;
    case CONTROL_CMD_ERROR:
    case CONTROL_CMD_DONE:
    case CONTROL_CMD_CONFVALUE:
    case CONTROL_CMD_EVENT:
      log_fn(LOG_WARN, "Received client-only '%s' command; ignoring.",
             control_cmd_to_string(command_type));
      send_control_error(conn, ERR_UNRECOGNIZED_TYPE,
                         "Command type only valid from server to tor client");
      break;
    default:
      log_fn(LOG_WARN, "Received unrecognized command type %d; ignoring.",
             (int)command_type);
      send_control_error(conn, ERR_UNRECOGNIZED_TYPE,
                         "Unrecognized command type");
      break;
  }
  tor_free(body);
  goto again; /* There might be more data. */
}

/** Something has happened to circuit <b>circ</b>: tell any interested
 * control connections. */
int
control_event_circuit_status(circuit_t *circ, circuit_status_event_t tp)
{
  char *path, *msg;
  size_t path_len;
  if (!EVENT_IS_INTERESTING(EVENT_CIRCUIT_STATUS))
    return 0;
  tor_assert(circ);
  tor_assert(CIRCUIT_IS_ORIGIN(circ));

  path = circuit_list_path(circ,0);
  path_len = strlen(path);
  msg = tor_malloc(1+4+path_len+1); /* event, circid, path, NUL. */
  msg[0] = (uint8_t) tp;
  set_uint32(msg+1, htonl(circ->global_identifier));
  strlcpy(msg+5,path,path_len+1);

  send_control_event(EVENT_CIRCUIT_STATUS, (uint16_t)(path_len+6), msg);
  tor_free(path);
  tor_free(msg);
  return 0;
}

/** Something has happened to the stream associated with AP connection
 * <b>conn</b>: tell any interested control connections. */
int
control_event_stream_status(connection_t *conn, stream_status_event_t tp)
{
  char *msg;
  size_t len;
  char buf[256];
  tor_assert(conn->type == CONN_TYPE_AP);
  tor_assert(conn->socks_request);

  if (!EVENT_IS_INTERESTING(EVENT_STREAM_STATUS))
    return 0;

  tor_snprintf(buf, sizeof(buf), "%s:%d",
               conn->socks_request->address, conn->socks_request->port),
  len = strlen(buf);
  msg = tor_malloc(5+len+1);
  msg[0] = (uint8_t) tp;
  set_uint32(msg+1, htonl(conn->s)); /* ???? Is this a security problem? */
  strlcpy(msg+5, buf, len+1);

  send_control_event(EVENT_STREAM_STATUS, (uint16_t)(5+len+1), msg);
  tor_free(msg);
  return 0;
}

/** Something has happened to the OR connection <b>conn</b>: tell any
 * interested control connections. */
int
control_event_or_conn_status(connection_t *conn,or_conn_status_event_t tp)
{
  char buf[HEX_DIGEST_LEN+3]; /* status, dollar, identity, NUL */
  size_t len;

  tor_assert(conn->type == CONN_TYPE_OR);

  if (!EVENT_IS_INTERESTING(EVENT_OR_CONN_STATUS))
    return 0;

  buf[0] = (uint8_t)tp;
  strlcpy(buf+1,conn->nickname,sizeof(buf)-1);
  len = strlen(buf+1);
  send_control_event(EVENT_OR_CONN_STATUS, (uint16_t)(len+1), buf);
  return 0;
}

/** A second or more has elapsed: tell any interested control
 * connections how much bandwidth we used. */
int
control_event_bandwidth_used(uint32_t n_read, uint32_t n_written)
{
  char buf[8];

  if (!EVENT_IS_INTERESTING(EVENT_BANDWIDTH_USED))
    return 0;

  set_uint32(buf, htonl(n_read));
  set_uint32(buf+4, htonl(n_written));
  send_control_event(EVENT_BANDWIDTH_USED, 8, buf);

  return 0;
}

/** We got a log message: tell any interested control connections. */
void
control_event_logmsg(int severity, const char *msg)
{
  size_t len;
  if (severity > LOG_NOTICE) /* Less important than notice? ignore for now. */
    return;
  if (!EVENT_IS_INTERESTING(EVENT_WARNING))
    return;

  len = strlen(msg);
  send_control_event(EVENT_WARNING, (uint16_t)(len+1), msg);
}

/** Choose a random authentication cookie and write it to disk.
 * Anybody who can read the cookie from disk will be considered
 * authorized to use the control connection. */
int
init_cookie_authentication(int enabled)
{
  char fname[512];

  if (!enabled) {
    authentication_cookie_is_set = 0;
    return 0;
  }

  tor_snprintf(fname, sizeof(fname), "%s/control_auth_cookie",
               get_options()->DataDirectory);
  crypto_rand(authentication_cookie, AUTHENTICATION_COOKIE_LEN);
  authentication_cookie_is_set = 1;
  if (write_bytes_to_file(fname, authentication_cookie,
                          AUTHENTICATION_COOKIE_LEN, 1)) {
    log_fn(LOG_WARN,"Error writing authentication cookie.");
    return -1;
  }

  return 0;
}

