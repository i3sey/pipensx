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
