/* Copyright 2001-2004 Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2008, The Tor Project, Inc. */
/* See LICENSE for licensing information */
/* $Id$ */
const char tor_main_c_id[] =
  "$Id$";

/* DOCDOC tor_svn_revision */
const char tor_svn_revision[] =
#ifndef _MSC_VER
#include "micro-revision.i"
#endif
  "";

/**
 * \file tor_main.c
 * \brief Stub module containing a main() function. Allows unit
 * test binary to link against main.c.
 **/

int tor_main(int argc, char *argv[]);

/** We keep main() in a separate file so that our unit tests can use
 * functions from main.c)
 */
int
main(int argc, char *argv[])
{
  return tor_main(argc, argv);
}

