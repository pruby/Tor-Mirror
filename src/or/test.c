/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include <stdio.h>
#include <fcntl.h>

#include "or.h"
#include "../common/test.h"

void
setup_directory() {
  char buf[256];
  sprintf(buf, "/tmp/tor_test");
  if (mkdir(buf, 0700) && errno != EEXIST)
    fprintf(stderr, "Can't create directory %s", buf);
}

void
test_buffers() {
  char str[256];
  char str2[256];

  char *buf;
  int buflen, buf_datalen;

  char *buf2;
  int buf2len, buf2_datalen;

  int s, i, j, eof;
  z_compression *comp;
  z_decompression *decomp;

  /****
   * buf_new
   ****/
  if (buf_new(&buf, &buflen, &buf_datalen))
    test_fail();

  test_eq(buflen, MAX_BUF_SIZE);
  test_eq(buf_datalen, 0);

  /****
   * read_to_buf
   ****/
  s = open("/tmp/tor_test/data", O_WRONLY|O_CREAT|O_TRUNC, 0600);
  for (j=0;j<256;++j) {
    str[j] = (char)j;
  }
  write(s, str, 256);
  close(s);
  
  s = open("/tmp/tor_test/data", O_RDONLY, 0);
  eof = 0;
  i = read_to_buf(s, 10, &buf, &buflen, &buf_datalen, &eof);
  test_eq(buflen, MAX_BUF_SIZE);
  test_eq(buf_datalen, 10);
  test_eq(eof, 0);
  test_eq(i, 10);
  test_memeq(str, buf, 10);

  /* Test reading 0 bytes. */
  i = read_to_buf(s, 0, &buf, &buflen, &buf_datalen, &eof);
  test_eq(buflen, MAX_BUF_SIZE);
  test_eq(buf_datalen, 10);
  test_eq(eof, 0);
  test_eq(i, 0);

  /* Now test when buffer is filled exactly. */
  buflen = 16;
  i = read_to_buf(s, 6, &buf, &buflen, &buf_datalen, &eof);
  test_eq(buflen, 16);
  test_eq(buf_datalen, 16);
  test_eq(eof, 0);
  test_eq(i, 6);
  test_memeq(str, buf, 16);
  
  /* Now test when buffer is filled with more data to read. */
  buflen = 32;
  i = read_to_buf(s, 128, &buf, &buflen, &buf_datalen, &eof);
  test_eq(buflen, 32);
  test_eq(buf_datalen, 32);
  test_eq(eof, 0);
  test_eq(i, 16);
  test_memeq(str, buf, 32);

  /* Now read to eof. */
  buflen = MAX_BUF_SIZE;
  test_assert(buflen > 256);
  i = read_to_buf(s, 1024, &buf, &buflen, &buf_datalen, &eof);
  test_eq(i, (256-32));
  test_eq(buflen, MAX_BUF_SIZE);
  test_eq(buf_datalen, 256);
  test_memeq(str, buf, 256);
  test_eq(eof, 0);

  i = read_to_buf(s, 1024, &buf, &buflen, &buf_datalen, &eof);
  test_eq(i, 0);
  test_eq(buflen, MAX_BUF_SIZE);
  test_eq(buf_datalen, 256);
  test_eq(eof, 1);

  close(s);

  /**** 
   * find_on_inbuf
   ****/

  test_eq(((int)'d') + 1, find_on_inbuf("abcd", 4, buf, buf_datalen));
  test_eq(-1, find_on_inbuf("xyzzy", 5, buf, buf_datalen));
  /* Make sure we don't look off the end of the buffef */
  buf[256] = 'A';
  buf[257] = 'X';
  test_eq(-1, find_on_inbuf("\xff" "A", 2, buf, buf_datalen));
  test_eq(-1, find_on_inbuf("AX", 2, buf, buf_datalen));
  /* Make sure we use the string length */
  test_eq(((int)'d')+1, find_on_inbuf("abcdX", 4, buf, buf_datalen));

  /****
   * fetch_from_buf
   ****/
  memset(str2, 255, 256);
  test_eq(246, fetch_from_buf(str2, 10, &buf, &buflen, &buf_datalen));
  test_memeq(str2, str, 10);
  test_memeq(str+10,buf,246);
  test_eq(buf_datalen,246);

  test_eq(-1, fetch_from_buf(str2, 247, &buf, &buflen, &buf_datalen));
  test_memeq(str+10,buf,246);
  test_eq(buf_datalen, 246);
  
  test_eq(0, fetch_from_buf(str2, 246, &buf, &buflen, &buf_datalen));
  test_memeq(str2, str+10, 246);
  test_eq(buflen,MAX_BUF_SIZE);
  test_eq(buf_datalen,0);

  /****
   * write_to_buf
   ****/
  memset(buf, (int)'-', 256);
  i = write_to_buf("Hello world", 11, &buf, &buflen, &buf_datalen);
  test_eq(i, 11);
  test_eq(buf_datalen, 11);
  test_memeq(buf, "Hello world", 11);
  i = write_to_buf("XYZZY", 5, &buf, &buflen, &buf_datalen);
  test_eq(i, 16);
  test_eq(buf_datalen, 16);
  test_memeq(buf, "Hello worldXYZZY", 16);
  /* Test when buffer is overfull. */
  buflen = 18;
  test_eq(-1, write_to_buf("This string will not fit.", 25, 
                           &buf, &buflen, &buf_datalen));
  test_eq(buf_datalen, 16);
  test_memeq(buf, "Hello worldXYZZY--", 18);
  buflen = MAX_BUF_SIZE;

  /****
   * flush_buf
   ****/
  /* XXXX Needs tests. */

  
  /***
   * compress_from_buf (simple)
   ***/
  buf_datalen = 0;
  comp = compression_new();
  for (i = 0; i < 20; ++i) {
    write_to_buf("Hello world.  ", 14, &buf, &buflen, &buf_datalen);
  }
  i = compress_from_buf(str, 256, &buf, &buflen, &buf_datalen, comp, 
                        Z_SYNC_FLUSH);
  test_eq(buf_datalen, 0);
  /*
  for (j = 0; j <i ; ++j) {
    printf("%x '%c'\n", ((int) str[j])&0xff, str[j]);
  }
  */
  /* Now try decompressing. */
  decomp = decompression_new();
  if (buf_new(&buf2, &buf2len, &buf2_datalen))
    test_fail();
  buf_datalen = 0;
  test_eq(i, write_to_buf(str, i, &buf, &buflen, &buf_datalen));
  j = decompress_buf_to_buf(&buf, &buflen, &buf_datalen,
                            &buf2, &buf2len, &buf2_datalen,
                            decomp, Z_SYNC_FLUSH);
  test_eq(buf2_datalen, 14*20);
  for (i = 0; i < 20; ++i) {
    test_memeq(buf2+(14*i), "Hello world.  ", 14);
  }
  
  /* Now compress more, into less room. */
  for (i = 0; i < 20; ++i) {
    write_to_buf("Hello wxrlx.  ", 14, &buf, &buflen, &buf_datalen);
  }
  i = compress_from_buf(str, 8, &buf, &buflen, &buf_datalen, comp, 
                        Z_SYNC_FLUSH);
  test_eq(buf_datalen, 0);
  test_eq(i, 8);
  memset(str+8,0,248);
  j = compress_from_buf(str+8, 248, &buf, &buflen, &buf_datalen, comp, 
                        Z_SYNC_FLUSH);
  /* test_eq(j, 2); XXXX This breaks, see below. */ 

  buf2_datalen=buf_datalen=0;
  write_to_buf(str, i+j, &buf, &buflen, &buf_datalen);
  memset(buf2, 0, buf2len);
  j = decompress_buf_to_buf(&buf, &buflen, &buf_datalen,
                            &buf2, &buf2len, &buf2_datalen,
                            decomp, Z_SYNC_FLUSH);
  test_eq(buf2_datalen, 14*20);
  for (i = 0; i < 20; ++i) {
    test_memeq(buf2+(14*i), "Hello wxrlx.  ", 14);
  }

  /* This situation is a bit messy.  We need to refactor our use of
   * zlib until the above code works.  Here's the problem: The zlib
   * documentation claims that we should reinvoke deflate immediately 
   * when the outbuf buffer is full and we get Z_OK, without adjusting
   * the input at all.  This implies that we need to tie a buffer to a
   * compression or decompression object.
   */

  compression_free(comp);
  decompression_free(decomp);

  buf_free(buf);
  buf_free(buf2);
}

void 
test_crypto() {
  crypto_cipher_env_t *env1, *env2;
  crypto_pk_env_t *pk1, *pk2;
  char *data1, *data2, *data3, *cp;
  FILE *f;
  int i, j;
  int str_ciphers[] = { CRYPTO_CIPHER_IDENTITY, 
                        CRYPTO_CIPHER_DES,
                        CRYPTO_CIPHER_RC4,
                        CRYPTO_CIPHER_3DES,
                        -1 };

  data1 = malloc(1024);
  data2 = malloc(1024);
  data3 = malloc(1024);
  test_assert(data1 && data2 && data3);

  /* Try out identity ciphers. */
  env1 = crypto_new_cipher_env(CRYPTO_CIPHER_IDENTITY);
  test_neq(env1, 0);
  test_eq(crypto_cipher_generate_key(env1), 0);
  test_eq(crypto_cipher_set_iv(env1, ""), 0);
  test_eq(crypto_cipher_encrypt_init_cipher(env1), 0);
  for(i = 0; i < 1024; ++i) {
    data1[i] = (char) i*73;
  }
  crypto_cipher_encrypt(env1, data1, 1024, data2); 
  test_memeq(data1, data2, 1024);
  crypto_free_cipher_env(env1);
  
  /* Now, test encryption and decryption with stream ciphers. */
  data1[0]='\0';
  for(i = 1023; i>0; i -= 35)
    strncat(data1, "Now is the time for all good onions", i);
  for(i=0; str_ciphers[i] >= 0; ++i) {
    /* For each cipher... */
    memset(data2, 0, 1024);
    memset(data3, 0, 1024);
    env1 = crypto_new_cipher_env(str_ciphers[i]);
    test_neq(env1, 0);
    env2 = crypto_new_cipher_env(str_ciphers[i]);
    test_neq(env2, 0);
    j = crypto_cipher_generate_key(env1);
    if (str_ciphers[i] != CRYPTO_CIPHER_IDENTITY) {
      crypto_cipher_set_key(env2, env1->key);
    }
    crypto_cipher_set_iv(env1, "12345678901234567890");
    crypto_cipher_set_iv(env2, "12345678901234567890");
    crypto_cipher_encrypt_init_cipher(env1);
    crypto_cipher_decrypt_init_cipher(env2);
    
    /* Try encrypting 512 chars. */
    crypto_cipher_encrypt(env1, data1, 512, data2);
    crypto_cipher_decrypt(env2, data2, 512, data3);
    test_memeq(data1, data3, 512);
    if (str_ciphers[i] != CRYPTO_CIPHER_IDENTITY) {
      test_memneq(data1, data2, 512);
    } else {
      test_memeq(data1, data2, 512);
    }
    /* Now encrypt 1 at a time, and get 1 at a time. */
    for (j = 512; j < 560; ++j) {
      crypto_cipher_encrypt(env1, data1+j, 1, data2+j);
    }
    for (j = 512; j < 560; ++j) {
      crypto_cipher_decrypt(env2, data2+j, 1, data3+j);
    }
    test_memeq(data1, data3, 560);
    /* Now encrypt 3 at a time, and get 5 at a time. */
    for (j = 560; j < 1024; j += 3) {
      crypto_cipher_encrypt(env1, data1+j, 3, data2+j);
    }
    for (j = 560; j < 1024; j += 5) {
      crypto_cipher_decrypt(env2, data2+j, 5, data3+j);
    }
    test_memeq(data1, data3, 1024-4);
    /* Now make sure that when we encrypt with different chunk sizes, we get
       the same results. */
    crypto_free_cipher_env(env2);

    memset(data3, 0, 1024);

    env2 = crypto_new_cipher_env(str_ciphers[i]);
    test_neq(env2, 0);
    if (str_ciphers[i] != CRYPTO_CIPHER_IDENTITY) {
      crypto_cipher_set_key(env2, env1->key);
    }
    crypto_cipher_set_iv(env2, "12345678901234567890");
    crypto_cipher_encrypt_init_cipher(env2);
    for (j = 0; j < 1024; j += 17) {
      crypto_cipher_encrypt(env2, data1+j, 17, data3+j);
    }
    for (j= 0; j < 1024-16; ++j) {
      if (data2[j] != data3[j]) {
        printf("%d:  %d\t%d\n", j, (int) data2[j], (int) data3[j]);
      }
    }
    test_memeq(data2, data3, 1024-16);
    
    crypto_free_cipher_env(env1);
    crypto_free_cipher_env(env2);
  }
  
  /* Test vectors for stream ciphers. */
  /* XXXX Look up some test vectors for the ciphers and make sure we match. */

  /* Test SHA-1 with a test vector from the specification. */
  i = crypto_SHA_digest("abc", 3, data1);
  test_memeq(data1,
             "\xA9\x99\x3E\x36\x47\x06\x81\x6A\xBA\x3E\x25\x71\x78"
             "\x50\xC2\x6C\x9C\xD0\xD8\x9D", 20);

  /* Public-key ciphers */
  pk1 = crypto_new_pk_env(CRYPTO_PK_RSA);
  pk2 = crypto_new_pk_env(CRYPTO_PK_RSA);
  test_assert(pk1 && pk2);
  test_assert(! crypto_pk_generate_key(pk1));
  test_assert(! crypto_pk_write_public_key_to_string(pk1, &cp, &i));
  test_assert(! crypto_pk_read_public_key_from_string(pk2, cp, i));
  test_eq(0, crypto_pk_cmp_keys(pk1, pk2));

  test_eq(128, crypto_pk_keysize(pk1));
  test_eq(128, crypto_pk_keysize(pk2));
  
  test_eq(128, crypto_pk_public_encrypt(pk2, "Hello whirled.", 15, data1,
                                        RSA_PKCS1_OAEP_PADDING));
  test_eq(128, crypto_pk_public_encrypt(pk1, "Hello whirled.", 15, data2,
                                        RSA_PKCS1_OAEP_PADDING));
  /* oaep padding should make encryption not match */
  test_memneq(data1, data2, 128);
  test_eq(15, crypto_pk_private_decrypt(pk1, data1, 128, data3,
                                        RSA_PKCS1_OAEP_PADDING));
  test_streq(data3, "Hello whirled.");
  memset(data3, 0, 1024);
  test_eq(15, crypto_pk_private_decrypt(pk1, data2, 128, data3,
                                        RSA_PKCS1_OAEP_PADDING));
  test_streq(data3, "Hello whirled.");
  /* Can't decrypt with public key. */
  test_eq(-1, crypto_pk_private_decrypt(pk2, data2, 128, data3,
                                        RSA_PKCS1_OAEP_PADDING));
  /* Try again with bad padding */
  memcpy(data2+1, "XYZZY", 5);  /* This has fails ~ once-in-2^40 */
  test_eq(-1, crypto_pk_private_decrypt(pk1, data2, 128, data3,
                                        RSA_PKCS1_OAEP_PADDING));

  /* File operations: save and load private key */
  f = fopen("/tmp/tor_test/pkey1", "wb");
  test_assert(! crypto_pk_write_private_key_to_file(pk1, f));
  fclose(f);
  f = fopen("/tmp/tor_test/pkey1", "rb");
  test_assert(! crypto_pk_read_private_key_from_file(pk2, f));
  fclose(f);
  test_eq(15, crypto_pk_private_decrypt(pk2, data1, 128, data3,
                                        RSA_PKCS1_OAEP_PADDING));
  test_assert(! crypto_pk_read_private_key_from_filename(pk2, 
                                               "/tmp/tor_test/pkey1"));
  test_eq(15, crypto_pk_private_decrypt(pk2, data1, 128, data3,
                                        RSA_PKCS1_OAEP_PADDING));
    

  crypto_free_pk_env(pk1);  
  crypto_free_pk_env(pk2);  

  free(data1);
  free(data2);
  free(data3);

}

int 
main(int c, char**v) {
  setup_directory();
  puts("========================= Buffers ==========================");
  test_buffers();
  puts("========================== Crypto ==========================");
  test_crypto();
  puts("");
  return 0;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
