#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../core/metainfo.h"
#include "../core/antizapret.h"

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

/*
 * RuTracker reachability probe (RF_ACCESS_PLAN W1).
 *
 * Sends a lightweight stub announce (zero info hash, numwant=0) to
 * `announce_url` through `route` (NULL = direct) with a short, cancellable
 * timeout, then classifies the outcome:
 *   REACHABLE — the tracker answered with a bencode reply,
 *   BLOCKED   — reached a DPI stub / a wall that is not the tracker,
 *   TIMEOUT   — transport failed (timeout / reset / refused / cancelled).
 * Shared by the connectivity wizard (W2) and the normal resolve path.
 * timeout_seconds <= 0 falls back to a built-in short default.
 */
typedef enum {
    TRACKER_PROBE_REACHABLE = 0,
    TRACKER_PROBE_BLOCKED,
    TRACKER_PROBE_TIMEOUT
} tracker_probe_result_t;

tracker_probe_result_t tracker_probe(const char *announce_url,
                                     const antizapret_route_t *route,
                                     int timeout_seconds,
                                     tracker_cancel_cb cancel_callback,
                                     void *cancel_user);

/*
 * Pure classifier for a completed probe attempt, exposed for testing without
 * a live network. transport_ok = curl returned CURLE_OK; status = HTTP status
 * code; body / body_len = response body (may be NULL / 0).
 */
tracker_probe_result_t tracker_probe_classify(int transport_ok, long status,
                                              const char *body,
                                              size_t body_len);
