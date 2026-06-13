#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net.h"

typedef struct dht_engine dht_engine_t;

typedef void (*dht_peer_cb)(void *ud, uint32_t ip_be, uint16_t port_be);

/*
 * Create DHT engine, bind UDP socket on listen_port.
 * node_id: 20 random bytes (our DHT identity).
 */
dht_engine_t *dht_engine_create(uint16_t listen_port,
                                const uint8_t node_id[20]);
void           dht_engine_destroy(dht_engine_t *e);

/*
 * Start searching for peers for info_hash.
 * on_peer callback fires for each found peer (ip/port in network byte order).
 */
void dht_engine_search(dht_engine_t *e, const uint8_t info_hash[20],
                       dht_peer_cb on_peer, void *ud);

/*
 * Bootstrap from well-known nodes.  Call once after create.
 */
void dht_engine_bootstrap(dht_engine_t *e);

/*
 * Call periodically from the main loop (pass current time).
 * Returns the udp socket fd (for inclusion in poll() set).
 */
int  dht_engine_fd(dht_engine_t *e);
void dht_engine_tick(dht_engine_t *e);  /* called when fd is readable OR on timeout */

/*
 * Save/load node cache to/from file (for fast restart).
 */
void dht_engine_save(dht_engine_t *e, const char *path);
void dht_engine_load(dht_engine_t *e, const char *path);

/* Stats */
void dht_engine_nodes(dht_engine_t *e, int *good, int *dubious);
