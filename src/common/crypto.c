/* Copyright 2001,2002,2003 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

/**
 * \file crypto.c
 *
 * \brief Low-level cryptographic functions.
 **/

#include "orconfig.h"

#ifdef MS_WINDOWS
#define WIN32_WINNT 0x400
#define _WIN32_WINNT 0x400
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

#include <string.h>

#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/opensslv.h>
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/rsa.h>
#include <openssl/dh.h>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif

#include "crypto.h"
#include "log.h"
#include "aes.h"
#include "util.h"

#if OPENSSL_VERSION_NUMBER < 0x00905000l
#error "We require openssl >= 0.9.5"
#elif OPENSSL_VERSION_NUMBER < 0x00906000l
#define OPENSSL_095
#endif

/* Certain functions that return a success code in OpenSSL 0.9.6 return void
 * (and don't indicate errors) in OpenSSL version 0.9.5.
 *
 * [OpenSSL 0.9.5 matters, because it ships with Redhat 6.2.]
 */
#ifdef OPENSSL_095
#define RETURN_SSL_OUTCOME(exp) (exp); return 0
#else
#define RETURN_SSL_OUTCOME(exp) return !(exp)
#endif

/** Macro: is k a valid RSA public or private key? */
#define PUBLIC_KEY_OK(k) ((k) && (k)->key && (k)->key->n)
/** Macro: is k a valid RSA private key? */
#define PRIVATE_KEY_OK(k) ((k) && (k)->key && (k)->key->p)

struct crypto_pk_env_t
{
  int refs; /* reference counting so we don't have to copy keys */
  RSA *key;
};

struct crypto_cipher_env_t
{
  unsigned char key[CIPHER_KEY_LEN];
  aes_cnt_cipher_t *cipher;
};

struct crypto_dh_env_t {
  DH *dh;
};

/** Return the number of bytes added by padding method <b>padding</b>.
 */
static INLINE int
crypto_get_rsa_padding_overhead(int padding) {
  switch(padding)
    {
    case RSA_NO_PADDING: return 0;
    case RSA_PKCS1_OAEP_PADDING: return 42;
    case RSA_PKCS1_PADDING: return 11;
    default: tor_assert(0); return -1;
    }
}

/** Given a padding method <b>padding</b>, return the correct OpenSSL constant.
 */
static INLINE int
crypto_get_rsa_padding(int padding) {
  switch(padding)
    {
    case PK_NO_PADDING: return RSA_NO_PADDING;
    case PK_PKCS1_PADDING: return RSA_PKCS1_PADDING;
    case PK_PKCS1_OAEP_PADDING: return RSA_PKCS1_OAEP_PADDING;
    default: tor_assert(0); return -1;
    }
}

/** Boolean: has OpenSSL's crypto been initialized? */
static int _crypto_global_initialized = 0;

/** Log all pending crypto errors at level <b>severity</b>.  Use
 * <b>doing</b> to describe our current activities.
 */
static void
crypto_log_errors(int severity, const char *doing)
{
  int err;
  const char *msg, *lib, *func;
  while ((err = ERR_get_error()) != 0) {
    msg = (const char*)ERR_reason_error_string(err);
    lib = (const char*)ERR_lib_error_string(err);
    func = (const char*)ERR_func_error_string(err);
    if (!msg) msg = "(null)";
    if (doing) {
      log(severity, "crypto error while %s: %s (in %s:%s)", doing, msg, lib,func);
    } else {
      log(severity, "crypto error: %s (in %s:%s)", msg, lib, func);
    }
  }
}

/** Initialize the crypto library.
 */
int crypto_global_init()
{
  if (!_crypto_global_initialized) {
      ERR_load_crypto_strings();
      _crypto_global_initialized = 1;
  }
  return 0;
}

/** Uninitialize the crypto library.
 */
int crypto_global_cleanup()
{
  ERR_free_strings();
  return 0;
}

/** used by tortls.c: wrap an RSA* in a crypto_pk_env_t. */
crypto_pk_env_t *_crypto_new_pk_env_rsa(RSA *rsa)
{
  crypto_pk_env_t *env;
  tor_assert(rsa);
  env = tor_malloc(sizeof(crypto_pk_env_t));
  env->refs = 1;
  env->key = rsa;
  return env;
}

/** used by tortls.c: return the RSA* from a crypto_pk_env_t. */
RSA *_crypto_pk_env_get_rsa(crypto_pk_env_t *env)
{
  return env->key;
}

/** used by tortls.c: get an equivalent EVP_PKEY* for a crypto_pk_env_t.  Iff
 * private is set, include the private-key portion of the key. */
EVP_PKEY *_crypto_pk_env_get_evp_pkey(crypto_pk_env_t *env, int private)
{
  RSA *key = NULL;
  EVP_PKEY *pkey = NULL;
  tor_assert(env->key);
  if (private) {
    if (!(key = RSAPrivateKey_dup(env->key)))
      goto error;
  } else {
    if (!(key = RSAPublicKey_dup(env->key)))
      goto error;
  }
  if (!(pkey = EVP_PKEY_new()))
    goto error;
  if (!(EVP_PKEY_assign_RSA(pkey, key)))
    goto error;
  return pkey;
 error:
  if (pkey)
    EVP_PKEY_free(pkey);
  if (key)
    RSA_free(key);
  return NULL;
}

/** Used by tortls.c: Get the DH* from a crypto_dh_env_t.
 */
DH *_crypto_dh_env_get_dh(crypto_dh_env_t *dh)
{
  return dh->dh;
}

/** Allocate and return storage for a public key.  The key itself will not yet
 * be set.
 */
crypto_pk_env_t *crypto_new_pk_env(void)
{
  RSA *rsa;

  rsa = RSA_new();
  if (!rsa) return NULL;
  return _crypto_new_pk_env_rsa(rsa);
}

/** Release a reference to an asymmetric key; when all the references
 * are released, free the key.
 */
void crypto_free_pk_env(crypto_pk_env_t *env)
{
  tor_assert(env);

  if(--env->refs > 0)
    return;

  if (env->key)
    RSA_free(env->key);

  free(env);
}

/** Create a new symmetric cipher for a given key and encryption flag
 * (1=encrypt, 0=decrypt).  Return the crypto object on success; NULL
 * on failure.
 */
crypto_cipher_env_t *
crypto_create_init_cipher(const char *key, int encrypt_mode)
{
  int r;
  crypto_cipher_env_t *crypto = NULL;

  if (! (crypto = crypto_new_cipher_env())) {
    log_fn(LOG_WARN, "Unable to allocate crypto object");
    return NULL;
  }

  if (crypto_cipher_set_key(crypto, key)) {
    crypto_log_errors(LOG_WARN, "setting symmetric key");
    goto error;
  }

  if (encrypt_mode)
    r = crypto_cipher_encrypt_init_cipher(crypto);
  else
    r = crypto_cipher_decrypt_init_cipher(crypto);

  if (r)
    goto error;
  return crypto;

 error:
  if (crypto)
    crypto_free_cipher_env(crypto);
  return NULL;
}

/** Allocate and return a new symmetric cipher.
 */
crypto_cipher_env_t *crypto_new_cipher_env()
{
  crypto_cipher_env_t *env;

  env = tor_malloc_zero(sizeof(crypto_cipher_env_t));
  env->cipher = aes_new_cipher();
  return env;
}

/** Free a symmetric cipher.
 */
void crypto_free_cipher_env(crypto_cipher_env_t *env)
{
  tor_assert(env);

  tor_assert(env->cipher);
  aes_free_cipher(env->cipher);
  tor_free(env);
}

/* public key crypto */

/** Generate a new public/private keypair in <b>env</b>.  Return 0 on
 * success, -1 on failure.
 */
int crypto_pk_generate_key(crypto_pk_env_t *env)
{
  tor_assert(env);

  if (env->key)
    RSA_free(env->key);
  env->key = RSA_generate_key(PK_BYTES*8,65537, NULL, NULL);
  if (!env->key) {
    crypto_log_errors(LOG_WARN, "generating RSA key");
    return -1;
  }

  return 0;
}

/** Read a PEM-encoded private key from <b>src</b> into <b>env</b>.
 */
static int crypto_pk_read_private_key_from_file(crypto_pk_env_t *env,
                                                FILE *src)
{
  tor_assert(env && src);

  if (env->key)
    RSA_free(env->key);
  env->key = PEM_read_RSAPrivateKey(src, NULL, NULL, NULL);
  if (!env->key) {
    crypto_log_errors(LOG_WARN, "reading private key from file");
    return -1;
  }

  return 0;
}

/** Read a PEM-encoded private key from the file named by
 * <b>keyfile</b> into <b>env</b>.  Return 0 on success, -1 on failure.
 */
int crypto_pk_read_private_key_from_filename(crypto_pk_env_t *env, const char *keyfile)
{
  FILE *f_pr;

  tor_assert(env && keyfile);

  if(strspn(keyfile,CONFIG_LEGAL_FILENAME_CHARACTERS) != strlen(keyfile)) {
    /* filename contains nonlegal characters */
    return -1;
  }

  /* open the keyfile */
  f_pr=fopen(keyfile,"rb");
  if (!f_pr)
    return -1;

  /* read the private key */
  if(crypto_pk_read_private_key_from_file(env, f_pr) < 0) {
    fclose(f_pr);
    return -1;
  }
  fclose(f_pr);

  /* check the private key */
  if (crypto_pk_check_key(env) <= 0)
    return -1;

  return 0;
}

/** PEM-encode the public key portion of <b>env</b> and write it to a
 * newly allocated string.  On success, set *<b>dest</b> to the new
 * string, *<b>len</b> to the string's length, and return 0.  On
 * failure, return -1.
 */
int crypto_pk_write_public_key_to_string(crypto_pk_env_t *env, char **dest, int *len) {
  BUF_MEM *buf;
  BIO *b;

  tor_assert(env && env->key && dest);

  b = BIO_new(BIO_s_mem()); /* Create a memory BIO */

  /* Now you can treat b as if it were a file.  Just use the
   * PEM_*_bio_* functions instead of the non-bio variants.
   */
  if(!PEM_write_bio_RSAPublicKey(b, env->key)) {
    crypto_log_errors(LOG_WARN, "writing public key to string");
    return -1;
  }

  BIO_get_mem_ptr(b, &buf);
  BIO_set_close(b, BIO_NOCLOSE); /* so BIO_free doesn't free buf */
  BIO_free(b);

  *dest = tor_malloc(buf->length+1);
  memcpy(*dest, buf->data, buf->length);
  (*dest)[buf->length] = 0; /* null terminate it */
  *len = buf->length;
  BUF_MEM_free(buf);

  return 0;
}

/** Read a PEM-encoded public key from the first <b>len</b> characters of
 * <b>src</b>, and store the result in <b>env</b>.  Return 0 on success, -1 on
 * failure.
 */
int crypto_pk_read_public_key_from_string(crypto_pk_env_t *env, const char *src, int len) {
  BIO *b;

  tor_assert(env && src);

  b = BIO_new(BIO_s_mem()); /* Create a memory BIO */

  BIO_write(b, src, len);

  if (env->key)
    RSA_free(env->key);
  env->key = PEM_read_bio_RSAPublicKey(b, NULL, NULL, NULL);
  BIO_free(b);
  if(!env->key) {
    crypto_log_errors(LOG_WARN, "reading public key from string");
    return -1;
  }

  return 0;
}

/* Write the private key from 'env' into the file named by 'fname',
 * PEM-encoded.  Return 0 on success, -1 on failure.
 */
int
crypto_pk_write_private_key_to_filename(crypto_pk_env_t *env,
                                        const char *fname)
{
  BIO *bio;
  char *cp;
  long len;
  char *s;
  int r;

  tor_assert(PRIVATE_KEY_OK(env));

  if (!(bio = BIO_new(BIO_s_mem())))
    return -1;
  if (PEM_write_bio_RSAPrivateKey(bio, env->key, NULL,NULL,0,NULL,NULL)
      == 0) {
    crypto_log_errors(LOG_WARN, "writing private key");
    BIO_free(bio);
    return -1;
  }
  len = BIO_get_mem_data(bio, &cp);
  s = tor_malloc(len+1);
  strncpy(s, cp, len);
  s[len] = '\0';
  r = write_str_to_file(fname, s);
  BIO_free(bio);
  free(s);
  return r;
}

/** Return true iff <b>env</b> has a valid key.
 */
int crypto_pk_check_key(crypto_pk_env_t *env)
{
  int r;
  tor_assert(env);

  r = RSA_check_key(env->key);
  if (r <= 0)
    crypto_log_errors(LOG_WARN,"checking RSA key");
  return r;
}

/** Compare the public-key components of a and b.  Return -1 if a\<b, 0
 * if a==b, and 1 if a\>b.
 */
int crypto_pk_cmp_keys(crypto_pk_env_t *a, crypto_pk_env_t *b) {
  int result;

  if (!a || !b)
    return -1;

  if (!a->key || !b->key)
    return -1;

  tor_assert(PUBLIC_KEY_OK(a));
  tor_assert(PUBLIC_KEY_OK(b));
  result = BN_cmp((a->key)->n, (b->key)->n);
  if (result)
    return result;
  return BN_cmp((a->key)->e, (b->key)->e);
}

/** Return the size of the public key modulus in <b>env</b>, in bytes. */
int crypto_pk_keysize(crypto_pk_env_t *env)
{
  tor_assert(env && env->key);

  return RSA_size(env->key);
}

/** Increase the reference count of <b>env</b>.
 */
crypto_pk_env_t *crypto_pk_dup_key(crypto_pk_env_t *env) {
  tor_assert(env && env->key);

  env->refs++;
  return env;
}

/** Encrypt <b>fromlen</b> bytes from <b>from</b> with the public key
 * in <b>env</b>, using the padding method <b>padding</b>.  On success,
 * write the result to <b>to</b>, and return the number of bytes
 * written.  On failure, return -1.
 */
int crypto_pk_public_encrypt(crypto_pk_env_t *env, const unsigned char *from, int fromlen, unsigned char *to, int padding)
{
  int r;
  tor_assert(env && from && to);

  r = RSA_public_encrypt(fromlen, (unsigned char*)from, to, env->key,
                            crypto_get_rsa_padding(padding));
  if (r<0) {
    crypto_log_errors(LOG_WARN, "performing RSA encryption");
    return -1;
  }
  return r;
}

/** Decrypt <b>fromlen</b> bytes from <b>from</b> with the private key
 * in <b>env</b>, using the padding method <b>padding</b>.  On success,
 * write the result to <b>to</b>, and return the number of bytes
 * written.  On failure, return -1.
 */
int crypto_pk_private_decrypt(crypto_pk_env_t *env, const unsigned char *from, int fromlen, unsigned char *to, int padding)
{
  int r;
  tor_assert(env && from && to && env->key);
  if (!env->key->p)
    /* Not a private key */
    return -1;

  r = RSA_private_decrypt(fromlen, (unsigned char*)from, to, env->key,
                             crypto_get_rsa_padding(padding));
  if (r<0) {
    crypto_log_errors(LOG_WARN, "performing RSA decryption");
    return -1;
  }
  return r;
}

/** Check the signature in <b>from</b> (<b>fromlen</b> bytes long) with the
 * public key in <b>env</b>, using PKCS1 padding.  On success, write the
 * signed data to <b>to</b>, and return the number of bytes written.
 * On failure, return -1.
 */
int crypto_pk_public_checksig(crypto_pk_env_t *env, const unsigned char *from, int fromlen, unsigned char *to)
{
  int r;
  tor_assert(env && from && to);
  r = RSA_public_decrypt(fromlen, (unsigned char*)from, to, env->key, RSA_PKCS1_PADDING);

  if (r<0) {
    crypto_log_errors(LOG_WARN, "checking RSA signature");
    return -1;
  }
  return r;
}

/** Sign <b>fromlen</b> bytes of data from <b>from</b> with the private key in
 * <b>env</b>, using PKCS1 padding.  On success, write the signature to
 * <b>to</b>, and return the number of bytes written.  On failure, return
 * -1.
 */
int crypto_pk_private_sign(crypto_pk_env_t *env, const unsigned char *from, int fromlen, unsigned char *to)
{
  int r;
  tor_assert(env && from && to);
  if (!env->key->p)
    /* Not a private key */
    return -1;

  r = RSA_private_encrypt(fromlen, (unsigned char*)from, to, env->key, RSA_PKCS1_PADDING);
  if (r<0) {
    crypto_log_errors(LOG_WARN, "generating RSA signature");
    return -1;
  }
  return r;
}

/** Check a siglen-byte long signature at <b>sig</b> against
 * <b>datalen</b> bytes of data at <b>data</b>, using the public key
 * in <b>env</b>. Return 0 if <b>sig</b> is a correct signature for
 * SHA1(data).  Else return -1.
 */
int crypto_pk_public_checksig_digest(crypto_pk_env_t *env, const unsigned char *data, int datalen, const unsigned char *sig, int siglen)
{
  char digest[DIGEST_LEN];
  char buf[PK_BYTES+1];
  int r;

  tor_assert(env && data && sig);

  if (crypto_digest(data,datalen,digest)<0) {
    log_fn(LOG_WARN, "couldn't compute digest");
    return -1;
  }
  r = crypto_pk_public_checksig(env,sig,siglen,buf);
  if (r != DIGEST_LEN) {
    log_fn(LOG_WARN, "Invalid signature");
    return -1;
  }
  if (memcmp(buf, digest, DIGEST_LEN)) {
    log_fn(LOG_WARN, "Signature mismatched with digest.");
    return -1;
  }

  return 0;
}

/** Compute a SHA1 digest of <b>fromlen</b> bytes of data stored at
 * <b>from</b>; sign the data with the private key in <b>env</b>, and
 * store it in <b>to</b>.  Return the number of bytes written on
 * success, and -1 on failure.
 */
int crypto_pk_private_sign_digest(crypto_pk_env_t *env, const unsigned char *from, int fromlen, unsigned char *to)
{
  char digest[DIGEST_LEN];
  if (crypto_digest(from,fromlen,digest)<0)
    return -1;
  return crypto_pk_private_sign(env,digest,DIGEST_LEN,to);
}


/** Perform a hybrid (public/secret) encryption on <b>fromlen</b>
 * bytes of data from <b>from</b>, with padding type 'padding',
 * storing the results on <b>to</b>.
 *
 * If no padding is used, the public key must be at least as large as
 * <b>from</b>.
 *
 * Returns the number of bytes written on success, -1 on failure.
 *
 * The encrypted data consists of:
 *   - The source data, padded and encrypted with the public key, if the
 *     padded source data is no longer than the public key, and <b>force</b>
 *     is false, OR
 *   - The beginning of the source data prefixed with a 16-byte symmetric key,
 *     padded and encrypted with the public key; followed by the rest of
 *     the source data encrypted in AES-CTR mode with the symmetric key.
 */
int crypto_pk_public_hybrid_encrypt(crypto_pk_env_t *env,
                                    const unsigned char *from,
                                    int fromlen, unsigned char *to,
                                    int padding, int force)
{
  int overhead, pkeylen, outlen, r, symlen;
  crypto_cipher_env_t *cipher = NULL;
  char buf[PK_BYTES+1];

  tor_assert(env && from && to);

  overhead = crypto_get_rsa_padding_overhead(crypto_get_rsa_padding(padding));
  pkeylen = crypto_pk_keysize(env);

  if (padding == PK_NO_PADDING && fromlen < pkeylen)
    return -1;

  if (!force && fromlen+overhead <= pkeylen) {
    /* It all fits in a single encrypt. */
    return crypto_pk_public_encrypt(env,from,fromlen,to,padding);
  }
  cipher = crypto_new_cipher_env();
  if (!cipher) return -1;
  if (crypto_cipher_generate_key(cipher)<0)
    goto err;
  /* You can't just run around RSA-encrypting any bitstream: if it's
   * greater than the RSA key, then OpenSSL will happily encrypt, and
   * later decrypt to the wrong value.  So we set the first bit of
   * 'cipher->key' to 0 if we aren't padding.  This means that our
   * symmetric key is really only 127 bits.
   */
  if (padding == PK_NO_PADDING)
    cipher->key[0] &= 0x7f;
  if (crypto_cipher_encrypt_init_cipher(cipher)<0)
    goto err;
  memcpy(buf, cipher->key, CIPHER_KEY_LEN);
  memcpy(buf+CIPHER_KEY_LEN, from, pkeylen-overhead-CIPHER_KEY_LEN);

  /* Length of symmetrically encrypted data. */
  symlen = fromlen-(pkeylen-overhead-CIPHER_KEY_LEN);

  outlen = crypto_pk_public_encrypt(env,buf,pkeylen-overhead,to,padding);
  if (outlen!=pkeylen) {
    goto err;
  }
  r = crypto_cipher_encrypt(cipher,
                            from+pkeylen-overhead-CIPHER_KEY_LEN, symlen,
                            to+outlen);

  if (r<0) goto err;
  memset(buf, 0, sizeof(buf));
  crypto_free_cipher_env(cipher);
  return outlen + symlen;
 err:
  memset(buf, 0, sizeof(buf));
  if (cipher) crypto_free_cipher_env(cipher);
  return -1;
}

/** Invert crypto_pk_public_hybrid_encrypt. */
int crypto_pk_private_hybrid_decrypt(crypto_pk_env_t *env,
                                     const unsigned char *from,
                                     int fromlen, unsigned char *to,
                                     int padding)
{
  int overhead, pkeylen, outlen, r;
  crypto_cipher_env_t *cipher = NULL;
  char buf[PK_BYTES+1];

  overhead = crypto_get_rsa_padding_overhead(crypto_get_rsa_padding(padding));
  pkeylen = crypto_pk_keysize(env);

  if (fromlen <= pkeylen) {
    return crypto_pk_private_decrypt(env,from,fromlen,to,padding);
  }
  outlen = crypto_pk_private_decrypt(env,from,pkeylen,buf,padding);
  if (outlen<0) {
    /* this is only log-levelinfo, because when we're decrypting
     * onions, we try several keys to see which will work */
    log_fn(LOG_INFO, "Error decrypting public-key data");
    return -1;
  }
  if (outlen < CIPHER_KEY_LEN) {
    log_fn(LOG_WARN, "No room for a symmetric key");
    return -1;
  }
  cipher = crypto_create_init_cipher(buf, 0);
  if (!cipher) {
    return -1;
  }
  memcpy(to,buf+CIPHER_KEY_LEN,outlen-CIPHER_KEY_LEN);
  outlen -= CIPHER_KEY_LEN;
  r = crypto_cipher_decrypt(cipher, from+pkeylen, fromlen-pkeylen,
                            to+outlen);
  if (r<0)
    goto err;
  memset(buf,0,sizeof(buf));
  crypto_free_cipher_env(cipher);
  return outlen + (fromlen-pkeylen);
 err:
  memset(buf,0,sizeof(buf));
  if (cipher) crypto_free_cipher_env(cipher);
  return -1;
}

/** ASN.1-encode the public portion of <b>pk</b> into <b>dest</b>.
 * Return -1 on error, or the number of characters used on success.
 */
int crypto_pk_asn1_encode(crypto_pk_env_t *pk, char *dest, int dest_len)
{
  int len;
  unsigned char *buf, *cp;
  len = i2d_RSAPublicKey(pk->key, NULL);
  if (len < 0 || len > dest_len)
    return -1;
  cp = buf = tor_malloc(len+1);
  len = i2d_RSAPublicKey(pk->key, &cp);
  if (len < 0) {
    crypto_log_errors(LOG_WARN,"encoding public key");
    tor_free(buf);
    return -1;
  }
  /* We don't encode directly into 'dest', because that would be illegal
   * type-punning.  (C99 is smarter than me, C99 is smarter than me...)
   */
  memcpy(dest,buf,len);
  tor_free(buf);
  return len;
}

/** Decode an ASN.1-encoded public key from <b>str</b>; return the result on
 * success and NULL on failure.
 */
crypto_pk_env_t *crypto_pk_asn1_decode(const char *str, int len)
{
  RSA *rsa;
  unsigned char *buf;
  /* This ifdef suppresses a type warning.  Take out the first case once
   * everybody is using openssl 0.9.7 or later.
   */
#if OPENSSL_VERSION_NUMBER < 0x00907000l
  unsigned char *cp;
#else
  const unsigned char *cp;
#endif
  cp = buf = tor_malloc(len);
  memcpy(buf,str,len);
  rsa = d2i_RSAPublicKey(NULL, &cp, len);
  tor_free(buf);
  if (!rsa) {
    crypto_log_errors(LOG_WARN,"decoding public key");
    return NULL;
  }
  return _crypto_new_pk_env_rsa(rsa);
}

/** Given a private or public key <b>pk</b>, put a SHA1 hash of the
 * public key into <b>digest_out</b> (must have DIGEST_LEN bytes of space).
 */
int crypto_pk_get_digest(crypto_pk_env_t *pk, char *digest_out)
{
  unsigned char *buf, *bufp;
  int len;

  len = i2d_RSAPublicKey(pk->key, NULL);
  if (len < 0)
    return -1;
  buf = bufp = tor_malloc(len+1);
  len = i2d_RSAPublicKey(pk->key, &bufp);
  if (len < 0) {
    crypto_log_errors(LOG_WARN,"encoding public key");
    free(buf);
    return -1;
  }
  if (crypto_digest(buf, len, digest_out) < 0) {
    free(buf);
    return -1;
  }
  free(buf);
  return 0;
}

/** Given a private or public key <b>pk</b>, put a fingerprint of the
 * public key into <b>fp_out</b> (must have at least FINGERPRINT_LEN+1 bytes of
 * space).
 *
 * Fingerprints are computed as the SHA1 digest of the ASN.1 encoding
 * of the public key, converted to hexadecimal, with a space after every
 * four digits.
 */
int
crypto_pk_get_fingerprint(crypto_pk_env_t *pk, char *fp_out)
{
  unsigned char *bufp;
  unsigned char digest[DIGEST_LEN];
  unsigned char buf[FINGERPRINT_LEN+1];
  int i;
  if (crypto_pk_get_digest(pk, digest)) {
    return -1;
  }
  bufp = buf;
  for (i = 0; i < DIGEST_LEN; ++i) {
    sprintf(bufp,"%02X",digest[i]);
    bufp += 2;
    if (i%2 && i != 19) {
      *bufp++ = ' ';
    }
  }
  *bufp = '\0';
  tor_assert(strlen(buf) == FINGERPRINT_LEN);
  tor_assert(crypto_pk_check_fingerprint_syntax(buf));
  strcpy(fp_out, buf);
  return 0;
}

/** Return true iff <b>s</b> is in the correct format for a fingerprint.
 */
int
crypto_pk_check_fingerprint_syntax(const char *s)
{
  int i;
  for (i = 0; i < FINGERPRINT_LEN; ++i) {
    if ((i%5) == 4) {
      if (!isspace((int)s[i])) return 0;
    } else {
      if (!isxdigit((int)s[i])) return 0;
    }
  }
  if (s[FINGERPRINT_LEN]) return 0;
  return 1;
}

/* symmetric crypto */

/** Generate a new random key for the symmetric cipher in <b>env</b>.
 * Return 0 on success, -1 on failure.  Does not initialize the cipher.
 */
int crypto_cipher_generate_key(crypto_cipher_env_t *env)
{
  tor_assert(env);

  return crypto_rand(CIPHER_KEY_LEN, env->key);
}

/** Set the symmetric key for the cipher in <b>env</b> to the first
 * CIPHER_KEY_LEN bytes of <b>key</b>. Does not initialize the cipher.
 */
int crypto_cipher_set_key(crypto_cipher_env_t *env, const unsigned char *key)
{
  tor_assert(env && key);

  if (!env->key)
    return -1;

  memcpy(env->key, key, CIPHER_KEY_LEN);

  return 0;
}

/** Return a pointer to the key set for the cipher in <b>env</b>.
 */
const unsigned char *crypto_cipher_get_key(crypto_cipher_env_t *env)
{
  return env->key;
}

/** Initialize the cipher in <b>env</b> for encryption.
 */
int crypto_cipher_encrypt_init_cipher(crypto_cipher_env_t *env)
{
  tor_assert(env);

  aes_set_key(env->cipher, env->key, CIPHER_KEY_LEN*8);
  return 0;
}

/** Initialize the cipher in <b>env</b> for decryption.
 */
int crypto_cipher_decrypt_init_cipher(crypto_cipher_env_t *env)
{
  tor_assert(env);

  aes_set_key(env->cipher, env->key, CIPHER_KEY_LEN*8);
  return 0;
}

/** Encrypt <b>fromlen</b> bytes from <b>from</b> using the cipher
 * <b>env</b>; on success, store the result to <b>to</b> and return 0.
 * On failure, return -1.
 */
int crypto_cipher_encrypt(crypto_cipher_env_t *env, const unsigned char *from, unsigned int fromlen, unsigned char *to)
{
  tor_assert(env && env->cipher && from && fromlen && to);

  aes_crypt(env->cipher, from, fromlen, to);
  return 0;
}

/** Decrypt <b>fromlen</b> bytes from <b>from</b> using the cipher
 * <b>env</b>; on success, store the result to <b>to</b> and return 0.
 * On failure, return -1.
 */
int crypto_cipher_decrypt(crypto_cipher_env_t *env, const unsigned char *from, unsigned int fromlen, unsigned char *to)
{
  tor_assert(env && from && to);

  aes_crypt(env->cipher, from, fromlen, to);
  return 0;
}

/** Move the position of the cipher stream backwards by <b>delta</b> bytes.
 */
int
crypto_cipher_rewind(crypto_cipher_env_t *env, long delta)
{
  return crypto_cipher_advance(env, -delta);
}

/** Move the position of the cipher stream forwards by <b>delta</b> bytes.
 */
int
crypto_cipher_advance(crypto_cipher_env_t *env, long delta)
{
  aes_adjust_counter(env->cipher, delta);
  return 0;
}

/* SHA-1 */

/** Compute the SHA1 digest of <b>len</b> bytes in data stored in
 * <b>m</b>.  Write the DIGEST_LEN byte result into <b>digest</b>.
 */
int crypto_digest(const unsigned char *m, int len, unsigned char *digest)
{
  tor_assert(m && digest);
  return (SHA1(m,len,digest) == NULL);
}

struct crypto_digest_env_t {
  SHA_CTX d;
};

/** Allocate and return a new digest object.
 */
crypto_digest_env_t *
crypto_new_digest_env(void)
{
  crypto_digest_env_t *r;
  r = tor_malloc(sizeof(crypto_digest_env_t));
  SHA1_Init(&r->d);
  return r;
}

/** Deallocate a digest object.
 */
void
crypto_free_digest_env(crypto_digest_env_t *digest) {
  tor_free(digest);
}

/** Add <b>len</b> bytes from <b>data</b> to the digest object.
 */
void
crypto_digest_add_bytes(crypto_digest_env_t *digest, const char *data,
                        size_t len)
{
  tor_assert(digest);
  tor_assert(data);
  SHA1_Update(&digest->d, (void*)data, len);
}

/** Compute the hash of the data that has been passed to the digest
 * object; write the first out_len bytes of the result to <b>out</b>.
 * <b>out_len</b> must be \<= DIGEST_LEN.
 */
void crypto_digest_get_digest(crypto_digest_env_t *digest,
                              char *out, size_t out_len)
{
  static char r[DIGEST_LEN];
  tor_assert(digest && out);
  tor_assert(out_len <= DIGEST_LEN);
  SHA1_Final(r, &digest->d);
  memcpy(out, r, out_len);
}

/** Allocate and return a new digest object with the same state as
 * <b>digest</b>
 */
crypto_digest_env_t *
crypto_digest_dup(const crypto_digest_env_t *digest)
{
  crypto_digest_env_t *r;
  tor_assert(digest);
  r = tor_malloc(sizeof(crypto_digest_env_t));
  memcpy(r,digest,sizeof(crypto_digest_env_t));
  return r;
}

/** Replace the state of the digest object <b>into</b> with the state
 * of the digest object <b>from</b>.
 */
void
crypto_digest_assign(crypto_digest_env_t *into,
                     const crypto_digest_env_t *from)
{
  tor_assert(into && from);
  memcpy(into,from,sizeof(crypto_digest_env_t));
}

/* DH */

/** Shared P parameter for our DH key exchanged. */
static BIGNUM *dh_param_p = NULL;
/** Shared G parameter for our DH key exchanges. */
static BIGNUM *dh_param_g = NULL;

/** Initialize dh_param_p and dh_param_g if they are not already
 * set. */
static void init_dh_param() {
  BIGNUM *p, *g;
  int r;
  if (dh_param_p && dh_param_g)
    return;

  p = BN_new();
  g = BN_new();
  tor_assert(p && g);

#if 0
  /* This is from draft-ietf-ipsec-ike-modp-groups-05.txt.  It's a safe
     prime, and supposedly it equals:
      2^1536 - 2^1472 - 1 + 2^64 * { [2^1406 pi] + 741804 }
  */
  r = BN_hex2bn(&p,
                "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
                "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
                "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
                "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
                "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
                "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
                "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
                "670C354E4ABC9804F1746C08CA237327FFFFFFFFFFFFFFFF");
#endif

  /* This is from rfc2409, section 6.2.  It's a safe prime, and
     supposedly it equals:
        2^1024 - 2^960 - 1 + 2^64 * { [2^894 pi] + 129093 }.
  */
  /* See also rfc 3536 */
  r = BN_hex2bn(&p,
                "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
                "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
                "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
                "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
                "49286651ECE65381FFFFFFFFFFFFFFFF");
  tor_assert(r);

  r = BN_set_word(g, 2);
  tor_assert(r);
  dh_param_p = p;
  dh_param_g = g;
}

/** Allocate and return a new DH object for a key echange.
 */
crypto_dh_env_t *crypto_dh_new()
{
  crypto_dh_env_t *res = NULL;

  if (!dh_param_p)
    init_dh_param();

  res = tor_malloc(sizeof(crypto_dh_env_t));
  res->dh = NULL;

  if (!(res->dh = DH_new()))
    goto err;

  if (!(res->dh->p = BN_dup(dh_param_p)))
    goto err;

  if (!(res->dh->g = BN_dup(dh_param_g)))
    goto err;

  return res;
 err:
  crypto_log_errors(LOG_WARN, "creating DH object");
  if (res && res->dh) DH_free(res->dh); /* frees p and g too */
  if (res) free(res);
  return NULL;
}

/** Return the length of the DH key in <b>dh</b>, in bytes.
 */
int crypto_dh_get_bytes(crypto_dh_env_t *dh)
{
  tor_assert(dh);
  return DH_size(dh->dh);
}

/** Generate \<x,g^x\> for our part of the key exchange.  Return 0 on
 * success, -1 on failure.
 */
int crypto_dh_generate_public(crypto_dh_env_t *dh)
{
  if (!DH_generate_key(dh->dh)) {
    crypto_log_errors(LOG_WARN, "generating DH key");
    return -1;
  }
  return 0;
}

/** Generate g^x as necessary, and write the g^x for the key exchange
 * as a <b>pubkey_len</b>-byte value into <b>pubkey</b>. Return 0 on
 * success, -1 on failure.  <b>pubkey_len</b> must be \>= DH_BYTES.
 */
int crypto_dh_get_public(crypto_dh_env_t *dh, char *pubkey, int pubkey_len)
{
  int bytes;
  tor_assert(dh);
  if (!dh->dh->pub_key) {
    if (crypto_dh_generate_public(dh)<0)
      return -1;
  }

  tor_assert(dh->dh->pub_key);
  bytes = BN_num_bytes(dh->dh->pub_key);
  if (pubkey_len < bytes)
    return -1;

  memset(pubkey, 0, pubkey_len);
  BN_bn2bin(dh->dh->pub_key, pubkey+(pubkey_len-bytes));

  return 0;
}

#undef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
/** Given a DH key exchange object, and our peer's value of g^y (as a
 * <b>pubkey_len</b> byte value in <b>pubkey</b>) generate
 * <b>secret_bytes_out</b> bytes of shared key material and write them
 * to <b>secret_out</b>.
 *
 * (We generate key material by computing
 *         SHA1( g^xy || "\x00" ) || SHA1( g^xy || "\x01" ) || ...
 * where || is concatenation.)
 */
int crypto_dh_compute_secret(crypto_dh_env_t *dh,
                             const char *pubkey, int pubkey_len,
                             char *secret_out, int secret_bytes_out)
{
  unsigned char hash[DIGEST_LEN];
  unsigned char *secret_tmp = NULL;
  BIGNUM *pubkey_bn = NULL;
  int secret_len;
  int i;
  tor_assert(dh);
  tor_assert(secret_bytes_out/DIGEST_LEN <= 255);

  if (!(pubkey_bn = BN_bin2bn(pubkey, pubkey_len, NULL)))
    goto error;
  secret_tmp = tor_malloc(crypto_dh_get_bytes(dh)+1);
  secret_len = DH_compute_key(secret_tmp, pubkey_bn, dh->dh);
  /* sometimes secret_len might be less than 128, e.g., 127. that's ok. */
  for (i = 0; i < secret_bytes_out; i += DIGEST_LEN) {
    secret_tmp[secret_len] = (unsigned char) i/DIGEST_LEN;
    if (crypto_digest(secret_tmp, secret_len+1, hash))
      goto error;
    memcpy(secret_out+i, hash, MIN(DIGEST_LEN, secret_bytes_out-i));
  }
  secret_len = secret_bytes_out;

  goto done;
 error:
  secret_len = -1;
 done:
  crypto_log_errors(LOG_WARN, "completing DH handshake");
  if (pubkey_bn)
    BN_free(pubkey_bn);
  tor_free(secret_tmp);
  return secret_len;
}
/** Free a DH key exchange object.
 */
void crypto_dh_free(crypto_dh_env_t *dh)
{
  tor_assert(dh && dh->dh);
  DH_free(dh->dh);
  free(dh);
}

/* random numbers */

/** Seed OpenSSL's random number generator with DIGEST_LEN bytes from the
 * operating system.
 */
int crypto_seed_rng()
{
#ifdef MS_WINDOWS
  static int provider_set = 0;
  static HCRYPTPROV provider;
  char buf[DIGEST_LEN+1];

  if (!provider_set) {
    if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL, 0)) {
      if (GetLastError() != NTE_BAD_KEYSET) {
        log_fn(LOG_ERR,"Can't get CryptoAPI provider [1]");
        return -1;
      }
      /* Yes, we need to try it twice. */
      if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL,
                               CRYPT_NEWKEYSET)) {
        log_fn(LOG_ERR,"Can't get CryptoAPI provider [2]");
        return -1;
      }
    }
    provider_set = 1;
  }
  if (!CryptGenRandom(provider, DIGEST_LEN, buf)) {
    log_fn(LOG_ERR,"Can't get entropy from CryptoAPI.");
    return -1;
  }
  RAND_seed(buf, DIGEST_LEN);
  /* And add the current screen state to the entopy pool for
   * good measure. */
  RAND_screen();
  return 0;
#else
  static char *filenames[] = {
    "/dev/srandom", "/dev/urandom", "/dev/random", NULL
  };
  int fd;
  int i, n;
  char buf[DIGEST_LEN+1];

  for (i = 0; filenames[i]; ++i) {
    fd = open(filenames[i], O_RDONLY, 0);
    if (fd<0) continue;
    log_fn(LOG_INFO, "Seeding RNG from %s", filenames[i]);
    n = read(fd, buf, DIGEST_LEN);
    close(fd);
    if (n != DIGEST_LEN) {
      log_fn(LOG_WARN, "Error reading from entropy source");
      return -1;
    }
    RAND_seed(buf, DIGEST_LEN);
    return 0;
  }

  log_fn(LOG_WARN, "Cannot seed RNG -- no entropy source found.");
  return -1;
#endif
}

/** Write n bytes of strong random data to <b>to</b>. Return 0 on
 * success, -1 on failure.
 */
int crypto_rand(unsigned int n, unsigned char *to)
{
  int r;
  tor_assert(to);
  r = RAND_bytes(to, n);
  if (r == 0)
    crypto_log_errors(LOG_WARN, "generating random data");
  return (r == 1) ? 0 : -1;
}

/** Write n bytes of pseudorandom data to <b>to</b>. Return 0 on
 * success, -1 on failure.
 */
void crypto_pseudo_rand(unsigned int n, unsigned char *to)
{
  tor_assert(to);
  if (RAND_pseudo_bytes(to, n) == -1) {
    log_fn(LOG_ERR, "RAND_pseudo_bytes failed unexpectedly.");
    crypto_log_errors(LOG_WARN, "generating random data");
    exit(1);
  }
}

/** Return a pseudorandom integer, choosen uniformly from the values
 * between 0 and max-1. */
int crypto_pseudo_rand_int(unsigned int max) {
  unsigned int val;
  unsigned int cutoff;
  tor_assert(max < UINT_MAX);
  tor_assert(max > 0); /* don't div by 0 */

  /* We ignore any values that are >= 'cutoff,' to avoid biasing the
   * distribution with clipping at the upper end of unsigned int's
   * range.
   */
  cutoff = UINT_MAX - (UINT_MAX%max);
  while(1) {
    crypto_pseudo_rand(sizeof(val), (unsigned char*) &val);
    if (val < cutoff)
      return val % max;
  }
}

/** Base-64 encode <b>srclen</b> bytes of data from <b>src</b>.  Write
 * the result into <b>dest</b>, if it will fit within <b>destlen</b>
 * bytes.  Return the number of bytes written on success; -1 if
 * destlen is too short, or other failure.
 */
int
base64_encode(char *dest, int destlen, const char *src, int srclen)
{
  EVP_ENCODE_CTX ctx;
  int len, ret;

  /* 48 bytes of input -> 64 bytes of output plus newline.
     Plus one more byte, in case I'm wrong.
  */
  if (destlen < ((srclen/48)+1)*66)
    return -1;

  EVP_EncodeInit(&ctx);
  EVP_EncodeUpdate(&ctx, dest, &len, (char*) src, srclen);
  EVP_EncodeFinal(&ctx, dest+len, &ret);
  ret += len;
  return ret;
}

/** Base-64 decode <b>srclen</b> bytes of data from <b>src</b>.  Write
 * the result into <b>dest</b>, if it will fit within <b>destlen</b>
 * bytes.  Return the number of bytes written on success; -1 if
 * destlen is too short, or other failure.
 */
int
base64_decode(char *dest, int destlen, const char *src, int srclen)
{
  EVP_ENCODE_CTX ctx;
  int len, ret;
  /* 64 bytes of input -> *up to* 48 bytes of output.
     Plus one more byte, in caes I'm wrong.
  */
  if (destlen < ((srclen/64)+1)*49)
    return -1;

  EVP_DecodeInit(&ctx);
  EVP_DecodeUpdate(&ctx, dest, &len, (char*) src, srclen);
  EVP_DecodeFinal(&ctx, dest, &ret);
  ret += len;
  return ret;
}

/** Implements base32 encoding as in rfc3548.  Limitation: Requires
 * that srclen is a multiple of 5.
 */
int
base32_encode(char *dest, int destlen, const char *src, int srclen)
{
  int nbits, i, bit, v, u;
  nbits = srclen * 8;

  if ((nbits%5) != 0)
    /* We need an even multiple of 5 bits. */
    return -1;
  if ((nbits/5)+1 > destlen)
    /* Not enough space. */
    return -1;

  for (i=0,bit=0; bit < nbits; ++i, bit+=5) {
    /* set v to the 16-bit value starting at src[bits/8], 0-padded. */
    v = ((uint8_t)src[bit/8]) << 8;
    if (bit+5<nbits) v += (uint8_t)src[(bit/8)+1];
    /* set u to the 5-bit value at the bit'th bit of src. */
    u = (v >> (11-(bit%8))) & 0x1F;
    dest[i] = BASE32_CHARS[u];
  }
  dest[i] = '\0';
  return 0;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
