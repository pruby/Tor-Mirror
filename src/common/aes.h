/* Copyright 2003 Roger Dingledine */
/* See LICENSE for licensing information */
/* $Id$ */

/* Implements a minimal interface to counter-mode AES. */

#ifndef __AES_H
#define __AES_H

#include "../common/torint.h"

struct aes_cnt_cipher;
typedef struct aes_cnt_cipher aes_cnt_cipher_t;

aes_cnt_cipher_t* aes_new_cipher();
void aes_free_cipher(aes_cnt_cipher_t *cipher);
void aes_set_key(aes_cnt_cipher_t *cipher, const unsigned char *key, int key_bits);
void aes_crypt(aes_cnt_cipher_t *cipher, const char *input, int len, char *output);
uint64_t aes_get_counter(aes_cnt_cipher_t *cipher);
void aes_set_counter(aes_cnt_cipher_t *cipher, uint64_t counter);
void aes_adjust_counter(aes_cnt_cipher_t *cipher, long delta);

#endif
