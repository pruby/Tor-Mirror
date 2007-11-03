/* Copyright 2003-2004 Roger Dingledine
 * Copyright 2004-2007 Roger Dingledine, Nick Mathewson */
/* See LICENSE for licensing information */
/* $Id$ */

#ifndef __CONTAINER_H
#define __CONTAINER_H
#define CONTAINER_H_ID \
  "$Id$"

#include "util.h"

/** A resizeable list of pointers, with associated helpful functionality.
 *
 * The members of this struct are exposed only so that macros and inlines can
 * use them; all access to smartlist internals should go throuch the functions
 * and macros defined here.
 **/
typedef struct smartlist_t {
  /** <b>list</b> has enough capacity to store exactly <b>capacity</b> elements
   * before it needs to be resized.  Only the first <b>num_used</b> (\<=
   * capacity) elements point to valid data.
   */
  void **list;
  int num_used;
  int capacity;
} smartlist_t;

smartlist_t *smartlist_create(void);
void smartlist_free(smartlist_t *sl);
void smartlist_set_capacity(smartlist_t *sl, int n);
void smartlist_clear(smartlist_t *sl);
void smartlist_add(smartlist_t *sl, void *element);
void smartlist_add_all(smartlist_t *sl, const smartlist_t *s2);
void smartlist_remove(smartlist_t *sl, const void *element);
void *smartlist_pop_last(smartlist_t *sl);
void smartlist_reverse(smartlist_t *sl);
void smartlist_string_remove(smartlist_t *sl, const char *element);
int smartlist_isin(const smartlist_t *sl, const void *element) ATTR_PURE;
int smartlist_string_isin(const smartlist_t *sl, const char *element)
  ATTR_PURE;
int smartlist_string_pos(const smartlist_t *, const char *elt) ATTR_PURE;
int smartlist_string_isin_case(const smartlist_t *sl, const char *element)
  ATTR_PURE;
int smartlist_string_num_isin(const smartlist_t *sl, int num) ATTR_PURE;
int smartlist_digest_isin(const smartlist_t *sl, const char *element)
  ATTR_PURE;
int smartlist_overlap(const smartlist_t *sl1, const smartlist_t *sl2)
  ATTR_PURE;
void smartlist_intersect(smartlist_t *sl1, const smartlist_t *sl2);
void smartlist_subtract(smartlist_t *sl1, const smartlist_t *sl2);

/* smartlist_choose() is defined in crypto.[ch] */
#ifdef DEBUG_SMARTLIST
/** Return the number of items in sl.
 */
static INLINE int smartlist_len(const smartlist_t *sl) ATTR_PURE;
static INLINE int smartlist_len(const smartlist_t *sl) {
  tor_assert(sl);
  return (sl)->num_used;
}
/** Return the <b>idx</b>th element of sl.
 */
static INLINE void *smartlist_get(const smartlist_t *sl, int idx) ATTR_PURE;
static INLINE void *smartlist_get(const smartlist_t *sl, int idx) {
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(sl->num_used > idx);
  return sl->list[idx];
}
static INLINE void smartlist_set(smartlist_t *sl, int idx, void *val) {
  tor_assert(sl);
  tor_assert(idx>=0);
  tor_assert(sl->num_used > idx);
  sl->list[idx] = val;
}
#else
#define smartlist_len(sl) ((sl)->num_used)
#define smartlist_get(sl, idx) ((sl)->list[idx])
#define smartlist_set(sl, idx, val) ((sl)->list[idx] = (val))
#endif

/** Exchange the elements at indices <b>idx1</b> and <b>idx2</b> of the
 * smartlist <b>sl</b>. */
static INLINE void smartlist_swap(smartlist_t *sl, int idx1, int idx2)
{
  if (idx1 != idx2) {
    void *elt = smartlist_get(sl, idx1);
    smartlist_set(sl, idx1, smartlist_get(sl, idx2));
    smartlist_set(sl, idx2, elt);
  }
}

void smartlist_del(smartlist_t *sl, int idx);
void smartlist_del_keeporder(smartlist_t *sl, int idx);
void smartlist_insert(smartlist_t *sl, int idx, void *val);
void smartlist_sort(smartlist_t *sl,
                    int (*compare)(const void **a, const void **b));
void smartlist_uniq(smartlist_t *sl,
                    int (*compare)(const void **a, const void **b),
                    void (*free_fn)(void *elt));
void smartlist_sort_strings(smartlist_t *sl);
void smartlist_sort_digests(smartlist_t *sl);
void smartlist_uniq_strings(smartlist_t *sl);
void smartlist_uniq_digests(smartlist_t *sl);
void *smartlist_bsearch(smartlist_t *sl, const void *key,
                        int (*compare)(const void *key, const void **member))
 ATTR_PURE;
int smartlist_bsearch_idx(const smartlist_t *sl, const void *key,
                          int (*compare)(const void *key, const void **member),
                          int *found_out)
 ATTR_PURE;

void smartlist_pqueue_add(smartlist_t *sl,
                          int (*compare)(const void *a, const void *b),
                          void *item);
void *smartlist_pqueue_pop(smartlist_t *sl,
                           int (*compare)(const void *a, const void *b));
void smartlist_pqueue_assert_ok(smartlist_t *sl,
                                int (*compare)(const void *a, const void *b));

#define SPLIT_SKIP_SPACE   0x01
#define SPLIT_IGNORE_BLANK 0x02
int smartlist_split_string(smartlist_t *sl, const char *str, const char *sep,
                           int flags, int max);
char *smartlist_join_strings(smartlist_t *sl, const char *join, int terminate,
                             size_t *len_out) ATTR_MALLOC;
char *smartlist_join_strings2(smartlist_t *sl, const char *join,
                              size_t join_len, int terminate, size_t *len_out)
  ATTR_MALLOC;

/** Iterate over the items in a smartlist <b>sl</b>, in order.  For each item,
 * assign it to a new local variable of type <b>type</b> named <b>var</b>, and
 * execute the statement <b>cmd</b>.  Inside the loop, the loop index can
 * be accessed as <b>var</b>_sl_idx and the length of the list can be accessed
 * as <b>var</b>_sl_len.
 *
 * NOTE: Do not change the length of the list while the loop is in progress,
 * unless you adjust the _sl_len variable correspondingly.  See second example
 * below.
 *
 * Example use:
 * <pre>
 *   smartlist_t *list = smartlist_split("A:B:C", ":", 0, 0);
 *   SMARTLIST_FOREACH(list, char *, cp,
 *   {
 *     printf("%d: %s\n", cp_sl_idx, cp);
 *     tor_free(cp);
 *   });
 *   smartlist_free(list);
 * </pre>
 *
 * Example use (advanced):
 * <pre>
 *   SMARTLIST_FOREACH(list, char *, cp,
 *   {
 *     if (!strcmp(cp, "junk")) {
 *       smartlist_del(list, cp_sl_idx);
 *       tor_free(cp);
 *       --cp_sl_len; // decrement length of list so we don't run off the end
 *       --cp_sl_idx; // decrement idx so we consider the item that moved here
 *     }
 *   });
 * </pre>
 */
#define SMARTLIST_FOREACH(sl, type, var, cmd)                   \
  STMT_BEGIN                                                    \
    int var ## _sl_idx, var ## _sl_len=(sl)->num_used;          \
    type var;                                                   \
    for (var ## _sl_idx = 0; var ## _sl_idx < var ## _sl_len;   \
         ++var ## _sl_idx) {                                    \
      var = (sl)->list[var ## _sl_idx];                         \
      cmd;                                                      \
    } STMT_END

/** Helper: While in a SMARTLIST_FOREACH loop over the list <b>sl</b> indexed
 * with the variable <b>var</b>, remove the current element in a way that
 * won't confuse the loop. */
#define SMARTLIST_DEL_CURRENT(sl, var)          \
  STMT_BEGIN                                    \
    smartlist_del(sl, var ## _sl_idx);          \
    --var ## _sl_idx;                           \
    --var ## _sl_len;                           \
  STMT_END

#define DECLARE_MAP_FNS(maptype, keytype, prefix)                       \
  typedef struct maptype maptype;                                       \
  typedef struct prefix##entry_t *prefix##iter_t;                       \
  maptype* prefix##new(void);                                           \
  void* prefix##set(maptype *map, keytype key, void *val);              \
  void* prefix##get(const maptype *map, keytype key);                   \
  void* prefix##remove(maptype *map, keytype key);                      \
  void prefix##free(maptype *map, void (*free_val)(void*));             \
  int prefix##isempty(const maptype *map);                              \
  int prefix##size(const maptype *map);                                 \
  prefix##iter_t *prefix##iter_init(maptype *map);                      \
  prefix##iter_t *prefix##iter_next(maptype *map, prefix##iter_t *iter); \
  prefix##iter_t *prefix##iter_next_rmv(maptype *map, prefix##iter_t *iter); \
  void prefix##iter_get(prefix##iter_t *iter, keytype *keyp, void **valp); \
  int prefix##iter_done(prefix##iter_t *iter);                          \
  void prefix##assert_ok(const maptype *map)

/* Map from const char * to void *. Implemented with a hash table. */
DECLARE_MAP_FNS(strmap_t, const char *, strmap_);
DECLARE_MAP_FNS(digestmap_t, const char *, digestmap_);
#undef DECLARE_MAP_FNS

void* strmap_set_lc(strmap_t *map, const char *key, void *val);
void* strmap_get_lc(const strmap_t *map, const char *key);
void* strmap_remove_lc(strmap_t *map, const char *key);

#define DECLARE_TYPED_DIGESTMAP_FNS(prefix, maptype, valtype)           \
  typedef struct maptype maptype;                                       \
  typedef struct prefix##iter_t prefix##iter_t;                         \
  static INLINE maptype* prefix##new(void)                              \
  {                                                                     \
    return (maptype*)digestmap_new();                                   \
  }                                                                     \
  static INLINE valtype* prefix##get(maptype *map, const char *key)     \
  {                                                                     \
    return (valtype*)digestmap_get((digestmap_t*)map, key);             \
  }                                                                     \
  static INLINE valtype* prefix##set(maptype *map, const char *key,     \
                                     valtype *val)                      \
  {                                                                     \
    return (valtype*)digestmap_set((digestmap_t*)map, key, val);        \
  }                                                                     \
  static INLINE valtype* prefix##remove(maptype *map, const char *key)  \
  {                                                                     \
    return (valtype*)digestmap_remove((digestmap_t*)map, key);          \
  }                                                                     \
  static INLINE void prefix##free(maptype *map, void (*free_val)(void*)) \
  {                                                                     \
    digestmap_free((digestmap_t*)map, free_val);                        \
  }                                                                     \
  static INLINE int prefix##isempty(maptype *map)                       \
  {                                                                     \
    return digestmap_isempty((digestmap_t*)map);                        \
  }                                                                     \
  static INLINE int prefix##size(maptype *map)                          \
  {                                                                     \
    return digestmap_size((digestmap_t*)map);                           \
  }                                                                     \
  static INLINE prefix##iter_t *prefix##iter_init(maptype *map)         \
  {                                                                     \
    return (prefix##iter_t*) digestmap_iter_init((digestmap_t*)map);    \
  }                                                                     \
  static INLINE prefix##iter_t *prefix##iter_next(maptype *map,         \
                                                  prefix##iter_t *iter) \
  {                                                                     \
    return (prefix##iter_t*) digestmap_iter_next(                       \
                       (digestmap_t*)map, (digestmap_iter_t*)iter);     \
  }                                                                     \
  static INLINE prefix##iter_t *prefix##iter_next_rmv(maptype *map,     \
                                                  prefix##iter_t *iter) \
  {                                                                     \
    return (prefix##iter_t*) digestmap_iter_next_rmv(                   \
                       (digestmap_t*)map, (digestmap_iter_t*)iter);     \
  }                                                                     \
  static INLINE void prefix##iter_get(prefix##iter_t *iter,             \
                                      const char **keyp,                \
                                      valtype **valp)                   \
  {                                                                     \
    void *v;                                                            \
    digestmap_iter_get((digestmap_iter_t*) iter, keyp, &v);             \
    *valp = v;                                                          \
  }                                                                     \
  static INLINE int prefix##iter_done(prefix##iter_t *iter)             \
  {                                                                     \
    return digestmap_iter_done((digestmap_iter_t*)iter);                \
  }

#if SIZEOF_INT == 4
#define BITARRAY_SHIFT 5
#elif SIZEOF_INT == 8
#define BITARRAY_SHIFT 6
#else
#error "int is neither 4 nor 8 bytes. I can't deal with that."
#endif
#define BITARRAY_MASK ((1u<<BITARRAY_SHIFT)-1)

/** A random-access array of one-bit-wide elements. */
typedef unsigned int bitarray_t;
/** Create a new bit array that can hold <b>n_bits</b> bits. */
static INLINE bitarray_t *
bitarray_init_zero(int n_bits)
{
  size_t sz = (n_bits+BITARRAY_MASK) / (1u << BITARRAY_SHIFT);
  return tor_malloc_zero(sz*sizeof(unsigned int));
}
/** Free the bit array <b>ba</b>. */
static INLINE void
bitarray_free(bitarray_t *ba)
{
  tor_free(ba);
}
/** Set the <b>bit</b>th bit in <b>b</b> to 1. */
static INLINE void
bitarray_set(bitarray_t *b, int bit)
{
  b[bit >> BITARRAY_SHIFT] |= (1u << (bit & BITARRAY_MASK));
}
/** Set the <b>bit</b>th bit in <b>b</b> to 0. */
static INLINE void
bitarray_clear(bitarray_t *b, int bit)
{
  b[bit >> BITARRAY_SHIFT] &= ~ (1u << (bit & BITARRAY_MASK));
}
/** Return true iff <b>bit</b>th bit in <b>b</b> is nonzero.  NOTE: does
 * not necessarily return 1 on true. */
static INLINE unsigned int
bitarray_is_set(bitarray_t *b, int bit)
{
  return b[bit >> BITARRAY_SHIFT] & (1u << (bit & BITARRAY_MASK));
}

/* These functions, given an <b>array</b> of <b>n_elements</b>, return the
 * <b>nth</b> lowest element. <b>nth</b>=0 gives the lowest element;
 * <b>n_elements</b>-1 gives the highest; and (<b>n_elements</b>-1) / 2 gives
 * the median.  As a side effect, the elements of <b>array</b> are sorted. */
int find_nth_int(int *array, int n_elements, int nth);
time_t find_nth_time(time_t *array, int n_elements, int nth);
double find_nth_double(double *array, int n_elements, int nth);
uint32_t find_nth_uint32(uint32_t *array, int n_elements, int nth);
static INLINE int
median_int(int *array, int n_elements)
{
  return find_nth_int(array, n_elements, (n_elements-1)/2);
}
static INLINE time_t
median_time(time_t *array, int n_elements)
{
  return find_nth_time(array, n_elements, (n_elements-1)/2);
}
static INLINE double
median_double(double *array, int n_elements)
{
  return find_nth_double(array, n_elements, (n_elements-1)/2);
}
static INLINE uint32_t
median_uint32(uint32_t *array, int n_elements)
{
  return find_nth_uint32(array, n_elements, (n_elements-1)/2);
}

#endif

