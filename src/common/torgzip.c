/* Copyright 2004 Roger Dingledine */
/* See LICENSE for licensing information */
/* $Id$ */

/**
 * \file torgzip.c
 *
 * \brief Simple in-memory gzip implementation.
 **/

#include "orconfig.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <zlib.h>

#include "util.h"
#include "log.h"
#include "torgzip.h"

static int gzip_is_supported = -1;

int
is_gzip_supported(void)
{
  if (gzip_is_supported >= 0)
    return gzip_is_supported;

  if (!strcmpstart(ZLIB_VERSION, "0.") ||
      !strcmpstart(ZLIB_VERSION, "1.0") ||
      !strcmpstart(ZLIB_VERSION, "1.1"))
    gzip_is_supported = 0;
  else
    gzip_is_supported = 1;

  return gzip_is_supported;
}

static INLINE int
method_bits(compress_method_t method)
{
  /* Bits+16 means "use gzip" in zlib >= 1.2 */
  return method == GZIP_METHOD ? 15+16 : 15;
}

int
tor_gzip_compress(char **out, size_t *out_len,
                  const char *in, size_t in_len,
                  compress_method_t method)
{
  struct z_stream_s *stream = NULL;
  size_t out_size;
  off_t offset;

  tor_assert(out && out_len && in);

  if (method == GZIP_METHOD && !is_gzip_supported()) {
    /* Old zlib version don't support gzip in deflateInit2 */
    log_fn(LOG_WARN, "Gzip not supported with zlib %s", ZLIB_VERSION);
    return -1;
  }

  *out = NULL;

  stream = tor_malloc_zero(sizeof(struct z_stream_s));
  stream->zalloc = Z_NULL;
  stream->zfree = Z_NULL;
  stream->opaque = NULL;
  stream->next_in = (unsigned char*) in;
  stream->avail_in = in_len;

  if (deflateInit2(stream, Z_BEST_COMPRESSION, Z_DEFLATED,
                   method_bits(method),
                   8, Z_DEFAULT_STRATEGY) != Z_OK) {
    log_fn(LOG_WARN, "Error from deflateInit2: %s",
           stream->msg?stream->msg:"<no message>");
    goto err;
  }

  /* Guess 50% compression. */
  out_size = in_len / 2;
  if (out_size < 1024) out_size = 1024;
  *out = tor_malloc(out_size);
  stream->next_out = *out;
  stream->avail_out = out_size;

  while (1) {
    switch (deflate(stream, Z_FINISH))
      {
      case Z_STREAM_END:
        goto done;
      case Z_OK:
        /* In case zlib doesn't work as I think .... */
        if (stream->avail_out >= stream->avail_in+16)
          break;
      case Z_BUF_ERROR:
        offset = stream->next_out - ((unsigned char*)*out);
        out_size *= 2;
        *out = tor_realloc(*out, out_size);
        stream->next_out = *out + offset;
        stream->avail_out = out_size - offset;
        break;
      default:
        log_fn(LOG_WARN, "Gzip compression didn't finish: %s",
               stream->msg ? stream->msg : "<no message>");
        goto err;
      }
  }
 done:
  *out_len = stream->total_out;
  if (deflateEnd(stream)!=Z_OK) {
    log_fn(LOG_WARN, "Error freeing gzip structures");
    goto err;
  }
  tor_free(stream);

  return 0;
 err:
  if (stream) {
    deflateEnd(stream);
    tor_free(stream);
  }
  if (*out) {
    tor_free(*out);
  }
  return -1;
}

int
tor_gzip_uncompress(char **out, size_t *out_len,
                    const char *in, size_t in_len,
                    compress_method_t method)
{
  struct z_stream_s *stream = NULL;
  size_t out_size;
  off_t offset;

  tor_assert(out && out_len && in);

  if (method == GZIP_METHOD && !is_gzip_supported()) {
    /* Old zlib version don't support gzip in inflateInit2 */
    log_fn(LOG_WARN, "Gzip not supported with zlib %s", ZLIB_VERSION);
    return -1;
  }

  *out = NULL;

  stream = tor_malloc_zero(sizeof(struct z_stream_s));
  stream->zalloc = Z_NULL;
  stream->zfree = Z_NULL;
  stream->opaque = NULL;
  stream->next_in = (unsigned char*) in;
  stream->avail_in = in_len;

  if (inflateInit2(stream,
                   method_bits(method)) != Z_OK) {
    log_fn(LOG_WARN, "Error from inflateInit2: %s",
           stream->msg?stream->msg:"<no message>");
    goto err;
  }

  out_size = in_len * 2;  /* guess 50% compression. */
  if (out_size < 1024) out_size = 1024;

  *out = tor_malloc(out_size);
  stream->next_out = *out;
  stream->avail_out = out_size;

  while (1) {
    switch(inflate(stream, Z_FINISH))
      {
      case Z_STREAM_END:
        goto done;
      case Z_OK:
        /* In case zlib doesn't work as I think.... */
        if (stream->avail_out >= stream->avail_in+16)
          break;
      case Z_BUF_ERROR:
        offset = stream->next_out - ((unsigned char*)*out);
        out_size *= 2;
        *out = tor_realloc(*out, out_size);
        stream->next_out = *out + offset;
        stream->avail_out = out_size - offset;
        break;
      default:
        log_fn(LOG_WARN, "Gzip decompression returned an error: %s",
               stream->msg ? stream->msg : "<no message>");
        goto err;
      }

  }
 done:
  *out_len = stream->total_out;
  if (inflateEnd(stream)!=Z_OK) {
    log_fn(LOG_WARN, "Error freeing gzip structures");
    goto err;
  }
  tor_free(stream);

  /* NUL-terminate output. */
  if (out_size == *out_len)
    *out = tor_realloc(*out, out_size + 1);
  (*out)[*out_len] = '\0';

  return 0;
 err:
  if (stream) {
    inflateEnd(stream);
    tor_free(stream);
  }
  if (*out) {
    tor_free(*out);
  }
  return -1;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
