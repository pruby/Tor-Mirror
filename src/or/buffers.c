/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

/* buffers.c */

#include "or.h"

extern or_options_t options; /* command-line and config-file options */

int buf_new(char **buf, int *buflen, int *buf_datalen) {

  assert(buf && buflen && buf_datalen);

  *buf = (char *)malloc(MAX_BUF_SIZE);
  if(!*buf)
    return -1;
  memset(*buf,0,MAX_BUF_SIZE);
  *buflen = MAX_BUF_SIZE;
  *buf_datalen = 0;

  return 0;
}

void buf_free(char *buf) {
  free(buf);
}

int read_to_buf(int s, int at_most, char **buf, int *buflen, int *buf_datalen, int *reached_eof) {

  /* read from socket s, writing onto buf+buf_datalen. If at_most is >= 0 then
   * read at most 'at_most' bytes, and in any case don't read more than will fit based on buflen.
   * If read() returns 0, set *reached_eof to 1 and return 0. If you want to tear
   * down the connection return -1, else return the number of bytes read.
   */

  int read_result;

  assert(buf && *buf && buflen && buf_datalen && reached_eof && (s>=0));

  /* this is the point where you would grow the buffer, if you want to */

  if(at_most < 0 || *buflen - *buf_datalen < at_most)
    at_most = *buflen - *buf_datalen; /* take the min of the two */
    /* (note that this only modifies at_most inside this function) */

  if(at_most == 0)
    return 0; /* we shouldn't read anything */

  if(!options.LinkPadding && at_most > 10*sizeof(cell_t)) {
    /* if no linkpadding: do a rudimentary round-robin so one
     * connection can't hog an outgoing connection
     */
    at_most = 10*sizeof(cell_t);
  }

//  log(LOG_DEBUG,"read_to_buf(): reading at most %d bytes.",at_most);
  read_result = read(s, *buf+*buf_datalen, at_most);
  if (read_result < 0) {
    if(errno!=EAGAIN) { /* it's a real error */
      return -1;
    }
    return 0;
  } else if (read_result == 0) {
    log(LOG_DEBUG,"read_to_buf(): Encountered eof");
    *reached_eof = 1;
    return 0;
  } else { /* we read some bytes */
    *buf_datalen += read_result;
//    log(LOG_DEBUG,"read_to_buf(): Read %d bytes. %d on inbuf.",read_result, *buf_datalen);
    return read_result;
  }

}

int flush_buf(int s, char **buf, int *buflen, int *buf_flushlen, int *buf_datalen) {

  /* push from buf onto s
   * then memmove to front of buf
   * return -1 or how many bytes remain to be flushed */

  int write_result;

  assert(buf && *buf && buflen && buf_flushlen && buf_datalen && (s>=0) && (*buf_flushlen <= *buf_datalen));

  if(*buf_flushlen == 0) /* nothing to flush */
    return 0;

  /* this is the point where you would grow the buffer, if you want to */

  write_result = write(s, *buf, *buf_flushlen);
  if (write_result < 0) {
    if(errno!=EAGAIN) { /* it's a real error */
      return -1;
    }
    log(LOG_DEBUG,"flush_buf(): write() would block, returning.");
    return 0;
  } else {
    *buf_datalen -= write_result;
    *buf_flushlen -= write_result;
    memmove(*buf, *buf+write_result, *buf_datalen);
//    log(LOG_DEBUG,"flush_buf(): flushed %d bytes, %d ready to flush, %d remain.",
//       write_result,*buf_flushlen,*buf_datalen);
    return *buf_flushlen;
  }
}

int write_to_buf(char *string, int string_len,
                 char **buf, int *buflen, int *buf_datalen) {

  /* append string to buf (growing as needed, return -1 if "too big")
   * return total number of bytes on the buf
   */

  assert(string && buf && *buf && buflen && buf_datalen);

  /* this is the point where you would grow the buffer, if you want to */

  if (string_len + *buf_datalen > *buflen) { /* we're out of luck */
    log(LOG_DEBUG, "write_to_buf(): buflen too small. Time to implement growing dynamic bufs.");
    return -1;
  }

  memcpy(*buf+*buf_datalen, string, string_len);
  *buf_datalen += string_len;
//  log(LOG_DEBUG,"write_to_buf(): added %d bytes to buf (now %d total).",string_len, *buf_datalen);
  return *buf_datalen;

}

int fetch_from_buf(char *string, int string_len,
                   char **buf, int *buflen, int *buf_datalen) {

  /* if there are string_len bytes in buf, write them onto string,
   * then memmove buf back (that is, remove them from buf) */

  assert(string && buf && *buf && buflen && buf_datalen);

  /* this is the point where you would grow the buffer, if you want to */

  if(string_len > *buf_datalen) /* we want too much. sorry. */
    return -1;
 
  memcpy(string,*buf,string_len);
  *buf_datalen -= string_len;
  memmove(*buf, *buf+string_len, *buf_datalen);
  return *buf_datalen;
}

int find_on_inbuf(char *string, int string_len,
                  char *buf, int buf_datalen) {
  /* find first instance of needle 'string' on haystack 'buf'. return how
   * many bytes from the beginning of buf to the end of string.
   * If it's not there, return -1.
   */

  char *location;
  char *last_possible = buf + buf_datalen - string_len;

  assert(string && string_len > 0 && buf);

  if(buf_datalen < string_len)
    return -1;

  for(location = buf; location <= last_possible; location++)
    if((*location == *string) && !memcmp(location+1, string+1, string_len-1))
      return location-buf+string_len;

  return -1;
}

