/* Copyright 2003 Roger Dingledine. */
/* See LICENSE for licensing information */
/* $Id$ */

/* TLS wrappers for The Onion Router.  (Unlike other tor functions, these
 * are prefixed with tor_ in order to avoid conflicting with OpenSSL
 * functions and variables.)
 */

#include "./crypto.h"
#include "./tortls.h"
#include "./util.h"

#include <assert.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/tls1.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>

struct tor_tls_context_st {
  SSL_CTX *ctx;
};

struct tor_tls_st {
  SSL *ssl;
  int socket;
  enum { 
    TOR_TLS_ST_HANDSHAKE, TOR_TLS_ST_OPEN, TOR_TLS_ST_GOTCLOSE, 
    TOR_TLS_ST_SENTCLOSE, TOR_TLS_ST_CLOSED
  } state;
  int isServer;
};

/* global tls context, keep it here because nobody else needs to touch it */
static tor_tls_context *global_tls_context=NULL;

#define _TOR_TLS_SYSCALL    -6
#define _TOR_TLS_ZERORETURN -5


/* These functions are declared in crypto.c but not exported. */
RSA *_crypto_pk_env_get_rsa(crypto_pk_env_t *env);
crypto_pk_env_t *_crypto_new_pk_env_rsa(RSA *rsa);

static int
tor_tls_get_error(tor_tls *tls, int r, int extra)
{
  int err = SSL_get_error(tls->ssl, r);
  switch (err) {
    case SSL_ERROR_NONE:
      return TOR_TLS_DONE;
    case SSL_ERROR_WANT_READ:
      return TOR_TLS_WANTREAD;
    case SSL_ERROR_WANT_WRITE:
      return TOR_TLS_WANTWRITE;
    case SSL_ERROR_SYSCALL:
      return extra ? _TOR_TLS_SYSCALL : TOR_TLS_ERROR;
    case SSL_ERROR_ZERO_RETURN:
      return extra ? _TOR_TLS_ZERORETURN : TOR_TLS_ERROR;
    default:
      return TOR_TLS_ERROR;
  }
}

static int always_accept_verify_cb(int preverify_ok, 
                                   X509_STORE_CTX *x509_ctx)
{
  /* XXXX Actually, this needs to get more complicated.  But for now,
     XXXX always accept peer certs. */
  return 1;
}

/* Generate a self-signed certificate with the private key 'rsa' and
 * commonName 'nickname', and write it, PEM-encoded, to the file named
 * by 'certfile'.  Return 0 on success, -1 for failure.
 */
int
tor_tls_write_certificate(char *certfile, crypto_pk_env_t *rsa, char *nickname)
{
  RSA *_rsa = NULL;
  time_t start_time, end_time;
  EVP_PKEY *pkey = NULL;
  X509 *x509 = NULL;
  X509_NAME *name = NULL;
  BIO *out = NULL;
  int nid;
  
  start_time = time(NULL);

  assert(rsa);
  if (!(_rsa = RSAPrivateKey_dup(_crypto_pk_env_get_rsa(rsa))))
    /* XXX we have a crypto_pk_dup_key(), it's a shame we can't use it here */
    return -1;
  if (!(pkey = EVP_PKEY_new()))
    return -1;
  if (!(EVP_PKEY_assign_RSA(pkey, _rsa)))
    return -1;
  if (!(x509 = X509_new()))
    return -1;
  if (!(X509_set_version(x509, 2)))
    return -1;
  if (!(ASN1_INTEGER_set(X509_get_serialNumber(x509), (long)start_time)))
    return -1;
  
  if (!(name = X509_NAME_new()))
    return -1;
  if ((nid = OBJ_txt2nid("organizationName")) != NID_undef) return -1;
  if (!(X509_NAME_add_entry_by_NID(name, nid, MBSTRING_ASC,
                                   "TOR", -1, -1, 0))) return -1;
  if ((nid = OBJ_txt2nid("commonName")) != NID_undef) return -1;
  if (!(X509_NAME_add_entry_by_NID(name, nid, MBSTRING_ASC,
                                   nickname, -1, -1, 0))) return -1;
  
  if (!(X509_set_issuer_name(x509, name)))
    return -1;
  if (!(X509_set_subject_name(x509, name)))
    return -1;
  if (!X509_time_adj(X509_get_notBefore(x509),0,&start_time))
    return -1;
  end_time = start_time + 24*60*60*365;
  if (!X509_time_adj(X509_get_notAfter(x509),0,&end_time))
    return -1;
  if (!X509_set_pubkey(x509, pkey))
    return -1;
  if (!X509_sign(x509, pkey, EVP_sha1()))
    return -1;
  if (!(out = BIO_new_file(certfile, "w")))
    return -1;
  if (!(PEM_write_bio_X509(out, x509)))
    return -1;
  BIO_free(out);
  X509_free(x509);
  EVP_PKEY_free(pkey);
  X509_NAME_free(name);
  return 0;
}

/* Create a new TLS context.  If we are going to be using it as a
 * server, it must have isServer set to true, certfile set to a
 * filename for a certificate file, and RSA set to the private key
 * used for that certificate. Return -1 if failure, else 0.
 */
int
tor_tls_context_new(char *certfile, crypto_pk_env_t *rsa, int isServer)
{
  crypto_dh_env_t *dh = NULL;
  RSA *_rsa = NULL;
  EVP_PKEY *pkey = NULL;
  tor_tls_context *result;

  assert((certfile && rsa) || (!certfile && !rsa));

  result = tor_malloc(sizeof(tor_tls_context));
  if (!(result->ctx = SSL_CTX_new(TLSv1_method())))
    return -1;
  /* XXXX This should use AES, but we'll need to require OpenSSL 0.9.7 first */
  if (!SSL_CTX_set_cipher_list(result->ctx, TLS1_TXT_DHE_DSS_WITH_RC4_128_SHA))
                               /* TLS1_TXT_DHE_RSA_WITH_AES_128_SHA)) */
    return -1;
  if (certfile && !SSL_CTX_use_certificate_file(result->ctx,certfile,
                                                SSL_FILETYPE_PEM))
    return -1;
  SSL_CTX_set_session_cache_mode(result->ctx, SSL_SESS_CACHE_OFF);
  if (rsa) {
    if (!(_rsa = RSAPrivateKey_dup(_crypto_pk_env_get_rsa(rsa))))
      return -1;
    if (!(pkey = EVP_PKEY_new()))
      return -1;
    if (!EVP_PKEY_assign_RSA(pkey, _rsa))
      return -1;
    if (!SSL_CTX_use_PrivateKey(result->ctx, pkey))
      return -1;
    EVP_PKEY_free(pkey);
    if (certfile) {
      if (!SSL_CTX_check_private_key(result->ctx))
        return -1;
    }
  }
  dh = crypto_dh_new();
  SSL_CTX_set_tmp_dh(result->ctx, dh->dh);
  crypto_dh_free(dh);
  SSL_CTX_set_verify(result->ctx, SSL_VERIFY_PEER, 
                     always_accept_verify_cb);
  
  global_tls_context = result;
  return 0;
}

/* Create a new TLS object from a TLS context, a filedescriptor, and 
 * a flag to determine whether it is functioning as a server.
 */
tor_tls *
tor_tls_new(int sock, int isServer)
{
  tor_tls *result = tor_malloc(sizeof(tor_tls));
  assert(global_tls_context); /* make sure somebody made it first */
  if (!(result->ssl = SSL_new(global_tls_context->ctx)))
    return NULL;
  result->socket = sock;
  SSL_set_fd(result->ssl, sock);
  result->state = TOR_TLS_ST_HANDSHAKE;
  result->isServer = isServer;
  return result;
}

/* Release resources associated with a TLS object.  Does not close the
 * underlying file descriptor.
 */
void
tor_tls_free(tor_tls *tls)
{
  SSL_free(tls->ssl);
  free(tls);
}

/* Underlying function for TLS reading.  Reads up to 'len' characters
 * from 'tls' into 'cp'.  On success, returns the number of characters
 * read.  On failure, returns TOR_TLS_ERROR, TOR_TLS_CLOSE,
 * TOR_TLS_WANTREAD, or TOR_TLS_WANTWRITE.
 */
int
tor_tls_read(tor_tls *tls, char *cp, int len)
{
  int r, err;
  assert(tls && tls->ssl);
  assert(tls->state == TOR_TLS_ST_OPEN);
  r = SSL_read(tls->ssl, cp, len);
  if (r > 0)
    return r;
  err = tor_tls_get_error(tls, r, 1);
  if (err == _TOR_TLS_SYSCALL)
    return TOR_TLS_ERROR;
  else if (err == _TOR_TLS_ZERORETURN) {
    tls->state = TOR_TLS_ST_CLOSED;
    return TOR_TLS_CLOSE;
  } else {
    /*  XXXX Make sure it's not TOR_TLS_DONE. */
    return err;
  }
}

/* Underlying function for TLS writing.  Write up to 'n' characters
 * from 'cp' onto 'tls'.  On success, returns the number of characters
 * written.  On failure, returns TOR_TLS_ERROR, TOR_TLS_WANTREAD, 
 * or TOR_TLS_WANTWRITE.
 */
int
tor_tls_write(tor_tls *tls, char *cp, int n)
{
  int r, err;
  assert(tls && tls->ssl);
  assert(tls->state == TOR_TLS_ST_OPEN);
  r = SSL_write(tls->ssl, cp, n);
  err = tor_tls_get_error(tls, r, 1);
  if (err == _TOR_TLS_ZERORETURN) {
    /* should never happen XXXX */
    return 0;
  } else if (err == TOR_TLS_DONE) {
    return r;
  } else {
    return err;
  }  
}

/* Perform initial handshake on 'tls'.  When finished, returns
 * TOR_TLS_DONE.  On failure, returns TOR_TLS_ERROR, TOR_TLS_WANTREAD,
 * or TOR_TLS_WANNTWRITE.
 */
int
tor_tls_handshake(tor_tls *tls)
{
  int r;
  assert(tls && tls->ssl);
  assert(tls->state == TOR_TLS_ST_HANDSHAKE);
  if (tls->isServer) {
    r = SSL_accept(tls->ssl);
  } else {
    r = SSL_connect(tls->ssl);
  }
  r = tor_tls_get_error(tls,r,0);
  if (r == TOR_TLS_DONE) {
    tls->state = TOR_TLS_ST_OPEN; 
  }
  return r;
}

/* Shut down an open tls connection 'tls'.  When finished, returns
 * TOR_TLS_DONE.  On failure, returns TOR_TLS_ERROR, TOR_TLS_WANTREAD,
 * or TOR_TLS_WANTWRITE.
 */
int
tor_tls_shutdown(tor_tls *tls)
{
  int r, err;
  char buf[128];
  assert(tls && tls->ssl);

  if (tls->state == TOR_TLS_ST_SENTCLOSE) {
    do {
      r = SSL_read(tls->ssl, buf, 128);
    } while (r>0);
    err = tor_tls_get_error(tls, r, 1);
    if (err == _TOR_TLS_ZERORETURN) {
      tls->state = TOR_TLS_ST_GOTCLOSE;
      /* fall through */
    } else {
      if (err == _TOR_TLS_SYSCALL)
        err = TOR_TLS_ERROR;
      return err;
    }
  }

  r = SSL_shutdown(tls->ssl);
  if (r == 1) {
    tls->state = TOR_TLS_ST_CLOSED;
    return TOR_TLS_DONE;
  }
  err = tor_tls_get_error(tls, r, 1);
  if (err == _TOR_TLS_SYSCALL)
    return TOR_TLS_ST_CLOSED; /* XXXX is this right? */
  else if (err == _TOR_TLS_ZERORETURN) {
    if (tls->state == TOR_TLS_ST_GOTCLOSE || 
        tls->state == TOR_TLS_ST_SENTCLOSE) {
      /* XXXX log; unexpected. */
      return TOR_TLS_ERROR;
    }
    tls->state = TOR_TLS_ST_SENTCLOSE;
    return tor_tls_shutdown(tls);
  } else {
    /* XXXX log if not error. */
    return err;
  }
}

/* Return true iff this TLS connection is authenticated.
 */
int
tor_tls_peer_has_cert(tor_tls *tls)
{
  X509 *cert;
  if (!(cert = SSL_get_peer_certificate(tls->ssl)))
    return 0;
  X509_free(cert);
  return 1;
}

/* If the provided tls connection is authenticated and has a
 * certificate that is currently valid and is correctly self-signed,
 * return its public key.  Otherwise return NULL.
 */
crypto_pk_env_t *
tor_tls_verify(tor_tls *tls)
{
  X509 *cert = NULL;
  EVP_PKEY *pkey = NULL;
  RSA *rsa = NULL;
  time_t now;
  crypto_pk_env_t *r = NULL;
  if (!(cert = SSL_get_peer_certificate(tls->ssl)))
    return 0;
  
  now = time(NULL);
  if (X509_cmp_time(X509_get_notBefore(cert), &now) > 0)
    goto done;
  if (X509_cmp_time(X509_get_notAfter(cert), &now) < 0)
    goto done;
  
  /* Get the public key. */
  if (!(pkey = X509_get_pubkey(cert)))
    goto done;
  if (X509_verify(cert, pkey) <= 0)
    goto done;

  rsa = EVP_PKEY_get1_RSA(pkey);
  EVP_PKEY_free(pkey);
  pkey = NULL;
  if (!rsa)
    goto done;

  r = _crypto_new_pk_env_rsa(rsa);
  rsa = NULL;
  
 done:
  if (cert)
    X509_free(cert);
  if (pkey)
    EVP_PKEY_free(pkey);
  if (rsa)
    RSA_free(rsa);
  return r;
}
