/* Copyright 2001,2002,2003 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"
#include "tree.h"

extern or_options_t options; /* command-line and config-file options */
extern char *conn_state_to_string[][_CONN_TYPE_MAX+1];

static int connection_ap_handshake_process_socks(connection_t *conn);
static int connection_ap_handshake_attach_circuit(connection_t *conn);
static void connection_ap_handshake_send_begin(connection_t *ap_conn, circuit_t *circ);
static int connection_ap_handshake_socks_reply(connection_t *conn, char *reply,
                                               int replylen, char success);

static int connection_exit_begin_conn(cell_t *cell, circuit_t *circ);
static void connection_edge_consider_sending_sendme(connection_t *conn);

static uint32_t client_dns_lookup_entry(const char *address);
static void client_dns_set_entry(const char *address, uint32_t val);

void relay_header_pack(char *dest, const relay_header_t *src) {
  *(uint8_t*)(dest)    = src->command;
  *(uint16_t*)(dest+1) = htons(src->recognized);
  *(uint16_t*)(dest+3) = htons(src->stream_id);
  *(uint32_t*)(dest+5) = htonl(src->integrity);
  *(uint16_t*)(dest+9) = htons(src->length);
}

void relay_header_unpack(relay_header_t *dest, const char *src) {
  dest->command    = *(uint8_t*)(src);
  dest->recognized = ntohs(*(uint16_t*)(src+1));
  dest->stream_id  = ntohs(*(uint16_t*)(src+3));
  dest->integrity  = ntohl(*(uint32_t*)(src+5));
  dest->length     = ntohs(*(uint16_t*)(src+9));
}

int connection_edge_process_inbuf(connection_t *conn) {

  assert(conn);
  assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  if(conn->inbuf_reached_eof) {
#ifdef HALF_OPEN
    /* eof reached; we're done reading, but we might want to write more. */
    conn->done_receiving = 1;
    shutdown(conn->s, 0); /* XXX check return, refactor NM */
    if (conn->done_sending) {
      if(connection_edge_end(conn, END_STREAM_REASON_DONE, conn->cpath_layer) < 0)
        log_fn(LOG_WARN,"1: I called connection_edge_end redundantly.");
    } else {
      connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_END,
                                   NULL, 0, conn->cpath_layer);
    }
    return 0;
#else
    /* eof reached, kill it. */
    log_fn(LOG_INFO,"conn (fd %d) reached eof. Closing.", conn->s);
    if(connection_edge_end(conn, END_STREAM_REASON_DONE, conn->cpath_layer) < 0)
      log_fn(LOG_WARN,"2: I called connection_edge_end redundantly.");
    return -1;
#endif
  }

  switch(conn->state) {
    case AP_CONN_STATE_SOCKS_WAIT:
      if(connection_ap_handshake_process_socks(conn) < 0) {
        if(connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer) < 0)
          log_fn(LOG_WARN,"3: I called connection_edge_end redundantly.");
        return -1;
      }
      return 0;
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      if(conn->package_window <= 0) {
        log_fn(LOG_WARN,"called with package_window %d. Tell Roger.", conn->package_window);
        return 0;
      }
      if(connection_edge_package_raw_inbuf(conn) < 0) {
        if(connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer) < 0)
          log_fn(LOG_WARN,"4: I called connection_edge_end redundantly.");
        return -1;
      }
      return 0;
    case EXIT_CONN_STATE_CONNECTING:
      log_fn(LOG_INFO,"text from server while in 'connecting' state at exit. Leaving it on buffer.");
      return 0;
  }

  return 0;
}

static char *connection_edge_end_reason(char *payload, uint16_t length) {
  if(length < 1) {
    log_fn(LOG_WARN,"End cell arrived with length 0. Should be at least 1.");
    return "MALFORMED";
  }
  if(*payload < END_STREAM_REASON_MISC || *payload > END_STREAM_REASON_DONE) {
    log_fn(LOG_WARN,"Reason for ending (%d) not recognized.",*payload);
    return "MALFORMED";
  }
  switch(*payload) {
    case END_STREAM_REASON_MISC:           return "misc error";
    case END_STREAM_REASON_RESOLVEFAILED:  return "resolve failed";
    case END_STREAM_REASON_CONNECTFAILED:  return "connect failed";
    case END_STREAM_REASON_EXITPOLICY:     return "exit policy failed";
    case END_STREAM_REASON_DESTROY:        return "destroyed";
    case END_STREAM_REASON_DONE:           return "closed normally";
  }
  assert(0);
  return "";
}

int connection_edge_end(connection_t *conn, char reason, crypt_path_t *cpath_layer) {
  char payload[5];
  int payload_len=1;
  circuit_t *circ;

  if(conn->has_sent_end) {
    log_fn(LOG_WARN,"It appears I've already sent the end. Are you calling me twice?");
    return -1;
  }

  payload[0] = reason;
  if(reason == END_STREAM_REASON_EXITPOLICY) {
    *(uint32_t *)(payload+1) = htonl(conn->addr);
    payload_len += 4;
  }

  circ = circuit_get_by_conn(conn);
  if(circ) {
    log_fn(LOG_DEBUG,"Marking conn (fd %d) and sending end.",conn->s);
    connection_edge_send_command(conn, circ, RELAY_COMMAND_END,
                                 payload, payload_len, cpath_layer);
  }

  conn->marked_for_close = 1;
  conn->has_sent_end = 1;
  return 0;
}

int connection_edge_send_command(connection_t *fromconn, circuit_t *circ, int relay_command,
                                 void *payload, int payload_len, crypt_path_t *cpath_layer) {
  cell_t cell;
  relay_header_t rh;
  int cell_direction;

  if(!circ) {
    log_fn(LOG_WARN,"no circ. Closing.");
    return -1;
  }

  memset(&cell, 0, sizeof(cell_t));
  cell.command = CELL_RELAY;
//  if(fromconn && fromconn->type == CONN_TYPE_AP) {
  if(cpath_layer) {
    cell.circ_id = circ->n_circ_id;
    cell_direction = CELL_DIRECTION_OUT;
  } else {
    cell.circ_id = circ->p_circ_id;
    cell_direction = CELL_DIRECTION_IN;
  }

  memset(&rh, 0, sizeof(rh));
  rh.command = relay_command;
  if(fromconn)
    rh.stream_id = fromconn->stream_id; /* else it's 0 */
  rh.length = payload_len;
  relay_header_pack(cell.payload, &rh);
  if(payload_len)
    memcpy(cell.payload+RELAY_HEADER_SIZE, payload, payload_len);

  log_fn(LOG_DEBUG,"delivering %d cell %s.", relay_command,
         cell_direction == CELL_DIRECTION_OUT ? "forward" : "backward");

  if(circuit_package_relay_cell(&cell, circ, cell_direction, cpath_layer) < 0) {
    log_fn(LOG_WARN,"circuit_package_relay_cell failed. Closing.");
    circuit_close(circ);
    return -1;
  }
  return 0;
}

/* an incoming relay cell has arrived. return -1 if you want to tear down the
 * circuit, else 0. */
int connection_edge_process_relay_cell(cell_t *cell, circuit_t *circ, connection_t *conn,
                                       int edge_type, crypt_path_t *layer_hint) {
  static int num_seen=0;
  uint32_t addr;
  relay_header_t rh;

  assert(cell && circ);

  relay_header_unpack(&rh, cell->payload);
//  log_fn(LOG_DEBUG,"command %d stream %d", rh.command, rh.stream_id);
  num_seen++;
  log_fn(LOG_DEBUG,"Now seen %d relay cells here.", num_seen);

  /* either conn is NULL, in which case we've got a control cell, or else
   * conn points to the recognized stream. */

  if(conn && conn->state != AP_CONN_STATE_OPEN && conn->state != EXIT_CONN_STATE_OPEN) {
    if(conn->type == CONN_TYPE_EXIT && rh.command == RELAY_COMMAND_END) {
      log_fn(LOG_INFO,"Exit got end (%s) before we're connected. Marking for close.",
        connection_edge_end_reason(cell->payload+RELAY_HEADER_SIZE, rh.length));
      if(conn->state == EXIT_CONN_STATE_RESOLVING) {
        log_fn(LOG_INFO,"...and informing resolver we don't want the answer anymore.");
        dns_cancel_pending_resolve(conn->address, conn);
      }
      conn->marked_for_close = 1;
      conn->has_sent_end = 1;
      return 0;
    } else {
      log_fn(LOG_WARN,"Got an unexpected relay command %d, in state %d (%s). Closing.",
             rh.command, conn->state, conn_state_to_string[conn->type][conn->state]);
      if(connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer) < 0)
        log_fn(LOG_WARN,"1: I called connection_edge_end redundantly.");
      return -1;
    }
  }

  switch(rh.command) {
    case RELAY_COMMAND_DROP:
      log_fn(LOG_INFO,"Got a relay-level padding cell. Dropping.");
      return 0;
    case RELAY_COMMAND_BEGIN:
      if(edge_type == EDGE_AP) {
        log_fn(LOG_WARN,"relay begin request unsupported at AP. Dropping.");
        return 0;
      }
      if(conn) {
        log_fn(LOG_WARN,"begin cell for known stream. Dropping.");
        return 0;
      }
      connection_exit_begin_conn(cell, circ);
      return 0;
    case RELAY_COMMAND_DATA:
      ++stats_n_data_cells_received;
      if((edge_type == EDGE_AP && --layer_hint->deliver_window < 0) ||
         (edge_type == EDGE_EXIT && --circ->deliver_window < 0)) {
        log_fn(LOG_WARN,"(relay data) circ deliver_window below 0. Killing.");
        if(connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer) < 0)
          log_fn(LOG_WARN,"2: I called connection_edge_end redundantly.");
        return -1;
      }
      log_fn(LOG_DEBUG,"circ deliver_window now %d.", edge_type == EDGE_AP ?
             layer_hint->deliver_window : circ->deliver_window);

      circuit_consider_sending_sendme(circ, edge_type, layer_hint);

      if(!conn) {
        log_fn(LOG_INFO,"data cell dropped, unknown stream.");
        return 0;
      }

      if(--conn->deliver_window < 0) { /* is it below 0 after decrement? */
        log_fn(LOG_WARN,"(relay data) conn deliver_window below 0. Killing.");
        return -1; /* somebody's breaking protocol. kill the whole circuit. */
      }

      stats_n_data_bytes_received += rh.length;
      connection_write_to_buf(cell->payload + RELAY_HEADER_SIZE,
                              rh.length, conn);
      connection_edge_consider_sending_sendme(conn);
      return 0;
    case RELAY_COMMAND_END:
      if(!conn) {
        log_fn(LOG_INFO,"end cell (%s) dropped, unknown stream.",
          connection_edge_end_reason(cell->payload+RELAY_HEADER_SIZE, rh.length));
        return 0;
      }
      if(rh.length >= 5 &&
         *(cell->payload+RELAY_HEADER_SIZE) == END_STREAM_REASON_EXITPOLICY) {
        /* No need to close the connection. We'll hold it open while
         * we try a new exit node.
         * cell->payload+RELAY_HEADER_SIZE+1 holds the destination addr.
         */
        addr = ntohl(*(uint32_t*)(cell->payload+RELAY_HEADER_SIZE+1));
        client_dns_set_entry(conn->socks_request->address, addr);
        conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
        switch(connection_ap_handshake_attach_circuit(conn)) {
          case -1: /* it will never work */
            break; /* conn will get closed below */
          case 0: /* no useful circuits available */
            if(!circuit_get_newest(conn, 0)) /* is one already on the way? */
              circuit_launch_new();
            return 0;
          case 1: /* it succeeded, great */
            return 0;
        }
      }
      log_fn(LOG_INFO,"end cell (%s) for stream %d. Removing stream.",
        connection_edge_end_reason(cell->payload+RELAY_HEADER_SIZE, rh.length),
        conn->stream_id);

#ifdef HALF_OPEN
      conn->done_sending = 1;
      shutdown(conn->s, 1); /* XXX check return; refactor NM */
      if (conn->done_receiving) {
        conn->marked_for_close = 1;
        conn->has_sent_end = 1; /* no need to send end, we just got one! */
      }
#else
      conn->marked_for_close = 1;
      conn->has_sent_end = 1; /* no need to send end, we just got one! */
#endif
      return 0;
    case RELAY_COMMAND_EXTEND:
      if(conn) {
        log_fn(LOG_WARN,"'extend' for non-zero stream. Dropping.");
        return 0;
      }
      return circuit_extend(cell, circ);
    case RELAY_COMMAND_EXTENDED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_WARN,"'extended' unsupported at exit. Dropping.");
        return 0;
      }
      log_fn(LOG_DEBUG,"Got an extended cell! Yay.");
      if(circuit_finish_handshake(circ, cell->payload+RELAY_HEADER_SIZE) < 0) {
        log_fn(LOG_WARN,"circuit_finish_handshake failed.");
        return -1;
      }
      if (circuit_send_next_onion_skin(circ)<0) {
        log_fn(LOG_INFO,"circuit_send_next_onion_skin() failed.");
        return -1;
      }
      return 0;
    case RELAY_COMMAND_TRUNCATE:
      if(edge_type == EDGE_AP) {
        log_fn(LOG_WARN,"'truncate' unsupported at AP. Dropping.");
        return 0;
      }
      if(circ->n_conn) {
        connection_send_destroy(circ->n_circ_id, circ->n_conn);
        circ->n_conn = NULL;
      }
      log_fn(LOG_DEBUG, "Processed 'truncate', replying.");
      connection_edge_send_command(NULL, circ, RELAY_COMMAND_TRUNCATED,
                                   NULL, 0, NULL);
      return 0;
    case RELAY_COMMAND_TRUNCATED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_WARN,"'truncated' unsupported at exit. Dropping.");
        return 0;
      }
      circuit_truncated(circ, layer_hint);
      return 0;
    case RELAY_COMMAND_CONNECTED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_WARN,"'connected' unsupported at exit. Dropping.");
        return 0;
      }
      if(!conn) {
        log_fn(LOG_INFO,"connected cell dropped, unknown stream.");
        return 0;
      }
      log_fn(LOG_INFO,"Connected! Notifying application.");
      if (rh.length >= 4) {
        addr = ntohl(*(uint32_t*)(cell->payload + RELAY_HEADER_SIZE));
        client_dns_set_entry(conn->socks_request->address, addr);
      }
      if(connection_ap_handshake_socks_reply(conn, NULL, 0, 1) < 0) {
        log_fn(LOG_INFO,"Writing to socks-speaking application failed. Closing.");
        if(connection_edge_end(conn, END_STREAM_REASON_MISC, conn->cpath_layer) < 0)
          log_fn(LOG_WARN,"3: I called connection_edge_end redundantly.");
      }
      return 0;
    case RELAY_COMMAND_SENDME:
      if(!conn) {
        if(edge_type == EDGE_AP) {
          assert(layer_hint);
          layer_hint->package_window += CIRCWINDOW_INCREMENT;
          log_fn(LOG_DEBUG,"circ-level sendme at AP, packagewindow %d.",
                 layer_hint->package_window);
          circuit_resume_edge_reading(circ, EDGE_AP, layer_hint);
        } else {
          assert(!layer_hint);
          circ->package_window += CIRCWINDOW_INCREMENT;
          log_fn(LOG_DEBUG,"circ-level sendme at exit, packagewindow %d.",
                 circ->package_window);
          circuit_resume_edge_reading(circ, EDGE_EXIT, layer_hint);
        }
        return 0;
      }
      conn->package_window += STREAMWINDOW_INCREMENT;
      log_fn(LOG_DEBUG,"stream-level sendme, packagewindow now %d.", conn->package_window);
      connection_start_reading(conn);
      connection_edge_package_raw_inbuf(conn); /* handle whatever might still be on the inbuf */
      return 0;
  }
  log_fn(LOG_WARN,"unknown relay command %d.",rh.command);
  return -1;
}

int connection_edge_finished_flushing(connection_t *conn) {
  unsigned char connected_payload[4];
  int e, len=sizeof(e);

  assert(conn);
  assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  switch(conn->state) {
    case EXIT_CONN_STATE_CONNECTING:
      if (getsockopt(conn->s, SOL_SOCKET, SO_ERROR, (void*)&e, &len) < 0)  { /* not yet */
        if(!ERRNO_CONN_EINPROGRESS(errno)) {
          /* yuck. kill it. */
          log_fn(LOG_DEBUG,"in-progress exit connect failed. Removing.");
          return -1;
        } else {
          log_fn(LOG_DEBUG,"in-progress exit connect still waiting.");
          return 0; /* no change, see if next time is better */
        }
      }
      /* the connect has finished. */

      log_fn(LOG_INFO,"Exit connection to %s:%u established.",
          conn->address,conn->port);

      conn->state = EXIT_CONN_STATE_OPEN;
      connection_watch_events(conn, POLLIN); /* stop writing, continue reading */
      if(connection_wants_to_flush(conn)) /* in case there are any queued relay cells */
        connection_start_writing(conn);
      /* deliver a 'connected' relay cell back through the circuit. */
      *(uint32_t*)connected_payload = htonl(conn->addr);
      if(connection_edge_send_command(conn, circuit_get_by_conn(conn),
         RELAY_COMMAND_CONNECTED, NULL, 0, NULL) < 0)
        return 0; /* circuit is closed, don't continue */
      assert(conn->package_window > 0);
      return connection_edge_process_inbuf(conn); /* in case the server has written anything */
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      connection_stop_writing(conn);
      connection_edge_consider_sending_sendme(conn);
      return 0;
    case AP_CONN_STATE_SOCKS_WAIT:
    case AP_CONN_STATE_CIRCUIT_WAIT:
      connection_stop_writing(conn);
      return 0;
    default:
      log_fn(LOG_WARN,"BUG: called in unexpected state: %d", conn->state);
      return -1;
  }
  return 0;
}

uint64_t stats_n_data_cells_packaged = 0;
uint64_t stats_n_data_bytes_packaged = 0;
uint64_t stats_n_data_cells_received = 0;
uint64_t stats_n_data_bytes_received = 0;

int connection_edge_package_raw_inbuf(connection_t *conn) {
  int amount_to_process, length;
  char payload[CELL_PAYLOAD_SIZE];
  circuit_t *circ;

  assert(conn);
  assert(!connection_speaks_cells(conn));

repeat_connection_edge_package_raw_inbuf:

  circ = circuit_get_by_conn(conn);
  if(!circ) {
    log_fn(LOG_INFO,"conn has no circuits! Closing.");
    return -1;
  }

  if(circuit_consider_stop_edge_reading(circ, conn->type, conn->cpath_layer))
    return 0;

  if(conn->package_window <= 0) {
    log_fn(LOG_WARN,"called with package_window %d. Tell Roger.", conn->package_window);
    connection_stop_reading(conn);
    return 0;
  }

  amount_to_process = buf_datalen(conn->inbuf);

  if(!amount_to_process)
    return 0;

  if(amount_to_process > RELAY_PAYLOAD_SIZE) {
    length = RELAY_PAYLOAD_SIZE;
  } else {
    length = amount_to_process;
  }
  stats_n_data_bytes_packaged += length;
  stats_n_data_cells_packaged += 1;

  connection_fetch_from_buf(payload, length, conn);

  log_fn(LOG_DEBUG,"(%d) Packaging %d bytes (%d waiting).", conn->s, length,
         (int)buf_datalen(conn->inbuf));

  if(connection_edge_send_command(conn, circ, RELAY_COMMAND_DATA,
                               payload, length, conn->cpath_layer) < 0)
    return 0; /* circuit is closed, don't continue */

  if(conn->type == CONN_TYPE_EXIT) {
    assert(circ->package_window > 0);
    circ->package_window--;
  } else { /* we're an AP */
    assert(conn->type == CONN_TYPE_AP);
    assert(conn->cpath_layer->package_window > 0);
    conn->cpath_layer->package_window--;
  }

  if(--conn->package_window <= 0) { /* is it 0 after decrement? */
    connection_stop_reading(conn);
    log_fn(LOG_DEBUG,"conn->package_window reached 0.");
    circuit_consider_stop_edge_reading(circ, conn->type, conn->cpath_layer);
    return 0; /* don't process the inbuf any more */
  }
  log_fn(LOG_DEBUG,"conn->package_window is now %d",conn->package_window);

  /* handle more if there's more, or return 0 if there isn't */
  goto repeat_connection_edge_package_raw_inbuf;
}

/* Tell any APs that are waiting for a new circuit that one is available */
void connection_ap_attach_pending(void)
{
  connection_t **carray;
  connection_t *conn;
  int n, i;

  get_connection_array(&carray, &n);

  for (i = 0; i < n; ++i) {
    conn = carray[i];
    if (conn->type != CONN_TYPE_AP ||
        conn->state != AP_CONN_STATE_CIRCUIT_WAIT)
      continue;
    switch(connection_ap_handshake_attach_circuit(conn)) {
      case -1: /* it will never work */
        conn->marked_for_close = 1;
        conn->has_sent_end = 1;
        break;
      case 0: /* we need to build another circuit */
        if(!circuit_get_newest(conn, 0)) {
          /* if there are no acceptable clean or not-very-dirty circs on the way */
          circuit_launch_new();
        }
        break;
      case 1: /* it succeeded, great */
        break;
    }
  }
}

static void connection_edge_consider_sending_sendme(connection_t *conn) {
  circuit_t *circ;

  if(connection_outbuf_too_full(conn))
    return;

  circ = circuit_get_by_conn(conn);
  if(!circ) {
    /* this can legitimately happen if the destroy has already
     * arrived and torn down the circuit */
    log_fn(LOG_INFO,"No circuit associated with conn. Skipping.");
    return;
  }

  while(conn->deliver_window < STREAMWINDOW_START - STREAMWINDOW_INCREMENT) {
    log_fn(LOG_DEBUG,"Outbuf %d, Queueing stream sendme.", conn->outbuf_flushlen);
    conn->deliver_window += STREAMWINDOW_INCREMENT;
    if(connection_edge_send_command(conn, circ, RELAY_COMMAND_SENDME,
                                    NULL, 0, conn->cpath_layer) < 0) {
      log_fn(LOG_WARN,"connection_edge_send_command failed. Returning.");
      return; /* the circuit's closed, don't continue */
    }
  }
}

static int connection_ap_handshake_process_socks(connection_t *conn) {
  socks_request_t *socks;
  int sockshere;

  assert(conn);
  assert(conn->type == CONN_TYPE_AP);
  assert(conn->state == AP_CONN_STATE_SOCKS_WAIT);
  assert(conn->socks_request);
  socks = conn->socks_request;

  log_fn(LOG_DEBUG,"entered.");

  sockshere = fetch_from_buf_socks(conn->inbuf, socks);
  if(sockshere == -1 || sockshere == 0) {
    if(socks->replylen) { /* we should send reply back */
      log_fn(LOG_DEBUG,"reply is already set for us. Using it.");
      connection_ap_handshake_socks_reply(conn, socks->reply, socks->replylen, 0);
    } else if(sockshere == -1) { /* send normal reject */
      log_fn(LOG_WARN,"Fetching socks handshake failed. Closing.");
      connection_ap_handshake_socks_reply(conn, NULL, 0, 0);
    } else {
      log_fn(LOG_DEBUG,"socks handshake not all here yet.");
    }
    return sockshere;
  } /* else socks handshake is done, continue processing */

  conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
  switch(connection_ap_handshake_attach_circuit(conn)) {
    case -1: /* it will never work */
      return -1;
    case 0: /* no useful circuits available */
      if(!circuit_get_newest(conn, 0)) /* is one already on the way? */
        circuit_launch_new();
      break;
    case 1: /* it succeeded, great */
      break;
  }
  return 0;
}

/* Try to find a safe live circuit for CONN_TYPE_AP connection conn. If
 * we don't find one: if conn cannot be handled by any known nodes,
 * warn and return -1; else tell conn to stop reading and return 0.
 * Otherwise, associate conn with a safe live circuit, start
 * sending a BEGIN cell down the circuit, and return 1.
 */
static int connection_ap_handshake_attach_circuit(connection_t *conn) {
  circuit_t *circ;
  uint32_t addr;

  assert(conn);
  assert(conn->type == CONN_TYPE_AP);
  assert(conn->state == AP_CONN_STATE_CIRCUIT_WAIT);
  assert(conn->socks_request);

  /* find the circuit that we should use, if there is one. */
  circ = circuit_get_newest(conn, 1);

  if(!circ) {
    log_fn(LOG_INFO,"No safe circuit ready for edge connection; delaying.");
    addr = client_dns_lookup_entry(conn->socks_request->address);
    if(router_exit_policy_all_routers_reject(addr, conn->socks_request->port)) {
      log_fn(LOG_WARN,"No node exists that will handle exit to %s:%d. Rejecting.",
             conn->socks_request->address, conn->socks_request->port);
      return -1;
    }
    connection_stop_reading(conn); /* don't read until the connected cell arrives */
    return 0;
  }

  connection_start_reading(conn);

  if(!circ->timestamp_dirty)
    circ->timestamp_dirty = time(NULL);

  /* add it into the linked list of streams on this circuit */
  log_fn(LOG_DEBUG,"attaching new conn to circ. n_circ_id %d.", circ->n_circ_id);
  conn->next_stream = circ->p_streams;
  /* assert_connection_ok(conn, time(NULL)); */
  circ->p_streams = conn;

  assert(circ->cpath && circ->cpath->prev);
  assert(circ->cpath->prev->state == CPATH_STATE_OPEN);
  conn->cpath_layer = circ->cpath->prev;

  connection_ap_handshake_send_begin(conn, circ);

  return 1;
}

/* Iterate over the two bytes of stream_id until we get one that is not
 * already in use. Return 0 if can't get a unique stream_id.
 */
static uint16_t get_unique_stream_id_by_circ(circuit_t *circ) {
  connection_t *tmpconn;
  uint16_t test_stream_id;
  uint32_t attempts=0;

again:
  test_stream_id = circ->next_stream_id++;
  if(++attempts > 1<<16) {
    /* Make sure we don't loop forever if all stream_id's are used. */
    log_fn(LOG_WARN,"No unused stream IDs. Failing.");
    return 0;
  }
  if (test_stream_id == 0)
    goto again;
  for(tmpconn = circ->p_streams; tmpconn; tmpconn=tmpconn->next_stream)
    if(tmpconn->stream_id == test_stream_id)
      goto again;
  return test_stream_id;
}

/* deliver the destaddr:destport in a relay cell */
static void connection_ap_handshake_send_begin(connection_t *ap_conn, circuit_t *circ)
{
  char payload[CELL_PAYLOAD_SIZE];
  int payload_len;
  struct in_addr in;
  const char *string_addr;

  assert(ap_conn->type == CONN_TYPE_AP);
  assert(ap_conn->state == AP_CONN_STATE_CIRCUIT_WAIT);
  assert(ap_conn->socks_request);

  ap_conn->stream_id = get_unique_stream_id_by_circ(circ);
  if (ap_conn->stream_id==0) {
    ap_conn->marked_for_close = 1;
    return;
  }

  in.s_addr = htonl(client_dns_lookup_entry(ap_conn->socks_request->address));
  string_addr = in.s_addr ? inet_ntoa(in) : NULL;

  snprintf(payload,RELAY_PAYLOAD_SIZE,
           "%s:%d",
           string_addr ? string_addr : ap_conn->socks_request->address,
           ap_conn->socks_request->port);
  payload_len = strlen(payload)+1;

  log_fn(LOG_DEBUG,"Sending relay cell to begin stream %d.",ap_conn->stream_id);

  if(connection_edge_send_command(ap_conn, circ, RELAY_COMMAND_BEGIN,
                               payload, payload_len, ap_conn->cpath_layer) < 0)
    return; /* circuit is closed, don't continue */

  ap_conn->package_window = STREAMWINDOW_START;
  ap_conn->deliver_window = STREAMWINDOW_START;
  ap_conn->state = AP_CONN_STATE_OPEN;
  /* XXX Right now, we rely on the socks client not to send us any data
   * XXX until we've sent back a socks reply.  (If it does, we could wind
   * XXX up packaging that data and sending it to the exit, then later having
   * XXX the exit refuse us.)
   * XXX Perhaps we should grow an AP_CONN_STATE_CONNECTING state.
   */
  log_fn(LOG_INFO,"Address/port sent, ap socket %d, n_circ_id %d",ap_conn->s,circ->n_circ_id);
  return;
}

static int connection_ap_handshake_socks_reply(connection_t *conn, char *reply,
                                               int replylen, char success) {
  char buf[256];

  if(replylen) { /* we already have a reply in mind */
    connection_write_to_buf(reply, replylen, conn);
    return flush_buf(conn->s, conn->outbuf, &conn->outbuf_flushlen); /* try to flush it */
  }
  assert(conn->socks_request);
  if(conn->socks_request->socks_version == 4) {
    memset(buf,0,SOCKS4_NETWORK_LEN);
#define SOCKS4_GRANTED          90
#define SOCKS4_REJECT           91
    buf[1] = (success ? SOCKS4_GRANTED : SOCKS4_REJECT);
    /* leave version, destport, destip zero */
    connection_write_to_buf(buf, SOCKS4_NETWORK_LEN, conn);
    return flush_buf(conn->s, conn->outbuf, &conn->outbuf_flushlen); /* try to flush it */
  }
  if(conn->socks_request->socks_version == 5) {
    buf[0] = 5; /* version 5 */
#define SOCKS5_SUCCESS          0
#define SOCKS5_GENERIC_ERROR    1
    buf[1] = success ? SOCKS5_SUCCESS : SOCKS5_GENERIC_ERROR;
    buf[2] = 0;
    buf[3] = 1; /* ipv4 addr */
    memset(buf+4,0,6); /* Set external addr/port to 0.
                          The spec doesn't seem to say what to do here. -RD */
    connection_write_to_buf(buf,10,conn);
    return flush_buf(conn->s, conn->outbuf, &conn->outbuf_flushlen); /* try to flush it */
  }
  return 0; /* if socks_version isn't 4 or 5, don't send anything */
}

static int connection_exit_begin_conn(cell_t *cell, circuit_t *circ) {
  connection_t *n_stream;
  relay_header_t rh;
  char *colon;

  relay_header_unpack(&rh, cell->payload);

  /* XXX currently we don't send an end cell back if we drop the
   * begin because it's malformed.
   */

  if(!memchr(cell->payload+RELAY_HEADER_SIZE, 0, rh.length)) {
    log_fn(LOG_WARN,"relay begin cell has no \\0. Dropping.");
    return 0;
  }
  colon = strchr(cell->payload+RELAY_HEADER_SIZE, ':');
  if(!colon) {
    log_fn(LOG_WARN,"relay begin cell has no colon. Dropping.");
    return 0;
  }
  *colon = 0;

  if(!atoi(colon+1)) { /* bad port */
    log_fn(LOG_WARN,"relay begin cell has invalid port. Dropping.");
    return 0;
  }

  log_fn(LOG_DEBUG,"Creating new exit connection.");
  n_stream = connection_new(CONN_TYPE_EXIT);

  n_stream->stream_id = rh.stream_id;
  n_stream->address = tor_strdup(cell->payload + RELAY_HEADER_SIZE);
  n_stream->port = atoi(colon+1);
  n_stream->state = EXIT_CONN_STATE_RESOLVING;
  /* leave n_stream->s at -1, because it's not yet valid */
  n_stream->package_window = STREAMWINDOW_START;
  n_stream->deliver_window = STREAMWINDOW_START;
  if(connection_add(n_stream) < 0) { /* no space, forget it */
    log_fn(LOG_WARN,"connection_add failed. Dropping.");
    connection_free(n_stream);
    return 0;
  }

  /* add it into the linked list of streams on this circuit */
  n_stream->next_stream = circ->n_streams;
  circ->n_streams = n_stream;

  /* send it off to the gethostbyname farm */
  switch(dns_resolve(n_stream)) {
    case 1: /* resolve worked */
      connection_exit_connect(n_stream);
      return 0;
    case -1: /* resolve failed */
      log_fn(LOG_INFO,"Resolve failed (%s).", n_stream->address);
      if(connection_edge_end(n_stream, END_STREAM_REASON_RESOLVEFAILED, NULL) < 0)
        log_fn(LOG_WARN,"1: I called connection_edge_end redundantly.");
    /* case 0, resolve added to pending list */
  }
  return 0;
}

void connection_exit_connect(connection_t *conn) {
  unsigned char connected_payload[4];

  if(router_compare_to_my_exit_policy(conn) < 0) {
    log_fn(LOG_INFO,"%s:%d failed exit policy. Closing.", conn->address, conn->port);
    if(connection_edge_end(conn, END_STREAM_REASON_EXITPOLICY, NULL) < 0)
      log_fn(LOG_WARN,"1: I called connection_edge_end redundantly.");
    return;
  }

  switch(connection_connect(conn, conn->address, conn->addr, conn->port)) {
    case -1:
      if(connection_edge_end(conn, END_STREAM_REASON_CONNECTFAILED, NULL) < 0)
        log_fn(LOG_WARN,"2: I called connection_edge_end redundantly.");
      return;
    case 0:
      connection_set_poll_socket(conn);
      conn->state = EXIT_CONN_STATE_CONNECTING;

      connection_watch_events(conn, POLLOUT | POLLIN | POLLERR);
      /* writable indicates finish, readable indicates broken link,
         error indicates broken link in windowsland. */
      return;
    /* case 1: fall through */
  }

  connection_set_poll_socket(conn);
  conn->state = EXIT_CONN_STATE_OPEN;
  if(connection_wants_to_flush(conn)) { /* in case there are any queued data cells */
    log_fn(LOG_WARN,"tell roger: newly connected conn had data waiting!");
//    connection_start_writing(conn);
  }
//   connection_process_inbuf(conn);
  connection_watch_events(conn, POLLIN);

  /* also, deliver a 'connected' cell back through the circuit. */
  *((uint32_t*) connected_payload) = htonl(conn->addr);
  connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_CONNECTED,
                               connected_payload, 4, NULL);
}

int connection_ap_can_use_exit(connection_t *conn, routerinfo_t *exit)
{
  uint32_t addr;

  assert(conn);
  assert(conn->type == CONN_TYPE_AP);
  assert(conn->socks_request);

  log_fn(LOG_DEBUG,"considering nickname %s, for address %s / port %d:",
         exit->nickname, conn->socks_request->address,
         conn->socks_request->port);
  addr = client_dns_lookup_entry(conn->socks_request->address);
  return router_supports_exit_address(addr, conn->socks_request->port, exit);
}

/* ***** Client DNS code ***** */

/* XXX Perhaps this should get merged with the dns.c code somehow. */
/* XXX But we can't just merge them, because then nodes that act as
 *     both OR and OP could be attacked: people could rig the dns cache
 *     by answering funny things to stream begin requests, and later
 *     other clients would reuse those funny addr's. Hm.
 */
struct client_dns_entry {
  SPLAY_ENTRY(client_dns_entry) node;
  char *address;
  uint32_t addr;
  time_t expires;
};
static int client_dns_size = 0;
static SPLAY_HEAD(client_dns_tree, client_dns_entry) client_dns_root;

static int compare_client_dns_entries(struct client_dns_entry *a,
                                      struct client_dns_entry *b)
{
  return strcasecmp(a->address, b->address);
}

static void client_dns_entry_free(struct client_dns_entry *ent)
{
  tor_free(ent->address);
  tor_free(ent);
}

SPLAY_PROTOTYPE(client_dns_tree, client_dns_entry, node, compare_client_dns_entries);
SPLAY_GENERATE(client_dns_tree, client_dns_entry, node, compare_client_dns_entries);

void client_dns_init(void) {
  SPLAY_INIT(&client_dns_root);
  client_dns_size = 0;
}

static uint32_t client_dns_lookup_entry(const char *address)
{
  struct client_dns_entry *ent;
  struct client_dns_entry search;
  struct in_addr in;
  time_t now;

  assert(address);

  if (inet_aton(address, &in)) {
    log_fn(LOG_DEBUG, "Using static address %s (%08lX)", address,
           (unsigned long)ntohl(in.s_addr));
    return ntohl(in.s_addr);
  }
  search.address = (char*)address;
  ent = SPLAY_FIND(client_dns_tree, &client_dns_root, &search);
  if (!ent) {
    log_fn(LOG_DEBUG, "No entry found for address %s", address);
    return 0;
  } else {
    now = time(NULL);
    if (ent->expires < now) {
      log_fn(LOG_DEBUG, "Expired entry found for address %s", address);
      SPLAY_REMOVE(client_dns_tree, &client_dns_root, ent);
      client_dns_entry_free(ent);
      --client_dns_size;
      return 0;
    }
    in.s_addr = htonl(ent->addr);
    log_fn(LOG_DEBUG, "Found cached entry for address %s: %s", address,
           inet_ntoa(in));
    return ent->addr;
  }
}
static void client_dns_set_entry(const char *address, uint32_t val)
{
  struct client_dns_entry *ent;
  struct client_dns_entry search;
  struct in_addr in;
  time_t now;

  assert(address);
  assert(val);

  if (inet_aton(address, &in))
    return;
  search.address = (char*) address;
  now = time(NULL);
  ent = SPLAY_FIND(client_dns_tree, &client_dns_root, &search);
  if (ent) {
    in.s_addr = htonl(val);
    log_fn(LOG_DEBUG, "Updating entry for address %s: %s", address,
           inet_ntoa(in));
    ent->addr = val;
    ent->expires = now+MAX_DNS_ENTRY_AGE;
  } else {
    in.s_addr = htonl(val);
    log_fn(LOG_DEBUG, "Caching result for address %s: %s", address,
           inet_ntoa(in));
    ent = tor_malloc(sizeof(struct client_dns_entry));
    ent->address = tor_strdup(address);
    ent->addr = val;
    ent->expires = now+MAX_DNS_ENTRY_AGE;
    SPLAY_INSERT(client_dns_tree, &client_dns_root, ent);
    ++client_dns_size;
  }
}

void client_dns_clean(void)
{
  struct client_dns_entry **expired_entries;
  int n_expired_entries = 0;
  struct client_dns_entry *ent;
  time_t now;
  int i;

  if(!client_dns_size)
    return;
  expired_entries = tor_malloc(client_dns_size *
                               sizeof(struct client_dns_entry *));

  now = time(NULL);
  SPLAY_FOREACH(ent, client_dns_tree, &client_dns_root) {
    if (ent->expires < now) {
      expired_entries[n_expired_entries++] = ent;
    }
  }
  for (i = 0; i < n_expired_entries; ++i) {
    SPLAY_REMOVE(client_dns_tree, &client_dns_root, expired_entries[i]);
    client_dns_entry_free(expired_entries[i]);
  }
  tor_free(expired_entries);
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
