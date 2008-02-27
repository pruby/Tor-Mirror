/* Copyright (c) 2001-2003, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2008, The Tor Project, Inc. */
/* See LICENSE for licensing information */
/* $Id$ */

#ifndef __TEST_H
#define __TEST_H
#define TEST_H_ID "$Id$"

/**
 * \file test.h
 * \brief Macros used by unit tests.
 */

#include "compat.h"

#ifdef __GNUC__
#define PRETTY_FUNCTION __PRETTY_FUNCTION__
#else
#define PRETTY_FUNCTION ""
#endif

extern int have_failed;

#define test_fail_msg(msg)                                      \
  STMT_BEGIN                                                    \
    have_failed = 1;                                            \
    printf("\nFile %s: line %d (%s): %s",                       \
      _SHORT_FILE_,                                             \
      __LINE__,                                                 \
      PRETTY_FUNCTION,                                          \
      msg);                                                     \
    return;                                                     \
  STMT_END

#define test_fail() test_fail_msg("Assertion failed.")

#define test_assert(expr)                                       \
  STMT_BEGIN                                                    \
  if (expr) { printf("."); fflush(stdout); } else {             \
    have_failed = 1;                                            \
    printf("\nFile %s: line %d (%s): assertion failed: (%s)\n", \
      _SHORT_FILE_,                                             \
      __LINE__,                                                 \
      PRETTY_FUNCTION,                                          \
      #expr);                                                   \
    return;                                                     \
  } STMT_END

#define test_eq_type(tp, fmt, expr1, expr2) \
  STMT_BEGIN                                                            \
  tp _test_v1=(tp)(expr1);                                              \
  tp _test_v2=(tp)(expr2);                                              \
  if (_test_v1==_test_v2) { printf("."); fflush(stdout); } else {       \
    have_failed = 1;                                                    \
    printf("\nFile %s: line %d (%s): Assertion failed: (%s==%s)\n"      \
           "      "fmt "!="fmt"\n",                                     \
             _SHORT_FILE_,                                              \
             __LINE__,                                                  \
             PRETTY_FUNCTION,                                           \
             #expr1, #expr2,                                            \
           _test_v1, _test_v2);                                         \
    return;                                                             \
  } STMT_END

#define test_eq(expr1, expr2)                   \
  test_eq_type(long, "%ld", expr1, expr2)

#define test_eq_ptr(expr1, expr2)               \
  test_eq_type(void*, "%p", expr1, expr2)

#define test_neq_type(tp, fmt, expr1, expr2)                            \
  STMT_BEGIN                                                            \
  tp _test_v1=(tp)(expr1);                                              \
  tp _test_v2=(tp)(expr2);                                              \
  if (_test_v1!=_test_v2) { printf("."); fflush(stdout); } else {       \
    have_failed = 1;                                                    \
    printf("\nFile %s: line %d (%s): Assertion failed: (%s!=%s)\n"      \
           "      ("fmt" == "fmt")\n",                                  \
           _SHORT_FILE_,                                                \
           __LINE__,                                                    \
           PRETTY_FUNCTION,                                             \
           #expr1, #expr2,                                              \
           _test_v1, _test_v2);                                         \
    return;                                                             \
  } STMT_END

#define test_neq(expr1, expr2)                  \
  test_neq_type(long, "%ld", expr1, expr2)

#define test_neq_ptr(expr1, expr2)              \
  test_neq_type(void *, "%p", expr1, expr2)

#define test_streq(expr1, expr2)                                \
  STMT_BEGIN                                                    \
    const char *_test_v1=(expr1), *_test_v2=(expr2);                        \
    if (!strcmp(_test_v1,_test_v2)) { printf("."); fflush(stdout); } else { \
    have_failed = 1;                                            \
    printf("\nFile %s: line %d (%s): Assertion failed: (%s==%s)\n"\
           "      (\"%s\" != \"%s\")\n",                        \
      _SHORT_FILE_,                                             \
      __LINE__,                                                 \
      PRETTY_FUNCTION,                                          \
      #expr1, #expr2,                                           \
      _test_v1, _test_v2);                                      \
    return;                                                     \
  } STMT_END

#define test_strneq(expr1, expr2)                               \
  STMT_BEGIN                                                    \
    const char *_test_v1=(expr1), *_test_v2=(expr2);                        \
    if (strcmp(_test_v1,_test_v2)) { printf("."); fflush(stdout); } else {  \
    have_failed = 1;                                            \
    printf("\nFile %s: line %d (%s): Assertion failed: (%s!=%s)\n"\
           "      (\"%s\" == \"%s\")\n",                        \
      _SHORT_FILE_,                                             \
      __LINE__,                                                 \
      PRETTY_FUNCTION,                                          \
      #expr1, #expr2,                                           \
      _test_v1, _test_v2);                                      \
    return;                                                     \
  } STMT_END

#define test_memeq(expr1, expr2, len)                           \
  STMT_BEGIN                                                    \
    const void *_test_v1=(expr1), *_test_v2=(expr2);            \
    char *mem1, *mem2;                                          \
    if (!memcmp(_test_v1,_test_v2,(len))) {                     \
      printf("."); fflush(stdout); } else {                     \
    have_failed = 1;                                            \
    mem1 = tor_malloc(len*2+1);                                    \
    mem2 = tor_malloc(len*2+1);                                    \
    base16_encode(mem1, len*2+1, _test_v1, len);                   \
    base16_encode(mem2, len*2+1, _test_v2, len);                   \
    printf("\nFile %s: line %d (%s): Assertion failed: (%s==%s)\n" \
           "      %s != %s\n",                                     \
      _SHORT_FILE_,                                             \
      __LINE__,                                                 \
      PRETTY_FUNCTION,                                          \
      #expr1, #expr2, mem1, mem2);                              \
    return;                                                     \
  } STMT_END

#define test_memneq(expr1, expr2, len)                          \
  STMT_BEGIN                                                    \
   void *_test_v1=(expr1), *_test_v2=(expr2);                   \
   if (memcmp(_test_v1,_test_v2,(len))) {                       \
     printf("."); fflush(stdout);                               \
   } else {                                                     \
    have_failed = 1;                                            \
    printf("\nFile %s: line %d (%s): Assertion failed: (%s!=%s)\n", \
      _SHORT_FILE_,                                             \
      __LINE__,                                                 \
      PRETTY_FUNCTION,                                          \
      #expr1, #expr2);                                          \
    return;                                                     \
  } STMT_END

#endif

