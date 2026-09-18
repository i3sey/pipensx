#include "peer_handoff.h"
#include "util.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PEER_HANDOFF_TTL_MS 120000u
#define PEER_HANDOFF_PENDING_MAX ((256u * 1024u) + 4u)

struct magnet_handoff {
    int used;
    uint8_t info_hash[20];
    socket_t fd;
    struct sockaddr_in addr;
    int mse_active;
    rc4_t send_rc4;
    rc4_t recv_rc4;
    uint8_t *pending;
    uint32_t pending_len;
    uint64_t stashed_ms;
};

static pthread_mutex_t g_handoff_mu = PTHREAD_MUTEX_INITIALIZER;
static struct magnet_handoff g_handoff[PEER_HANDOFF_MAX];

static void handoff_clear(struct magnet_handoff *h) {
    if (!h || !h->used)
        return;
    if (h->fd != INVALID_SOCK)
        net_close(h->fd);
    free(h->pending);
    memset(h, 0, sizeof(*h));
    h->fd = INVALID_SOCK;
}

static void handoff_sweep_locked(uint64_t now) {
    for (int i = 0; i < PEER_HANDOFF_MAX; i++) {
        if (!g_handoff[i].used)
            continue;
        if (now - g_handoff[i].stashed_ms >= PEER_HANDOFF_TTL_MS)
            handoff_clear(&g_handoff[i]);
    }
}

void torrent_stash_peer(const uint8_t info_hash[20],
                        const torrent_peer_handoff_t *in) {
    if (!info_hash || !in || in->fd == INVALID_SOCK)
        return;
    pthread_mutex_lock(&g_handoff_mu);
    uint64_t now = now_ms();
    handoff_sweep_locked(now);

    int slot = -1;
    for (int i = 0; i < PEER_HANDOFF_MAX; i++) {
        if (g_handoff[i].used &&
            memcmp(g_handoff[i].info_hash, info_hash, 20) == 0 &&
            g_handoff[i].addr.sin_addr.s_addr == in->addr.sin_addr.s_addr &&
            g_handoff[i].addr.sin_port == in->addr.sin_port) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < PEER_HANDOFF_MAX; i++) {
            if (!g_handoff[i].used) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        uint64_t oldest = (uint64_t)-1;
        for (int i = 0; i < PEER_HANDOFF_MAX; i++) {
            if (g_handoff[i].stashed_ms <= oldest) {
                oldest = g_handoff[i].stashed_ms;
                slot = i;
            }
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_handoff_mu);
        net_close(in->fd);
        return;
    }
    if (g_handoff[slot].used)
        handoff_clear(&g_handoff[slot]);

    struct magnet_handoff *h = &g_handoff[slot];
    h->used = 1;
    memcpy(h->info_hash, info_hash, 20);
    h->fd = in->fd;
    h->addr = in->addr;
    h->mse_active = in->mse_active;
    h->send_rc4 = in->send_rc4;
    h->recv_rc4 = in->recv_rc4;
    h->pending = NULL;
    h->pending_len = 0;
    if (in->pending && in->pending_len) {
        uint32_t n = in->pending_len;
        if (n > PEER_HANDOFF_PENDING_MAX)
            n = PEER_HANDOFF_PENDING_MAX;
        h->pending = (uint8_t*)malloc(n);
        if (h->pending) {
            memcpy(h->pending, in->pending, n);
            h->pending_len = n;
        }
    }
    h->stashed_ms = now;
    pthread_mutex_unlock(&g_handoff_mu);
    log_msg("[torrent] stash peer fd=%d mse=%d pending=%u\n",
            in->fd, in->mse_active, h->pending_len);
}

void torrent_drop_stashed_peers(const uint8_t info_hash[20]) {
    if (!info_hash)
        return;
    pthread_mutex_lock(&g_handoff_mu);
    for (int i = 0; i < PEER_HANDOFF_MAX; i++) {
        if (g_handoff[i].used &&
            memcmp(g_handoff[i].info_hash, info_hash, 20) == 0)
            handoff_clear(&g_handoff[i]);
    }
    pthread_mutex_unlock(&g_handoff_mu);
}

int torrent_take_stashed_peers(const uint8_t info_hash[20],
                               torrent_peer_handoff_t *out, int max) {
    if (!info_hash || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&g_handoff_mu);
    handoff_sweep_locked(now_ms());
    int n = 0;
    for (int i = 0; i < PEER_HANDOFF_MAX && n < max; i++) {
        struct magnet_handoff *h = &g_handoff[i];
        if (!h->used)
            continue;
        if (memcmp(h->info_hash, info_hash, 20) != 0)
            continue;
        out[n].fd = h->fd;
        out[n].addr = h->addr;
        out[n].mse_active = h->mse_active;
        out[n].send_rc4 = h->send_rc4;
        out[n].recv_rc4 = h->recv_rc4;
        out[n].pending = h->pending;
        out[n].pending_len = h->pending_len;
        h->fd = INVALID_SOCK;
        h->pending = NULL;
        memset(h, 0, sizeof(*h));
        h->fd = INVALID_SOCK;
        n++;
    }
    pthread_mutex_unlock(&g_handoff_mu);
    return n;
}
