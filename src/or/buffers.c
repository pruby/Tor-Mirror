/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2007, Roger Dingledine, Nick Mathewson. */
/* See LICENSE for licensing information */
/* $Id$ */
const char buffers_c_id[] =
  "$Id$";

/**
 * \file buffers.c
 * \brief Implements a generic buffer interface.  Buffers are
 * fairly opaque string holders that can read to or flush from:
 * memory, file descriptors, or TLS connections.
 **/

#include "or.h"

#undef SENTINELS
#undef CHECK_AFTER_RESIZE
#undef PARANOIA
#undef NOINLINE

/* If SENTINELS is defined, check for attempts to write beyond the
 * end/before the start of the buffer.
 */
#ifdef SENTINELS
/** 4-byte value to write at the start of each buffer memory region. */
#define START_MAGIC 0x70370370u
/** 4-byte value to write at the end of each buffer memory region. */
#define END_MAGIC 0xA0B0C0D0u
/** Given buf-&gt;mem, yield a pointer to the raw memory region (for free(),
 * realloc(), and so on). */
#define RAW_MEM(m) ((void*)(((char*)m)-4))
/** Given a pointer to the raw memory region (from malloc() or realloc()),
 * yield the correct value for buf-&gt;mem (just past the first sentinel). */
#define GUARDED_MEM(m) ((void*)(((char*)m)+4))
/** How much memory do we need to allocate for a buffer to hold <b>ln</b> bytes
 * of data? */
#define ALLOC_LEN(ln) ((ln)+8)
/** Initialize the sentinel values on <b>m</b> (a value of buf-&gt;mem), which
 * has <b>ln</b> useful bytes. */
#define SET_GUARDS(m, ln) \
  STMT_BEGIN                         \
    set_uint32((m)-4,START_MAGIC);   \
    set_uint32((m)+ln,END_MAGIC);    \
  STMT_END
#else
#define RAW_MEM(m) (m)
#define GUARDED_MEM(m) (m)
#define ALLOC_LEN(ln) (ln)
#define SET_GUARDS(m,ln) STMT_NIL
#endif

#ifdef PARANOIA
#define check() STMT_BEGIN assert_buf_ok(buf); STMT_END
#else
#define check() STMT_NIL
#endif

#ifdef NOINLINE
#undef INLINE
#define INLINE
#endif

/** Magic value for buf_t.magic, to catch pointer errors. */
#define BUFFER_MAGIC 0xB0FFF312u
/** A resizeable buffer, optimized for reading and writing. */
struct buf_t {
  uint32_t magic; /**< Magic cookie for debugging: Must be set to
                   *   BUFFER_MAGIC. */
  char *mem;      /**< Storage for data in the buffer. */
  char *cur;      /**< The first byte used for storing data in the buffer. */
  size_t highwater; /**< Largest observed datalen since last buf_shrink. */
  size_t len;     /**< Maximum amount of data that <b>mem</b> can hold. */
  size_t memsize; /**< How many bytes did we actually allocate? Can be less
                   * than 'len' if we shortened 'len' by a few bytes to make
                   * zlib wrap around more easily. */
  size_t datalen; /**< Number of bytes currently in <b>mem</b>. */
};

/** Size, in bytes, for newly allocated buffers.  Should be a power of 2. */
#define INITIAL_BUF_SIZE (4*1024)
/** Size, in bytes, for minimum 'shrink' size for buffers.  Buffers may start
 * out smaller than this, but they will never autoshrink to less
 * than this size. */
#define MIN_LAZY_SHRINK_SIZE (4*1024)

static INLINE void peek_from_buf(char *string, size_t string_len, buf_t *buf);

/** If the contents of buf wrap around the end of the allocated space,
 * malloc a new buf and copy the contents in starting at the
 * beginning. This operation is relatively expensive, so it shouldn't
 * be used e.g. for every single read or write.
 */
static void
buf_normalize(buf_t *buf)
{
  check();
  if (buf->cur + buf->datalen <= buf->mem+buf->len) {
    return;
  } else {
    char *newmem, *oldmem;
    size_t sz = (buf->mem+buf->len)-buf->cur;
    log_warn(LD_BUG, "Unexpected non-normalized buffer.");
    newmem = GUARDED_MEM(tor_malloc(ALLOC_LEN(buf->memsize)));
    SET_GUARDS(newmem, buf->memsize);
    memcpy(newmem, buf->cur, sz);
    memcpy(newmem+sz, buf->mem, buf->datalen-sz);
    oldmem = RAW_MEM(buf->mem);
    tor_free(oldmem); /* Can't use tor_free directly. */
    buf->mem = buf->cur = newmem;
    buf->len = buf->memsize;
    check();
  }
}

/** Return the point in the buffer where the next byte will get stored. */
static INLINE char *
_buf_end(buf_t *buf)
{
  char *next = buf->cur + buf->datalen;
  char *end = buf->mem + buf->len;
  return (next < end) ? next : (next - buf->len);
}

/** If the pointer <b>cp</b> has passed beyond the end of the buffer, wrap it
 * around. */
static INLINE char *
_wrap_ptr(buf_t *buf, char *cp)
{
  return (cp >= buf->mem + buf->len) ? (cp - buf->len) : cp;
}

/** Return the offset of <b>cp</b> within the buffer. */
static INLINE int
_buf_offset(buf_t *buf, char *cp)
{
  if (cp >= buf->cur)
    return cp - buf->cur;
  else
    /* return (cp - buf->mem) + buf->mem+buf->len - buf->cur */
    return cp + buf->len - buf->cur;
}

/** If the range of *<b>len</b> bytes starting at <b>at</b> wraps around the
 * end of the buffer, then set *<b>len</b> to the number of bytes starting
 * at <b>at</b>, and set *<b>more_len</b> to the number of bytes starting
 * at <b>buf-&gt;mem</b>.  Otherwise, set *<b>more_len</b> to 0.
 */
static INLINE void
_split_range(buf_t *buf, char *at, size_t *len,
                                size_t *more_len)
{
  char *eos = at + *len;
  check();
  if (eos >= (buf->mem + buf->len)) {
    *more_len = eos - (buf->mem + buf->len);
    *len -= *more_len;
  } else {
    *more_len = 0;
  }
}

/** A freelist of buffer RAM chunks. */
typedef struct free_mem_list_t {
  char *list; /**< The first item on the list; begins with pointer to the
               * next item. */
  int len; /**< How many entries in <b>list</b>. */
  int lowwater; /**< The smallest that list has gotten since the last call to
                 * buf_shrink_freelists(). */
  const size_t chunksize; /**< How big are the items on the list? */
  const int slack; /**< We always keep at least this many items on the list
                    * when shrinking it. */
  const int max; /**< How many elements are we willing to throw onto the list?
                  */
} free_mem_list_t;

/** Freelists to hold 4k and 16k memory chunks.  This seems to be what
 * we use most. */
static free_mem_list_t free_mem_list_4k = { NULL, 0, 0, 4096, 16, INT_MAX };
static free_mem_list_t free_mem_list_8k = { NULL, 0, 0, 8192 , 8, 128 };
static free_mem_list_t free_mem_list_16k = { NULL, 0, 0, 16384, 4, 64 };

/** Macro: True iff the size is one for which we keep a freelist. */
#define IS_FREELIST_SIZE(sz) ((sz) == 4096 || (sz) == 8192 || (sz) == 16384)

/** Return the proper freelist for chunks of size <b>sz</b>, or fail
 * with an assertion. */
static INLINE free_mem_list_t *
get_free_mem_list(size_t sz)
{
  if (sz == 4096) {
    return &free_mem_list_4k;
  } else if (sz == 8192) {
    return &free_mem_list_8k;
  } else {
    tor_assert(sz == 16384);
    return &free_mem_list_16k;
  }
}

/** Write the sizes of the buffer freelists at log level <b>severity</b> */
void
buf_dump_freelist_sizes(int severity)
{
  size_t sz;
  log(severity, LD_MM, "======= Buffer freelists.");
  for (sz = 4096; sz <= 16384; sz *= 2) {
    uint64_t total_size;
    free_mem_list_t *lst;
    if (!IS_FREELIST_SIZE(sz))
      continue;
    lst = get_free_mem_list(sz);
    total_size = ((uint64_t)sz)*lst->len;
    log(severity, LD_MM,
        U64_FORMAT" bytes in %d %d-byte buffers. (low-water: %d)",
        U64_PRINTF_ARG(total_size), lst->len, (int)sz, lst->lowwater);
  }
}

/** Throw the memory from <b>buf</b> onto the appropriate freelist.
 * Return true if we added the memory, 0 if the freelist was full. */
static int
add_buf_mem_to_freelist(buf_t *buf)
{
  char *mem;
  free_mem_list_t *list;

  tor_assert(buf->datalen == 0);
  tor_assert(buf->mem);
  list = get_free_mem_list(buf->len);

  if (list->len >= list->max)
    return 0;

  mem = RAW_MEM(buf->mem);
  buf->len = buf->memsize = 0;
  buf->mem = buf->cur = NULL;

  *(char**)mem = list->list;
  list->list = mem;
  ++list->len;
  log_debug(LD_GENERAL, "Add buf mem to %d-byte freelist.  Freelist has "
            "%d entries.", (int)list->chunksize, list->len);

  return 1;
}

/** Pull memory of size <b>sz</b> from the appropriate freelist for use by
 * <b>buf</b>, or allocate it as needed. */
static void
buf_get_initial_mem(buf_t *buf, size_t sz)
{
  char *mem;
  free_mem_list_t *list = get_free_mem_list(sz);
  tor_assert(!buf->mem);

  if (list->list) {
    mem = list->list;
    list->list = *(char**)mem;
    if (--list->len < list->lowwater)
      list->lowwater = list->len;
    log_debug(LD_GENERAL, "Got buf mem from %d-byte freelist. Freelist has "
             "%d entries.", (int)list->chunksize, list->len);
  } else {
    log_debug(LD_GENERAL, "%d-byte freelist empty; allocating another chunk.",
             (int)list->chunksize);
    tor_assert(list->len == 0);
    mem = tor_malloc(ALLOC_LEN(sz));
  }
  buf->mem = GUARDED_MEM(mem);
  SET_GUARDS(buf->mem, sz);
  buf->len = sz;
  buf->memsize = ALLOC_LEN(sz);
  buf->cur = buf->mem;
}

/** Remove elements from the freelists that haven't been needed since the
 * last call to this function. If <b>free_all</b>, we're exiting and we
 * should clear the whole lists. */
void
buf_shrink_freelists(int free_all)
{
  int list_elt_size;
  for (list_elt_size = 4096; list_elt_size <= 16384; list_elt_size *= 2) {
    free_mem_list_t *list = get_free_mem_list(list_elt_size);
    if (list->lowwater > list->slack || free_all) {
      int i, n_to_skip, n_to_free;
      char **ptr;
      if (free_all) { /* Free every one of them */
        log_info(LD_GENERAL, "Freeing all %d elements from %d-byte freelist.",
                 list->len, (int)list->chunksize);
        n_to_free = list->len;
      } else { /* Skip over the slack and non-lowwater entries */
        log_info(LD_GENERAL, "We haven't used %d/%d allocated %d-byte buffer "
               "memory chunks since the last call; freeing all but %d of them",
               list->lowwater, list->len, (int)list->chunksize, list->slack);
        n_to_free = list->lowwater - list->slack;
      }
      n_to_skip = list->len - n_to_free;
      for (ptr = &list->list, i = 0; i < n_to_skip; ++i) {
        char *mem = *ptr;
        tor_assert(mem);
        ptr = (char**)mem;
      }
      /* And free the remaining entries. */
      for (i = 0; i < n_to_free; ++i) {
        char *mem = *ptr;
        tor_assert(mem);
        *ptr = *(char**)mem;
        tor_free(mem);
        --list->len;
      }
    }
    list->lowwater = list->len;
  }
}

/** Change a buffer's capacity. <b>new_capacity</b> must be \>=
 * buf->datalen. */
static void
buf_resize(buf_t *buf, size_t new_capacity)
{
  off_t offset;
#ifdef CHECK_AFTER_RESIZE
  char *tmp, *tmp2;
#endif
  tor_assert(buf->datalen <= new_capacity);
  tor_assert(new_capacity);

#ifdef CHECK_AFTER_RESIZE
  assert_buf_ok(buf);
  tmp = tor_malloc(buf->datalen);
  tmp2 = tor_malloc(buf->datalen);
  peek_from_buf(tmp, buf->datalen, buf);
#endif

  if (buf->len == new_capacity)
    return;

  offset = buf->cur - buf->mem;
  if (offset + buf->datalen > new_capacity) {
    /* We need to move stuff before we shrink. */
    if (offset + buf->datalen > buf->len) {
      /* We have:
       *
       * mem[0] ... mem[datalen-(len-offset)] (end of data)
       * mem[offset] ... mem[len-1]           (the start of the data)
       *
       * We're shrinking the buffer by (len-new_capacity) bytes, so we need
       * to move the start portion back by that many bytes.
       */
      memmove(buf->cur-(buf->len-new_capacity), buf->cur,
              (size_t)(buf->len-offset));
      offset -= (buf->len-new_capacity);
    } else {
      /* The data doesn't wrap around, but it does extend beyond the new
       * buffer length:
       *   mem[offset] ... mem[offset+datalen-1] (the data)
       */
      memmove(buf->mem, buf->cur, buf->datalen);
      offset = 0;
    }
  }

  if (buf->len == 0 && new_capacity < MIN_LAZY_SHRINK_SIZE)
    new_capacity = MIN_LAZY_SHRINK_SIZE;

  if (buf->len == 0 && IS_FREELIST_SIZE(new_capacity)) {
    tor_assert(!buf->mem);
    buf_get_initial_mem(buf, new_capacity);
  } else {
    char *raw;
    if (buf->mem)
      raw = tor_realloc(RAW_MEM(buf->mem), ALLOC_LEN(new_capacity));
    else {
      log_info(LD_GENERAL, "Jumping straight from 0 bytes to %d",
               (int)new_capacity);
      raw = tor_malloc(ALLOC_LEN(new_capacity));
    }
    buf->mem = GUARDED_MEM(raw);
    SET_GUARDS(buf->mem, new_capacity);
    buf->cur = buf->mem+offset;
  }

  if (offset + buf->datalen > buf->len) {
    /* We need to move data now that we are done growing.  The buffer
     * now contains:
     *
     * mem[0] ... mem[datalen-(len-offset)] (end of data)
     * mem[offset] ... mem[len-1]           (the start of the data)
     * mem[len]...mem[new_capacity]         (empty space)
     *
     * We're growing by (new_capacity-len) bytes, so we need to move the
     * end portion forward by that many bytes.
     */
    memmove(buf->cur+(new_capacity-buf->len), buf->cur,
            (size_t)(buf->len-offset));
    buf->cur += new_capacity-buf->len;
  }
  buf->len = new_capacity;
  buf->memsize = ALLOC_LEN(buf->len);

#ifdef CHECK_AFTER_RESIZE
  assert_buf_ok(buf);
  peek_from_buf(tmp2, buf->datalen, buf);
  if (memcmp(tmp, tmp2, buf->datalen)) {
    tor_assert(0);
  }
  tor_free(tmp);
  tor_free(tmp2);
#endif
}

/** If the buffer is not large enough to hold <b>capacity</b> bytes, resize
 * it so that it can.  (The new size will be a power of 2 times the old
 * size.)
 */
static INLINE int
buf_ensure_capacity(buf_t *buf, size_t capacity)
{
  size_t new_len, min_len;
  if (buf->len >= capacity)  /* Don't grow if we're already big enough. */
    return 0;
  if (capacity > MAX_BUF_SIZE) /* Don't grow past the maximum. */
    return -1;
  /* Find the smallest new_len equal to (2**X) for some X; such that
   * new_len is at least capacity, and at least 2*buf->len.
   */
  min_len = buf->len*2;
  new_len = 16;
  while (new_len < min_len)
    new_len *= 2;
  while (new_len < capacity)
    new_len *= 2;
  /* Resize the buffer. */
  log_debug(LD_MM,"Growing buffer from %d to %d bytes.",
            (int)buf->len, (int)new_len);
  buf_resize(buf,new_len);
  return 0;
}

/** Resize buf so it won't hold extra memory that we haven't been
 * using lately (that is, since the last time we called buf_shrink).
 * Try to shrink the buf until it is the largest factor of two that
 * can contain <b>buf</b>-&gt;highwater, but never smaller than
 * MIN_LAZY_SHRINK_SIZE.
 */
void
buf_shrink(buf_t *buf)
{
  size_t new_len;

  new_len = buf->len;
  /* Actually, we ignore highwater here if we're going to throw it on the
   * freelist, since it's way cheaper to use the freelist than to use (some)
   * platform mallocs.
   *
   * DOCDOC If it turns out to be a good idea, add it to the doxygen for this
   * function.
   */
  if (buf->datalen == 0 && // buf->highwater == 0 &&
      IS_FREELIST_SIZE(buf->len)) {
    buf->highwater = 0;
    if (add_buf_mem_to_freelist(buf))
      return;
  }
  while (buf->highwater < (new_len>>2) && new_len > MIN_LAZY_SHRINK_SIZE*2)
    new_len >>= 1;

  buf->highwater = buf->datalen;
  if (new_len == buf->len)
    return;

  log_debug(LD_MM,"Shrinking buffer from %d to %d bytes.",
            (int)buf->len, (int)new_len);
  buf_resize(buf, new_len);
}

/** Remove the first <b>n</b> bytes from buf. */
static INLINE void
buf_remove_from_front(buf_t *buf, size_t n)
{
  tor_assert(buf->datalen >= n);
  buf->datalen -= n;
  if (buf->datalen) {
    buf->cur = _wrap_ptr(buf, buf->cur+n);
  } else {
    buf->cur = buf->mem;
    if (IS_FREELIST_SIZE(buf->len)) {
      buf->highwater = 0;

      if (add_buf_mem_to_freelist(buf))
        return;
    }
  }
  check();
}

/** Make sure that the memory in buf ends with a zero byte. */
static INLINE int
buf_nul_terminate(buf_t *buf)
{
  if (buf_ensure_capacity(buf,buf->datalen+1)<0)
    return -1;
  *_buf_end(buf) = '\0';
  return 0;
}

/** Create and return a new buf with capacity <b>size</b>.
 * (Used for testing). */
buf_t *
buf_new_with_capacity(size_t size)
{
  buf_t *buf;
  buf = tor_malloc_zero(sizeof(buf_t));
  buf->magic = BUFFER_MAGIC;
  if (IS_FREELIST_SIZE(size)) {
    buf_get_initial_mem(buf, size);
  } else {
    buf->cur = buf->mem = GUARDED_MEM(tor_malloc(ALLOC_LEN(size)));
    SET_GUARDS(buf->mem, size);
    buf->len = size;
    buf->memsize = ALLOC_LEN(size);
  }

  assert_buf_ok(buf);
  return buf;
}

/** Allocate and return a new buffer with default capacity. */
buf_t *
buf_new(void)
{
  return buf_new_with_capacity(INITIAL_BUF_SIZE);
}

/** Remove all data from <b>buf</b>. */
void
buf_clear(buf_t *buf)
{
  buf->datalen = 0;
  buf->cur = buf->mem;
  /* buf->len = buf->memsize; bad. */
}

/** Return the number of bytes stored in <b>buf</b> */
size_t
buf_datalen(const buf_t *buf)
{
  return buf->datalen;
}

/** Return the maximum bytes that can be stored in <b>buf</b> before buf
 * needs to resize. */
size_t
buf_capacity(const buf_t *buf)
{
  return buf->len;
}

/** For testing only: Return a pointer to the raw memory stored in
 * <b>buf</b>. */
const char *
_buf_peek_raw_buffer(const buf_t *buf)
{
  return buf->cur;
}

/** Release storage held by <b>buf</b>. */
void
buf_free(buf_t *buf)
{
  char *oldmem;
  assert_buf_ok(buf);
  buf->magic = 0xDEADBEEF;
  if (IS_FREELIST_SIZE(buf->len)) {
    buf->datalen = 0; /* Avoid assert in add_buf_mem_to_freelist. */
    add_buf_mem_to_freelist(buf);
  }
  if (buf->mem) {
    /* The freelist didn't want the RAM. */
    oldmem = RAW_MEM(buf->mem);
    tor_free(oldmem);
  }
  tor_free(buf);
}

/** Helper for read_to_buf(): read no more than at_most bytes from
 * socket s into buffer buf, starting at the position pos.  (Does not
 * check for overflow.)  Set *reached_eof to true on EOF.  Return
 * number of bytes read on success, 0 if the read would block, -1 on
 * failure.
 */
static INLINE int
read_to_buf_impl(int s, size_t at_most, buf_t *buf,
                 char *pos, int *reached_eof)
{
  int read_result;

//  log_fn(LOG_DEBUG,"reading at most %d bytes.",at_most);
  read_result = tor_socket_recv(s, pos, at_most, 0);
  if (read_result < 0) {
    int e = tor_socket_errno(s);
    if (!ERRNO_IS_EAGAIN(e)) { /* it's a real error */
#ifdef MS_WINDOWS
      if (e == WSAENOBUFS)
        log_warn(LD_NET,"recv() failed: WSAENOBUFS. Not enough ram?");
#endif
      return -1;
    }
    return 0; /* would block. */
  } else if (read_result == 0) {
    log_debug(LD_NET,"Encountered eof");
    *reached_eof = 1;
    return 0;
  } else { /* we read some bytes */
    buf->datalen += read_result;
    if (buf->datalen > buf->highwater)
      buf->highwater = buf->datalen;
    log_debug(LD_NET,"Read %d bytes. %d on inbuf.",read_result,
              (int)buf->datalen);
    return read_result;
  }
}

/** Read from socket <b>s</b>, writing onto end of <b>buf</b>.  Read at most
 * <b>at_most</b> bytes, resizing the buffer as necessary.  If recv()
 * returns 0, set *<b>reached_eof</b> to 1 and return 0. Return -1 on error;
 * else return the number of bytes read.  Return 0 if recv() would
 * block.
 */
int
read_to_buf(int s, size_t at_most, buf_t *buf, int *reached_eof)
{
  int r;
  char *next;
  size_t at_start;

  /* assert_buf_ok(buf); */
  tor_assert(reached_eof);
  tor_assert(s>=0);

  if (buf_ensure_capacity(buf,buf->datalen+at_most))
    return -1;

  if (at_most + buf->datalen > buf->len)
    at_most = buf->len - buf->datalen; /* take the min of the two */

  if (at_most == 0)
    return 0; /* we shouldn't read anything */

  next = _buf_end(buf);
  _split_range(buf, next, &at_most, &at_start);

  r = read_to_buf_impl(s, at_most, buf, next, reached_eof);
  check();
  if (r < 0 || (size_t)r < at_most) {
    return r; /* Either error, eof, block, or no more to read. */
  }

  if (at_start) {
    int r2;
    tor_assert(_buf_end(buf) == buf->mem);
    r2 = read_to_buf_impl(s, at_start, buf, buf->mem, reached_eof);
    check();
    if (r2 < 0) {
      return r2;
    } else {
      r += r2;
    }
  }
  return r;
}

/** Helper for read_to_buf_tls(): read no more than <b>at_most</b>
 * bytes from the TLS connection <b>tls</b> into buffer <b>buf</b>,
 * starting at the position <b>next</b>.  (Does not check for overflow.)
 * Return number of bytes read on success, 0 if the read would block,
 * -1 on failure.
 */
static INLINE int
read_to_buf_tls_impl(tor_tls_t *tls, size_t at_most, buf_t *buf, char *next)
{
  int r;

  log_debug(LD_NET,"before: %d on buf, %d pending, at_most %d.",
            (int)buf_datalen(buf), (int)tor_tls_get_pending_bytes(tls),
            (int)at_most);
  r = tor_tls_read(tls, next, at_most);
  if (r<0)
    return r;
  buf->datalen += r;
  if (buf->datalen > buf->highwater)
    buf->highwater = buf->datalen;
  log_debug(LD_NET,"Read %d bytes. %d on inbuf; %d pending",r,
            (int)buf->datalen,(int)tor_tls_get_pending_bytes(tls));
  return r;
}

/** As read_to_buf, but reads from a TLS connection.
 *
 * Using TLS on OR connections complicates matters in two ways.
 *
 * First, a TLS stream has its own read buffer independent of the
 * connection's read buffer.  (TLS needs to read an entire frame from
 * the network before it can decrypt any data.  Thus, trying to read 1
 * byte from TLS can require that several KB be read from the network
 * and decrypted.  The extra data is stored in TLS's decrypt buffer.)
 * Because the data hasn't been read by Tor (it's still inside the TLS),
 * this means that sometimes a connection "has stuff to read" even when
 * poll() didn't return POLLIN. The tor_tls_get_pending_bytes function is
 * used in connection.c to detect TLS objects with non-empty internal
 * buffers and read from them again.
 *
 * Second, the TLS stream's events do not correspond directly to network
 * events: sometimes, before a TLS stream can read, the network must be
 * ready to write -- or vice versa.
 */
int
read_to_buf_tls(tor_tls_t *tls, size_t at_most, buf_t *buf)
{
  int r;
  char *next;
  size_t at_start;

  tor_assert(tls);
  assert_buf_ok(buf);

  log_debug(LD_NET,"start: %d on buf, %d pending, at_most %d.",
            (int)buf_datalen(buf), (int)tor_tls_get_pending_bytes(tls),
            (int)at_most);

  if (buf_ensure_capacity(buf, at_most+buf->datalen))
    return TOR_TLS_ERROR_MISC;

  if (at_most + buf->datalen > buf->len)
    at_most = buf->len - buf->datalen;

  if (at_most == 0)
    return 0;

  next = _buf_end(buf);
  _split_range(buf, next, &at_most, &at_start);

  r = read_to_buf_tls_impl(tls, at_most, buf, next);
  check();
  if (r < 0 || (size_t)r < at_most)
    return r; /* Either error, eof, block, or no more to read. */

  if (at_start) {
    int r2;
    tor_assert(_buf_end(buf) == buf->mem);
    r2 = read_to_buf_tls_impl(tls, at_start, buf, buf->mem);
    check();
    if (r2 < 0)
      return r2;
    else
      r += r2;
  }
  return r;
}

/** Helper for flush_buf(): try to write <b>sz</b> bytes from buffer
 * <b>buf</b> onto socket <b>s</b>.  On success, deduct the bytes written
 * from *<b>buf_flushlen</b>.
 * Return the number of bytes written on success, -1 on failure.
 */
static INLINE int
flush_buf_impl(int s, buf_t *buf, size_t sz, size_t *buf_flushlen)
{
  int write_result;

  write_result = tor_socket_send(s, buf->cur, sz, 0);
  if (write_result < 0) {
    int e = tor_socket_errno(s);
    if (!ERRNO_IS_EAGAIN(e)) { /* it's a real error */
#ifdef MS_WINDOWS
      if (e == WSAENOBUFS)
        log_warn(LD_NET,"write() failed: WSAENOBUFS. Not enough ram?");
#endif
      return -1;
    }
    log_debug(LD_NET,"write() would block, returning.");
    return 0;
  } else {
    *buf_flushlen -= write_result;
    buf_remove_from_front(buf, write_result);
    return write_result;
  }
}

/** Write data from <b>buf</b> to the socket <b>s</b>.  Write at most
 * <b>sz</b> bytes, decrement *<b>buf_flushlen</b> by
 * the number of bytes actually written, and remove the written bytes
 * from the buffer.  Return the number of bytes written on success,
 * -1 on failure.  Return 0 if write() would block.
 */
int
flush_buf(int s, buf_t *buf, size_t sz, size_t *buf_flushlen)
{
  int r;
  size_t flushed = 0;
  size_t flushlen0, flushlen1;

  /* assert_buf_ok(buf); */
  tor_assert(buf_flushlen);
  tor_assert(s>=0);
  tor_assert(*buf_flushlen <= buf->datalen);
  tor_assert(sz <= *buf_flushlen);

  if (sz == 0) /* nothing to flush */
    return 0;

  flushlen0 = sz;
  _split_range(buf, buf->cur, &flushlen0, &flushlen1);

  r = flush_buf_impl(s, buf, flushlen0, buf_flushlen);
  check();

  log_debug(LD_NET,"%d: flushed %d bytes, %d ready to flush, %d remain.",
            s,r,(int)*buf_flushlen,(int)buf->datalen);
  if (r < 0 || (size_t)r < flushlen0)
    return r; /* Error, or can't flush any more now. */
  flushed = r;

  if (flushlen1) {
    tor_assert(buf->cur == buf->mem);
    r = flush_buf_impl(s, buf, flushlen1, buf_flushlen);
    check();
    log_debug(LD_NET,"%d: flushed %d bytes, %d ready to flush, %d remain.",
              s,r,(int)*buf_flushlen,(int)buf->datalen);
    if (r<0)
      return r;
    flushed += r;
  }
  return flushed;
}

/** Helper for flush_buf_tls(): try to write <b>sz</b> bytes (or more if
 * required by a previous write) from buffer <b>buf</b> onto TLS object
 * <b>tls</b>.  On success, deduct the bytes written from
 * *<b>buf_flushlen</b>.  Return the number of bytes written on success, -1 on
 * failure.
 */
static INLINE int
flush_buf_tls_impl(tor_tls_t *tls, buf_t *buf, size_t sz, size_t *buf_flushlen)
{
  int r;
  size_t forced;

  forced = tor_tls_get_forced_write_size(tls);
  if (forced > sz)
    sz = forced;
  r = tor_tls_write(tls, buf->cur, sz);
  if (r < 0) {
    return r;
  }
  *buf_flushlen -= r;
  buf_remove_from_front(buf, r);
  log_debug(LD_NET,"flushed %d bytes, %d ready to flush, %d remain.",
            r,(int)*buf_flushlen,(int)buf->datalen);
  return r;
}

/** As flush_buf(), but writes data to a TLS connection.
 */
int
flush_buf_tls(tor_tls_t *tls, buf_t *buf, size_t sz, size_t *buf_flushlen)
{
  int r;
  size_t flushed=0;
  size_t flushlen0, flushlen1;
  /* assert_buf_ok(buf); */
  tor_assert(tls);
  tor_assert(buf_flushlen);
  tor_assert(*buf_flushlen <= buf->datalen);
  tor_assert(sz <= *buf_flushlen);

  /* we want to let tls write even if flushlen is zero, because it might
   * have a partial record pending */
  check_no_tls_errors();

  flushlen0 = sz;
  _split_range(buf, buf->cur, &flushlen0, &flushlen1);
  if (flushlen1) {
    size_t forced = tor_tls_get_forced_write_size(tls);
    tor_assert(forced <= flushlen0);
  }

  r = flush_buf_tls_impl(tls, buf, flushlen0, buf_flushlen);
  check();
  if (r < 0 || (size_t)r < flushlen0)
    return r; /* Error, or can't flush any more now. */
  flushed = r;

  if (flushlen1) {
    tor_assert(buf->cur == buf->mem);
    r = flush_buf_tls_impl(tls, buf, flushlen1, buf_flushlen);
    check();
    if (r<0)
      return r;
    flushed += r;
  }
  return flushed;
}

/** Append <b>string_len</b> bytes from <b>string</b> to the end of
 * <b>buf</b>.
 *
 * Return the new length of the buffer on success, -1 on failure.
 */
int
write_to_buf(const char *string, size_t string_len, buf_t *buf)
{
  char *next;
  size_t len2;

  /* append string to buf (growing as needed, return -1 if "too big")
   * return total number of bytes on the buf
   */

  tor_assert(string);
  /* assert_buf_ok(buf); */

  if (buf_ensure_capacity(buf, buf->datalen+string_len)) {
    log_warn(LD_MM, "buflen too small, can't hold %d bytes.",
             (int)(buf->datalen+string_len));
    return -1;
  }

  next = _buf_end(buf);
  _split_range(buf, next, &string_len, &len2);

  memcpy(next, string, string_len);
  buf->datalen += string_len;

  if (len2) {
    tor_assert(_buf_end(buf) == buf->mem);
    memcpy(buf->mem, string+string_len, len2);
    buf->datalen += len2;
  }
  if (buf->datalen > buf->highwater)
    buf->highwater = buf->datalen;
  log_debug(LD_NET,"added %d bytes to buf (now %d total).",
            (int)string_len, (int)buf->datalen);
  check();
  return buf->datalen;
}

/** Helper: copy the first <b>string_len</b> bytes from <b>buf</b>
 * onto <b>string</b>.
 */
static INLINE void
peek_from_buf(char *string, size_t string_len, buf_t *buf)
{
  size_t len2;

  /* There must be string_len bytes in buf; write them onto string,
   * then memmove buf back (that is, remove them from buf).
   *
   * Return the number of bytes still on the buffer. */

  tor_assert(string);
  /* make sure we don't ask for too much */
  tor_assert(string_len <= buf->datalen);
  /* assert_buf_ok(buf); */

  _split_range(buf, buf->cur, &string_len, &len2);

  memcpy(string, buf->cur, string_len);
  if (len2) {
    memcpy(string+string_len,buf->mem,len2);
  }
}

/** Remove <b>string_len</b> bytes from the front of <b>buf</b>, and store
 * them into <b>string</b>.  Return the new buffer size.  <b>string_len</b>
 * must be \<= the number of bytes on the buffer.
 */
int
fetch_from_buf(char *string, size_t string_len, buf_t *buf)
{
  /* There must be string_len bytes in buf; write them onto string,
   * then memmove buf back (that is, remove them from buf).
   *
   * Return the number of bytes still on the buffer. */

  check();
  peek_from_buf(string, string_len, buf);
  buf_remove_from_front(buf, string_len);
  check();
  return buf->datalen;
}

/** Move up to *<b>buf_flushlen</b> bytes from <b>buf_in</b> to
 * <b>buf_out</b>, and modify *<b>buf_flushlen</b> appropriately.
 * Return the number of bytes actually copied.
 */
int
move_buf_to_buf(buf_t *buf_out, buf_t *buf_in, size_t *buf_flushlen)
{
  char b[4096];
  size_t cp, len;
  len = *buf_flushlen;
  if (len > buf_in->datalen)
    len = buf_in->datalen;

  cp = len; /* Remember the number of bytes we intend to copy. */
  while (len) {
    /* This isn't the most efficient implementation one could imagine, since
     * it does two copies instead of 1, but I kinda doubt that this will be
     * critical path. */
    size_t n = len > sizeof(b) ? sizeof(b) : len;
    fetch_from_buf(b, n, buf_in);
    write_to_buf(b, n, buf_out);
    len -= n;
  }
  *buf_flushlen -= cp;
  return cp;
}

/** There is a (possibly incomplete) http statement on <b>buf</b>, of the
 * form "\%s\\r\\n\\r\\n\%s", headers, body. (body may contain nuls.)
 * If a) the headers include a Content-Length field and all bytes in
 * the body are present, or b) there's no Content-Length field and
 * all headers are present, then:
 *
 *  - strdup headers into <b>*headers_out</b>, and nul-terminate it.
 *  - memdup body into <b>*body_out</b>, and nul-terminate it.
 *  - Then remove them from <b>buf</b>, and return 1.
 *
 *  - If headers or body is NULL, discard that part of the buf.
 *  - If a headers or body doesn't fit in the arg, return -1.
 *  (We ensure that the headers or body don't exceed max len,
 *   _even if_ we're planning to discard them.)
 *  - If force_complete is true, then succeed even if not all of the
 *    content has arrived.
 *
 * Else, change nothing and return 0.
 */
int
fetch_from_buf_http(buf_t *buf,
                    char **headers_out, size_t max_headerlen,
                    char **body_out, size_t *body_used, size_t max_bodylen,
                    int force_complete)
{
  char *headers, *body, *p;
  size_t headerlen, bodylen, contentlen;

  /* assert_buf_ok(buf); */
  buf_normalize(buf);

  if (buf_nul_terminate(buf)<0) {
    log_warn(LD_BUG,"Couldn't nul-terminate buffer");
    return -1;
  }
  headers = buf->cur;
  body = strstr(headers,"\r\n\r\n");
  if (!body) {
    log_debug(LD_HTTP,"headers not all here yet.");
    return 0;
  }
  body += 4; /* Skip the the CRLFCRLF */
  headerlen = body-headers; /* includes the CRLFCRLF */
  bodylen = buf->datalen - headerlen;
  log_debug(LD_HTTP,"headerlen %d, bodylen %d.", (int)headerlen, (int)bodylen);

  if (max_headerlen <= headerlen) {
    log_warn(LD_HTTP,"headerlen %d larger than %d. Failing.",
             (int)headerlen, (int)max_headerlen-1);
    return -1;
  }
  if (max_bodylen <= bodylen) {
    log_warn(LD_HTTP,"bodylen %d larger than %d. Failing.",
             (int)bodylen, (int)max_bodylen-1);
    return -1;
  }

#define CONTENT_LENGTH "\r\nContent-Length: "
  p = strstr(headers, CONTENT_LENGTH);
  if (p) {
    int i;
    i = atoi(p+strlen(CONTENT_LENGTH));
    if (i < 0) {
      log_warn(LD_PROTOCOL, "Content-Length is less than zero; it looks like "
               "someone is trying to crash us.");
      return -1;
    }
    contentlen = i;
    /* if content-length is malformed, then our body length is 0. fine. */
    log_debug(LD_HTTP,"Got a contentlen of %d.",(int)contentlen);
    if (bodylen < contentlen) {
      if (!force_complete) {
        log_debug(LD_HTTP,"body not all here yet.");
        return 0; /* not all there yet */
      }
    }
    if (bodylen > contentlen) {
      bodylen = contentlen;
      log_debug(LD_HTTP,"bodylen reduced to %d.",(int)bodylen);
    }
  }
  /* all happy. copy into the appropriate places, and return 1 */
  if (headers_out) {
    *headers_out = tor_malloc(headerlen+1);
    memcpy(*headers_out,buf->cur,headerlen);
    (*headers_out)[headerlen] = 0; /* nul terminate it */
  }
  if (body_out) {
    tor_assert(body_used);
    *body_used = bodylen;
    *body_out = tor_malloc(bodylen+1);
    memcpy(*body_out,buf->cur+headerlen,bodylen);
    (*body_out)[bodylen] = 0; /* nul terminate it */
  }
  buf_remove_from_front(buf, headerlen+bodylen);
  return 1;
}

/** There is a (possibly incomplete) socks handshake on <b>buf</b>, of one
 * of the forms
 *  - socks4: "socksheader username\\0"
 *  - socks4a: "socksheader username\\0 destaddr\\0"
 *  - socks5 phase one: "version #methods methods"
 *  - socks5 phase two: "version command 0 addresstype..."
 * If it's a complete and valid handshake, and destaddr fits in
 *   MAX_SOCKS_ADDR_LEN bytes, then pull the handshake off the buf,
 *   assign to <b>req</b>, and return 1.
 *
 * If it's invalid or too big, return -1.
 *
 * Else it's not all there yet, leave buf alone and return 0.
 *
 * If you want to specify the socks reply, write it into <b>req->reply</b>
 *   and set <b>req->replylen</b>, else leave <b>req->replylen</b> alone.
 *
 * If <b>log_sockstype</b> is non-zero, then do a notice-level log of whether
 * the connection is possibly leaking DNS requests locally or not.
 *
 * If <b>safe_socks</b> is true, then reject unsafe socks protocols.
 *
 * If returning 0 or -1, <b>req->address</b> and <b>req->port</b> are
 * undefined.
 */
int
fetch_from_buf_socks(buf_t *buf, socks_request_t *req,
                     int log_sockstype, int safe_socks)
{
  unsigned int len;
  char tmpbuf[INET_NTOA_BUF_LEN];
  uint32_t destip;
  enum {socks4, socks4a} socks4_prot = socks4a;
  char *next, *startaddr;
  struct in_addr in;

  /* If the user connects with socks4 or the wrong variant of socks5,
   * then log a warning to let him know that it might be unwise. */
  static int have_warned_about_unsafe_socks = 0;

  if (buf->datalen < 2) /* version and another byte */
    return 0;
  buf_normalize(buf);

  switch (*(buf->cur)) { /* which version of socks? */

    case 5: /* socks5 */

      if (req->socks_version != 5) { /* we need to negotiate a method */
        unsigned char nummethods = (unsigned char)*(buf->cur+1);
        tor_assert(!req->socks_version);
        if (buf->datalen < 2u+nummethods)
          return 0;
        if (!nummethods || !memchr(buf->cur+2, 0, nummethods)) {
          log_warn(LD_APP,
                   "socks5: offered methods don't include 'no auth'. "
                   "Rejecting.");
          req->replylen = 2; /* 2 bytes of response */
          req->reply[0] = 5;
          req->reply[1] = '\xFF'; /* reject all methods */
          return -1;
        }
        /* remove packet from buf. also remove any other extraneous
         * bytes, to support broken socks clients. */
        buf_clear(buf);

        req->replylen = 2; /* 2 bytes of response */
        req->reply[0] = 5; /* socks5 reply */
        req->reply[1] = SOCKS5_SUCCEEDED;
        req->socks_version = 5; /* remember we've already negotiated auth */
        log_debug(LD_APP,"socks5: accepted method 0");
        return 0;
      }
      /* we know the method; read in the request */
      log_debug(LD_APP,"socks5: checking request");
      if (buf->datalen < 8) /* basic info plus >=2 for addr plus 2 for port */
        return 0; /* not yet */
      req->command = (unsigned char) *(buf->cur+1);
      if (req->command != SOCKS_COMMAND_CONNECT &&
          req->command != SOCKS_COMMAND_CONNECT_DIR &&
          req->command != SOCKS_COMMAND_RESOLVE &&
          req->command != SOCKS_COMMAND_RESOLVE_PTR) {
        /* not a connect or resolve or a resolve_ptr? we don't support it. */
        log_warn(LD_APP,"socks5: command %d not recognized. Rejecting.",
                 req->command);
        return -1;
      }
      switch (*(buf->cur+3)) { /* address type */
        case 1: /* IPv4 address */
          log_debug(LD_APP,"socks5: ipv4 address type");
          if (buf->datalen < 10) /* ip/port there? */
            return 0; /* not yet */

          destip = ntohl(*(uint32_t*)(buf->cur+4));
          in.s_addr = htonl(destip);
          tor_inet_ntoa(&in,tmpbuf,sizeof(tmpbuf));
          if (strlen(tmpbuf)+1 > MAX_SOCKS_ADDR_LEN) {
            log_warn(LD_APP,
                     "socks5 IP takes %d bytes, which doesn't fit in %d. "
                     "Rejecting.",
                     (int)strlen(tmpbuf)+1,(int)MAX_SOCKS_ADDR_LEN);
            return -1;
          }
          strlcpy(req->address,tmpbuf,sizeof(req->address));
          req->port = ntohs(*(uint16_t*)(buf->cur+8));
          buf_remove_from_front(buf, 10);
          if (req->command != SOCKS_COMMAND_RESOLVE_PTR &&
              !addressmap_have_mapping(req->address) &&
              !have_warned_about_unsafe_socks) {
            log_warn(LD_APP,
                "Your application (using socks5 to port %d) is giving "
                "Tor only an IP address. Applications that do DNS resolves "
                "themselves may leak information. Consider using Socks4A "
                "(e.g. via privoxy or socat) instead. For more information, "
                "please see http://wiki.noreply.org/noreply/TheOnionRouter/"
                "TorFAQ#SOCKSAndDNS.%s", req->port,
                safe_socks ? " Rejecting." : "");
//            have_warned_about_unsafe_socks = 1; // (for now, warn every time)
            control_event_client_status(LOG_WARN,
                          "DANGEROUS_SOCKS PROTOCOL=SOCKS5 ADDRESS=%s:%d",
                          req->address, req->port);
            if (safe_socks)
              return -1;
          }
          return 1;
        case 3: /* fqdn */
          log_debug(LD_APP,"socks5: fqdn address type");
          if (req->command == SOCKS_COMMAND_RESOLVE_PTR) {
            log_warn(LD_APP, "socks5 received RESOLVE_PTR command with "
                     "hostname type. Rejecting.");
            return -1;
          }
          len = (unsigned char)*(buf->cur+4);
          if (buf->datalen < 7+len) /* addr/port there? */
            return 0; /* not yet */
          if (len+1 > MAX_SOCKS_ADDR_LEN) {
            log_warn(LD_APP,
                     "socks5 hostname is %d bytes, which doesn't fit in "
                     "%d. Rejecting.", len+1,MAX_SOCKS_ADDR_LEN);
            return -1;
          }
          memcpy(req->address,buf->cur+5,len);
          req->address[len] = 0;
          req->port = ntohs(get_uint16(buf->cur+5+len));
          buf_remove_from_front(buf, 5+len+2);
          if (!tor_strisprint(req->address) || strchr(req->address,'\"')) {
            log_warn(LD_PROTOCOL,
                     "Your application (using socks5 to port %d) gave Tor "
                     "a malformed hostname: %s. Rejecting the connection.",
                     req->port, escaped(req->address));
            return -1;
          }
          if (log_sockstype)
            log_notice(LD_APP,
                  "Your application (using socks5 to port %d) gave "
                  "Tor a hostname, which means Tor will do the DNS resolve "
                  "for you. This is good.", req->port);
          return 1;
        default: /* unsupported */
          log_warn(LD_APP,"socks5: unsupported address type %d. Rejecting.",
                   *(buf->cur+3));
          return -1;
      }
      tor_assert(0);
    case 4: /* socks4 */
      /* http://archive.socks.permeo.com/protocol/socks4.protocol */
      /* http://archive.socks.permeo.com/protocol/socks4a.protocol */

      req->socks_version = 4;
      if (buf->datalen < SOCKS4_NETWORK_LEN) /* basic info available? */
        return 0; /* not yet */

      req->command = (unsigned char) *(buf->cur+1);
      if (req->command != SOCKS_COMMAND_CONNECT &&
          req->command != SOCKS_COMMAND_CONNECT_DIR &&
          req->command != SOCKS_COMMAND_RESOLVE) {
        /* not a connect or resolve? we don't support it. (No resolve_ptr with
         * socks4.) */
        log_warn(LD_APP,"socks4: command %d not recognized. Rejecting.",
                 req->command);
        return -1;
      }

      req->port = ntohs(*(uint16_t*)(buf->cur+2));
      destip = ntohl(*(uint32_t*)(buf->mem+4));
      if ((!req->port && req->command!=SOCKS_COMMAND_RESOLVE) || !destip) {
        log_warn(LD_APP,"socks4: Port or DestIP is zero. Rejecting.");
        return -1;
      }
      if (destip >> 8) {
        log_debug(LD_APP,"socks4: destip not in form 0.0.0.x.");
        in.s_addr = htonl(destip);
        tor_inet_ntoa(&in,tmpbuf,sizeof(tmpbuf));
        if (strlen(tmpbuf)+1 > MAX_SOCKS_ADDR_LEN) {
          log_debug(LD_APP,"socks4 addr (%d bytes) too long. Rejecting.",
                    (int)strlen(tmpbuf));
          return -1;
        }
        log_debug(LD_APP,
                  "socks4: successfully read destip (%s)", safe_str(tmpbuf));
        socks4_prot = socks4;
      }

      next = memchr(buf->cur+SOCKS4_NETWORK_LEN, 0,
                    buf->datalen-SOCKS4_NETWORK_LEN);
      if (!next) {
        log_debug(LD_APP,"socks4: Username not here yet.");
        return 0;
      }
      tor_assert(next < buf->cur+buf->datalen);

      startaddr = NULL;
      if (socks4_prot != socks4a &&
          !addressmap_have_mapping(tmpbuf) &&
          !have_warned_about_unsafe_socks) {
        log_warn(LD_APP,
                 "Your application (using socks4 to port %d) is giving Tor "
                 "only an IP address. Applications that do DNS resolves "
                 "themselves may leak information. Consider using Socks4A "
                 "(e.g. via privoxy or socat) instead. For more information, "
                 "please see http://wiki.noreply.org/noreply/TheOnionRouter/"
                 "TorFAQ#SOCKSAndDNS.%s", req->port,
                 safe_socks ? " Rejecting." : "");
//      have_warned_about_unsafe_socks = 1; // (for now, warn every time)
        control_event_client_status(LOG_WARN,
                        "DANGEROUS_SOCKS PROTOCOL=SOCKS4 ADDRESS=%s:%d",
                        tmpbuf, req->port);
        if (safe_socks)
          return -1;
      }
      if (socks4_prot == socks4a) {
        if (next+1 == buf->cur+buf->datalen) {
          log_debug(LD_APP,"socks4: No part of destaddr here yet.");
          return 0;
        }
        startaddr = next+1;
        next = memchr(startaddr, 0, buf->cur+buf->datalen-startaddr);
        if (!next) {
          log_debug(LD_APP,"socks4: Destaddr not all here yet.");
          return 0;
        }
        if (MAX_SOCKS_ADDR_LEN <= next-startaddr) {
          log_warn(LD_APP,"socks4: Destaddr too long. Rejecting.");
          return -1;
        }
        tor_assert(next < buf->cur+buf->datalen);

        if (log_sockstype)
          log_notice(LD_APP,
                     "Your application (using socks4a to port %d) gave "
                     "Tor a hostname, which means Tor will do the DNS resolve "
                     "for you. This is good.", req->port);
      }
      log_debug(LD_APP,"socks4: Everything is here. Success.");
      strlcpy(req->address, startaddr ? startaddr : tmpbuf,
              sizeof(req->address));
      if (!tor_strisprint(req->address) || strchr(req->address,'\"')) {
        log_warn(LD_PROTOCOL,
                 "Your application (using socks4 to port %d) gave Tor "
                 "a malformed hostname: %s. Rejecting the connection.",
                 req->port, escaped(req->address));
        return -1;
      }
      /* next points to the final \0 on inbuf */
      buf_remove_from_front(buf, next-buf->cur+1);
      return 1;

    case 'G': /* get */
    case 'H': /* head */
    case 'P': /* put/post */
    case 'C': /* connect */
      strlcpy(req->reply,
"HTTP/1.0 501 Tor is not an HTTP Proxy\r\n"
"Content-Type: text/html; charset=iso-8859-1\r\n\r\n"
"<html>\n"
"<head>\n"
"<title>Tor is not an HTTP Proxy</title>\n"
"</head>\n"
"<body>\n"
"<h1>Tor is not an HTTP Proxy</h1>\n"
"<p>\n"
"It appears you have configured your web browser to use Tor as an HTTP proxy."
"\n"
"This is not correct: Tor is a SOCKS proxy, not an HTTP proxy.\n"
"Please configure your client accordingly.\n"
"</p>\n"
"<p>\n"
"See <a href=\"http://tor.eff.org/documentation.html\">"
           "http://tor.eff.org/documentation.html</a> for more information.\n"
"<!-- Plus this comment, to make the body response more than 512 bytes, so "
"     IE will be willing to display it. Comment comment comment comment "
"     comment comment comment comment comment comment comment comment.-->\n"
"</p>\n"
"</body>\n"
"</html>\n"
             , MAX_SOCKS_REPLY_LEN);
      req->replylen = strlen(req->reply)+1;
      /* fall through */
    default: /* version is not socks4 or socks5 */
      log_warn(LD_APP,
               "Socks version %d not recognized. (Tor is not an http proxy.)",
               *(buf->cur));
      {
        char *tmp = tor_strndup(buf->cur, 8);
        control_event_client_status(LOG_WARN,
                                    "SOCKS_UNKNOWN_PROTOCOL DATA=\"%s\"",
                                    escaped(tmp));
        tor_free(tmp);
      }
      return -1;
  }
}

/** Return 1 iff buf looks more like it has an (obsolete) v0 controller
 * command on it than any valid v1 controller command. */
int
peek_buf_has_control0_command(buf_t *buf)
{
  if (buf->datalen >= 4) {
    char header[4];
    uint16_t cmd;
    peek_from_buf(header, sizeof(header), buf);
    cmd = ntohs(get_uint16(header+2));
    if (cmd <= 0x14)
      return 1; /* This is definitely not a v1 control command. */
  }
  return 0;
}

/** Helper: return a pointer to the first instance of <b>c</b> in the
 * <b>len</b>characters after <b>start</b> on <b>buf</b>. Return NULL if the
 * character isn't found. */
static char *
find_char_on_buf(buf_t *buf, char *start, size_t len, char c)
{
  size_t len_rest;
  char *cp;
  _split_range(buf, start, &len, &len_rest);
  cp = memchr(start, c, len);
  if (cp || !len_rest)
    return cp;
  return memchr(buf->mem, c, len_rest);
}

/** Try to read a single LF-terminated line from <b>buf</b>, and write it,
 * NUL-terminated, into the *<b>data_len</b> byte buffer at <b>data_out</b>.
 * Set *<b>data_len</b> to the number of bytes in the line, not counting the
 * terminating NUL.  Return 1 if we read a whole line, return 0 if we don't
 * have a whole line yet, and return -1 if the line length exceeds
 *<b>data_len</b>.
 */
int
fetch_from_buf_line(buf_t *buf, char *data_out, size_t *data_len)
{
  char *cp;
  size_t sz;

  size_t remaining = buf->datalen - _buf_offset(buf,buf->cur);
  cp = find_char_on_buf(buf, buf->cur, remaining, '\n');
  if (!cp)
    return 0;
  sz = _buf_offset(buf, cp);
  if (sz+2 > *data_len) {
    *data_len = sz+2;
    return -1;
  }
  fetch_from_buf(data_out, sz+1, buf);
  data_out[sz+1] = '\0';
  *data_len = sz+1;
  return 1;
}

/** Compress on uncompress the <b>data_len</b> bytes in <b>data</b> using the
 * zlib state <b>state</b>, appending the result to <b>buf</b>.  If
 * <b>done</b> is true, flush the data in the state and finish the
 * compression/uncompression.  Return -1 on failure, 0 on success. */
int
write_to_buf_zlib(buf_t *buf, tor_zlib_state_t *state,
                  const char *data, size_t data_len,
                  int done)
{
  char *next;
  size_t old_avail, avail;
  int over = 0;
  do {
    buf_ensure_capacity(buf, buf->datalen + 1024);
    next = _buf_end(buf);
    if (next < buf->cur)
      old_avail = avail = buf->cur - next;
    else
      old_avail = avail = (buf->mem + buf->len) - next;
    switch (tor_zlib_process(state, &next, &avail, &data, &data_len, done)) {
      case TOR_ZLIB_DONE:
        over = 1;
        break;
      case TOR_ZLIB_ERR:
        return -1;
      case TOR_ZLIB_OK:
        if (data_len == 0)
          over = 1;
        break;
      case TOR_ZLIB_BUF_FULL:
        if (avail && buf->len >= 1024 + buf->datalen) {
          /* Zlib says we need more room (ZLIB_BUF_FULL), and we're not about
           * to wrap around (avail != 0), and resizing won't actually make us
           * un-full: we're at the end of the buffer, and zlib refuses to
           * append more here, but there's a pile of free space at the start
           * of the buffer (about 1K).  So chop a few characters off the
           * end of the buffer.  This feels silly; anybody got a better hack?
           *
           * (We don't just want to expand the buffer nevertheless. Consider a
           * 1/3 full buffer with a single byte free at the end. zlib will
           * often refuse to append to that, and so we want to use the
           * beginning, not double the buffer to be just 1/6 full.)
           */
          tor_assert(next >= buf->cur);
          buf->len -= avail;
        }
        break;
    }
    buf->datalen += old_avail - avail;
    if (buf->datalen > buf->highwater)
      buf->highwater = buf->datalen;
  } while (!over);
  return 0;
}

/** Log an error and exit if <b>buf</b> is corrupted.
 */
void
assert_buf_ok(buf_t *buf)
{
  tor_assert(buf);
  tor_assert(buf->magic == BUFFER_MAGIC);
  tor_assert(buf->highwater <= buf->len);
  tor_assert(buf->datalen <= buf->highwater);

  if (buf->mem) {
    tor_assert(buf->cur >= buf->mem);
    tor_assert(buf->cur < buf->mem+buf->len);
    tor_assert(buf->memsize == ALLOC_LEN(buf->len));
  } else {
    tor_assert(!buf->cur);
    tor_assert(!buf->len);
    tor_assert(!buf->memsize);
  }

#ifdef SENTINELS
  if (buf->mem) {
    uint32_t u32 = get_uint32(buf->mem - 4);
    tor_assert(u32 == START_MAGIC);
    u32 = get_uint32(buf->mem + buf->memsize - 8);
    tor_assert(u32 == END_MAGIC);
  }
#endif
}

