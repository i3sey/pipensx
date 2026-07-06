#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../core/metainfo.h"

/*
 * Announce to trackers and collect compact peer list.
 * Called synchronously (blocking, with timeout).
 * Returns number of peers found; fills compact_out (6 bytes each: ip+port BE).
 * max_peers = max entries that fit in compact_out.
 */
uint32_t tracker_announce(const metainfo_t *mi,
                          const uint8_t *peer_id,
                          uint16_t       listen_port,
                          int64_t        downloaded,
                          int64_t        left,
                          uint8_t       *compact_out,
                          uint32_t       max_peers);

/*
 * Announce a bare info hash to one tracker. This is used while resolving a
 * magnet, before a metainfo dictionary exists.
 */
uint32_t tracker_announce_url(const char    *url,
                              const uint8_t *info_hash,
                              const uint8_t *peer_id,
                              uint16_t       listen_port,
                              int64_t        downloaded,
                              int64_t        left,
                              uint8_t       *compact_out,
                              uint32_t       max_peers);

typedef struct tracker_announce_result {
    uint32_t peers;
    int request_ok;
    int tracker_failure;
    char failure_reason[128];
} tracker_announce_result_t;

typedef int (*tracker_cancel_cb)(void *user);

uint32_t tracker_announce_url_ex(const char    *url,
                                 const uint8_t *info_hash,
                                 const uint8_t *peer_id,
                                 uint16_t       listen_port,
                                 int64_t        downloaded,
                                 int64_t        left,
                                 uint8_t       *compact_out,
                                 uint32_t       max_peers,
                                 tracker_announce_result_t *result);

uint32_t tracker_announce_url_ex_cancel(
    const char *url, const uint8_t *info_hash, const uint8_t *peer_id,
    uint16_t listen_port, int64_t downloaded, int64_t left,
    uint8_t *compact_out, uint32_t max_peers,
    tracker_announce_result_t *result, tracker_cancel_cb cancel,
    void *cancel_user);
