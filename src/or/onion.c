/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

extern or_options_t options; /* command-line and config-file options */

static int count_acceptable_routers(routerinfo_t **rarray, int rarray_len);

int decide_aci_type(uint32_t local_addr, uint16_t local_port,
                    uint32_t remote_addr, uint16_t remote_port) {

  if(local_addr > remote_addr)
    return ACI_TYPE_HIGHER;
  if(local_addr < remote_addr)
    return ACI_TYPE_LOWER;
  if(local_port > remote_port)
    return ACI_TYPE_HIGHER;
   /* else */
   return ACI_TYPE_LOWER; 
}

struct onion_queue_t {
  circuit_t *circ;
  struct onion_queue_t *next;
};

/* global (within this file) variables used by the next few functions */
static struct onion_queue_t *ol_list=NULL;
static struct onion_queue_t *ol_tail=NULL;
static int ol_length=0;

int onion_pending_add(circuit_t *circ) {
  struct onion_queue_t *tmp;

  tmp = tor_malloc(sizeof(struct onion_queue_t));
  memset(tmp, 0, sizeof(struct onion_queue_t));
  tmp->circ = circ;

  if(!ol_tail) {
    assert(!ol_list);
    assert(!ol_length);
    ol_list = tmp;
    ol_tail = tmp;
    ol_length++;
    return 0;
  }

  assert(ol_list);
  assert(!ol_tail->next);

  if(ol_length >= options.MaxOnionsPending) {
    log_fn(LOG_INFO,"Already have %d onions queued. Closing.", ol_length);
    free(tmp);
    return -1;
  }

  ol_length++;
  ol_tail->next = tmp;
  ol_tail = tmp;
  return 0;

}

circuit_t *onion_next_task(void) {
  circuit_t *circ;

  if(!ol_list)
    return NULL; /* no onions pending, we're done */

  assert(ol_list->circ);
  if(!ol_list->circ->p_conn) {
    log_fn(LOG_INFO,"ol_list->circ->p_conn null, must have died?");
    onion_pending_remove(ol_list->circ);
    return onion_next_task(); /* recurse: how about the next one? */
  }

  assert(ol_length > 0);
  circ = ol_list->circ;
  onion_pending_remove(ol_list->circ);
  return circ;
}

/* go through ol_list, find the onion_queue_t element which points to
 * circ, remove and free that element. leave circ itself alone.
 */
void onion_pending_remove(circuit_t *circ) {
  struct onion_queue_t *tmpo, *victim;

  if(!ol_list)
    return; /* nothing here. */

  /* first check to see if it's the first entry */
  tmpo = ol_list;
  if(tmpo->circ == circ) {
    /* it's the first one. remove it from the list. */
    ol_list = tmpo->next;
    if(!ol_list)
      ol_tail = NULL;
    ol_length--;
    victim = tmpo;
  } else { /* we need to hunt through the rest of the list */
    for( ;tmpo->next && tmpo->next->circ != circ; tmpo=tmpo->next) ;
    if(!tmpo->next) {
      log_fn(LOG_DEBUG,"circ (p_aci %d) not in list, probably at cpuworker.",circ->p_aci);
      return;
    }
    /* now we know tmpo->next->circ == circ */
    victim = tmpo->next;
    tmpo->next = victim->next;
    if(ol_tail == victim)
      ol_tail = tmpo;
    ol_length--;
  }

  /* now victim points to the element that needs to be removed */

  free(victim); 
}

/* given a response payload and keys, initialize, then send a created cell back */
int onionskin_answer(circuit_t *circ, unsigned char *payload, unsigned char *keys) {
  unsigned char iv[16];
  cell_t cell;

  memset(iv, 0, 16);

  memset(&cell, 0, sizeof(cell_t));
  cell.command = CELL_CREATED;
  cell.aci = circ->p_aci;
  cell.length = DH_KEY_LEN;

  circ->state = CIRCUIT_STATE_OPEN;

  log_fn(LOG_DEBUG,"Entering.");

  memcpy(cell.payload, payload, DH_KEY_LEN);

  log_fn(LOG_DEBUG,"init cipher forward %d, backward %d.", *(int*)keys, *(int*)(keys+16));

  if (!(circ->n_crypto =
        crypto_create_init_cipher(CIRCUIT_CIPHER,keys,iv,0))) {
    log_fn(LOG_ERR,"Cipher initialization failed (n).");
    return -1;
  }

  if (!(circ->p_crypto =
        crypto_create_init_cipher(CIRCUIT_CIPHER,keys+16,iv,1))) {
    log_fn(LOG_ERR,"Cipher initialization failed (p).");
    return -1;
  }

  if(connection_write_cell_to_buf(&cell, circ->p_conn) < 0) {
    return -1;
  }
  log_fn(LOG_DEBUG,"Finished sending 'created' cell.");

  return 0;
}

/* uses a weighted coin with weight cw to choose a route length */
static int chooselen(double cw) {
  int len = 2;
  uint8_t coin;
  
  if ((cw < 0) || (cw >= 1)) /* invalid parameter */
    return -1;
  
  while(1)
  {
    if (CRYPTO_PSEUDO_RAND_INT(coin))
      return -1;
    
    if (coin > cw*255) /* don't extend */
      break;
    else
      len++;
  }
  
  return len;
}

/* returns an array of pointers to routent that define a new route through the OR network
 * int cw is the coin weight to use when choosing the route 
 * order of routers is from last to first
 */
static unsigned int *new_route(double cw, routerinfo_t **rarray, int rarray_len, int *routelen) {
  int i;
  int num_acceptable_routers;
  unsigned int *route;
  unsigned int oldchoice, choice;

  assert((cw >= 0) && (cw < 1) && (rarray) && (routelen) ); /* valid parameters */

  *routelen = chooselen(cw);
  if (*routelen == -1) {
    log(LOG_ERR,"Choosing route length failed.");
    return NULL;
  }
  log(LOG_DEBUG,"new_route(): Chosen route length %d.",*routelen);

  num_acceptable_routers = count_acceptable_routers(rarray, rarray_len);

  if(num_acceptable_routers < 2) {
    log(LOG_INFO,"new_route(): Not enough acceptable routers. Failing.");
    return NULL;
  }

  if(num_acceptable_routers < *routelen) {
    log(LOG_NOTICE,"new_route(): Cutting routelen from %d to %d.",*routelen, num_acceptable_routers);
    *routelen = num_acceptable_routers;
  }

  if(*routelen < 1) {
    log(LOG_ERR,"new_route(): Didn't find any acceptable routers. Failing.");
    return NULL;
  }

  /* allocate memory for the new route */
  route = (unsigned int *)tor_malloc(*routelen * sizeof(unsigned int));
 
  oldchoice = rarray_len;
  for(i=0;i<*routelen;i++) {
    log(LOG_DEBUG,"new_route(): Choosing hop %u.",i);
    if (CRYPTO_PSEUDO_RAND_INT(choice)) {
      free((void *)route);
      return NULL;
    }

    choice = choice % rarray_len;
    log(LOG_DEBUG,"new_route(): Contemplating router %u.",choice);
    if(choice == oldchoice ||
       (oldchoice < rarray_len && !crypto_pk_cmp_keys(rarray[choice]->onion_pkey, rarray[oldchoice]->onion_pkey)) ||
      (options.OnionRouter && !connection_twin_get_by_addr_port(rarray[choice]->addr, rarray[choice]->or_port))) {
      /* Same router as last choice, or router twin,
       *   or no routers with that key are connected to us.
       * Try again. */
      log(LOG_DEBUG,"new_route(): Picked a router %d that won't work as next hop.",choice);
      i--;
      continue;  
    }
    log(LOG_DEBUG,"new_route(): Chosen router %u for hop %u.",choice,i);
    oldchoice = choice;
    route[i] = choice;
  }
   
  return route;
}

static int count_acceptable_routers(routerinfo_t **rarray, int rarray_len) {
  int i, j;
  int num=0;
  connection_t *conn;

  for(i=0;i<rarray_len;i++) {
    log(LOG_DEBUG,"Contemplating whether router %d is a new option...",i);
    if(options.OnionRouter) {
      conn = connection_exact_get_by_addr_port(rarray[i]->addr, rarray[i]->or_port);
      if(!conn || conn->type != CONN_TYPE_OR || conn->state != OR_CONN_STATE_OPEN) {
        log(LOG_DEBUG,"Nope, %d is not connected.",i);
        goto next_i_loop;
      }
    }
    for(j=0;j<i;j++) {
      if(!crypto_pk_cmp_keys(rarray[i]->onion_pkey, rarray[j]->onion_pkey)) {
        /* these guys are twins. so we've already counted him. */
        log(LOG_DEBUG,"Nope, %d is a twin of %d.",i,j);
        goto next_i_loop;
      }
    }
    num++;
    log(LOG_DEBUG,"I like %d. num_acceptable_routers now %d.",i, num);
    next_i_loop:
      ; /* our compiler may need an explicit statement after the label */
  }

  return num;
}

crypt_path_t *onion_generate_cpath(routerinfo_t **firsthop) {
  int routelen; /* length of the route */
  unsigned int *route; /* hops in the route as an array of indexes into rarray */
  crypt_path_t *cpath=NULL;
  directory_t *dir;
  routerinfo_t **rarray;
  int rarray_len;
  int i;
  crypt_path_t *hop;
  routerinfo_t *router;
  struct in_addr netaddr;

  router_get_directory(&dir);
  rarray = dir->routers;
  rarray_len = dir->n_routers;

  /* choose a route */
  route = new_route(options.CoinWeight, rarray, rarray_len, &routelen);
  if (!route) {
    log(LOG_ERR,"onion_generate_cpath(): Error choosing a route through the OR network.");
    return NULL;
  }
  log(LOG_DEBUG,"onion_generate_cpath(): Chosen a route of length %u: ",routelen);

  *firsthop = rarray[route[routelen-1]];
  assert(*firsthop); /* should always be defined */

  for(i=0; i<routelen; i++) {
    netaddr.s_addr = htonl((rarray[route[i]])->addr);

    log(LOG_DEBUG,"onion_generate_cpath(): %u : %s:%u, %u/%u",routelen-i,
        inet_ntoa(netaddr),
        (rarray[route[i]])->or_port,
        (int) (rarray[route[i]])->onion_pkey,
        crypto_pk_keysize((rarray[route[i]])->onion_pkey));
  }

  /* create the cpath layer by layer, starting at the last hop */
  for (i=0;i<routelen;i++) {
    router = rarray[route[i]];

    /* build up the crypt_path */
    hop = (crypt_path_t *)tor_malloc(sizeof(crypt_path_t));
    memset(hop, 0, sizeof(crypt_path_t));

    /* link hop into the cpath, at the front */
    hop->next = cpath;
    hop->prev = NULL;
    hop->state = CPATH_STATE_CLOSED;
    if(cpath) {
      cpath->prev = hop;
    }
    cpath = hop;

    hop->port = rarray[route[i]]->or_port;
    hop->addr = rarray[route[i]]->addr;

    hop->package_window = CIRCWINDOW_START;
    hop->deliver_window = CIRCWINDOW_START;

    log(LOG_DEBUG,"onion_generate_cpath() : Building hop %u of crypt path.",i+1);
  }

  /* now link cpath->prev to the end of cpath */
  for(hop=cpath; hop->next; hop=hop->next) ;
  hop->next = cpath;
  cpath->prev = hop;

  free(route);
  return cpath;
}

/*----------------------------------------------------------------------*/

/* Given a router's public key, generates a 144-byte encrypted DH pubkey,
 * and stores it into onion_skin out.  Stores the DH private key into 
 * handshake_state_out for later completion of the handshake.
 *
 * The encrypted pubkey is formed as follows:
 *    16 bytes of symmetric key
 *   128 bytes of g^x for DH.
 * The first 128 bytes are RSA-encrypted with the server's public key,
 * and the last 16 are encrypted with the symmetric key.
 */
int
onion_skin_create(crypto_pk_env_t *dest_router_key,
                  crypto_dh_env_t **handshake_state_out,
                  char *onion_skin_out) /* Must be DH_ONIONSKIN_LEN bytes long */
{
  char iv[16];
  char *pubkey = NULL;
  crypto_dh_env_t *dh = NULL;
  crypto_cipher_env_t *cipher = NULL;
  int dhbytes, pkbytes;

  *handshake_state_out = NULL;
  memset(onion_skin_out, 0, DH_ONIONSKIN_LEN);
  memset(iv, 0, 16);

  if (!(dh = crypto_dh_new()))
    goto err;
  
  dhbytes = crypto_dh_get_bytes(dh);
  pkbytes = crypto_pk_keysize(dest_router_key);
  assert(dhbytes+16 == DH_ONIONSKIN_LEN);
  pubkey = (char *)tor_malloc(dhbytes+16);

  if (crypto_rand(16, pubkey))
    goto err;
  
  /* XXXX You can't just run around RSA-encrypting any bitstream: if it's
   *      greater than the RSA key, then OpenSSL will happily encrypt,
   *      and later decrypt to the wrong value.  So we set the first bit
   *      of 'pubkey' to 0.  This means that our symmetric key is really only
   *      127 bits long, but since it shouldn't be necessary to encrypt
   *      DH public keys values in the first place, we should be fine.
   */
  pubkey[0] &= 0x7f; 

  if (crypto_dh_get_public(dh, pubkey+16, dhbytes))
    goto err;

#ifdef DEBUG_ONION_SKINS
#define PA(a,n) \
  { int _i; for (_i = 0; _i<n; ++_i) printf("%02x ",((int)(a)[_i])&0xFF); }

  printf("Client: client g^x:");
  PA(pubkey+16,3);
  printf("...");
  PA(pubkey+141,3);
  puts("");

  printf("Client: client symkey:");
  PA(pubkey+0,16);
  puts("");
#endif

  cipher = crypto_create_init_cipher(ONION_CIPHER, pubkey, iv, 1);

  if (!cipher)
    goto err;

  if (crypto_pk_public_encrypt(dest_router_key, pubkey, pkbytes,
                               onion_skin_out, RSA_NO_PADDING)==-1)
    goto err;

  if (crypto_cipher_encrypt(cipher, pubkey+pkbytes, dhbytes+16-pkbytes,
                            onion_skin_out+pkbytes))
    goto err;

  free(pubkey);
  crypto_free_cipher_env(cipher);
  *handshake_state_out = dh;

  return 0;
 err:
  if (pubkey) free(pubkey);
  if (dh) crypto_dh_free(dh);
  if (cipher) crypto_free_cipher_env(cipher);
  return -1;
}

/* Given an encrypted DH public key as generated by onion_skin_create,
 * and the private key for this onion router, generate the 128-byte DH
 * reply, and key_out_len bytes of key material, stored in key_out.
 */
int
onion_skin_server_handshake(char *onion_skin, /* DH_ONIONSKIN_LEN bytes long */
                            crypto_pk_env_t *private_key,
                            char *handshake_reply_out, /* DH_KEY_LEN bytes long */
                            char *key_out,
                            int key_out_len)
{
  char buf[DH_ONIONSKIN_LEN];
  char iv[16];
  crypto_dh_env_t *dh = NULL;
  crypto_cipher_env_t *cipher = NULL;
  int pkbytes;
  int len;
  
  memset(iv, 0, 16);
  pkbytes = crypto_pk_keysize(private_key);

  if (crypto_pk_private_decrypt(private_key,
                                onion_skin, pkbytes,
                                buf, RSA_NO_PADDING) == -1)
    goto err;
  
#ifdef DEBUG_ONION_SKINS
  printf("Server: client symkey:");
  PA(buf+0,16);
  puts("");
#endif

  cipher = crypto_create_init_cipher(ONION_CIPHER, buf, iv, 0);

  if (crypto_cipher_decrypt(cipher, onion_skin+pkbytes, DH_ONIONSKIN_LEN-pkbytes,
                            buf+pkbytes))
    goto err;

#ifdef DEBUG_ONION_SKINS
  printf("Server: client g^x:");
  PA(buf+16,3);
  printf("...");
  PA(buf+141,3);
  puts("");
#endif
  
  dh = crypto_dh_new();
  if (crypto_dh_get_public(dh, handshake_reply_out, DH_KEY_LEN))
    goto err;

#ifdef DEBUG_ONION_SKINS
  printf("Server: server g^y:");
  PA(handshake_reply_out+0,3);
  printf("...");
  PA(handshake_reply_out+125,3);
  puts("");
#endif

  len = crypto_dh_compute_secret(dh, buf+16, DH_KEY_LEN, key_out, key_out_len);
  if (len < 0)
    goto err;

#ifdef DEBUG_ONION_SKINS
  printf("Server: key material:");
  PA(buf, DH_KEY_LEN);
  puts("");
  printf("Server: keys out:");
  PA(key_out, key_out_len);
  puts("");
#endif

  crypto_free_cipher_env(cipher);
  crypto_dh_free(dh);
  return 0;
 err:
  if (cipher) crypto_free_cipher_env(cipher);
  if (dh) crypto_dh_free(dh);

  return -1;
}

/* Finish the client side of the DH handshake.
 * Given the 128 byte DH reply as generated by onion_skin_server_handshake
 * and the handshake state generated by onion_skin_create, generate
 * key_out_len bytes of shared key material and store them in key_out.
 *
 * After the invocation, call crypto_dh_free on handshake_state.
 */
int
onion_skin_client_handshake(crypto_dh_env_t *handshake_state,
                            char *handshake_reply,/* Must be DH_KEY_LEN bytes long*/
                            char *key_out,
                            int key_out_len) 
{
  int len;
  assert(crypto_dh_get_bytes(handshake_state) == DH_KEY_LEN);
  
#ifdef DEBUG_ONION_SKINS
  printf("Client: server g^y:");
  PA(handshake_reply+0,3);
  printf("...");
  PA(handshake_reply+125,3);
  puts("");
#endif

  len = crypto_dh_compute_secret(handshake_state, handshake_reply, DH_KEY_LEN,
                                 key_out, key_out_len);
  if (len < 0)
    return -1;
  
#ifdef DEBUG_ONION_SKINS
  printf("Client: keys out:");
  PA(key_out, key_out_len);
  puts("");
#endif

  return 0;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
