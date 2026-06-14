#pragma once
#include "metainfo.h"
#include "piece.h"
#include "dht.h"
#include <stdint.h>

typedef struct torrent torrent_t;

typedef struct {
    uint32_t num_pieces_done;
    uint32_t num_pieces;
    uint32_t num_peers;
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
void        torrent_destroy(torrent_t *t);

/*
 * Run one tick of the event loop.
 * Returns 0 when download is complete.
 */
int  torrent_tick(torrent_t *t);

/* Fill stats for UI */
void torrent_stat(const torrent_t *t, torrent_stat_t *s);
