/* Copyright 2003-2004 Roger Dingledine; Copyright 2004 Nick Mathewson */
/* See LICENSE for licensing information */
/* $Id$ */

#ifndef __CONTAINER_H
#define __CONTAINER_H

/** Generic resizeable array. */
typedef struct smartlist_t smartlist_t;

smartlist_t *smartlist_create(void);
void smartlist_free(smartlist_t *sl);
void smartlist_set_capacity(smartlist_t *sl, int n);
void smartlist_clear(smartlist_t *sl);
void smartlist_truncate(smartlist_t *sl, int n);
void smartlist_add(smartlist_t *sl, void *element);
void smartlist_add_all(smartlist_t *sl, const smartlist_t *s2);
void smartlist_remove(smartlist_t *sl, void *element);
int smartlist_isin(const smartlist_t *sl, void *element);
int smartlist_string_isin(const smartlist_t *sl, const char *element);
int smartlist_overlap(const smartlist_t *sl1, const smartlist_t *sl2);
void smartlist_intersect(smartlist_t *sl1, const smartlist_t *sl2);
void smartlist_subtract(smartlist_t *sl1, const smartlist_t *sl2);
/* smartlist_choose() is defined in crypto.[ch] */
void *smartlist_get(const smartlist_t *sl, int idx);
void *smartlist_set(smartlist_t *sl, int idx, void *val);
void *smartlist_del(smartlist_t *sl, int idx);
void *smartlist_del_keeporder(smartlist_t *sl, int idx);
void smartlist_insert(smartlist_t *sl, int idx, void *val);
int smartlist_len(const smartlist_t *sl);
#define SPLIT_SKIP_SPACE   0x01
#define SPLIT_IGNORE_BLANK 0x02
int smartlist_split_string(smartlist_t *sl, const char *str, const char *sep,
                           int flags, int max);
char *smartlist_join_strings(smartlist_t *sl, const char *join, int terminate);

#define SMARTLIST_FOREACH(sl, type, var, cmd)                   \
  do {                                                          \
    int var ## _sl_idx, var ## _sl_len=smartlist_len(sl);       \
    type var;                                                   \
    for(var ## _sl_idx = 0; var ## _sl_idx < var ## _sl_len;    \
        ++var ## _sl_idx) {                                     \
      var = smartlist_get((sl),var ## _sl_idx);                 \
      cmd;                                                      \
    } } while (0)

/* Map from const char * to void*. Implemented with a splay tree. */
typedef struct strmap_t strmap_t;
typedef struct strmap_entry_t strmap_entry_t;
typedef struct strmap_entry_t strmap_iter_t;
strmap_t* strmap_new(void);
void* strmap_set(strmap_t *map, const char *key, void *val);
void* strmap_get(strmap_t *map, const char *key);
void* strmap_remove(strmap_t *map, const char *key);
void* strmap_set_lc(strmap_t *map, const char *key, void *val);
void* strmap_get_lc(strmap_t *map, const char *key);
void* strmap_remove_lc(strmap_t *map, const char *key);
typedef void* (*strmap_foreach_fn)(const char *key, void *val, void *data);
void strmap_foreach(strmap_t *map, strmap_foreach_fn fn, void *data);
void strmap_free(strmap_t *map, void (*free_val)(void*));
int strmap_isempty(strmap_t *map);

strmap_iter_t *strmap_iter_init(strmap_t *map);
strmap_iter_t *strmap_iter_next(strmap_t *map, strmap_iter_t *iter);
strmap_iter_t *strmap_iter_next_rmv(strmap_t *map, strmap_iter_t *iter);
void strmap_iter_get(strmap_iter_t *iter, const char **keyp, void **valp);

int strmap_iter_done(strmap_iter_t *iter);

#endif
/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/

