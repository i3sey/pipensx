#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net.h"

/*
 * Shared DHT engine. jech's dht.c keeps its state in globals, so the process
 * owns exactly one engine; concurrent torrents (and the magnet resolver)
 * attach to it instead of racing to create their own. The first attach binds
 * the UDP socket on DHT_SHARED_PORT, warm-starts from the node cache and
 * spawns a dedicated engine thread; later attaches only refcount. The last
 * detach joins the thread, persists the node cache and closes the socket.
 *
 * Each attachment is one info-hash search. Found peers land in a
 * per-session mailbox the owner drains from its own thread with
 * dht_session_poll — the engine thread never calls into attacher code.
 */
typedef struct dht_session dht_session_t;

#define DHT_SHARED_PORT 51413

/*
 * announce_port: this attachment's TCP listen port, announced to the swarm
 * (jech carries it per search); 0 = pure lookup without announce (magnet
 * resolver). Returns NULL only when the engine cannot start (UDP bind or
 * init failure). Attachments with the same info-hash share one search;
 * the most recent search issue wins the announce port.
 */
dht_session_t *dht_attach(const uint8_t info_hash[20], uint16_t announce_port);
void           dht_detach(dht_session_t *s);

/*
 * Drain peers found for this session's info-hash; call from the owning
 * thread. out: compact IPv4 endpoints (4-byte address + 2-byte port,
 * network order). Returns the number of endpoints written.
 */
int dht_session_poll(dht_session_t *s, uint8_t (*out)[6], int max);

/*
 * Node-cache persistence (fast warm start). Set the path once at startup,
 * before any session exists; NULL or "" disables persistence (the default).
 * The first attach of a busy period pings the cached nodes (live ones
 * re-enter the routing table with their true ID within ~1s) and reuses the
 * stored node ID; the last detach rewrites the cache with the current good
 * nodes (atomic tmp+rename, skipped when the table is empty).
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

/* Stats for the shared engine's routing table. */
void dht_shared_nodes(int *good, int *dubious);
