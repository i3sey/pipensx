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
 * Node-cache persistence (fast warm start). Set the path once at startup,
 * before any engine exists; NULL or "" disables persistence (the default).
 * Every dht_engine_create then pings the cached nodes (live ones re-enter
 * the routing table with their true ID within ~1s) and reuses the stored
 * node ID; every dht_engine_destroy rewrites the cache with the current
 * good nodes (atomic tmp+rename, skipped when the table is empty).
 */
void dht_engine_set_cache_path(const char *path);

/*
 * Cache file codec, exposed for tests. Format: "PXD1" magic, 20-byte node
 * ID, u16 LE count (<= DHT_CACHE_MAX_NODES), then count compact endpoints
 * (4-byte IPv4 + 2-byte port, network order). Read returns the node count
 * (clamped to max_nodes) or -1 when the file is missing or malformed;
 * write returns 1 on success.
 */
#define DHT_CACHE_MAGIC "PXD1"
#define DHT_CACHE_MAX_NODES 256
int dht_cache_read(const char *path, uint8_t node_id[20],
                   uint8_t (*nodes)[6], int max_nodes);
int dht_cache_write(const char *path, const uint8_t node_id[20],
                    const uint8_t (*nodes)[6], int count);

/* Stats */
void dht_engine_nodes(dht_engine_t *e, int *good, int *dubious);
