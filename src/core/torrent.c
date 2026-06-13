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

#ifdef __SWITCH__
#  include <miniupnpc/miniupnpc.h>
#  include <miniupnpc/upnpcommands.h>
#endif

#define MAX_PEER_QUEUE 512
#define CONNECT_INTERVAL_MS 500
#define TRACKER_REANNOUNCE_MS (30*60*1000ULL)  /* 30 min */
#define DHT_TICK_INTERVAL_MS  1000
#define PEER_TIMEOUT_MS       60000
#define MAX_ACTIVE_PEERS      MAX_PEERS

struct peer_addr {
    uint32_t ip;   /* network byte order */
    uint16_t port; /* network byte order */
};

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

    uint16_t listen_port;

    /* Stats */
    uint64_t downloaded;
    uint64_t speed_bytes; /* accumulated since last speed update */
    uint64_t speed_bps;
    uint64_t speed_time_ms;

    uint64_t last_tracker_ms;
    uint64_t last_dht_ms;
    uint64_t last_connect_ms;

#ifdef __SWITCH__
    struct UPNPUrls upnp_urls;
    struct IGDdatas upnp_data;
    char            upnp_lanaddr[64];
    char            upnp_port_str[16];
    int             upnp_mapped;
#endif
};

/* ---- peer queue ---- */
static void queue_push(torrent_t *t, uint32_t ip, uint16_t port) {
    if (t->qsize >= MAX_PEER_QUEUE) return;
    /* Dedup: skip if already queued or connected */
    for (int i = 0; i < t->num_peers; i++) {
        if (t->peers[i] && t->peers[i]->addr.sin_addr.s_addr == ip &&
            t->peers[i]->addr.sin_port == port) return;
    }
    t->queue[t->qtail].ip   = ip;
    t->queue[t->qtail].port = port;
    t->qtail = (t->qtail + 1) % MAX_PEER_QUEUE;
    t->qsize++;
}

static int queue_pop(torrent_t *t, uint32_t *ip, uint16_t *port) {
    if (t->qsize == 0) return 0;
    *ip   = t->queue[t->qhead].ip;
    *port = t->queue[t->qhead].port;
    t->qhead = (t->qhead + 1) % MAX_PEER_QUEUE;
    t->qsize--;
    return 1;
}

/* ---- callbacks ---- */
static void cb_block(void *ud, uint32_t idx, uint32_t off,
                     const uint8_t *data, uint32_t len) {
    torrent_t *t = (torrent_t*)ud;
    t->downloaded   += len;
    t->speed_bytes  += len;
    piece_mgr_got_block(t->pm, idx, off, data, len);
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

static void cb_dht_peer(void *ud, uint32_t ip_be, uint16_t port_be) {
    torrent_t *t = (torrent_t*)ud;
    queue_push(t, ip_be, port_be);
    log_msg("[torrent] dht peer %u.%u.%u.%u:%u\n",
            ip_be&0xFF,(ip_be>>8)&0xFF,(ip_be>>16)&0xFF,(ip_be>>24)&0xFF,
            ntohs(port_be));
}

/* ---- peer_ctx helper ---- */
static void fill_ctx(torrent_t *t, peer_ctx_t *ctx) {
    ctx->info_hash   = t->mi.info_hash;
    ctx->peer_id     = t->peer_id;
    ctx->num_pieces  = t->mi.num_pieces;
    ctx->bf_bytes    = (t->mi.num_pieces + 7) / 8;
    ctx->our_bf      = t->pm->have_bf;
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
    if (fd == INVALID_SOCK) return;

    peer_ctx_t ctx; fill_ctx(t, &ctx);
    peer_t *p = peer_create(fd, addr, &ctx);
    if (!p) { net_close(fd); return; }

    /* Find free slot */
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (!t->peers[i]) {
            t->peers[i] = p;
            t->num_peers++;
            log_msg("[torrent] connecting %s\n", p->addr_str);
            return;
        }
    }
    peer_destroy(p);
}

/* ---- schedule block requests ---- */
static void schedule_requests(torrent_t *t, peer_t *p) {
    if (p->am_choked || p->state != PS_ACTIVE) return;
    while (p->pipeline_len < MAX_PIPELINE) {
        uint32_t pidx = piece_mgr_pick(t->pm, p->bitfield, p->bf_bytes);
        if (pidx == (uint32_t)-1) break;
        piece_mgr_mark_pending(t->pm, pidx);

        piece_slot_t *sl = &t->pm->slots[pidx];
        int64_t plen = piece_len(t->pm, pidx);
        uint32_t nb   = sl->num_blocks;
        int queued = 0;

        for (uint32_t b = 0; b < nb && p->pipeline_len < MAX_PIPELINE; b++) {
            /* Skip blocks already received */
            if (b < 32 && (sl->have_blocks & (1u << b))) continue;
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
            if (peer_request_block(p, pidx, off, blen)) queued++;
        }
        if (!queued) break; /* nothing left to request for this piece */
    }
}

/* ---- create ---- */
torrent_t *torrent_create(const metainfo_t *mi,
                          uint16_t listen_port,
                          const char *outdir) {
    torrent_t *t = (torrent_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    memcpy(&t->mi, mi, sizeof(*mi));
    t->listen_port = listen_port;

    /* Peer ID: "-PN0001-" + 12 random bytes */
    memcpy(t->peer_id, "-PN0001-", 8);
    rand_bytes(t->peer_id + 8, 12);

    t->store = storage_open(mi, outdir);
    if (!t->store) { free(t); return NULL; }

    t->pm = piece_mgr_create(mi, t->store);
    if (!t->pm) { storage_close(t->store); free(t); return NULL; }

    /* DHT */
    uint8_t dht_id[20];
    rand_bytes(dht_id, 20);
    t->dht = dht_engine_create(listen_port, dht_id);
    if (t->dht) {
#ifdef __SWITCH__
        dht_engine_load(t->dht, "/switch/pipensx/dht_nodes.bin");
#else
        dht_engine_load(t->dht, "/tmp/pipensx_dht.bin");
#endif
        dht_engine_bootstrap(t->dht);
        dht_engine_search(t->dht, mi->info_hash, cb_dht_peer, t);
    }

#ifdef __SWITCH__
    /* UPnP port mapping — best-effort, helps with NAT traversal for DHT/peers */
    {
        int upnp_err = 0;
        struct UPNPDev *devlist = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &upnp_err);
        if (devlist) {
            int ret = UPNP_GetValidIGD(devlist, &t->upnp_urls, &t->upnp_data,
                                       t->upnp_lanaddr, sizeof(t->upnp_lanaddr));
            if (ret == 1) {
                snprintf(t->upnp_port_str, sizeof(t->upnp_port_str), "%u", listen_port);
                int r1 = UPNP_AddPortMapping(t->upnp_urls.controlURL,
                    t->upnp_data.first.servicetype,
                    t->upnp_port_str, t->upnp_port_str,
                    t->upnp_lanaddr, "pipensx", "TCP", NULL, "0");
                int r2 = UPNP_AddPortMapping(t->upnp_urls.controlURL,
                    t->upnp_data.first.servicetype,
                    t->upnp_port_str, t->upnp_port_str,
                    t->upnp_lanaddr, "pipensx", "UDP", NULL, "0");
                t->upnp_mapped = (r1 == UPNPCOMMAND_SUCCESS || r2 == UPNPCOMMAND_SUCCESS);
                log_msg("[upnp] port %u: TCP=%s UDP=%s\n", listen_port,
                        r1 == UPNPCOMMAND_SUCCESS ? "ok" : "fail",
                        r2 == UPNPCOMMAND_SUCCESS ? "ok" : "fail");
            } else {
                log_msg("[upnp] no IGD found (ret=%d)\n", ret);
            }
            freeUPNPDevlist(devlist);
        } else {
            log_msg("[upnp] discover failed (err=%d)\n", upnp_err);
        }
    }
#endif

    /* Tracker announce (async-ish: blocking but short timeout) */
    uint8_t compact[200*6];
    uint32_t n = tracker_announce(mi, t->peer_id, listen_port,
                                  0, mi->total_length, compact, 200);
    for (uint32_t i = 0; i < n; i++)
        queue_push(t, *(uint32_t*)(compact+i*6), *(uint16_t*)(compact+i*6+4));
    t->last_tracker_ms  = now_ms();
    t->speed_time_ms    = now_ms();

    log_msg("[torrent] started: %u peers queued from trackers\n", n);
    return t;
}

void torrent_destroy(torrent_t *t) {
    if (!t) return;
#ifdef __SWITCH__
    if (t->upnp_mapped) {
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
#ifdef __SWITCH__
        dht_engine_save(t->dht, "/switch/pipensx/dht_nodes.bin");
#else
        dht_engine_save(t->dht, "/tmp/pipensx_dht.bin");
#endif
        dht_engine_destroy(t->dht);
    }
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++)
        if (t->peers[i]) peer_destroy(t->peers[i]);
    piece_mgr_destroy(t->pm);
    storage_close(t->store);
    /* NOTE: t->mi is a shallow copy of the caller's metainfo_t — the caller
     * owns the heap members (piece_hashes, files) and must call metainfo_free()
     * on its own copy. Freeing here would cause a double-free on exit. */
    free(t);
}

int torrent_tick(torrent_t *t) {
    if (t->pm->num_done == t->pm->num_pieces) return 0; /* done */

    uint64_t now = now_ms();

    /* Speed update every 1 second */
    if (now - t->speed_time_ms >= 1000) {
        uint64_t elapsed_ms = now - t->speed_time_ms;
        t->speed_bps      = t->speed_bytes * 1000 / (elapsed_ms + 1);
        t->speed_bytes    = 0;
        t->speed_time_ms  = now;
    }

    /* DHT tick */
    if (t->dht && now - t->last_dht_ms >= DHT_TICK_INTERVAL_MS) {
        dht_engine_tick(t->dht);
        t->last_dht_ms = now;
    }

    /* Re-announce tracker */
    if (now - t->last_tracker_ms >= TRACKER_REANNOUNCE_MS) {
        uint8_t compact[200*6];
        uint32_t n = tracker_announce(&t->mi, t->peer_id, t->listen_port,
                                      t->downloaded, t->mi.total_length - t->downloaded,
                                      compact, 200);
        for (uint32_t i = 0; i < n; i++)
            queue_push(t, *(uint32_t*)(compact+i*6), *(uint16_t*)(compact+i*6+4));
        t->last_tracker_ms = now;
    }

    /* Connect new peers */
    if (now - t->last_connect_ms >= CONNECT_INTERVAL_MS) {
        try_connect(t);
        t->last_connect_ms = now;
    }

    /* Build poll set */
    struct pollfd pfds[MAX_ACTIVE_PEERS + 2];
    int           pfd_peer[MAX_ACTIVE_PEERS];
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
        pfds[npfd].events = POLLIN | (p->state == PS_CONNECTING ? POLLOUT : 0);
        pfds[npfd].revents = 0;
        pfd_peer[npfd] = i;
        npfd++;
    }

    int r = poll(pfds, npfd, 50); /* 50ms timeout */
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

        int err = peer_recv(p, &ctx, cb_block, cb_have, cb_peers, t);
        if (err < 0) {
            log_msg("[torrent] peer %s dead\n", p->addr_str);
            peer_destroy(p);
            t->peers[slot] = NULL;
            t->num_peers--;
            continue;
        }
        /* Schedule requests */
        schedule_requests(t, p);
    }

    /* Timeout sweep — separate pass after poll so last_recv_ms is fully updated */
    uint64_t now2 = now_ms();
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p) continue;
        /* Kill peers stuck in CONNECTING or HANDSHAKE for > 30s */
        if (p->state == PS_CONNECTING || p->state == PS_HANDSHAKE) {
            if (p->connect_time_ms <= now2 && now2 - p->connect_time_ms > 30000) {
                log_msg("[torrent] peer %s connect/handshake timeout\n", p->addr_str);
                peer_destroy(p);
                t->peers[i] = NULL;
                t->num_peers--;
            }
            continue;
        }
        if (p->state != PS_ACTIVE) continue;
        /* Guard against unsigned underflow: only check if last_recv_ms <= now2 */
        if (p->last_recv_ms <= now2 && now2 - p->last_recv_ms > PEER_TIMEOUT_MS) {
            log_msg("[torrent] peer %s timeout\n", p->addr_str);
            peer_destroy(p);
            t->peers[i] = NULL;
            t->num_peers--;
        }
    }

    return (t->pm->num_done < t->pm->num_pieces) ? 1 : 0;
}

void torrent_stat(const torrent_t *t, torrent_stat_t *s) {
    s->num_pieces_done = t->pm->num_done;
    s->num_pieces      = t->pm->num_pieces;
    s->num_peers       = (uint32_t)t->num_peers;
    if (t->dht) {
        int g=0, d=0;
        dht_engine_nodes((dht_engine_t*)t->dht, &g, &d);
        s->dht_good    = (uint32_t)g;
        s->dht_dubious = (uint32_t)d;
    }
    s->downloaded = t->downloaded;
    s->speed_bps  = t->speed_bps;
    s->complete   = (t->pm->num_done == t->pm->num_pieces);
}
