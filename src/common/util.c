/* Copyright 2003 Roger Dingledine */
/* See LICENSE for licensing information */
/* $Id$ */

/**
 * \file util.c
 *
 * \brief Common functions for strings, IO, network, data structures,
 * process control, and cross-platform portability.
 **/

/* This is required on rh7 to make strptime not complain.
 */
#define _GNU_SOURCE

#include "orconfig.h"

#ifdef MS_WINDOWS
#define WIN32_WINNT 0x400
#define _WIN32_WINNT 0x400
#define WIN32_LEAN_AND_MEAN
#if _MSC_VER > 1300
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(_MSC_VER)
#include <winsock.h>
#endif
#include <io.h>
#include <process.h>
#include <direct.h>
#include <windows.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#ifndef HAVE_GETTIMEOFDAY
#ifdef HAVE_FTIME
#include <sys/timeb.h>
#endif
#endif

#include "util.h"
#include "log.h"
#include "crypto.h"
#include "../or/tree.h"

#ifdef HAVE_UNAME
#include <sys/utsname.h>
#endif
#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h> /* FreeBSD needs this to know what version it is */
#endif
#ifdef HAVE_SYS_LIMITS_H
#include <sys/limits.h>
#endif
#ifdef HAVE_MACHINE_LIMITS_H
#ifndef __FreeBSD__
  /* FreeBSD has a bug where it complains that this file is obsolete,
     and I should migrate to using sys/limits. It complains even when
     I include both. */
#include <machine/limits.h>
#endif
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h> /* Must be included before sys/stat.h for Ultrix */
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

/* used by inet_addr, not defined on solaris anywhere!? */
#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long) -1)
#endif

/* Inline the strl functions if the plaform doesn't have them. */
#ifndef HAVE_STRLCPY
#include "strlcpy.c"
#endif
#ifndef HAVE_STRLCAT
#include "strlcat.c"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

/** Allocate a chunk of <b>size</b> bytes of memory, and return a pointer to
 * result.  On error, log and terminate the process.  (Same as malloc(size),
 * but never returns NULL.)
 */
void *tor_malloc(size_t size) {
  void *result;

  /* Some libcs don't do the right thing on size==0. Override them. */
  if (size==0) {
    size=1;
  }
  result = malloc(size);

  if(!result) {
    log_fn(LOG_ERR, "Out of memory. Dying.");
    exit(1);
  }
//  memset(result,'X',size); /* deadbeef to encourage bugs */
  return result;
}

/* Allocate a chunk of <b>size</b> bytes of memory, fill the memory with
 * zero bytes, and return a pointer to the result.  Log and terminate
 * the process on error.  (Same as calloc(size,1), but never returns NULL.)
 */
void *tor_malloc_zero(size_t size) {
  void *result = tor_malloc(size);
  memset(result, 0, size);
  return result;
}

/** Change the size of the memory block pointed to by <b>ptr</b> to <b>size</b>
 * bytes long; return the new memory block.  On error, log and
 * terminate. (Like realloc(ptr,size), but never returns NULL.)
 */
void *tor_realloc(void *ptr, size_t size) {
  void *result;

  result = realloc(ptr, size);
  if (!result) {
    log_fn(LOG_ERR, "Out of memory. Dying.");
    exit(1);
  }
  return result;
}

/** Return a newly allocated copy of the NUL-terminated string s. On
 * error, log and terminate.  (Like strdup(s), but never returns
 * NULL.)
 */
char *tor_strdup(const char *s) {
  char *dup;
  tor_assert(s);

  dup = strdup(s);
  if(!dup) {
    log_fn(LOG_ERR,"Out of memory. Dying.");
    exit(1);
  }
  return dup;
}

/** Allocate and return a new string containing the first <b>n</b>
 * characters of <b>s</b>.  If <b>s</b> is longer than <b>n</b>
 * characters, only the first <b>n</b> are copied.  The result is
 * always NUL-terminated.  (Like strndup(s,n), but never returns
 * NULL.)
 */
char *tor_strndup(const char *s, size_t n) {
  char *dup;
  tor_assert(s);
  dup = tor_malloc(n+1);
  strlcpy(dup, s, n+1);
  return dup;
}

/** Remove from the string <b>s</b> every character which appears in
 * <b>strip</b>.  Return the number of characters removed. */
int tor_strstrip(char *s, const char *strip)
{
  char *read = s;
  while (*read) {
    if (strchr(strip, *read)) {
      ++read;
    } else {
      *s++ = *read++;
    }
  }
  *s = '\0';
  return read-s;
}

/** Set the <b>dest_len</b>-byte buffer <b>buf</b> to contain the
 * string <b>s</b>, with the string <b>insert</b> inserted after every
 * <b>n</b> characters.  Return 0 on success, -1 on failure.
 *
 * If <b>rule</b> is ALWAYS_TERMINATE, then always end the string with
 * <b>insert</b>, even if its length is not a multiple of <b>n</b>.  If
 * <b>rule</b> is NEVER_TERMINATE, then never end the string with
 * <b>insert</b>, even if its length <i>is</i> a multiple of <b>n</b>.
 * If <b>rule</b> is TERMINATE_IF_EVEN, then end the string with <b>insert</b>
 * exactly when its length <i>is</i> a multiple of <b>n</b>.
 */
int tor_strpartition(char *dest, size_t dest_len,
                     const char *s, const char *insert, size_t n,
                     part_finish_rule_t rule)
{
  char *destp;
  size_t len_in, len_out, len_ins;
  int is_even, remaining;
  tor_assert(s);
  tor_assert(insert);
  tor_assert(n > 0);
  len_in = strlen(s);
  len_ins = strlen(insert);
  len_out = len_in + (len_in/n)*len_ins;
  is_even = (len_in%n) == 0;
  switch(rule)
    {
    case ALWAYS_TERMINATE:
      if (!is_even) len_out += len_ins;
      break;
    case NEVER_TERMINATE:
      if (is_even && len_in) len_out -= len_ins;
      break;
    case TERMINATE_IF_EVEN:
      break;
    }
  if (dest_len < len_out+1)
    return -1;
  destp = dest;
  remaining = len_in;
  while(remaining) {
    strncpy(destp, s, n);
    remaining -= n;
    if (remaining < 0) {
      if (rule == ALWAYS_TERMINATE)
        strcpy(destp+n+remaining,insert);
      break;
    } else if (remaining == 0 && rule == NEVER_TERMINATE) {
      *(destp+n) = '\0';
      break;
    }
    strcpy(destp+n, insert);
    s += n;
    destp += n+len_ins;
  }
  tor_assert(len_out == strlen(dest));
  return 0;
}

#ifndef UNALIGNED_INT_ACCESS_OK
/**
 * Read a 16-bit value beginning at <b>cp</b>.  Equaivalent to
 * *(uint16_t*)(cp), but will not cause segfaults on platforms that forbid
 * unaligned memory access.
 */
uint16_t get_uint16(const char *cp)
{
  uint16_t v;
  memcpy(&v,cp,2);
  return v;
}
/**
 * Read a 32-bit value beginning at <b>cp</b>.  Equaivalent to
 * *(uint32_t*)(cp), but will not cause segfaults on platforms that forbid
 * unaligned memory access.
 */
uint32_t get_uint32(const char *cp)
{
  uint32_t v;
  memcpy(&v,cp,4);
  return v;
}
/**
 * Set a 16-bit value beginning at <b>cp</b> to <b>v</b>. Equivalent to
 * *(uint16_t)(cp) = v, but will not cause segfaults on platforms that forbid
 * unaligned memory access. */
void set_uint16(char *cp, uint16_t v)
{
  memcpy(cp,&v,2);
}
/**
 * Set a 32-bit value beginning at <b>cp</b> to <b>v</b>. Equivalent to
 * *(uint32_t)(cp) = v, but will not cause segfaults on platforms that forbid
 * unaligned memory access. */
void set_uint32(char *cp, uint32_t v)
{
  memcpy(cp,&v,4);
}
#endif


/** Return a pointer to a NUL-terminated hexidecimal string encoding
 * the first <b>fromlen</b> bytes of <b>from</b>. (fromlen must be \<= 32.) The
 * result does not need to be deallocated, but repeated calls to
 * hex_str will trash old results.
 */
const char *hex_str(const char *from, size_t fromlen)
{
  static char buf[65];
  if (fromlen>(sizeof(buf)-1)/2)
    fromlen = (sizeof(buf)-1)/2;
  base16_encode(buf,sizeof(buf),from,fromlen);
  return buf;
}

/*****
 * smartlist_t: a simple resizeable array abstraction.
 *****/

/* All newly allocated smartlists have this capacity.
 */
#define SMARTLIST_DEFAULT_CAPACITY 32


struct smartlist_t {
  /** <b>list</b> has enough capacity to store exactly <b>capacity</b> elements
   * before it needs to be resized.  Only the first <b>num_used</b> (\<=
   * capacity) elements point to valid data.
   */
  void **list;
  int num_used;
  int capacity;
};

/** Allocate and return an empty smartlist.
 */
smartlist_t *smartlist_create() {
  smartlist_t *sl = tor_malloc(sizeof(smartlist_t));
  sl->num_used = 0;
  sl->capacity = SMARTLIST_DEFAULT_CAPACITY;
  sl->list = tor_malloc(sizeof(void *) * sl->capacity);
  return sl;
}

/** Deallocate a smartlist.  Does not release storage associated with the
 * list's elements.
 */
void smartlist_free(smartlist_t *sl) {
  free(sl->list);
  free(sl);
}

/** Change the capacity of the smartlist to <b>n</b>, so that we can grow
 * the list up to <b>n</b> elements with no further reallocation or wasted
 * space.  If <b>n</b> is less than or equal to the number of elements
 * currently in the list, reduce the list's capacity as much as
 * possible without losing elements.
 */
void smartlist_set_capacity(smartlist_t *sl, int n) {
  if (n < sl->num_used)
    n = sl->num_used;
  if (sl->capacity != n) {
    sl->capacity = n;
    sl->list = tor_realloc(sl->list, sizeof(void*)*sl->capacity);
  }
}

/** Remove all elements from the list.
 */
void smartlist_clear(smartlist_t *sl) {
  sl->num_used = 0;
}

/** Set the list's new length to <b>len</b> (which must be \<= the list's
 * current size). Remove the last smartlist_len(sl)-len elements from the
 * list.
 */
void smartlist_truncate(smartlist_t *sl, int len)
{
  tor_assert(len <= sl->num_used);
  sl->num_used = len;
}

/** Append element to the end of the list. */
void smartlist_add(smartlist_t *sl, void *element) {
  if (sl->num_used >= sl->capacity) {
    sl->capacity *= 2;
    sl->list = tor_realloc(sl->list, sizeof(void*)*sl->capacity);
  }
  sl->list[sl->num_used++] = element;
}

/** Append each element from S2 to the end of S1. */
void smartlist_add_all(smartlist_t *sl, const smartlist_t *s2)
{
  SMARTLIST_FOREACH(s2, void *, element, smartlist_add(sl, element));
}

/** Remove all elements E from sl such that E==element.  Does not preserve
 * the order of s1.
 */
void smartlist_remove(smartlist_t *sl, void *element) {
  int i;
  if(element == NULL)
    return;
  for(i=0; i < sl->num_used; i++)
    if(sl->list[i] == element) {
      sl->list[i] = sl->list[--sl->num_used]; /* swap with the end */
      i--; /* so we process the new i'th element */
    }
}

/** Return true iff some element E of sl has E==element.
 */
int smartlist_isin(const smartlist_t *sl, void *element) {
  int i;
  for(i=0; i < sl->num_used; i++)
    if(sl->list[i] == element)
      return 1;
  return 0;
}

int smartlist_string_isin(const smartlist_t *sl, const char *element) {
  int i;
  for(i=0; i < sl->num_used; i++)
    if(strcmp((const char*)sl->list[i],element)==0)
      return 1;
  return 0;
}

/** Return true iff some element E of sl2 has smartlist_isin(sl1,E).
 */
int smartlist_overlap(const smartlist_t *sl1, const smartlist_t *sl2) {
  int i;
  for(i=0; i < sl2->num_used; i++)
    if(smartlist_isin(sl1, sl2->list[i]))
      return 1;
  return 0;
}

/** Remove every element E of sl1 such that !smartlist_isin(sl2,E).
 * Does not preserve the order of sl1.
 */
void smartlist_intersect(smartlist_t *sl1, const smartlist_t *sl2) {
  int i;
  for(i=0; i < sl1->num_used; i++)
    if(!smartlist_isin(sl2, sl1->list[i])) {
      sl1->list[i] = sl1->list[--sl1->num_used]; /* swap with the end */
      i--; /* so we process the new i'th element */
    }
}

/** Remove every element E of sl1 such that smartlist_isin(sl2,E).
 * Does not preserve the order of sl1.
 */
void smartlist_subtract(smartlist_t *sl1, const smartlist_t *sl2) {
  int i;
  for(i=0; i < sl2->num_used; i++)
    smartlist_remove(sl1, sl2->list[i]);
}

/** Return the <b>idx</b>th element of sl.
 */
void *smartlist_get(const smartlist_t *sl, int idx)
{
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(idx < sl->num_used);
  return sl->list[idx];
}
/** Change the value of the <b>idx</b>th element of sl to <b>val</b>; return the old
 * value of the <b>idx</b>th element.
 */
void *smartlist_set(smartlist_t *sl, int idx, void *val)
{
  void *old;
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(idx < sl->num_used);
  old = sl->list[idx];
  sl->list[idx] = val;
  return old;
}
/** Remove the <b>idx</b>th element of sl; if idx is not the last
 * element, swap the last element of sl into the <b>idx</b>th space.
 * Return the old value of the <b>idx</b>th element.
 */
void *smartlist_del(smartlist_t *sl, int idx)
{
  void *old;
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(idx < sl->num_used);
  old = sl->list[idx];
  sl->list[idx] = sl->list[--sl->num_used];
  return old;
}
/** Remove the <b>idx</b>th element of sl; if idx is not the last element,
 * moving all subsequent elements back one space. Return the old value
 * of the <b>idx</b>th element.
 */
void *smartlist_del_keeporder(smartlist_t *sl, int idx)
{
  void *old;
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(idx < sl->num_used);
  old = sl->list[idx];
  --sl->num_used;
  if (idx < sl->num_used)
    memmove(sl->list+idx, sl->list+idx+1, sizeof(void*)*(sl->num_used-idx));
  return old;
}
/** Return the number of items in sl.
 */
int smartlist_len(const smartlist_t *sl)
{
  return sl->num_used;
}
/** Insert the value <b>val</b> as the new <b>idx</b>th element of
 * <b>sl</b>, moving all items previously at <b>idx</b> or later
 * forward one space.
 */
void smartlist_insert(smartlist_t *sl, int idx, void *val)
{
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(idx <= sl->num_used);
  if (idx == sl->num_used) {
    smartlist_add(sl, val);
  } else {
    /* Ensure sufficient capacity */
    if (sl->num_used >= sl->capacity) {
      sl->capacity *= 2;
      sl->list = tor_realloc(sl->list, sizeof(void*)*sl->capacity);
    }
    /* Move other elements away */
    if (idx < sl->num_used)
      memmove(sl->list + idx + 1, sl->list + idx,
              sizeof(void*)*(sl->num_used-idx));
    sl->num_used++;
    sl->list[idx] = val;
  }
}

/**
 * Split a string <b>str</b> along all occurences of <b>sep</b>,
 * adding the split strings, in order, to <b>sl</b>.  If
 * <b>flags</b>&amp;SPLIT_SKIP_SPACE is true, remove initial and
 * trailing space from each entry.  If
 * <b>flags</b>&amp;SPLIT_IGNORE_BLANK is true, remove any entries of
 * length 0.  If max>0, divide the string into no more than <b>max</b>
 * pieces.
 */
int smartlist_split_string(smartlist_t *sl, const char *str, const char *sep,
                           int flags, int max)
{
  const char *cp, *end, *next;
  int n = 0;

  tor_assert(sl);
  tor_assert(str);
  tor_assert(sep);

  cp = str;
  while (1) {
    if (flags&SPLIT_SKIP_SPACE) {
      while (isspace((int)*cp)) ++cp;
    }

    if (max>0 && n == max-1) {
      end = strchr(cp,'\0');
    } else {
      end = strstr(cp,sep);
      if (!end)
        end = strchr(cp,'\0');
    }
    if (!*end) {
      next = NULL;
    } else {
      next = end+strlen(sep);
    }

    if (flags&SPLIT_SKIP_SPACE) {
      while (end > cp && isspace((int)*(end-1)))
        --end;
    }
    if (end != cp || !(flags&SPLIT_IGNORE_BLANK)) {
      smartlist_add(sl, tor_strndup(cp, end-cp));
      ++n;
    }
    if (!next)
      break;
    cp = next;
  }

  return n;
}

/** Allocate and return a new string containing the concatenation of
 * the elements of <b>sl</b>, in order, separated by <b>join</b>.  If
 * <b>terminate</b> is true, also terminate the string with <b>join</b>.
 * Requires that every element of <b>sl</b> is NUL-terminated string.
 */
char *smartlist_join_strings(smartlist_t *sl, const char *join, int terminate)
{
  int i;
  size_t n = 0, jlen;
  char *r = NULL, *dst, *src;

  tor_assert(sl);
  tor_assert(join);
  jlen = strlen(join);
  for (i = 0; i < sl->num_used; ++i) {
    n += strlen(sl->list[i]);
    n += jlen;
  }
  if (!terminate) n -= jlen;
  dst = r = tor_malloc(n+1);
  for (i = 0; i < sl->num_used; ) {
    for (src = sl->list[i]; *src; )
      *dst++ = *src++;
    if (++i < sl->num_used || terminate) {
      memcpy(dst, join, jlen);
      dst += jlen;
    }
  }
  *dst = '\0';
  return r;
}

/* Splay-tree implementation of string-to-void* map
 */
struct strmap_entry_t {
  SPLAY_ENTRY(strmap_entry_t) node;
  char *key;
  void *val;
};

struct strmap_t {
  SPLAY_HEAD(strmap_tree, strmap_entry_t) head;
};

static int compare_strmap_entries(struct strmap_entry_t *a,
                                 struct strmap_entry_t *b)
{
  return strcmp(a->key, b->key);
}

SPLAY_PROTOTYPE(strmap_tree, strmap_entry_t, node, compare_strmap_entries);
SPLAY_GENERATE(strmap_tree, strmap_entry_t, node, compare_strmap_entries);

/** Create a new empty map from strings to void*'s.
 */
strmap_t* strmap_new(void)
{
  strmap_t *result;
  result = tor_malloc(sizeof(strmap_t));
  SPLAY_INIT(&result->head);
  return result;
}

/** Set the current value for <b>key</b> to <b>val</b>.  Returns the previous
 * value for <b>key</b> if one was set, or NULL if one was not.
 *
 * This function makes a copy of <b>key</b> if necessary, but not of <b>val</b>.
 */
void* strmap_set(strmap_t *map, const char *key, void *val)
{
  strmap_entry_t *resolve;
  strmap_entry_t search;
  void *oldval;
  tor_assert(map);
  tor_assert(key);
  tor_assert(val);
  search.key = (char*)key;
  resolve = SPLAY_FIND(strmap_tree, &map->head, &search);
  if (resolve) {
    oldval = resolve->val;
    resolve->val = val;
    return oldval;
  } else {
    resolve = tor_malloc_zero(sizeof(strmap_entry_t));
    resolve->key = tor_strdup(key);
    resolve->val = val;
    SPLAY_INSERT(strmap_tree, &map->head, resolve);
    return NULL;
  }
}

/** Return the current value associated with <b>key</b>, or NULL if no
 * value is set.
 */
void* strmap_get(strmap_t *map, const char *key)
{
  strmap_entry_t *resolve;
  strmap_entry_t search;
  tor_assert(map);
  tor_assert(key);
  search.key = (char*)key;
  resolve = SPLAY_FIND(strmap_tree, &map->head, &search);
  if (resolve) {
    return resolve->val;
  } else {
    return NULL;
  }
}

/** Remove the value currently associated with <b>key</b> from the map.
 * Return the value if one was set, or NULL if there was no entry for
 * <b>key</b>.
 *
 * Note: you must free any storage associated with the returned value.
 */
void* strmap_remove(strmap_t *map, const char *key)
{
  strmap_entry_t *resolve;
  strmap_entry_t search;
  void *oldval;
  tor_assert(map);
  tor_assert(key);
  search.key = (char*)key;
  resolve = SPLAY_FIND(strmap_tree, &map->head, &search);
  if (resolve) {
    oldval = resolve->val;
    SPLAY_REMOVE(strmap_tree, &map->head, resolve);
    tor_free(resolve->key);
    tor_free(resolve);
    return oldval;
  } else {
    return NULL;
  }
}

/** Same as strmap_set, but first converts <b>key</b> to lowercase. */
void* strmap_set_lc(strmap_t *map, const char *key, void *val)
{
  /* We could be a little faster by using strcasecmp instead, and a separate
   * type, but I don't think it matters. */
  void *v;
  char *lc_key = tor_strdup(key);
  tor_strlower(lc_key);
  v = strmap_set(map,lc_key,val);
  tor_free(lc_key);
  return v;
}
/** Same as strmap_get, but first converts <b>key</b> to lowercase. */
void* strmap_get_lc(strmap_t *map, const char *key)
{
  void *v;
  char *lc_key = tor_strdup(key);
  tor_strlower(lc_key);
  v = strmap_get(map,lc_key);
  tor_free(lc_key);
  return v;
}
/** Same as strmap_remove, but first converts <b>key</b> to lowercase */
void* strmap_remove_lc(strmap_t *map, const char *key)
{
  void *v;
  char *lc_key = tor_strdup(key);
  tor_strlower(lc_key);
  v = strmap_remove(map,lc_key);
  tor_free(lc_key);
  return v;
}

/** Invoke fn() on every entry of the map, in order.  For every entry,
 * fn() is invoked with that entry's key, that entry's value, and the
 * value of <b>data</b> supplied to strmap_foreach.  fn() must return a new
 * (possibly unmodified) value for each entry: if fn() returns NULL, the
 * entry is removed.
 *
 * Example:
 * \code
 *   static void* upcase_and_remove_empty_vals(const char *key, void *val,
 *                                             void* data) {
 *     char *cp = (char*)val;
 *     if (!*cp) {  // val is an empty string.
 *       free(val);
 *       return NULL;
 *     } else {
 *       for (; *cp; cp++)
 *         *cp = toupper(*cp);
 *       }
 *       return val;
 *     }
 *   }
 *
 *   ...
 *
 *   strmap_foreach(map, upcase_and_remove_empty_vals, NULL);
 * \endcode
 */
void strmap_foreach(strmap_t *map,
                    void* (*fn)(const char *key, void *val, void *data),
                    void *data)
{
  strmap_entry_t *ptr, *next;
  tor_assert(map);
  tor_assert(fn);
  for (ptr = SPLAY_MIN(strmap_tree, &map->head); ptr != NULL; ptr = next) {
    /* This remove-in-place usage is specifically blessed in tree(3). */
    next = SPLAY_NEXT(strmap_tree, &map->head, ptr);
    ptr->val = fn(ptr->key, ptr->val, data);
    if (!ptr->val) {
      SPLAY_REMOVE(strmap_tree, &map->head, ptr);
      tor_free(ptr->key);
      tor_free(ptr);
    }
  }
}

/** return an <b>iterator</b> pointer to the front of a map.
 *
 * Iterator example:
 *
 * \code
 * // uppercase values in "map", removing empty values.
 *
 * strmap_iter_t *iter;
 * const char *key;
 * void *val;
 * char *cp;
 *
 * for (iter = strmap_iter_init(map); !strmap_iter_done(iter); ) {
 *    strmap_iter_get(iter, &key, &val);
 *    cp = (char*)val;
 *    if (!*cp) {
 *       iter = strmap_iter_next_rmv(iter);
 *       free(val);
 *    } else {
 *       for(;*cp;cp++) *cp = toupper(*cp);
 *       iter = strmap_iter_next(iter);
 *    }
 * }
 * \endcode
 *
 */
strmap_iter_t *strmap_iter_init(strmap_t *map)
{
  tor_assert(map);
  return SPLAY_MIN(strmap_tree, &map->head);
}
/** Advance the iterator <b>iter</b> for map a single step to the next entry.
 */
strmap_iter_t *strmap_iter_next(strmap_t *map, strmap_iter_t *iter)
{
  tor_assert(map);
  tor_assert(iter);
  return SPLAY_NEXT(strmap_tree, &map->head, iter);
}
/** Advance the iterator <b>iter</b> a single step to the next entry, removing
 * the current entry.
 */
strmap_iter_t *strmap_iter_next_rmv(strmap_t *map, strmap_iter_t *iter)
{
  strmap_iter_t *next;
  tor_assert(map);
  tor_assert(iter);
  next = SPLAY_NEXT(strmap_tree, &map->head, iter);
  SPLAY_REMOVE(strmap_tree, &map->head, iter);
  tor_free(iter->key);
  tor_free(iter);
  return next;
}
/** Set *keyp and *valp to the current entry pointed to by iter.
 */
void strmap_iter_get(strmap_iter_t *iter, const char **keyp, void **valp)
{
  tor_assert(iter);
  tor_assert(keyp);
  tor_assert(valp);
  *keyp = iter->key;
  *valp = iter->val;
}
/** Return true iff iter has advanced past the last entry of map.
 */
int strmap_iter_done(strmap_iter_t *iter)
{
  return iter == NULL;
}
/** Remove all entries from <b>map</b>, and deallocate storage for those entries.
 * If free_val is provided, it is invoked on every value in <b>map</b>.
 */
void strmap_free(strmap_t *map, void (*free_val)(void*))
{
  strmap_entry_t *ent, *next;
  for (ent = SPLAY_MIN(strmap_tree, &map->head); ent != NULL; ent = next) {
    next = SPLAY_NEXT(strmap_tree, &map->head, ent);
    SPLAY_REMOVE(strmap_tree, &map->head, ent);
    tor_free(ent->key);
    if (free_val)
      tor_free(ent->val);
  }
  tor_assert(SPLAY_EMPTY(&map->head));
  tor_free(map);
}

int strmap_isempty(strmap_t *map)
{
  return SPLAY_EMPTY(&map->head);
}

/*
 *    String manipulation
 */

/** Convert all alphabetic characters in the nul-terminated string <b>s</b> to
 * lowercase. */
void tor_strlower(char *s)
{
  while (*s) {
    *s = tolower(*s);
    ++s;
  }
}

/* Compares the first strlen(s2) characters of s1 with s2.  Returns as for
 * strcmp.
 */
int strcmpstart(const char *s1, const char *s2)
{
  size_t n = strlen(s2);
  return strncmp(s1, s2, n);
}


/** Return a pointer to the first char of s that is not whitespace and
 * not a comment, or to the terminating NUL if no such character exists.
 */
const char *eat_whitespace(const char *s) {
  tor_assert(s);

  while(isspace((int)*s) || *s == '#') {
    while(isspace((int)*s))
      s++;
    if(*s == '#') { /* read to a \n or \0 */
      while(*s && *s != '\n')
        s++;
      if(!*s)
        return s;
    }
  }
  return s;
}

/** Return a pointer to the first char of s that is not a space or a tab,
 * or to the terminating NUL if no such character exists. */
const char *eat_whitespace_no_nl(const char *s) {
  while(*s == ' ' || *s == '\t')
    ++s;
  return s;
}

/** Return a pointer to the first char of s that is whitespace or <b>#</b>,
 * or to the terminating NUL if no such character exists.
 */
const char *find_whitespace(const char *s) {
  tor_assert(s);

  while(*s && !isspace((int)*s) && *s != '#')
    s++;

  return s;
}

/*
 * Time
 */

/** Set *timeval to the current time of day.  On error, log and terminate.
 * (Same as gettimeofday(timeval,NULL), but never returns -1.)
 */
void tor_gettimeofday(struct timeval *timeval) {
#ifdef HAVE_GETTIMEOFDAY
  if (gettimeofday(timeval, NULL)) {
    log_fn(LOG_ERR, "gettimeofday failed.");
    /* If gettimeofday dies, we have either given a bad timezone (we didn't),
       or segfaulted.*/
    exit(1);
  }
#elif defined(HAVE_FTIME)
  struct timeb tb;
  ftime(&tb);
  timeval->tv_sec = tb.time;
  timeval->tv_usec = tb.millitm * 1000;
#else
#error "No way to get time."
#endif
  return;
}

/** Return the number of microseconds elapsed between *start and *end.
 * If start is after end, return 0.
 */
long
tv_udiff(struct timeval *start, struct timeval *end)
{
  long udiff;
  long secdiff = end->tv_sec - start->tv_sec;

  if (secdiff+1 > LONG_MAX/1000000) {
    log_fn(LOG_WARN, "comparing times too far apart.");
    return LONG_MAX;
  }

  udiff = secdiff*1000000L + (end->tv_usec - start->tv_usec);
  if(udiff < 0) {
    log_fn(LOG_INFO, "start (%ld.%ld) is after end (%ld.%ld). Returning 0.",
           (long)start->tv_sec, (long)start->tv_usec, (long)end->tv_sec, (long)end->tv_usec);
    return 0;
  }
  return udiff;
}

/** Return -1 if *a \< *b, 0 if *a==*b, and 1 if *a \> *b.
 */
int tv_cmp(struct timeval *a, struct timeval *b) {
  if (a->tv_sec > b->tv_sec)
    return 1;
  if (a->tv_sec < b->tv_sec)
    return -1;
  if (a->tv_usec > b->tv_usec)
    return 1;
  if (a->tv_usec < b->tv_usec)
    return -1;
  return 0;
}

/** Increment *a by the number of seconds and microseconds in *b.
 */
void tv_add(struct timeval *a, struct timeval *b) {
  a->tv_usec += b->tv_usec;
  a->tv_sec += b->tv_sec + (a->tv_usec / 1000000);
  a->tv_usec %= 1000000;
}

/** Increment *a by <b>ms</b> milliseconds.
 */
void tv_addms(struct timeval *a, long ms) {
  a->tv_usec += (ms * 1000) % 1000000;
  a->tv_sec += ((ms * 1000) / 1000000) + (a->tv_usec / 1000000);
  a->tv_usec %= 1000000;
}


#define IS_LEAPYEAR(y) (!(y % 4) && ((y % 100) || !(y % 400)))
static int n_leapdays(int y1, int y2) {
  --y1;
  --y2;
  return (y2/4 - y1/4) - (y2/100 - y1/100) + (y2/400 - y1/400);
}
/** Number of days per month in non-leap year; used by tor_timegm. */
static const int days_per_month[] =
  { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/** Return a time_t given a struct tm.  The result is given in GMT, and
 * does not account for leap seconds.
 */
time_t tor_timegm (struct tm *tm) {
  /* This is a pretty ironclad timegm implementation, snarfed from Python2.2.
   * It's way more brute-force than fiddling with tzset().
   */
  time_t ret;
  unsigned long year, days, hours, minutes;
  int i;
  year = tm->tm_year + 1900;
  tor_assert(year >= 1970);
  tor_assert(tm->tm_mon >= 0);
  tor_assert(tm->tm_mon <= 11);
  days = 365 * (year-1970) + n_leapdays(1970,year);
  for (i = 0; i < tm->tm_mon; ++i)
    days += days_per_month[i];
  if (tm->tm_mon > 1 && IS_LEAPYEAR(year))
    ++days;
  days += tm->tm_mday - 1;
  hours = days*24 + tm->tm_hour;

  minutes = hours*60 + tm->tm_min;
  ret = minutes*60 + tm->tm_sec;
  return ret;
}

/* strftime is locale-specific, so we need to replace those parts */
static const char *WEEKDAY_NAMES[] =
  { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MONTH_NAMES[] =
  { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void format_rfc1123_time(char *buf, time_t t) {
  struct tm *tm = gmtime(&t);

  strftime(buf, RFC1123_TIME_LEN+1, "XXX, %d XXX %Y %H:%M:%S GMT", tm);
  tor_assert(tm->tm_wday >= 0);
  tor_assert(tm->tm_wday <= 6);
  memcpy(buf, WEEKDAY_NAMES[tm->tm_wday], 3);
  tor_assert(tm->tm_wday >= 0);
  tor_assert(tm->tm_mon <= 11);
  memcpy(buf+8, MONTH_NAMES[tm->tm_mon], 3);
}

int parse_rfc1123_time(const char *buf, time_t *t) {
  struct tm tm;
  char month[4];
  char weekday[4];
  int i, m;

  if (strlen(buf) != RFC1123_TIME_LEN)
    return -1;
  memset(&tm, 0, sizeof(tm));
  if (sscanf(buf, "%3s, %d %3s %d %d:%d:%d GMT", weekday,
             &tm.tm_mday, month, &tm.tm_year, &tm.tm_hour,
             &tm.tm_min, &tm.tm_sec) < 7) {
    log_fn(LOG_WARN, "Got invalid RFC1123 time \"%s\"", buf);
    return -1;
  }

  m = -1;
  for (i = 0; i < 12; ++i) {
    if (!strcmp(month, MONTH_NAMES[i])) {
      m = i;
      break;
    }
  }
  if (m<0) {
    log_fn(LOG_WARN, "Got invalid RFC1123 time \"%s\"", buf);
    return -1;
  }

  tm.tm_mon = m;
  tm.tm_year -= 1900;
  *t = tor_timegm(&tm);
  return 0;
}

void format_iso_time(char *buf, time_t t) {
  strftime(buf, ISO_TIME_LEN+1, "%Y-%m-%d %H:%M:%S", gmtime(&t));
}

int parse_iso_time(const char *cp, time_t *t) {
  struct tm st_tm;
#ifdef HAVE_STRPTIME
  if (!strptime(cp, "%Y-%m-%d %H:%M:%S", &st_tm)) {
    log_fn(LOG_WARN, "Published time was unparseable"); return -1;
  }
#else
  unsigned int year=0, month=0, day=0, hour=100, minute=100, second=100;
  if (sscanf(cp, "%u-%u-%u %u:%u:%u", &year, &month,
                &day, &hour, &minute, &second) < 6) {
        log_fn(LOG_WARN, "Published time was unparseable"); return -1;
  }
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
          hour > 23 || minute > 59 || second > 61) {
        log_fn(LOG_WARN, "Published time was nonsensical"); return -1;
  }
  st_tm.tm_year = year;
  st_tm.tm_mon = month-1;
  st_tm.tm_mday = day;
  st_tm.tm_hour = hour;
  st_tm.tm_min = minute;
  st_tm.tm_sec = second;
#endif
  *t = tor_timegm(&st_tm);
  return 0;
}


/*
 *   Low-level I/O.
 */

/** Write <b>count</b> bytes from <b>buf</b> to <b>fd</b>.  <b>isSocket</b>
 * must be 1 if fd was returned by socket() or accept(), and 0 if fd
 * was returned by open().  Return the number of bytes written, or -1
 * on error.  Only use if fd is a blocking fd.  */
int write_all(int fd, const char *buf, size_t count, int isSocket) {
  size_t written = 0;
  int result;

  while(written != count) {
    if (isSocket)
      result = send(fd, buf+written, count-written, 0);
    else
      result = write(fd, buf+written, count-written);
    if(result<0)
      return -1;
    written += result;
  }
  return count;
}

/** Read from <b>fd</b> to <b>buf</b>, until we get <b>count</b> bytes
 * or reach the end of the file.
 * isSocket must be 1 if fd
 * was returned by socket() or accept(), and 0 if fd was returned by
 * open().  Return the number of bytes read, or -1 on error. Only use
 * if fd is a blocking fd. */
int read_all(int fd, char *buf, size_t count, int isSocket) {
  size_t numread = 0;
  int result;

  while(numread != count) {
    if (isSocket)
      result = recv(fd, buf+numread, count-numread, 0);
    else
      result = read(fd, buf+numread, count-numread);
    if(result<0)
      return -1;
    else if (result == 0)
      break;
    numread += result;
  }
  return count;
}

/** Turn <b>socket</b> into a nonblocking socket.
 */
void set_socket_nonblocking(int socket)
{
#ifdef MS_WINDOWS
  /* Yes means no and no means yes.  Do you not want to be nonblocking? */
  int nonblocking = 0;
  ioctlsocket(socket, FIONBIO, (unsigned long*) &nonblocking);
#else
  fcntl(socket, F_SETFL, O_NONBLOCK);
#endif
}

/*
 *   Process control
 */

/** Minimalist interface to run a void function in the background.  On
 * unix calls fork, on win32 calls beginthread.  Returns -1 on failure.
 * func should not return, but rather should call spawn_exit.
 */
int spawn_func(int (*func)(void *), void *data)
{
#ifdef MS_WINDOWS
  int rv;
  rv = _beginthread(func, 0, data);
  if (rv == (unsigned long) -1)
    return -1;
  return 0;
#else
  pid_t pid;
  pid = fork();
  if (pid<0)
    return -1;
  if (pid==0) {
    /* Child */
    func(data);
    tor_assert(0); /* Should never reach here. */
    return 0; /* suppress "control-reaches-end-of-non-void" warning. */
  } else {
    /* Parent */
    return 0;
  }
#endif
}

/** End the current thread/process.
 */
void spawn_exit()
{
#ifdef MS_WINDOWS
  _endthread();
#else
  exit(0);
#endif
}


/**
 * Allocate a pair of connected sockets.  (Like socketpair(family,
 * type,protocol,fd), but works on systems that don't have
 * socketpair.)
 *
 * Currently, only (AF_UNIX, SOCK_STREAM, 0 ) sockets are supported.
 *
 * Note that on systems without socketpair, this call will fail if
 * localhost is inaccessible (for example, if the networking
 * stack is down). And even if it succeeds, the socket pair will not
 * be able to read while localhost is down later (the socket pair may
 * even close, depending on OS-specific timeouts).
 **/
int
tor_socketpair(int family, int type, int protocol, int fd[2])
{
#ifdef HAVE_SOCKETPAIR
    return socketpair(family, type, protocol, fd);
#else
    /* This socketpair does not work when localhost is down. So
     * it's really not the same thing at all. But it's close enough
     * for now, and really, when localhost is down sometimes, we
     * have other problems too.
     */
    int listener = -1;
    int connector = -1;
    int acceptor = -1;
    struct sockaddr_in listen_addr;
    struct sockaddr_in connect_addr;
    int size;

    if (protocol
#ifdef AF_UNIX
        || family != AF_UNIX
#endif
        ) {
#ifdef MS_WINDOWS
        errno = WSAEAFNOSUPPORT;
#else
        errno = EAFNOSUPPORT;
#endif
        return -1;
    }
    if (!fd) {
        errno = EINVAL;
        return -1;
    }

    listener = socket(AF_INET, type, 0);
    if (listener == -1)
      return -1;
    memset (&listen_addr, 0, sizeof (listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    listen_addr.sin_port = 0;   /* kernel choses port.  */
    if (bind(listener, (struct sockaddr *) &listen_addr, sizeof (listen_addr))
        == -1)
        goto tidy_up_and_fail;
    if (listen(listener, 1) == -1)
        goto tidy_up_and_fail;

    connector = socket(AF_INET, type, 0);
    if (connector == -1)
        goto tidy_up_and_fail;
    /* We want to find out the port number to connect to.  */
    size = sizeof (connect_addr);
    if (getsockname(listener, (struct sockaddr *) &connect_addr, &size) == -1)
        goto tidy_up_and_fail;
    if (size != sizeof (connect_addr))
        goto abort_tidy_up_and_fail;
    if (connect(connector, (struct sockaddr *) &connect_addr,
                sizeof (connect_addr)) == -1)
        goto tidy_up_and_fail;

    size = sizeof (listen_addr);
    acceptor = accept(listener, (struct sockaddr *) &listen_addr, &size);
    if (acceptor == -1)
        goto tidy_up_and_fail;
    if (size != sizeof(listen_addr))
        goto abort_tidy_up_and_fail;
    tor_close_socket(listener);
    /* Now check we are talking to ourself by matching port and host on the
       two sockets.  */
    if (getsockname(connector, (struct sockaddr *) &connect_addr, &size) == -1)
        goto tidy_up_and_fail;
    if (size != sizeof (connect_addr)
        || listen_addr.sin_family != connect_addr.sin_family
        || listen_addr.sin_addr.s_addr != connect_addr.sin_addr.s_addr
        || listen_addr.sin_port != connect_addr.sin_port) {
        goto abort_tidy_up_and_fail;
    }
    fd[0] = connector;
    fd[1] = acceptor;
    return 0;

  abort_tidy_up_and_fail:
#ifdef MS_WINDOWS
  errno = WSAECONNABORTED;
#else
  errno = ECONNABORTED; /* I hope this is portable and appropriate.  */
#endif
  tidy_up_and_fail:
    {
        int save_errno = errno;
        if (listener != -1)
            tor_close_socket(listener);
        if (connector != -1)
            tor_close_socket(connector);
        if (acceptor != -1)
            tor_close_socket(acceptor);
        errno = save_errno;
        return -1;
    }
#endif
}

/**
 * On Windows, WSAEWOULDBLOCK is not always correct: when you see it,
 * you need to ask the socket for its actual errno.  Also, you need to
 * get your errors from WSAGetLastError, not errno.  (If you supply a
 * socket of -1, we check WSAGetLastError, but don't correct
 * WSAEWOULDBLOCKs.)
 */
#ifdef MS_WINDOWS
int tor_socket_errno(int sock)
{
  int optval, optvallen=sizeof(optval);
  int err = WSAGetLastError();
  if (err == WSAEWOULDBLOCK && sock >= 0) {
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (void*)&optval, &optvallen))
      return err;
    if (optval)
      return optval;
  }
  return err;
}
#endif

#ifdef MS_WINDOWS
#define E(code, s) { code, (s " [" #code " ]") }
struct { int code; const char *msg; } windows_socket_errors[] = {
  E(WSAEINTR, "Interrupted function call"),
  E(WSAEACCES, "Permission denied"),
  E(WSAEFAULT, "Bad address"),
  E(WSAEINVAL, "Invalid argument"),
  E(WSAEMFILE, "Too many open files"),
  E(WSAEWOULDBLOCK,  "Resource temporarily unavailable"),
  E(WSAEINPROGRESS, "Operation now in progress"),
  E(WSAEALREADY, "Operation already in progress"),
  E(WSAENOTSOCK, "Socket operation on nonsocket"),
  E(WSAEDESTADDRREQ, "Destination address required"),
  E(WSAEMSGSIZE, "Message too long"),
  E(WSAEPROTOTYPE, "Protocol wrong for socket"),
  E(WSAENOPROTOOPT, "Bad protocol option"),
  E(WSAEPROTONOSUPPORT, "Protocol not supported"),
  E(WSAESOCKTNOSUPPORT, "Socket type not supported"),
  /* What's the difference between NOTSUPP and NOSUPPORT? :) */
  E(WSAEOPNOTSUPP, "Operation not supported"),
  E(WSAEPFNOSUPPORT,  "Protocol family not supported"),
  E(WSAEAFNOSUPPORT, "Address family not supported by protocol family"),
  E(WSAEADDRINUSE, "Address already in use"),
  E(WSAEADDRNOTAVAIL, "Cannot assign requested address"),
  E(WSAENETDOWN, "Network is down"),
  E(WSAENETUNREACH, "Network is unreachable"),
  E(WSAENETRESET, "Network dropped connection on reset"),
  E(WSAECONNABORTED, "Software caused connection abort"),
  E(WSAECONNRESET, "Connection reset by peer"),
  E(WSAENOBUFS, "No buffer space avaialable"),
  E(WSAEISCONN, "Socket is already connected"),
  E(WSAENOTCONN, "Socket is not connected"),
  E(WSAESHUTDOWN, "Cannot send after socket shutdown"),
  E(WSAETIMEDOUT, "Connection timed out"),
  E(WSAECONNREFUSED, "Connection refused"),
  E(WSAEHOSTDOWN, "Host is down"),
  E(WSAEHOSTUNREACH, "No route to host"),
  E(WSAEPROCLIM, "Too many processes"),
  /* Yes, some of these start with WSA, not WSAE. No, I don't know why. */
  E(WSASYSNOTREADY, "Network subsystem is unavailable"),
  E(WSAVERNOTSUPPORTED, "Winsock.dll out of range"),
  E(WSANOTINITIALISED, "Successful WSAStartup not yet performed"),
  E(WSAEDISCON, "Graceful shutdown now in progress"),
#ifdef WSATYPE_NOT_FOUND
  E(WSATYPE_NOT_FOUND, "Class type not found"),
#endif
  E(WSAHOST_NOT_FOUND, "Host not found"),
  E(WSATRY_AGAIN, "Nonauthoritative host not found"),
  E(WSANO_RECOVERY, "This is a nonrecoverable error"),
  E(WSANO_DATA, "Valid name, no data record of requested type)"),

  /* There are some more error codes whose numeric values are marked
   * <b>OS dependent</b>. They start with WSA_, apparently for the same
   * reason that practitioners of some craft traditions deliberately
   * introduce imperfections into their baskets and rugs "to allow the
   * evil spirits to escape."  If we catch them, then our binaries
   * might not report consistent results across versions of Windows.
   * Thus, I'm going to let them all fall through.
   */
  { -1, NULL },
};
/** There does not seem to be a strerror equivalent for winsock errors.
 * Naturally, we have to roll our own.
 */
const char *tor_socket_strerror(int e)
{
  int i;
  for (i=0; windows_socket_errors[i].code >= 0; ++i) {
    if (e == windows_socket_errors[i].code)
      return windows_socket_errors[i].msg;
  }
  return strerror(e);
}
#endif

/*
 *    Filesystem operations.
 */

/** Return FN_ERROR if filename can't be read, FN_NOENT if it doesn't
 * exist, FN_FILE if it is a regular file, or FN_DIR if it's a
 * directory. */
file_status_t file_status(const char *fname)
{
  struct stat st;
  if (stat(fname, &st)) {
    if (errno == ENOENT) {
      return FN_NOENT;
    }
    return FN_ERROR;
  }
  if (st.st_mode & S_IFDIR)
    return FN_DIR;
  else if (st.st_mode & S_IFREG)
    return FN_FILE;
  else
    return FN_ERROR;
}

/** Check whether dirname exists and is private.  If yes return 0.  If
 * it does not exist, and create is set, try to create it and return 0
 * on success.  Else return -1. */
int check_private_dir(const char *dirname, int create)
{
  int r;
  struct stat st;
  tor_assert(dirname);
  if (stat(dirname, &st)) {
    if (errno != ENOENT) {
      log(LOG_WARN, "Directory %s cannot be read: %s", dirname,
          strerror(errno));
      return -1;
    }
    if (!create) {
      log(LOG_WARN, "Directory %s does not exist.", dirname);
      return -1;
    }
    log(LOG_INFO, "Creating directory %s", dirname);
#ifdef MS_WINDOWS
    r = mkdir(dirname);
#else
    r = mkdir(dirname, 0700);
#endif
    if (r) {
      log(LOG_WARN, "Error creating directory %s: %s", dirname,
          strerror(errno));
      return -1;
    } else {
      return 0;
    }
  }
  if (!(st.st_mode & S_IFDIR)) {
    log(LOG_WARN, "%s is not a directory", dirname);
    return -1;
  }
#ifndef MS_WINDOWS
  if (st.st_uid != getuid()) {
    log(LOG_WARN, "%s is not owned by this UID (%d). You must fix this to proceed.", dirname, (int)getuid());
    return -1;
  }
  if (st.st_mode & 0077) {
    log(LOG_WARN, "Fixing permissions on directory %s", dirname);
    if (chmod(dirname, 0700)) {
      log(LOG_WARN, "Could not chmod directory %s: %s", dirname,
          strerror(errno));
      return -1;
    } else {
      return 0;
    }
  }
#endif
  return 0;
}

/** Create a file named <b>fname</b> with the contents <b>str</b>.  Overwrite the
 * previous <b>fname</b> if possible.  Return 0 on success, -1 on failure.
 *
 * This function replaces the old file atomically, if possible.
 */
int
write_str_to_file(const char *fname, const char *str, int bin)
{
  char tempname[1024];
  int fd;
  size_t len;
  int result;
  if ((strlcpy(tempname,fname,1024) >= 1024) ||
      (strlcat(tempname,".tmp",1024) >= 1024)) {
    log(LOG_WARN, "Filename %s.tmp too long (>1024 chars)", fname);
    return -1;
  }
  if ((fd = open(tempname, O_WRONLY|O_CREAT|O_TRUNC|(bin?O_BINARY:0), 0600))
      < 0) {
    log(LOG_WARN, "Couldn't open %s for writing: %s", tempname,
        strerror(errno));
    return -1;
  }
  len = strlen(str);
  result = write_all(fd, str, len, 0);
  if(result < 0 || (size_t)result != len) {
    log(LOG_WARN, "Error writing to %s: %s", tempname, strerror(errno));
    close(fd);
    return -1;
  }
  if (close(fd)) {
    log(LOG_WARN,"Error flushing to %s: %s", tempname, strerror(errno));
    return -1;
  }

#ifdef MS_WINDOWS
  /* On Windows, rename doesn't replace.  We could call ReplaceFile, but
   * that's hard, and we can probably sneak by without atomicity. */
  switch (file_status(fname)) {
    case FN_ERROR:
      log(LOG_WARN, "Error replacing %s: %s", fname, strerror(errno));
      return -1;
    case FN_DIR:
      log(LOG_WARN, "Error replacing %s: is directory", fname);
      return -1;
    case FN_FILE:
      if (unlink(fname)) {
        log(LOG_WARN, "Error replacing %s while removing old copy: %s",
            fname, strerror(errno));
        return -1;
      }
      break;
    case FN_NOENT:
      ;
  }
#endif
  if (rename(tempname, fname)) {
    log(LOG_WARN, "Error replacing %s: %s", fname, strerror(errno));
    return -1;
  }
  return 0;
}

/** Read the contents of <b>filename</b> into a newly allocated string; return the
 * string on success or NULL on failure.
 */
char *read_file_to_str(const char *filename, int bin) {
  int fd; /* router file */
  struct stat statbuf;
  char *string;
  int r;

  tor_assert(filename);

  if(stat(filename, &statbuf) < 0) {
    log_fn(LOG_INFO,"Could not stat %s.",filename);
    return NULL;
  }

  fd = open(filename,O_RDONLY|(bin?O_BINARY:0),0);
  if (fd<0) {
    log_fn(LOG_WARN,"Could not open %s.",filename);
    return NULL;
  }

  string = tor_malloc(statbuf.st_size+1);

  r = read_all(fd,string,statbuf.st_size,0);
  if (r<0) {
    log_fn(LOG_WARN,"Error reading from file '%s': %s", filename,
           strerror(errno));
    tor_free(string);
    close(fd);
    return NULL;
  } else if (bin && r != statbuf.st_size) {
    /* If we're in binary mode, then we'd better have an exact match for
     * size.  Otherwise, win32 encoding may throw us off, and that's okay. */
    log_fn(LOG_WARN,"Could read only %d of %ld bytes of file '%s'.",
           r, (long)statbuf.st_size,filename);
    tor_free(string);
    close(fd);
    return NULL;
  }
  close(fd);

  string[statbuf.st_size] = 0; /* null terminate it */
  return string;
}

/** read lines from f (no more than maxlen-1 bytes each) until we
 * get a non-whitespace line. If it isn't of the form "key value"
 * (value can have spaces), return -1.
 * Point *key to the first word in line, point *value * to the second.
 * Put a \0 at the end of key, remove everything at the end of value
 * that is whitespace or comment.
 * Return 1 if success, 0 if no more lines, -1 if error.
 */
int parse_line_from_file(char *line, size_t maxlen, FILE *f, char **key_out, char **value_out) {
  char *s, *key, *end, *value;

try_next_line:
  if(!fgets(line, maxlen, f)) {
    if(feof(f))
      return 0;
    return -1; /* real error */
  }

  if((s = strchr(line,'#'))) /* strip comments */
    *s = 0; /* stop the line there */

  /* remove end whitespace */
  s = strchr(line, 0); /* now we're at the null */
  do {
    *s = 0;
    s--;
  } while (s >= line && isspace((int)*s));

  key = line;
  while(isspace((int)*key))
    key++;
  if(*key == 0)
    goto try_next_line; /* this line has nothing on it */
  end = key;
  while(*end && !isspace((int)*end))
    end++;
  value = end;
  while(*value && isspace((int)*value))
    value++;

#if 0
  if(!*end || !*value) { /* only a key on this line. no value. */
    *end = 0;
    log_fn(LOG_WARN,"Line has keyword '%s' but no value. Failing.",key);
    return -1;
  }
#endif
  *end = 0; /* null it out */

  tor_assert(key);
  tor_assert(value);
  log_fn(LOG_DEBUG,"got keyword '%s', value '%s'", key, value);
  *key_out = key, *value_out = value;
  return 1;
}

/** Expand any homedir prefix on 'filename'; return a newly allocated
 * string. */
char *expand_filename(const char *filename)
{
  tor_assert(filename);
  /* XXXX Should eventually check for ~username/ */
  if (!strncmp(filename,"~/",2)) {
    size_t len;
    const char *home = getenv("HOME");
    char *result;
    if (!home) {
      log_fn(LOG_WARN, "Couldn't find $HOME environment variable while expanding %s", filename);
      return NULL;
    }
    /* minus two characters for ~/, plus one for /, plus one for NUL.
     * Round up to 16 in case we can't do math. */
    len = strlen(home)+strlen(filename)+16;
    result = tor_malloc(len);
    tor_snprintf(result,len,"%s/%s",home,filename+2);
    return result;
  } else {
    return tor_strdup(filename);
  }
}

/**
 * Rename the file 'from' to the file 'to'.  On unix, this is the same as
 * rename(2).  On windows, this removes 'to' first if it already exists.
 * Returns 0 on success.  Returns -1 and sets errno on failure.
 */
int replace_file(const char *from, const char *to)
{
#ifndef MS_WINDOWS
  return rename(from,to);
#else
  switch(file_status(to))
    {
    case FN_NOENT:
      break;
    case FN_FILE:
      if (unlink(to)) return -1;
      break;
    case FN_ERROR:
      return -1;
    case FN_DIR:
      errno = EISDIR;
      return -1;
    }
  return rename(from,to);
#endif
}

/** Return true iff <b>ip</b> (in host order) is an IP reserved to localhost,
 * or reserved for local networks by RFC 1918.
 */
int is_internal_IP(uint32_t ip) {

  if (((ip & 0xff000000) == 0x0a000000) || /*       10/8 */
      ((ip & 0xff000000) == 0x00000000) || /*        0/8 */
      ((ip & 0xff000000) == 0x7f000000) || /*      127/8 */
      ((ip & 0xffff0000) == 0xa9fe0000) || /* 169.254/16 */
      ((ip & 0xfff00000) == 0xac100000) || /*  172.16/12 */
      ((ip & 0xffff0000) == 0xc0a80000))   /* 192.168/16 */
    return 1;
  return 0;
}

/** Return true iff <b>ip</b> (in host order) is judged to be on the
 * same network as us. For now, check if it's an internal IP. For XXX008,
 * also check if it's on the same class C network as our public IP.
 */
int is_local_IP(uint32_t ip) {
  return is_internal_IP(ip);
}

/* Hold the result of our call to <b>uname</b>. */
static char uname_result[256];
/* True iff uname_result is set. */
static int uname_result_is_set = 0;

/* Return a pointer to a description of our platform.
 */
const char *
get_uname(void)
{
#ifdef HAVE_UNAME
  struct utsname u;
#endif
  if (!uname_result_is_set) {
#ifdef HAVE_UNAME
    if (uname(&u) != -1) {
      /* (linux says 0 is success, solaris says 1 is success) */
      tor_snprintf(uname_result, sizeof(uname_result), "%s %s %s",
               u.sysname, u.nodename, u.machine);
    } else
#endif
      {
        strlcpy(uname_result, "Unknown platform", sizeof(uname_result));
      }
    uname_result_is_set = 1;
  }
  return uname_result;
}

#ifndef MS_WINDOWS
/* Based on code contributed by christian grothoff */
static int start_daemon_called = 0;
static int finish_daemon_called = 0;
static int daemon_filedes[2];
/** Start putting the process into daemon mode: fork and drop all resources
 * except standard fds.  The parent process never returns, but stays around
 * until finish_daemon is called.  (Note: it's safe to call this more
 * than once: calls after the first are ignored.)
 */
void start_daemon(const char *desired_cwd)
{
  pid_t pid;

  if (start_daemon_called)
    return;
  start_daemon_called = 1;

  if(!desired_cwd)
    desired_cwd = "/";
   /* Don't hold the wrong FS mounted */
  if (chdir(desired_cwd) < 0) {
    log_fn(LOG_ERR,"chdir to %s failed. Exiting.",desired_cwd);
    exit(1);
  }

  pipe(daemon_filedes);
  pid = fork();
  if (pid < 0) {
    log_fn(LOG_ERR,"fork failed. Exiting.");
    exit(1);
  }
  if (pid) {  /* Parent */
    int ok;
    char c;

    close(daemon_filedes[1]); /* we only read */
    ok = -1;
    while (0 < read(daemon_filedes[0], &c, sizeof(char))) {
      if (c == '.')
        ok = 1;
    }
    fflush(stdout);
    if (ok == 1)
      exit(0);
    else
      exit(1); /* child reported error */
  } else { /* Child */
    close(daemon_filedes[0]); /* we only write */

    pid = setsid(); /* Detach from controlling terminal */
    /*
     * Fork one more time, so the parent (the session group leader) can exit.
     * This means that we, as a non-session group leader, can never regain a
     * controlling terminal.   This part is recommended by Stevens's
     * _Advanced Programming in the Unix Environment_.
     */
    if (fork() != 0) {
      exit(0);
    }
    return;
  }
}

/** Finish putting the process into daemon mode: drop standard fds, and tell
 * the parent process to exit.  (Note: it's safe to call this more than once:
 * calls after the first are ignored.  Calls start_daemon first if it hasn't
 * been called already.)
 */
void finish_daemon(void)
{
  int nullfd;
  char c = '.';
  if (finish_daemon_called)
    return;
  if (!start_daemon_called)
    start_daemon(NULL);
  finish_daemon_called = 1;

  nullfd = open("/dev/null",
                O_CREAT | O_RDWR | O_APPEND);
  if (nullfd < 0) {
    log_fn(LOG_ERR,"/dev/null can't be opened. Exiting.");
    exit(1);
  }
  /* close fds linking to invoking terminal, but
   * close usual incoming fds, but redirect them somewhere
   * useful so the fds don't get reallocated elsewhere.
   */
  if (dup2(nullfd,0) < 0 ||
      dup2(nullfd,1) < 0 ||
      dup2(nullfd,2) < 0) {
    log_fn(LOG_ERR,"dup2 failed. Exiting.");
    exit(1);
  }
  write(daemon_filedes[1], &c, sizeof(char)); /* signal success */
  close(daemon_filedes[1]);
}
#else
/* defined(MS_WINDOWS) */
void start_daemon(const char *cp) {}
void finish_daemon(void) {}
#endif

/** Write the current process ID, followed by NL, into <b>filename</b>.
 */
void write_pidfile(char *filename) {
#ifndef MS_WINDOWS
  FILE *pidfile;

  if ((pidfile = fopen(filename, "w")) == NULL) {
    log_fn(LOG_WARN, "Unable to open %s for writing: %s", filename,
           strerror(errno));
  } else {
    fprintf(pidfile, "%d\n", (int)getpid());
    fclose(pidfile);
  }
#endif
}

/** Call setuid and setgid to run as <b>user</b>:<b>group</b>.  Return 0 on
 * success.  On failure, log and return -1.
 */
int switch_id(char *user, char *group) {
#ifndef MS_WINDOWS
  struct passwd *pw = NULL;
  struct group *gr = NULL;

  if (user) {
    pw = getpwnam(user);
    if (pw == NULL) {
      log_fn(LOG_ERR,"User '%s' not found.", user);
      return -1;
    }
  }

  /* switch the group first, while we still have the privileges to do so */
  if (group) {
    gr = getgrnam(group);
    if (gr == NULL) {
      log_fn(LOG_ERR,"Group '%s' not found.", group);
      return -1;
    }

    if (setgid(gr->gr_gid) != 0) {
      log_fn(LOG_ERR,"Error setting GID: %s", strerror(errno));
      return -1;
    }
  } else if (user) {
    if (setgid(pw->pw_gid) != 0) {
      log_fn(LOG_ERR,"Error setting GID: %s", strerror(errno));
      return -1;
    }
  }

  /* now that the group is switched, we can switch users and lose
     privileges */
  if (user) {
    if (setuid(pw->pw_uid) != 0) {
      log_fn(LOG_ERR,"Error setting UID: %s", strerror(errno));
      return -1;
    }
  }

  return 0;
#endif

  log_fn(LOG_ERR,
         "User or group specified, but switching users is not supported.");

  return -1;
}

/** Set *addr to the IP address (in dotted-quad notation) stored in c.
 * Return 1 on success, 0 if c is badly formatted.  (Like inet_aton(c,addr),
 * but works on Windows and Solaris.)
 */
int tor_inet_aton(const char *c, struct in_addr* addr)
{
#ifdef HAVE_INET_ATON
  return inet_aton(c, addr);
#else
  uint32_t r;
  tor_assert(c);
  tor_assert(addr);
  if (strcmp(c, "255.255.255.255") == 0) {
    addr->s_addr = 0xFFFFFFFFu;
    return 1;
  }
  r = inet_addr(c);
  if (r == INADDR_NONE)
    return 0;
  addr->s_addr = r;
  return 1;
#endif
}

/** Similar behavior to Unix gethostbyname: resolve <b>name</b>, and set
 * *addr to the proper IP address, in network byte order.  Returns 0
 * on success, -1 on failure; 1 on transient failure.
 *
 * (This function exists because standard windows gethostbyname
 * doesn't treat raw IP addresses properly.)
 */
int tor_lookup_hostname(const char *name, uint32_t *addr)
{
  /* Perhaps eventually this should be replaced by a tor_getaddrinfo or
   * something.
   */
  struct in_addr iaddr;
  struct hostent *ent;
  tor_assert(addr);
  if (!*name) {
    /* Empty address is an error. */
    return -1;
  } else if (tor_inet_aton(name, &iaddr)) {
    /* It's an IP. */
    memcpy(addr, &iaddr.s_addr, 4);
    return 0;
  } else {
    ent = gethostbyname(name);
    if (ent) {
      /* break to remind us if we move away from IPv4 */
      tor_assert(ent->h_length == 4);
      memcpy(addr, ent->h_addr, 4);
      return 0;
    }
    memset(addr, 0, 4);
#ifdef MS_WINDOWS
    return (WSAGetLastError() == WSATRY_AGAIN) ? 1 : -1;
#else
    return (h_errno == TRY_AGAIN) ? 1 : -1;
#endif
  }
}

/** Parse a string of the form "host[:port]" from <b>addrport</b>.  If
 * <b>address</b> is provided, set *<b>address</b> to a copy of the
 * host portion of the string.  If <b>addr</b> is provided, try to
 * resolve the host portion of the string and store it into
 * *<b>addr</b> (in host byte order).  If <b>port</b> is provided,
 * store the port number into *<b>port</b>, or 0 if no port is given.
 * Return 0 on success, -1 on failure.
 */
int
parse_addr_port(const char *addrport, char **address, uint32_t *addr,
                uint16_t *port)
{
  const char *colon;
  char *_address = NULL;
  int _port;
  int ok = 1;

  tor_assert(addrport);
  tor_assert(port);

  colon = strchr(addrport, ':');
  if (colon) {
    _address = tor_strndup(addrport, colon-addrport);
    _port = (int) tor_parse_long(colon+1,10,1,65535,NULL,NULL);
    if (!_port) {
      log_fn(LOG_WARN, "Port '%s' out of range", colon+1);
      ok = 0;
    }
  } else {
    _address = tor_strdup(addrport);
    _port = 0;
  }

  if (addr) {
    /* There's an addr pointer, so we need to resolve the hostname. */
    if (tor_lookup_hostname(_address,addr)) {
      log_fn(LOG_WARN, "Couldn't look up '%s'", _address);
      ok = 0;
      *addr = 0;
    }
    *addr = ntohl(*addr);
  }

  if (address && ok) {
    *address = _address;
  } else {
    if (address)
      *address = NULL;
    tor_free(_address);
  }
  if (port)
    *port =  ok ? ((uint16_t) _port) : 0;

  return ok ? 0 : -1;
}

/** Parse a string <b>s</b> in the format of
 * (IP(/mask|/mask-bits)?|*):(*|port(-maxport)?), setting the various
 * *out pointers as appropriate.  Return 0 on success, -1 on failure.
 */
int
parse_addr_and_port_range(const char *s, uint32_t *addr_out,
                          uint32_t *mask_out, uint16_t *port_min_out,
                          uint16_t *port_max_out)
{
  char *address;
  char *mask, *port, *endptr;
  struct in_addr in;
  int bits;

  tor_assert(s);
  tor_assert(addr_out);
  tor_assert(mask_out);
  tor_assert(port_min_out);
  tor_assert(port_max_out);

  address = tor_strdup(s);
  /* Break 'address' into separate strings.
   */
  mask = strchr(address,'/');
  port = strchr(mask?mask:address,':');
  if (mask)
    *mask++ = '\0';
  if (port)
    *port++ = '\0';
  /* Now "address" is the IP|'*' part...
   *     "mask" is the Mask|Maskbits part...
   * and "port" is the *|port|min-max part.
   */

  if (strcmp(address,"*")==0) {
    *addr_out = 0;
  } else if (tor_inet_aton(address, &in) != 0) {
    *addr_out = ntohl(in.s_addr);
  } else {
    log_fn(LOG_WARN, "Malformed IP %s in address pattern; rejecting.",address);
    goto err;
  }

  if (!mask) {
    if (strcmp(address,"*")==0)
      *mask_out = 0;
    else
      *mask_out = 0xFFFFFFFFu;
  } else {
    endptr = NULL;
    bits = (int) strtol(mask, &endptr, 10);
    if (!*endptr) {
      /* strtol handled the whole mask. */
      if (bits < 0 || bits > 32) {
        log_fn(LOG_WARN, "Bad number of mask bits on address range; rejecting.");
        goto err;
      }
      *mask_out = ~((1<<(32-bits))-1);
    } else if (tor_inet_aton(mask, &in) != 0) {
      *mask_out = ntohl(in.s_addr);
    } else {
      log_fn(LOG_WARN, "Malformed mask %s on address range; rejecting.",
             mask);
      goto err;
    }
  }

  if (!port || strcmp(port, "*") == 0) {
    *port_min_out = 1;
    *port_max_out = 65535;
  } else {
    endptr = NULL;
    *port_min_out =  (uint16_t) tor_parse_long(port, 10, 1, 65535,
                                               NULL, &endptr);
    if (*endptr == '-') {
      port = endptr+1;
      endptr = NULL;
      *port_max_out = (uint16_t) tor_parse_long(port, 10, 1, 65535, NULL,
                                                &endptr);
      if (*endptr || !*port_max_out) {
      log_fn(LOG_WARN, "Malformed port %s on address range rejecting.",
             port);
      }
    } else if (*endptr || !*port_min_out) {
      log_fn(LOG_WARN, "Malformed port %s on address range; rejecting.",
             port);
      goto err;
    } else {
      *port_max_out = *port_min_out;
    }
    if (*port_min_out > *port_max_out) {
      log_fn(LOG_WARN,"Insane port range on address policy; rejecting.");
      goto err;
    }
  }

  tor_free(address);
  return 0;
 err:
  tor_free(address);
  return -1;
}


/** Extract a long from the start of s, in the given numeric base.  If
 * there is unconverted data and next is provided, set *next to the
 * first unconverted character.  An error has occurred if no characters
 * are converted; or if there are unconverted characters and next is NULL; or
 * if the parsed value is not between min and max.  When no error occurs,
 * return the parsed value and set *ok (if provided) to 1.  When an error
 * ocurs, return 0 and set *ok (if provided) to 0.
 */
long
tor_parse_long(const char *s, int base, long min, long max,
               int *ok, char **next)
{
  char *endptr;
  long r;

  r = strtol(s, &endptr, base);
  /* Was at least one character converted? */
  if (endptr == s)
    goto err;
  /* Were there unexpected unconverted characters? */
  if (!next && *endptr)
    goto err;
  /* Is r within limits? */
  if (r < min || r > max)
    goto err;

  if (ok) *ok = 1;
  if (next) *next = endptr;
  return r;
 err:
  if (ok) *ok = 0;
  if (next) *next = endptr;
  return 0;
}

unsigned long
tor_parse_ulong(const char *s, int base, unsigned long min,
                unsigned long max, int *ok, char **next)
{
  char *endptr;
  unsigned long r;

  r = strtol(s, &endptr, base);
  /* Was at least one character converted? */
  if (endptr == s)
    goto err;
  /* Were there unexpected unconverted characters? */
  if (!next && *endptr)
    goto err;
  /* Is r within limits? */
  if (r < min || r > max)
    goto err;

  if (ok) *ok = 1;
  if (next) *next = endptr;
  return r;
 err:
  if (ok) *ok = 0;
  if (next) *next = endptr;
  return 0;
}

/** Replacement for snprintf.  Differs from platform snprintf in two
 * ways: First, always NUL-terminates its output.  Second, always
 * returns -1 if the result is truncated.  (Note that this return
 * behavior does <i>not</i> conform to C99; it just happens to be the
 * easiest to emulate "return -1" with conformant implementations than
 * it is to emulate "return number that would be written" with
 * non-conformant implementations.) */
int tor_snprintf(char *str, size_t size, const char *format, ...)
{
  va_list ap;
  int r;
  va_start(ap,format);
  r = tor_vsnprintf(str,size,format,ap);
  va_end(ap);
  return r;
}

/** Replacement for vsnpritnf; behavior differs as tor_snprintf differs from
 * snprintf.
 */
int tor_vsnprintf(char *str, size_t size, const char *format, va_list args)
{
  int r;
#ifdef MS_WINDOWS
  r = _vsnprintf(str, size, format, args);
#else
  r = vsnprintf(str, size, format, args);
#endif
  str[size-1] = '\0';
  if (r < 0 || ((size_t)r) >= size)
    return -1;
  return r;
}


#ifndef MS_WINDOWS
struct tor_mutex_t {
};
tor_mutex_t *tor_mutex_new(void) { return NULL; }
void tor_mutex_acquire(tor_mutex_t *m) { }
void tor_mutex_release(tor_mutex_t *m) { }
void tor_mutex_free(tor_mutex_t *m) { }
#else
struct tor_mutex_t {
  HANDLE handle;
};
tor_mutex_t *tor_mutex_new(void)
{
  tor_mutex_t *m;
  m = tor_malloc_zero(sizeof(tor_mutex_t));
  m->handle = CreateMutex(NULL, FALSE, NULL);
  tor_assert(m->handle != NULL);
  return m;
}
void tor_mutex_free(tor_mutex_t *m)
{
  CloseHandle(m->handle);
  tor_free(m);
}
void tor_mutex_acquire(tor_mutex_t *m)
{
  DWORD r;
  r = WaitForSingleObject(m->handle, INFINITE);
  switch (r) {
    case WAIT_ABANDONED: /* holding thread exited. */
        case WAIT_OBJECT_0: /* we got the mutex normally. */
      break;
    case WAIT_TIMEOUT: /* Should never happen. */
          tor_assert(0);
      break;
        case WAIT_FAILED:
      log_fn(LOG_WARN, "Failed to acquire mutex: %d", GetLastError());
  }
}
void tor_mutex_release(tor_mutex_t *m)
{
  BOOL r;
  r = ReleaseMutex(m->handle);
  if (!r) {
    log_fn(LOG_WARN, "Failed to release mutex: %d", GetLastError());
  }
}

#endif

void base16_encode(char *dest, size_t destlen, const char *src, size_t srclen)
{
  const char *end;
  char *cp;

  tor_assert(destlen >= srclen*2+1);

  cp = dest;
  end = src+srclen;
  while (src<end) {
    sprintf(cp,"%02X",*(const uint8_t*)src);
    ++src;
    cp += 2;
  }
  *cp = '\0';
}

static const char HEX_DIGITS[] = "0123456789ABCDEFabcdef";

static INLINE int hex_decode_digit(char c)
{
  const char *cp;
  int n;
  cp = strchr(HEX_DIGITS, c);
  if (!cp)
    return -1;
  n = cp-HEX_DIGITS;
  if (n<=15)
    return n; /* digit or uppercase */
  else
    return n-6; /* lowercase */
}

int base16_decode(char *dest, size_t destlen, const char *src, size_t srclen)
{
  const char *end;
  int v1,v2;
  if ((srclen % 2) != 0)
    return -1;
  if (destlen < srclen/2)
    return -1;
  end = src+srclen;
  while (src<end) {
    v1 = hex_decode_digit(*src);
    v2 = hex_decode_digit(*(src+1));
    if(v1<0||v2<0)
      return -1;
    *(uint8_t*)dest = (v1<<4)|v2;
    ++dest;
    src+=2;
  }
  return 0;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
