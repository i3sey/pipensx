#include "torrent.h"
#include "piece.h"
#include "tracker.h"
#include "net.h"
#include "peer.h"
#include "util.h"
#include "../platform/storage.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#ifdef __SWITCH__
#  include <miniupnpc/miniupnpc.h>
#  include <miniupnpc/upnpcommands.h>
#endif

#define MAX_PEER_QUEUE 1024
#define CONNECT_INTERVAL_MS 50
#define CONNECT_TIMEOUT_MS  6000
#define TRACKER_REANNOUNCE_MS (30*60*1000ULL)  /* 30 min */
/* When the swarm is peer-starved, don't wait the full interval — re-announce
   early to pull in a fresh peer set (PERF_PLAN 1.6). Rate-limited so we never
   hammer the tracker more than once per this window. */
#define TRACKER_STARVED_REANNOUNCE_MS 15000ULL
#define TRACKER_STARVED_ACTIVE_PEERS  5
#define DHT_TICK_INTERVAL_MS  1000
#define PEER_TIMEOUT_MS       60000
#define REQUEST_TIMEOUT_MS    15000
#define MAX_ACTIVE_PEERS      MAX_PEERS
#define TELEMETRY_INTERVAL_MS 5000
#define MIN_REQUEST_PIPELINE  8
/* Adaptive per-peer request pipeline (PERF_PLAN 5.2): size the in-flight window
   to roughly PIPELINE_TARGET_MS of the peer's measured download rate. Until a
   peer has a rate sample, assume PIPELINE_BOOTSTRAP_BPS so it can ramp. */
#define PIPELINE_TARGET_MS     2000ULL
#define PIPELINE_BOOTSTRAP_BPS (1024ULL * 1024ULL)  /* 1 MiB/s */
#define TIMEOUT_COOLDOWN_BASE_MS 2000
#define TIMEOUT_COOLDOWN_MAX_MS  10000
#define TIMEOUT_DISCONNECT_STRIKES 3
#define TIMEOUT_DISCONNECT_IDLE_MS (REQUEST_TIMEOUT_MS * 2)
#define MAX_HEDGES_PER_TICK   4
#define MAX_HEDGED_BLOCKS     16
#define HEDGE_INTERVAL_MS     250
/* Adaptive hedge threshold (PERF_PLAN 5.1): duplicate a head-window request
   once it is HEDGE_LATENCY_MULT times older than the median active-peer block
   latency, instead of waiting for the static hedge_after_ms. The static value
   stays as the upper bound; the floor keeps a noisy estimate from spraying
   duplicates. Below HEDGE_MIN_LATENCY_PEERS sampled peers the static value is
   used — with one peer there is nobody to hedge to anyway. */
#define HEDGE_LATENCY_MULT      4
#define HEDGE_ADAPTIVE_MIN_MS   500
#define HEDGE_MIN_LATENCY_PEERS 2
#define MAX_PEER_TELEMETRY    8
#define PEER_BLOCKLIST_SIZE   64
#define PEER_BLOCKLIST_MS     60000

struct peer_addr {
    uint32_t ip;   /* network byte order */
    uint16_t port; /* network byte order */
};

static uint64_t ema_update(uint64_t previous, uint64_t sample) {
    if (sample >= previous)
        return previous + (sample - previous) * 3 / 10;
    return previous - (previous - sample) * 3 / 10;
}

struct torrent {
    metainfo_t   mi;
    piece_mgr_t *pm;
    storage_t   *store;
    dht_engine_t *dht;

    uint8_t peer_id[20];

    peer_t  *peers[MAX_ACTIVE_PEERS];
    int      num_peers;

    struct peer_addr queue[MAX_PEER_QUEUE];
    int      qhead, qtail, qsize;

    struct {
        uint32_t ip;
        uint16_t port;
        uint64_t until_ms;
    } blocklist[PEER_BLOCKLIST_SIZE];
    uint32_t blocklist_next;

    uint16_t listen_port;

    /* Stats */
    uint64_t downloaded;
    uint64_t speed_bytes; /* accumulated since last speed update */
    uint64_t speed_bps;
    uint64_t speed_time_ms;
    uint64_t last_health_ms;
    uint32_t expired_requests;

    char     telemetry_tag[48];
    uint32_t telemetry_generation;
    uint64_t telemetry_last_ms;
    uint64_t telemetry_cb_bytes;
    uint64_t telemetry_verified_bytes;
    uint64_t telemetry_cb_work_us;
    uint64_t telemetry_cb_max_us;
    uint32_t telemetry_cb_calls;
    uint32_t telemetry_expired_requests;
    uint32_t telemetry_hedged_requests;
    uint32_t telemetry_cancelled_requests;
    uint32_t telemetry_released_requests;

    uint32_t request_pipeline_limit;
    /* Requests are curtailed by the application's install gate (PERF_PLAN
       7.2): peers idle through no fault of their own, so per-peer rate EMAs
       hold instead of decaying to bootstrap. */
    int      rate_freeze;
    uint32_t hedge_after_ms;
    uint32_t hedge_effective_ms; /* last adaptive threshold, for telemetry */
    uint32_t schedule_cursor;
    uint64_t last_hedge_ms;

    uint64_t last_tracker_ms;
    uint64_t last_dht_ms;
    uint64_t last_connect_ms;

    /* Async tracker announce: tracker_announce() blocks up to 5 s per tracker,
       so it runs on a worker thread instead of freezing the event loop. The
       loop launches a run (announce_start), keeps servicing peers, and drains
       results on a later tick (announce_collect). async_ok is 0 only if the
       mutex failed to init, in which case announces fall back to synchronous. */
    pthread_t       announce_thread;
    pthread_mutex_t announce_mutex;
    int             async_ok;
    int             announce_active;  /* thread launched, not yet collected */
    int             announce_done;    /* guarded by announce_mutex */
    uint32_t        announce_count;   /* guarded by announce_mutex */
    uint8_t         announce_compact[200*6];
    int64_t         announce_downloaded;
    int64_t         announce_left;
    uint32_t startup_verify_index;
    int      startup_verifying;
    uint32_t final_verify_index;
    int      final_verifying;
    int      fatal_error;
    char     error[256];

#ifdef __SWITCH__
    pthread_t       upnp_thread;
    int             upnp_thread_active;
    struct UPNPUrls upnp_urls;
    struct IGDdatas upnp_data;
    char            upnp_lanaddr[64];
    char            upnp_port_str[16];
    int             upnp_mapped;
#endif
};

static void blocklist_add(torrent_t *t, uint32_t ip, uint16_t port,
                          uint64_t now) {
    uint32_t index = t->blocklist_next++ % PEER_BLOCKLIST_SIZE;
    t->blocklist[index].ip = ip;
    t->blocklist[index].port = port;
    t->blocklist[index].until_ms = now + PEER_BLOCKLIST_MS;
}

static int blocklist_blocked(const torrent_t *t, uint32_t ip, uint16_t port,
                             uint64_t now) {
    for (uint32_t i = 0; i < PEER_BLOCKLIST_SIZE; ++i) {
        if (t->blocklist[i].ip == ip && t->blocklist[i].port == port &&
            t->blocklist[i].until_ms > now)
            return 1;
    }
    return 0;
}

/* ---- peer queue ---- */
static int queue_insert(torrent_t *t, uint32_t ip, uint16_t port, int front) {
    uint16_t host_port = ntohs(port);
    if (ip == 0 || ip == INADDR_NONE || host_port < 2)
        return 0;
    uint32_t host_ip = ntohl(ip);
    uint8_t first = (uint8_t)(host_ip >> 24);
    uint8_t second = (uint8_t)(host_ip >> 16);
    if (first == 0 || first == 10 || first == 127 || first >= 224 ||
        (first == 169 && second == 254) ||
        (first == 172 && second >= 16 && second <= 31) ||
        (first == 192 && second == 168))
        return 0;
    if (t->qsize >= MAX_PEER_QUEUE) return 0;
    /* Dedup: skip if already queued or connected */
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (t->peers[i] && t->peers[i]->addr.sin_addr.s_addr == ip &&
            t->peers[i]->addr.sin_port == port) return 0;
    }
    for (int i = 0; i < t->qsize; i++) {
        int index = (t->qhead + i) % MAX_PEER_QUEUE;
        if (t->queue[index].ip == ip && t->queue[index].port == port)
            return 0;
    }
    if (blocklist_blocked(t, ip, port, now_ms()))
        return 0;
    int index;
    if (front) {
        t->qhead = (t->qhead + MAX_PEER_QUEUE - 1) % MAX_PEER_QUEUE;
        index = t->qhead;
    } else {
        index = t->qtail;
        t->qtail = (t->qtail + 1) % MAX_PEER_QUEUE;
    }
    t->queue[index].ip   = ip;
    t->queue[index].port = port;
    t->qsize++;
    return 1;
}

static int queue_push(torrent_t *t, uint32_t ip, uint16_t port) {
    return queue_insert(t, ip, port, 0);
}

static int queue_push_front(torrent_t *t, uint32_t ip, uint16_t port) {
    return queue_insert(t, ip, port, 1);
}

static int queue_pop(torrent_t *t, uint32_t *ip, uint16_t *port) {
    if (t->qsize == 0) return 0;
    *ip   = t->queue[t->qhead].ip;
    *port = t->queue[t->qhead].port;
    t->qhead = (t->qhead + 1) % MAX_PEER_QUEUE;
    t->qsize--;
    return 1;
}

uint32_t torrent_add_initial_peers(torrent_t *t, const uint8_t *compact,
                                   uint32_t count) {
    if (!t || !compact || count == 0)
        return 0;
    uint32_t accepted = 0;
    for (uint32_t i = count; i > 0; --i) {
        uint32_t ip;
        uint16_t port;
        const uint8_t *endpoint = compact + (i - 1) * 6;
        int seen_earlier = 0;
        for (uint32_t j = 0; j + 1 < i; ++j) {
            if (memcmp(endpoint, compact + j * 6, 6) == 0) {
                seen_earlier = 1;
                break;
            }
        }
        if (seen_earlier)
            continue;
        memcpy(&ip, endpoint, sizeof(ip));
        memcpy(&port, endpoint + sizeof(ip), sizeof(port));
        accepted += (uint32_t)queue_push_front(t, ip, port);
    }
    log_msg("[torrent] queued %u/%u verified initial peers\n",
            accepted, count);
    telemetry_log("torrent", t->telemetry_tag,
                  "event=initial_peers supplied=%u accepted=%u",
                  count, accepted);
    return accepted;
}

static void cancel_duplicate_requests(torrent_t *t, uint32_t piece,
                                      uint32_t offset, uint32_t len) {
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        peer_t *peer = t->peers[i];
        if (!peer || peer->state != PS_ACTIVE)
            continue;
        if (peer_cancel_block(peer, piece, offset, len)) {
            peer->telemetry_cancelled_requests++;
            t->telemetry_cancelled_requests++;
        }
    }
}

/* ---- callbacks ---- */
static void cb_block(void *ud, uint32_t idx, uint32_t off,
                     const uint8_t *data, uint32_t len) {
    torrent_t *t = (torrent_t*)ud;
    uint64_t started_us = telemetry_enabled() ? now_us() : 0;
    t->downloaded   += len;
    t->speed_bytes  += len;
    uint32_t block = off / BLOCK_SIZE;
    int duplicated = piece_mgr_block_request_count(t->pm, idx, block) > 1;
    int result = piece_mgr_got_block(t->pm, idx, off, data, len);
    if (result >= 1 && duplicated)
        cancel_duplicate_requests(t, idx, off, len);
    if (started_us) {
        uint64_t elapsed_us = now_us() - started_us;
        t->telemetry_cb_bytes += len;
        t->telemetry_cb_work_us += elapsed_us;
        if (elapsed_us > t->telemetry_cb_max_us)
            t->telemetry_cb_max_us = elapsed_us;
        t->telemetry_cb_calls++;
        if (result == 2)
            t->telemetry_verified_bytes += (uint64_t)piece_len(t->pm, idx);
    }
    if (result < 0) {
        t->fatal_error = 1;
        snprintf(t->error, sizeof(t->error), "%s",
                 storage_error(t->store)[0]
                    ? storage_error(t->store)
                    : "piece processing failed");
    }
}

static void reset_telemetry_window(torrent_t *t, uint64_t now) {
    t->telemetry_generation = telemetry_generation();
    t->telemetry_last_ms = now;
    t->telemetry_cb_bytes = 0;
    t->telemetry_verified_bytes = 0;
    t->telemetry_cb_work_us = 0;
    t->telemetry_cb_max_us = 0;
    t->telemetry_cb_calls = 0;
    t->telemetry_expired_requests = 0;
    t->telemetry_hedged_requests = 0;
    t->telemetry_cancelled_requests = 0;
    t->telemetry_released_requests = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        peer_t *peer = t->peers[i];
        if (!peer)
            continue;
        peer->telemetry_piece_bytes = 0;
        peer->telemetry_expired_requests = 0;
        peer->telemetry_hedged_requests = 0;
        peer->telemetry_cancelled_requests = 0;
        peer->telemetry_released_requests = 0;
    }
}

static int64_t last_piece_age_ms(uint64_t now, uint64_t last_piece_ms) {
    if (!last_piece_ms || last_piece_ms > now)
        return -1;
    return (int64_t)(now - last_piece_ms);
}

static void emit_telemetry(torrent_t *t, uint64_t now) {
    uint32_t generation = telemetry_generation();
    if (!telemetry_enabled()) {
        if (t->telemetry_generation != generation)
            reset_telemetry_window(t, now);
        return;
    }
    if (t->telemetry_generation != generation) {
        reset_telemetry_window(t, now);
        return;
    }
    uint64_t elapsed_ms = now - t->telemetry_last_ms;
    if (elapsed_ms < TELEMETRY_INTERVAL_MS)
        return;

    int connecting = 0, handshaking = 0, active = 0;
    int unchoked = 0, choked = 0, inflight = 0;
    int cooldown_peers = 0, penalized_peers = 0;
    uint64_t oldest_request_ms = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        peer_t *p = t->peers[i];
        if (!p)
            continue;
        if (p->state == PS_CONNECTING) {
            connecting++;
            continue;
        }
        if (p->state == PS_HANDSHAKE || p->state == PS_EXTENSION) {
            handshaking++;
            continue;
        }
        if (p->state != PS_ACTIVE)
            continue;
        active++;
        if (p->am_choked) choked++; else unchoked++;
        if (p->request_cooldown_until_ms > now) cooldown_peers++;
        if (p->timeout_strikes > 0) penalized_peers++;
        inflight += p->pipeline_len;
        for (int j = 0; j < p->pipeline_len; ++j) {
            uint64_t requested = p->pipeline[j].requested_ms;
            if (requested <= now && now - requested > oldest_request_ms)
                oldest_request_ms = now - requested;
        }
    }

    uint32_t head = UINT32_MAX;
    uint32_t head_done = 0, head_total = 0, head_requested = 0;
    uint32_t head_request_copies = 0, head_hedged = 0;
    uint32_t count = t->pm->piece_order_count
                   ? t->pm->piece_order_count : t->pm->num_pieces;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t idx = t->pm->piece_order_count
                     ? t->pm->piece_order[n] : n;
        if (idx >= t->pm->num_pieces ||
            t->pm->slots[idx].state == PS_DONE)
            continue;
        head = idx;
        piece_slot_t *slot = &t->pm->slots[idx];
        head_done = slot->num_blocks_done;
        head_total = slot->num_blocks;
        for (uint32_t block = 0; block < slot->num_blocks; ++block) {
            uint32_t requests = piece_mgr_block_request_count(
                t->pm, idx, block);
            if (requests > 0)
                head_requested++;
            head_request_copies += requests;
            if (requests > 1)
                head_hedged++;
        }
        break;
    }

    uint64_t rx_bps = t->telemetry_cb_bytes * 1000 / (elapsed_ms + 1);
    uint64_t verified_bps = t->telemetry_verified_bytes * 1000 /
                            (elapsed_ms + 1);
    uint64_t cb_busy_permille = t->telemetry_cb_work_us * 1000 /
                                (elapsed_ms * 1000 + 1);
    telemetry_log("torrent", t->telemetry_tag,
        "interval_ms=%llu rx_bps=%llu verified_bps=%llu speed_bps=%llu "
        "connecting=%d handshaking=%d active=%d unchoked=%d choked=%d "
        "inflight=%d expired=%u oldest_request_ms=%llu peer_queue=%d "
        "cooldown_peers=%d penalized_peers=%d request_limit=%u "
        "lookahead=%u hedge_ms=%u "
        "hedged=%u cancelled=%u released=%u "
        "head_piece=%u head_done=%u head_total=%u head_requested=%u "
        "head_request_copies=%u head_hedged=%u "
        "piece_cb_calls=%u piece_cb_busy_permille=%llu piece_cb_max_us=%llu",
        (unsigned long long)elapsed_ms,
        (unsigned long long)rx_bps,
        (unsigned long long)verified_bps,
        (unsigned long long)t->speed_bps,
        connecting, handshaking, active, unchoked, choked, inflight,
        t->telemetry_expired_requests,
        (unsigned long long)oldest_request_ms,
        t->qsize, cooldown_peers, penalized_peers,
        t->request_pipeline_limit,
        t->pm->strict_order_lookahead, t->hedge_effective_ms,
        t->telemetry_hedged_requests,
        t->telemetry_cancelled_requests, t->telemetry_released_requests,
        head, head_done, head_total, head_requested,
        head_request_copies, head_hedged,
        t->telemetry_cb_calls, (unsigned long long)cb_busy_permille,
        (unsigned long long)t->telemetry_cb_max_us);

    int selected[MAX_ACTIVE_PEERS] = {0};
    int emitted = 0;
    for (int pass = 0; pass < 2 && emitted < MAX_PEER_TELEMETRY; ++pass) {
        for (int i = 0; i < MAX_ACTIVE_PEERS &&
                        emitted < MAX_PEER_TELEMETRY; ++i) {
            peer_t *peer = t->peers[i];
            if (!peer || peer->state != PS_ACTIVE || selected[i])
                continue;
            int unhealthy = peer->timeout_strikes > 0 ||
                            peer->telemetry_expired_requests > 0;
            if ((pass == 0) != unhealthy)
                continue;
            selected[i] = 1;
            emitted++;
            uint64_t peer_rx_bps = peer->telemetry_piece_bytes * 1000 /
                                   (elapsed_ms + 1);
            int64_t last_piece_age = last_piece_age_ms(
                now, peer->last_piece_ms);
            uint64_t cooldown_ms = peer->request_cooldown_until_ms > now
                                 ? peer->request_cooldown_until_ms - now : 0;
            telemetry_log("peer", t->telemetry_tag,
                "rx_bps=%llu pipeline=%d unchoked=%d "
                "expired=%u hedged=%u cancelled=%u released=%u strikes=%u "
                "cooldown_ms=%llu last_piece_age_ms=%lld",
                (unsigned long long)peer_rx_bps,
                peer->pipeline_len, !peer->am_choked,
                peer->telemetry_expired_requests,
                peer->telemetry_hedged_requests,
                peer->telemetry_cancelled_requests,
                peer->telemetry_released_requests,
                peer->timeout_strikes,
                (unsigned long long)cooldown_ms,
                (long long)last_piece_age);
        }
    }
    reset_telemetry_window(t, now);
}

static void cb_have(void *ud, uint32_t idx) {
    (void)ud; (void)idx;
}

static void cb_peers(void *ud, const uint8_t *compact, uint32_t cnt) {
    torrent_t *t = (torrent_t*)ud;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t ip   = *(uint32_t*)(compact + i*6);
        uint16_t port = *(uint16_t*)(compact + i*6 + 4);
        queue_push(t, ip, port);
    }
}

static void clear_request(void *ud, const block_req_t *req) {
    torrent_t *t = (torrent_t*)ud;
    if (!t || !req || req->index < 0 || req->offset < 0)
        return;
    piece_mgr_clear_block_requested(t->pm, (uint32_t)req->index,
                                    (uint32_t)req->offset / BLOCK_SIZE);
}

static void clear_peer_requests(torrent_t *t, peer_t *p) {
    if (!t || !p)
        return;
    for (int i = 0; i < p->pipeline_len; i++)
        clear_request(t, &p->pipeline[i]);
    p->pipeline_len = 0;
}

static void cb_dht_peer(void *ud, uint32_t ip_be, uint16_t port_be) {
    torrent_t *t = (torrent_t*)ud;
    uint16_t host_port = ntohs(port_be);
    if (ip_be == 0 || ip_be == INADDR_NONE || host_port < 2)
        return;
    queue_push(t, ip_be, port_be);
}

/* ---- peer_ctx helper ---- */
static void fill_ctx(torrent_t *t, peer_ctx_t *ctx) {
    ctx->info_hash   = t->mi.info_hash;
    ctx->peer_id     = t->peer_id;
    ctx->num_pieces  = t->mi.num_pieces;
    ctx->bf_bytes    = (t->mi.num_pieces + 7) / 8;
    ctx->our_bf      = t->pm->available_bf;
    ctx->listen_port = t->listen_port;
}

/* ---- connect next peer from queue ---- */
static void try_connect(torrent_t *t) {
    if (t->num_peers >= MAX_ACTIVE_PEERS) return;
    uint32_t ip; uint16_t port;
    if (!queue_pop(t, &ip, &port)) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = port;

    socket_t fd = net_tcp_connect(&addr);
    if (fd == INVALID_SOCK) {
        blocklist_add(t, ip, port, now_ms());
        return;
    }

    peer_ctx_t ctx; fill_ctx(t, &ctx);
    peer_t *p = peer_create(fd, addr, &ctx);
    if (!p) {
        net_close(fd);
        blocklist_add(t, ip, port, now_ms());
        return;
    }

    p->dl_rate_bps = PIPELINE_BOOTSTRAP_BPS;

    /* Find free slot */
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (!t->peers[i]) {
            t->peers[i] = p;
            t->num_peers++;
            log_msg("[torrent] connecting peer\n");
            return;
        }
    }
    blocklist_add(t, ip, port, now_ms());
    peer_destroy(p);
}

/* ---- schedule block requests ---- */
static uint32_t peer_pipeline_limit(const torrent_t *t, const peer_t *p) {
    /* Bandwidth-delay heuristic (PERF_PLAN 5.2): size the in-flight window to
       ~PIPELINE_TARGET_MS of the peer's measured download rate, so a fast peer
       gets a deep queue up to the ceiling while a slow peer holds only what it
       can service. Replaces the flat per-peer constant that pinned every peer
       at request_pipeline_limit regardless of speed. */
    uint64_t want = p->dl_rate_bps * PIPELINE_TARGET_MS / 1000 / BLOCK_SIZE;
    uint32_t ceiling = t->request_pipeline_limit; /* per-peer max in flight */
    if (want > ceiling)
        want = ceiling;
    /* Back off geometrically for peers that keep timing out. */
    uint32_t shifts = p->timeout_strikes > 2 ? 2 : p->timeout_strikes;
    want >>= shifts;
    return want < MIN_REQUEST_PIPELINE ? MIN_REQUEST_PIPELINE : (uint32_t)want;
}

static int peer_has_piece(const peer_t *peer, uint32_t piece) {
    return peer && piece / 8 < peer->bf_bytes &&
           bf_has(peer->bitfield, piece);
}

static int peer_has_request(const peer_t *peer, uint32_t piece,
                            uint32_t offset) {
    for (int i = 0; peer && i < peer->pipeline_len; ++i) {
        if (peer->pipeline[i].index == (int)piece &&
            peer->pipeline[i].offset == (int)offset)
            return 1;
    }
    return 0;
}

static void schedule_requests(torrent_t *t, peer_t *p, uint64_t now) {
    if (p->am_choked || p->state != PS_ACTIVE ||
        p->request_cooldown_until_ms > now)
        return;
    uint32_t limit = peer_pipeline_limit(t, p);
    while ((uint32_t)p->pipeline_len < limit) {
        uint32_t pidx = piece_mgr_pick(t->pm, p->bitfield, p->bf_bytes);
        if (pidx == (uint32_t)-1) break;
        piece_mgr_mark_pending(t->pm, pidx);

        piece_slot_t *sl = &t->pm->slots[pidx];
        int64_t plen = piece_len(t->pm, pidx);
        uint32_t nb   = sl->num_blocks;
        int queued = 0;

        for (uint32_t b = 0; b < nb &&
                           (uint32_t)p->pipeline_len < limit; b++) {
            /* Skip blocks already received */
            if (piece_mgr_has_block(t->pm, pidx, b)) continue;
            if (piece_mgr_block_requested(t->pm, pidx, b)) continue;
            /* Skip blocks already in this peer's pipeline */
            uint32_t off  = b * BLOCK_SIZE;
            uint32_t blen = ((int64_t)off + BLOCK_SIZE <= plen)
                            ? BLOCK_SIZE : (uint32_t)(plen - off);
            int in_pipe = 0;
            for (int j = 0; j < p->pipeline_len; j++) {
                if (p->pipeline[j].index == (int)pidx &&
                    p->pipeline[j].offset == (int)off) { in_pipe = 1; break; }
            }
            if (in_pipe) continue;
            if (peer_request_block(p, pidx, off, blen)) {
                piece_mgr_mark_block_requested(t->pm, pidx, b);
                queued++;
            }
        }
        if (!queued) break; /* nothing left to request for this piece */
    }
}

static void schedule_all_peers(torrent_t *t, uint64_t now) {
    uint32_t start = t->schedule_cursor++ % MAX_ACTIVE_PEERS;
    for (uint32_t n = 0; n < MAX_ACTIVE_PEERS; ++n) {
        uint32_t index = (start + n) % MAX_ACTIVE_PEERS;
        peer_t *peer = t->peers[index];
        if (peer)
            schedule_requests(t, peer, now);
    }
}

static uint32_t current_head_piece(const torrent_t *t) {
    uint32_t count = t->pm->piece_order_count
                   ? t->pm->piece_order_count : t->pm->num_pieces;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t piece = t->pm->piece_order_count
                       ? t->pm->piece_order[n] : n;
        if (piece < t->pm->num_pieces &&
            t->pm->slots[piece].state != PS_DONE)
            return piece;
    }
    return UINT32_MAX;
}

static peer_t *pick_hedge_peer(torrent_t *t, const peer_t *primary,
                               uint32_t piece, uint32_t offset,
                               uint64_t now) {
    peer_t *best = NULL;
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        peer_t *peer = t->peers[i];
        if (!peer || peer == primary || peer->state != PS_ACTIVE ||
            peer->am_choked || peer->request_cooldown_until_ms > now ||
            !peer_has_piece(peer, piece) ||
            peer_has_request(peer, piece, offset) ||
            (uint32_t)peer->pipeline_len >= peer_pipeline_limit(t, peer))
            continue;
        if (!best || peer->timeout_strikes < best->timeout_strikes ||
            (peer->timeout_strikes == best->timeout_strikes &&
             peer->last_piece_ms > best->last_piece_ms) ||
            (peer->timeout_strikes == best->timeout_strikes &&
             peer->last_piece_ms == best->last_piece_ms &&
             peer->pipeline_len < best->pipeline_len)) {
            best = peer;
        }
    }
    return best;
}

/* PERF_PLAN 5.1: hedge threshold derived from the swarm's measured block
   latency — median of per-peer latency EMAs times HEDGE_LATENCY_MULT, clamped
   to [HEDGE_ADAPTIVE_MIN_MS, hedge_after_ms]. */
static uint32_t adaptive_hedge_after_ms(const torrent_t *t) {
    uint32_t latencies[MAX_ACTIVE_PEERS];
    uint32_t count = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        const peer_t *peer = t->peers[i];
        if (peer && peer->state == PS_ACTIVE && peer->block_lat_ema_ms)
            latencies[count++] = peer->block_lat_ema_ms;
    }
    if (count < HEDGE_MIN_LATENCY_PEERS)
        return t->hedge_after_ms;
    /* Insertion sort — count <= MAX_ACTIVE_PEERS, runs at most 1/HEDGE_INTERVAL. */
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t value = latencies[i];
        uint32_t j = i;
        while (j > 0 && latencies[j - 1] > value) {
            latencies[j] = latencies[j - 1];
            j--;
        }
        latencies[j] = value;
    }
    uint64_t threshold = (uint64_t)latencies[count / 2] * HEDGE_LATENCY_MULT;
    if (threshold < HEDGE_ADAPTIVE_MIN_MS)
        threshold = HEDGE_ADAPTIVE_MIN_MS;
    if (threshold > t->hedge_after_ms)
        threshold = t->hedge_after_ms;
    return (uint32_t)threshold;
}

static void schedule_hedged_requests(torrent_t *t, uint64_t now) {
    if (!t->hedge_after_ms || !t->pm->strict_order)
        return;
    if (t->last_hedge_ms <= now &&
        now - t->last_hedge_ms < HEDGE_INTERVAL_MS)
        return;
    t->last_hedge_ms = now;
    uint32_t head = current_head_piece(t);
    if (head == UINT32_MAX)
        return;
    uint32_t hedge_after_ms = adaptive_hedge_after_ms(t);
    t->hedge_effective_ms = hedge_after_ms;

    uint32_t outstanding = 0;
    piece_slot_t *headSlot = &t->pm->slots[head];
    for (uint32_t block = 0; block < headSlot->num_blocks; ++block) {
        if (piece_mgr_block_request_count(t->pm, head, block) > 1)
            outstanding++;
    }
    if (outstanding >= MAX_HEDGED_BLOCKS)
        return;
    uint32_t budget = MAX_HEDGED_BLOCKS - outstanding;
    if (budget > MAX_HEDGES_PER_TICK)
        budget = MAX_HEDGES_PER_TICK;
    uint32_t hedged = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS &&
                    hedged < budget; ++i) {
        peer_t *primary = t->peers[i];
        if (!primary || primary->state != PS_ACTIVE)
            continue;
        for (int j = 0; j < primary->pipeline_len &&
                        hedged < budget; ++j) {
            block_req_t request = primary->pipeline[j];
            if (request.index != (int)head || request.offset < 0 ||
                request.requested_ms > now ||
                now - request.requested_ms < hedge_after_ms)
                continue;
            uint32_t block = (uint32_t)request.offset / BLOCK_SIZE;
            if (piece_mgr_has_block(t->pm, head, block) ||
                piece_mgr_block_request_count(t->pm, head, block) != 1)
                continue;
            peer_t *candidate = pick_hedge_peer(
                t, primary, head, (uint32_t)request.offset, now);
            if (!candidate)
                continue;
            if (peer_request_block(candidate, head,
                                   (uint32_t)request.offset,
                                   (uint32_t)request.length)) {
                piece_mgr_mark_block_requested(t->pm, head, block);
                candidate->telemetry_hedged_requests++;
                t->telemetry_hedged_requests++;
                hedged++;
            }
        }
    }
}

static int check_completion(torrent_t *t) {
    if (t->pm->num_done != t->pm->num_pieces) {
        t->final_verifying = 0;
        t->final_verify_index = 0;
        return 1;
    }

    if (!t->final_verifying) {
        if (!storage_flush(t->store)) {
            log_msg("[torrent] final storage flush failed\n");
            return 1;
        }
        t->final_verifying = 1;
        t->final_verify_index = 0;
        log_msg("[torrent] final verification started\n");
    }

    if (t->final_verify_index < t->pm->num_pieces) {
        uint32_t idx = t->final_verify_index;
        if (!piece_mgr_verify_piece(t->pm, idx)) {
            t->final_verifying = 0;
            t->final_verify_index = 0;
            return 1;
        }
        t->final_verify_index++;
        return 1;
    }

    return 0;
}

/* ---- async tracker announce ---- */
static void announce_push_results(torrent_t *t, const uint8_t *compact,
                                  uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        queue_push(t, *(uint32_t*)(compact+i*6), *(uint16_t*)(compact+i*6+4));
}

static void *announce_worker(void *arg) {
    torrent_t *t = (torrent_t*)arg;
    /* mi/peer_id/listen_port are immutable for the torrent's life;
       downloaded/left were snapshotted before the thread was launched. */
    uint8_t compact[200*6];
    uint32_t n = tracker_announce(&t->mi, t->peer_id, t->listen_port,
                                  t->announce_downloaded, t->announce_left,
                                  compact, 200);
    pthread_mutex_lock(&t->announce_mutex);
    memcpy(t->announce_compact, compact, (size_t)n*6);
    t->announce_count = n;
    t->announce_done  = 1;
    pthread_mutex_unlock(&t->announce_mutex);
    return NULL;
}

/* Kick off an announce. No-op if one is already in flight. Falls back to a
   synchronous announce if threading is unavailable or spawn fails. */
static void announce_start(torrent_t *t, int64_t downloaded, int64_t left) {
    if (t->announce_active)
        return;
    t->announce_downloaded = downloaded;
    t->announce_left       = left;
    if (t->async_ok) {
        t->announce_done  = 0;
        t->announce_count = 0;
        if (pthread_create(&t->announce_thread, NULL, announce_worker, t) == 0) {
            t->announce_active = 1;
            return;
        }
        log_msg("[torrent] announce thread spawn failed, running inline\n");
    }
    uint8_t compact[200*6];
    uint32_t n = tracker_announce(&t->mi, t->peer_id, t->listen_port,
                                  downloaded, left, compact, 200);
    announce_push_results(t, compact, n);
    log_msg("[torrent] announce (sync): %u peers\n", n);
}

/* Drain a finished async announce into the peer queue. Called every tick. */
static void announce_collect(torrent_t *t) {
    if (!t->announce_active)
        return;
    pthread_mutex_lock(&t->announce_mutex);
    int done = t->announce_done;
    pthread_mutex_unlock(&t->announce_mutex);
    if (!done)
        return;
    pthread_join(t->announce_thread, NULL); /* publishes worker's writes */
    t->announce_active = 0;
    announce_push_results(t, t->announce_compact, t->announce_count);
    log_msg("[torrent] announce (async): %u peers\n", t->announce_count);
}

#ifdef __SWITCH__
/* UPnP discover + port mapping blocks ~2 s; run it once in the background at
   start so torrent_create_ex returns immediately. Joined in torrent_destroy. */
static void *upnp_worker(void *arg) {
    torrent_t *t = (torrent_t*)arg;
    int upnp_err = 0;
    struct UPNPDev *devlist = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &upnp_err);
    if (devlist) {
        int ret = UPNP_GetValidIGD(devlist, &t->upnp_urls, &t->upnp_data,
                                   t->upnp_lanaddr, sizeof(t->upnp_lanaddr));
        if (ret == 1) {
            snprintf(t->upnp_port_str, sizeof(t->upnp_port_str), "%u",
                     (unsigned)t->listen_port);
            int r1 = UPNP_AddPortMapping(t->upnp_urls.controlURL,
                t->upnp_data.first.servicetype,
                t->upnp_port_str, t->upnp_port_str,
                t->upnp_lanaddr, "pipensx", "TCP", NULL, "0");
            int r2 = UPNP_AddPortMapping(t->upnp_urls.controlURL,
                t->upnp_data.first.servicetype,
                t->upnp_port_str, t->upnp_port_str,
                t->upnp_lanaddr, "pipensx", "UDP", NULL, "0");
            t->upnp_mapped = (r1 == UPNPCOMMAND_SUCCESS || r2 == UPNPCOMMAND_SUCCESS);
            log_msg("[upnp] port %u: TCP=%s UDP=%s\n", (unsigned)t->listen_port,
                    r1 == UPNPCOMMAND_SUCCESS ? "ok" : "fail",
                    r2 == UPNPCOMMAND_SUCCESS ? "ok" : "fail");
        } else {
            log_msg("[upnp] no IGD found (ret=%d)\n", ret);
        }
        freeUPNPDevlist(devlist);
    } else {
        log_msg("[upnp] discover failed (err=%d)\n", upnp_err);
    }
    return NULL;
}
#endif

/* ---- create ---- */
torrent_t *torrent_create_ex(const metainfo_t *mi,
                             uint16_t listen_port,
                             const char *outdir,
                             const torrent_options_t *options) {
    torrent_t *t = (torrent_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    memcpy(&t->mi, mi, sizeof(*mi));
    t->listen_port = listen_port;
    t->startup_verifying = 1;
    t->request_pipeline_limit = options && options->request_pipeline_limit
                              ? options->request_pipeline_limit
                              : MAX_PIPELINE;
    if (t->request_pipeline_limit > MAX_PIPELINE)
        t->request_pipeline_limit = MAX_PIPELINE;
    if (t->request_pipeline_limit < MIN_REQUEST_PIPELINE)
        t->request_pipeline_limit = MIN_REQUEST_PIPELINE;
    t->hedge_after_ms = options ? options->hedge_after_ms : 0;
    t->hedge_effective_ms = t->hedge_after_ms;
    if (options && options->telemetry_tag)
        snprintf(t->telemetry_tag, sizeof(t->telemetry_tag), "%s",
                 options->telemetry_tag);
    else
        snprintf(t->telemetry_tag, sizeof(t->telemetry_tag), "-");
    reset_telemetry_window(t, now_ms());

    /* Peer ID: "-PN0001-" + 12 random bytes */
    memcpy(t->peer_id, "-PN0001-", 8);
    rand_bytes(t->peer_id + 8, 12);

    t->store = storage_open_ex(mi, outdir,
                               options ? options->files : NULL);
    if (!t->store) { free(t); return NULL; }

    t->pm = piece_mgr_create_ex(mi, t->store,
                                options ? options->strict_piece_order : 0,
                                options ? options->piece_order : NULL,
                                options ? options->piece_order_count : 0);
    if (!t->pm) { storage_close(t->store); free(t); return NULL; }
    if (options) {
        t->pm->request_allowed = options->request_allowed;
        t->pm->request_allowed_user = options->request_allowed_user;
        piece_mgr_set_strict_policy(t->pm,
                                    options->strict_order_lookahead,
                                    options->strict_fill_pending_first);
    }

    /* DHT */
    uint8_t dht_id[20];
    rand_bytes(dht_id, 20);
    t->dht = dht_engine_create(listen_port, dht_id);
    if (t->dht) {
        dht_engine_bootstrap(t->dht);
        dht_engine_search(t->dht, mi->info_hash, cb_dht_peer, t);
    }

    t->async_ok = (pthread_mutex_init(&t->announce_mutex, NULL) == 0);
    if (!t->async_ok)
        log_msg("[torrent] announce mutex init failed, announces run inline\n");

#ifdef __SWITCH__
    /* UPnP port mapping — best-effort, one-shot in the background so it does
       not block startup. Joined in torrent_destroy. */
    if (pthread_create(&t->upnp_thread, NULL, upnp_worker, t) == 0)
        t->upnp_thread_active = 1;
    else
        log_msg("[upnp] worker spawn failed\n");
#endif

    /* Kick off the first tracker announce off-thread; peers are drained by
       announce_collect on an upcoming torrent_tick. */
    t->last_tracker_ms  = now_ms();
    t->speed_time_ms    = now_ms();
    t->last_health_ms   = t->speed_time_ms;
    announce_start(t, 0, (int64_t)mi->total_length);

    log_msg("[torrent] started: tracker announce dispatched\n");
    telemetry_log("torrent", t->telemetry_tag,
                  "event=start pieces=%u piece_bytes=%lld trackers=%u "
                  "tracker_peers=%u "
                  "request_limit=%u lookahead=%u pending_first=%d "
                  "hedge_after_ms=%u",
                  mi->num_pieces, (long long)mi->piece_length,
                  mi->num_trackers, 0u, t->request_pipeline_limit,
                  t->pm->strict_order_lookahead,
                  t->pm->strict_fill_pending_first, t->hedge_after_ms);
    return t;
}

torrent_t *torrent_create(const metainfo_t *mi,
                          uint16_t listen_port,
                          const char *outdir) {
    return torrent_create_ex(mi, listen_port, outdir, NULL);
}

void torrent_destroy(torrent_t *t) {
    if (!t) return;
    log_msg("[torrent] destroy begin\n");
    /* Join any in-flight announce (bounded by the tracker curl timeout) before
       tearing down state it reads. */
    if (t->announce_active) {
        pthread_join(t->announce_thread, NULL);
        t->announce_active = 0;
    }
    if (t->async_ok) {
        pthread_mutex_destroy(&t->announce_mutex);
        t->async_ok = 0;
    }
#ifdef __SWITCH__
    /* Join the UPnP worker so upnp_mapped/urls are fully written before use. */
    if (t->upnp_thread_active) {
        pthread_join(t->upnp_thread, NULL);
        t->upnp_thread_active = 0;
    }
    if (t->upnp_mapped) {
        log_msg("[torrent] removing UPnP mapping\n");
        UPNP_DeletePortMapping(t->upnp_urls.controlURL,
            t->upnp_data.first.servicetype,
            t->upnp_port_str, "TCP", NULL);
        UPNP_DeletePortMapping(t->upnp_urls.controlURL,
            t->upnp_data.first.servicetype,
            t->upnp_port_str, "UDP", NULL);
        FreeUPNPUrls(&t->upnp_urls);
    }
#endif
    if (t->dht) {
        log_msg("[torrent] destroying DHT\n");
        dht_engine_destroy(t->dht);
        t->dht = NULL;
        log_msg("[torrent] DHT destroyed\n");
    }
    log_msg("[torrent] destroying peers\n");
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++)
        if (t->peers[i]) {
            peer_destroy(t->peers[i]);
            t->peers[i] = NULL;
        }
    log_msg("[torrent] peers destroyed\n");
    piece_mgr_destroy(t->pm);
    t->pm = NULL;
    log_msg("[torrent] piece manager destroyed\n");
    storage_close(t->store);
    t->store = NULL;
    log_msg("[torrent] storage closed\n");
    /* NOTE: t->mi is a shallow copy of the caller's metainfo_t — the caller
     * owns the heap members (piece_hashes, files) and must call metainfo_free()
     * on its own copy. Freeing here would cause a double-free on exit. */
    free(t);
    log_msg("[torrent] destroy complete\n");
}

/* Per-peer download-rate EMA feeds the adaptive request pipeline
   (PERF_PLAN 5.2). Sampled from the monotonic `downloaded` counter, so it
   needs no telemetry to be enabled. While the install gate curtails
   requests (rate_freeze, PERF_PLAN 7.2) a peer's low throughput says
   nothing about the peer: keep the EMA and discard the interval, so
   pipelines regain their pre-gate depth immediately on resume. */
static void sample_peer_rates(torrent_t *t, uint64_t elapsed_ms) {
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p) continue;
        if (t->rate_freeze) {
            p->rate_last_downloaded = p->downloaded;
            continue;
        }
        uint64_t sample = (p->downloaded - p->rate_last_downloaded) * 1000 /
                          (elapsed_ms + 1);
        p->dl_rate_bps = ema_update(p->dl_rate_bps, sample);
        p->rate_last_downloaded = p->downloaded;
    }
}

int torrent_tick(torrent_t *t) {
    if (t->fatal_error)
        return -1;
    if (t->startup_verifying) {
        if (t->startup_verify_index < t->pm->num_pieces) {
            piece_mgr_check_existing(t->pm, t->startup_verify_index);
            t->startup_verify_index++;
            return 1;
        }
        t->startup_verifying = 0;
        log_msg("[torrent] startup verification complete: %u/%u pieces\n",
                t->pm->num_done, t->pm->num_pieces);
    }

    if (t->pm->num_done == t->pm->num_pieces)
        return check_completion(t);

    uint64_t now = now_ms();

    /* Speed update every 1 second */
    if (now - t->speed_time_ms >= 1000) {
        uint64_t elapsed_ms = now - t->speed_time_ms;
        uint64_t sample_bps = t->speed_bytes * 1000 / (elapsed_ms + 1);
        t->speed_bps      = ema_update(t->speed_bps, sample_bps);
        t->speed_bytes    = 0;
        sample_peer_rates(t, elapsed_ms);
        t->speed_time_ms  = now;
    }
    emit_telemetry(t, now);
    if (now - t->last_health_ms >= 10000) {
        int active = 0;
        int unchoked = 0;
        int inflight = 0;
        for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
            peer_t *p = t->peers[i];
            if (!p || p->state != PS_ACTIVE)
                continue;
            active++;
            if (!p->am_choked)
                unchoked++;
            inflight += p->pipeline_len;
        }
        log_msg("[torrent] health active=%d unchoked=%d inflight=%d "
                "expired=%u speed=%llu\n",
                active, unchoked, inflight, t->expired_requests,
                (unsigned long long)t->speed_bps);
        t->expired_requests = 0;
        t->last_health_ms = now;
    }

    /* DHT tick */
    if (t->dht && now - t->last_dht_ms >= DHT_TICK_INTERVAL_MS) {
        dht_engine_tick(t->dht);
        t->last_dht_ms = now;
    }

    /* Re-announce tracker off-thread. Drain a finished run, then start the
       next one if due and none is in flight — never block the event loop. */
    announce_collect(t);
    if (!t->announce_active) {
        uint64_t since = now - t->last_tracker_ms;
        int due = since >= TRACKER_REANNOUNCE_MS;
        /* Not yet due on the normal 30-min interval, but past the starved
           window: re-announce early if we are short on active peers. The
           starved-window gate both rate-limits the tracker and keeps this
           active-peer count off the per-tick hot path. */
        if (!due && since >= TRACKER_STARVED_REANNOUNCE_MS) {
            int active = 0;
            for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
                peer_t *p = t->peers[i];
                if (p && p->state == PS_ACTIVE &&
                    ++active >= TRACKER_STARVED_ACTIVE_PEERS)
                    break;
            }
            if (active < TRACKER_STARVED_ACTIVE_PEERS) {
                due = 1;
                log_msg("[torrent] peer-starved (active=%d), re-announcing "
                        "early\n", active);
            }
        }
        if (due) {
            uint64_t announced_downloaded = t->downloaded;
            if (announced_downloaded > (uint64_t)t->mi.total_length)
                announced_downloaded = (uint64_t)t->mi.total_length;
            announce_start(t, (int64_t)announced_downloaded,
                           (int64_t)t->mi.total_length -
                               (int64_t)announced_downloaded);
            t->last_tracker_ms = now;
        }
    }

    /* Connect new peers */
    if (now - t->last_connect_ms >= CONNECT_INTERVAL_MS) {
        try_connect(t);
        t->last_connect_ms = now;
    }

    /* Build poll set */
    struct pollfd pfds[MAX_ACTIVE_PEERS + 2];
    int           pfd_peer[MAX_ACTIVE_PEERS + 2];
    int npfd = 0;

    /* DHT fd */
    int dht_pfd_idx = -1;
    if (t->dht) {
        pfds[npfd].fd      = dht_engine_fd(t->dht);
        pfds[npfd].events  = POLLIN;
        pfds[npfd].revents = 0;
        dht_pfd_idx = npfd++;
    }

    /* Peer fds */
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p || p->state == PS_DEAD) continue;
        pfds[npfd].fd     = p->fd;
        pfds[npfd].events = POLLIN |
            ((p->state == PS_CONNECTING || p->sbuf_len > 0) ? POLLOUT : 0);
        pfds[npfd].revents = 0;
        pfd_peer[npfd] = i;
        npfd++;
    }

    int r = poll(pfds, npfd, 10);
    if (r < 0) return 1;

    /* DHT readable */
    if (dht_pfd_idx >= 0 && pfds[dht_pfd_idx].revents & POLLIN)
        dht_engine_tick(t->dht);

    peer_ctx_t ctx; fill_ctx(t, &ctx);

    /* Peer events */
    for (int pi = (dht_pfd_idx >= 0 ? 1 : 0); pi < npfd; pi++) {
        if (!(pfds[pi].revents & (POLLIN|POLLOUT|POLLERR|POLLHUP))) continue;
        int slot = pfd_peer[pi];
        peer_t *p = t->peers[slot];
        if (!p) continue;

        int err = 0;
        if (p->state != PS_CONNECTING && (pfds[pi].revents & POLLOUT) &&
            !peer_flush(p))
            err = -1;
        if (err == 0)
            err = peer_recv(p, &ctx, cb_block, cb_have, cb_peers, t);
        if (err < 0) {
            log_msg("[torrent] peer connection closed\n");
            blocklist_add(t, p->addr.sin_addr.s_addr, p->addr.sin_port, now);
            clear_peer_requests(t, p);
            peer_destroy(p);
            t->peers[slot] = NULL;
            t->num_peers--;
            continue;
        }
        if (p->am_choked && p->pipeline_len > 0) {
            uint32_t released = (uint32_t)p->pipeline_len;
            clear_peer_requests(t, p);
            p->telemetry_released_requests += released;
            t->telemetry_released_requests += released;
            p->request_cooldown_until_ms = now_ms() +
                                           TIMEOUT_COOLDOWN_BASE_MS;
        }
    }

    /* Timeout sweep — separate pass after poll so last_recv_ms is fully updated */
    uint64_t now2 = now_ms();
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p) continue;
        /* Replace unreachable peers quickly so they do not occupy every slot. */
        if (p->state == PS_CONNECTING || p->state == PS_HANDSHAKE) {
            if (p->connect_time_ms <= now2 &&
                now2 - p->connect_time_ms > CONNECT_TIMEOUT_MS) {
                log_msg("[torrent] peer connect/handshake timeout\n");
                blocklist_add(t, p->addr.sin_addr.s_addr,
                              p->addr.sin_port, now2);
                peer_destroy(p);
                t->peers[i] = NULL;
                t->num_peers--;
            }
            continue;
        }
        if (p->state != PS_ACTIVE) continue;
        int expired = peer_expire_requests(p, now2, REQUEST_TIMEOUT_MS,
                                           clear_request, t);
        if (expired > 0) {
            t->expired_requests += (uint32_t)expired;
            t->telemetry_expired_requests += (uint32_t)expired;
            p->telemetry_expired_requests += (uint32_t)expired;
            if (p->timeout_strikes != UINT32_MAX)
                p->timeout_strikes++;
            uint64_t cooldown = TIMEOUT_COOLDOWN_BASE_MS *
                                (uint64_t)p->timeout_strikes;
            if (cooldown > TIMEOUT_COOLDOWN_MAX_MS)
                cooldown = TIMEOUT_COOLDOWN_MAX_MS;
            p->request_cooldown_until_ms = now2 + cooldown;
            telemetry_log("peer", t->telemetry_tag,
                "event=request_timeout expired=%d strikes=%u "
                "cooldown_ms=%llu pipeline=%d",
                expired, p->timeout_strikes,
                (unsigned long long)cooldown, p->pipeline_len);

            uint64_t progress_ms = p->last_piece_ms
                                 ? p->last_piece_ms : p->connect_time_ms;
            if (p->timeout_strikes >= TIMEOUT_DISCONNECT_STRIKES &&
                progress_ms <= now2 &&
                now2 - progress_ms >= TIMEOUT_DISCONNECT_IDLE_MS) {
                log_msg("[torrent] peer dropped after %u request "
                        "timeout strikes\n", p->timeout_strikes);
                blocklist_add(t, p->addr.sin_addr.s_addr,
                              p->addr.sin_port, now2);
                clear_peer_requests(t, p);
                peer_destroy(p);
                t->peers[i] = NULL;
                t->num_peers--;
                continue;
            }
        }
        /* Guard against unsigned underflow: only check if last_recv_ms <= now2 */
        if (p->last_recv_ms <= now2 && now2 - p->last_recv_ms > PEER_TIMEOUT_MS) {
            log_msg("[torrent] peer timeout\n");
            blocklist_add(t, p->addr.sin_addr.s_addr,
                          p->addr.sin_port, now2);
            clear_peer_requests(t, p);
            peer_destroy(p);
            t->peers[i] = NULL;
            t->num_peers--;
        }
    }

    /* Hedging runs before refill so old critical blocks get first choice. */
    schedule_hedged_requests(t, now2);
    schedule_all_peers(t, now2);

    return check_completion(t);
}

const char *torrent_last_error(const torrent_t *t) {
    return t && t->error[0] ? t->error : "";
}

void torrent_set_strict_lookahead(torrent_t *t, uint32_t lookahead) {
    if (!t || !t->pm || !t->pm->strict_order || !lookahead)
        return;
    if (t->pm->strict_order_lookahead == lookahead)
        return;
    piece_mgr_set_strict_policy(t->pm, lookahead,
                                t->pm->strict_fill_pending_first);
}

void torrent_set_rate_freeze(torrent_t *t, int freeze) {
    if (!t)
        return;
    freeze = freeze ? 1 : 0;
    if (t->rate_freeze != freeze)
        log_msg("[torrent] peer rate sampling %s\n",
                freeze ? "frozen (request gate)" : "resumed");
    t->rate_freeze = freeze;
}

void torrent_stat(const torrent_t *t, torrent_stat_t *s) {
    memset(s, 0, sizeof(*s));
    s->num_pieces_done = t->pm->num_done;
    s->num_pieces      = t->pm->num_pieces;
    s->num_peers       = (uint32_t)t->num_peers;
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (t->peers[i] && t->peers[i]->state == PS_ACTIVE)
            s->num_active_peers++;
    }
    if (t->dht) {
        int g=0, d=0;
        dht_engine_nodes((dht_engine_t*)t->dht, &g, &d);
        s->dht_good    = (uint32_t)g;
        s->dht_dubious = (uint32_t)d;
    }
    s->downloaded = t->downloaded;
    s->total_bytes = (uint64_t)t->mi.total_length;
    s->completed_bytes = t->pm->completed_bytes;
    s->speed_bps  = t->speed_bps;
    s->num_pieces_verified = t->startup_verifying
                           ? t->startup_verify_index
                           : t->final_verify_index;
    s->verifying = t->startup_verifying || t->final_verifying;
    s->complete = t->final_verifying &&
                  t->final_verify_index == t->pm->num_pieces &&
                  t->pm->num_done == t->pm->num_pieces;
}
