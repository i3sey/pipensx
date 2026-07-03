#pragma once
#include "metainfo.h"
#include "piece.h"
#include "dht.h"
#include <stdint.h>

typedef struct torrent torrent_t;

typedef struct {
    const storage_file_config_t *files; /* metainfo.num_files entries */
    int strict_piece_order;
    const uint32_t *piece_order;
    uint32_t piece_order_count;
    int (*request_allowed)(void *user, uint32_t piece);
    void *request_allowed_user;
    uint32_t strict_order_lookahead; /* 0 = default */
    uint32_t request_pipeline_limit; /* per peer, 0 = MAX_PIPELINE */
    uint32_t hedge_after_ms; /* duplicate critical requests after this age */
    int strict_fill_pending_first;
    const char *telemetry_tag; /* copied by torrent_create_ex */
} torrent_options_t;

typedef struct {
    uint32_t num_pieces_done;
    uint32_t num_pieces;
    uint32_t num_peers;        /* occupied peer slots, incl. connecting */
    uint32_t num_active_peers; /* peers past handshake (PS_ACTIVE) */
    uint32_t dht_good;
    uint32_t dht_dubious;
    uint64_t downloaded;  /* bytes received during this session */
    uint64_t completed_bytes;
    uint64_t total_bytes;
    uint64_t speed_bps;   /* bytes/sec, updated ~1/sec */
    uint32_t num_pieces_verified;
    int      verifying;   /* startup or final verification is in progress */
    int      complete;    /* 1 when all pieces verified */
} torrent_stat_t;

/*
 * Create torrent engine.
 * Starts network (tracker, DHT) automatically.
 * listen_port: port for incoming peers and DHT.
 * outdir: where to write downloaded files.
 */
torrent_t *torrent_create(const metainfo_t *mi,
                          uint16_t listen_port,
                          const char *outdir);
torrent_t *torrent_create_ex(const metainfo_t *mi,
                             uint16_t listen_port,
                             const char *outdir,
                             const torrent_options_t *options);
void        torrent_destroy(torrent_t *t);

/* Queue compact IPv4 endpoints (4-byte address + 2-byte port, network order)
 * before the first tick. Returns the number accepted after validation and
 * deduplication. */
uint32_t torrent_add_initial_peers(torrent_t *t, const uint8_t *compact,
                                   uint32_t count);

/*
 * Run one tick of the event loop.
 * Returns 0 when download is complete.
 */
int  torrent_tick(torrent_t *t);

/* Fill stats for UI */
void torrent_stat(const torrent_t *t, torrent_stat_t *s);
const char *torrent_last_error(const torrent_t *t);

/*
 * Resize the strict-order lookahead window at runtime (PERF_PLAN 5.1).
 * No-op unless the torrent runs in strict piece order; lookahead 0 is ignored.
 */
void torrent_set_strict_lookahead(torrent_t *t, uint32_t lookahead);

/*
 * Freeze or resume per-peer download-rate sampling (PERF_PLAN 7.2). While
 * frozen, dl_rate EMAs hold their last value and intervals are discarded:
 * used when the application's request gate curtails new requests, so peers
 * idling through no fault of their own keep their pipeline depth.
 */
void torrent_set_rate_freeze(torrent_t *t, int freeze);
