/*
 * Glue layer for jech's dht.c (MIT).
 * Implements the user-provided callbacks and wraps the public API.
 */
#include "dht.h"
#include "util.h"
/* dht.h needs stdio.h for FILE* declaration */
#include <stdio.h>
#include "../../vendor/dht/dht.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* ---- jech dht.c user-provided callbacks ---- */

/* dht_hash: SHA-1 */
#include "../core/sha1.h"
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    if (v1 && len1 > 0) sha1_update(&ctx, v1, len1);
    if (v2 && len2 > 0) sha1_update(&ctx, v2, len2);
    if (v3 && len3 > 0) sha1_update(&ctx, v3, len3);
    uint8_t digest[20];
    sha1_final(&ctx, digest);
    int n = hash_size < 20 ? hash_size : 20;
    memcpy(hash_return, digest, n);
}

int dht_random_bytes(void *buf, size_t size) {
    rand_bytes((uint8_t*)buf, size);
    return 0;
}

int dht_blacklisted(const struct sockaddr *sa, int salen) {
    (void)sa; (void)salen;
    return 0; /* no blacklist */
}

/* sendto wrapper — called by dht.c to send packets */
int dht_sendto(int s, const void *buf, int len, int flags,
               const struct sockaddr *to, int tolen) {
    ssize_t r = sendto(s, buf, len, flags, to, (socklen_t)tolen);
    if (r < 0) {
        static uint64_t last_log_ms = 0;
        static uint32_t suppressed = 0;
        uint64_t now = now_ms();
        if (now - last_log_ms >= 10000) {
            log_msg("[dht] sendto failed errno=%d suppressed=%u\n",
                    errno, suppressed);
            last_log_ms = now;
            suppressed = 0;
        } else {
            suppressed++;
        }
    }
    return (int)r;
}

/* ---- node cache (fast warm start) ---- */

static char g_cache_path[512];

void dht_engine_set_cache_path(const char *path) {
    if (!path) {
        g_cache_path[0] = '\0';
        return;
    }
    snprintf(g_cache_path, sizeof(g_cache_path), "%s", path);
}

int dht_cache_read(const char *path, uint8_t node_id[20],
                   uint8_t (*nodes)[6], int max_nodes) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t magic[4], cnt_le[2];
    int ok = fread(magic, 1, 4, f) == 4 &&
             memcmp(magic, DHT_CACHE_MAGIC, 4) == 0 &&
             fread(node_id, 1, 20, f) == 20 &&
             fread(cnt_le, 1, 2, f) == 2;
    int n = 0;
    if (ok) {
        n = cnt_le[0] | (cnt_le[1] << 8);
        if (n > DHT_CACHE_MAX_NODES) ok = 0;
    }
    if (ok) {
        if (n > max_nodes) n = max_nodes;
        ok = fread(nodes, 6, (size_t)n, f) == (size_t)n;
    }
    fclose(f);
    return ok ? n : -1;
}

int dht_cache_write(const char *path, const uint8_t node_id[20],
                    const uint8_t (*nodes)[6], int count) {
    if (count <= 0 || count > DHT_CACHE_MAX_NODES) return 0;
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    uint8_t cnt_le[2] = { (uint8_t)(count & 0xff), (uint8_t)(count >> 8) };
    int ok = fwrite(DHT_CACHE_MAGIC, 1, 4, f) == 4 &&
             fwrite(node_id, 1, 20, f) == 20 &&
             fwrite(cnt_le, 1, 2, f) == 2 &&
             fwrite(nodes, 6, (size_t)count, f) == (size_t)count;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return 0;
    }
    if (rename(tmp, path) != 0) {
        /* FAT (sdmc) may refuse rename-over-existing; retry after unlink. */
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            return 0;
        }
    }
    return 1;
}

/* ---- engine ---- */
struct dht_engine {
    socket_t   fd;
    uint8_t    node_id[20];
    uint16_t   listen_port;

    /* Current search */
    uint8_t    search_hash[20];
    int        searching;
    dht_peer_cb peer_cb;
    void       *peer_ud;
};

static struct dht_engine *g_engine = NULL; /* jech dht is a global singleton */

/* jech's dht.c keeps its state in globals, so only one engine may exist at a
   time. The flag makes create/destroy safe when the torrent loop and the
   magnet resolver race for the engine: the loser gets NULL and runs without
   DHT. */
static atomic_flag g_engine_busy = ATOMIC_FLAG_INIT;

static void dht_callback(void *closure, int event,
                         const uint8_t *info_hash,
                         const void *data, size_t data_len) {
    (void)closure; (void)info_hash;
    struct dht_engine *e = g_engine;
    if (!e) return;

    if (event == DHT_EVENT_VALUES) {
        /* data is a list of compact IPv4 peers (6 bytes each) */
        const uint8_t *p = (const uint8_t*)data;
        uint32_t count = (uint32_t)(data_len / 6);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t ip   = *(uint32_t*)(p + i*6);
            uint16_t port = *(uint16_t*)(p + i*6 + 4);
            if (e->peer_cb)
                e->peer_cb(e->peer_ud, ip, port);
        }
    } else if (event == DHT_EVENT_SEARCH_DONE) {
        log_msg("[dht] search done\n");
    }
}

dht_engine_t *dht_engine_create(uint16_t listen_port, const uint8_t node_id[20]) {
    if (atomic_flag_test_and_set(&g_engine_busy))
        return NULL;
    struct dht_engine *e = (struct dht_engine*)calloc(1, sizeof(*e));
    if (!e) {
        atomic_flag_clear(&g_engine_busy);
        return NULL;
    }
    /* Warm start: a cached node list from the last orderly teardown. The
       stored node ID is reused so remote buckets that still remember us keep
       us as the same node. */
    uint8_t cached_nodes[DHT_CACHE_MAX_NODES][6];
    uint8_t cached_id[20];
    int cached_count = -1;
    if (g_cache_path[0]) {
        cached_count = dht_cache_read(g_cache_path, cached_id, cached_nodes,
                                      DHT_CACHE_MAX_NODES);
        if (cached_count >= 0) node_id = cached_id;
    }
    memcpy(e->node_id, node_id, 20);
    e->listen_port = listen_port;

    e->fd = net_udp_socket(listen_port);
    if (e->fd == INVALID_SOCK) {
        free(e);
        atomic_flag_clear(&g_engine_busy);
        return NULL;
    }

    if (dht_init(e->fd, -1, node_id, (const uint8_t*)"PIPENSX1") < 0) {
        net_close(e->fd);
        free(e);
        atomic_flag_clear(&g_engine_busy);
        return NULL;
    }

    /* Ping cached nodes instead of inserting them: live ones reply and enter
       the routing table with their true ID (jech marks ping replies as
       confirmed), dead ones never pollute it. */
    for (int i = 0; i < cached_count; i++) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr, cached_nodes[i], 4);
        memcpy(&addr.sin_port, cached_nodes[i] + 4, 2);
        dht_ping_node((struct sockaddr*)&addr, sizeof(addr));
    }
    if (cached_count > 0)
        log_msg("[dht] cache: pinged %d nodes\n", cached_count);

    g_engine = e;
    log_msg("[dht] init port=%u\n", listen_port);
    return e;
}

void dht_engine_destroy(dht_engine_t *e) {
    if (!e) return;
    /* Persist good nodes for the next warm start. Skipped when the table is
       empty so an offline session cannot clobber a useful cache. */
    if (g_cache_path[0]) {
        struct sockaddr_in sins[128];
        int num = 128, num6 = 0;
        /* num6 must be a real pointer: dht_get_nodes writes *num6
           unconditionally. */
        dht_get_nodes(sins, &num, NULL, &num6);
        if (num > 0) {
            uint8_t nodes[128][6];
            for (int i = 0; i < num; i++) {
                memcpy(nodes[i], &sins[i].sin_addr, 4);
                memcpy(nodes[i] + 4, &sins[i].sin_port, 2);
            }
            if (dht_cache_write(g_cache_path, e->node_id, nodes, num))
                log_msg("[dht] saved %d nodes to %s\n", num, g_cache_path);
        }
    }
    dht_uninit();
    net_close(e->fd);
    g_engine = NULL;
    free(e);
    atomic_flag_clear(&g_engine_busy);
}

void dht_engine_search(dht_engine_t *e, const uint8_t info_hash[20],
                       dht_peer_cb on_peer, void *ud) {
    memcpy(e->search_hash, info_hash, 20);
    e->searching = 1;
    e->peer_cb   = on_peer;
    e->peer_ud   = ud;
    dht_search(info_hash, e->listen_port, AF_INET, dht_callback, NULL);
    log_msg("[dht] search started\n");
}

/* Bootstrap nodes (well-known public DHT routers) */
static const struct { const char *host; uint16_t port; } BOOTSTRAP[] = {
    {"router.bittorrent.com",    6881},
    {"dht.transmissionbt.com",   6881},
    {"router.utorrent.com",      6881},
    {"dht.aelitis.com",          6881},
    {"dht.libtorrent.org",      25401},
    {"dht2.opentracker.is",      1337},
};

void dht_engine_bootstrap(dht_engine_t *e __attribute__((unused))) {
    for (size_t i = 0; i < sizeof(BOOTSTRAP)/sizeof(BOOTSTRAP[0]); i++) {
        struct sockaddr_in addr;
        if (net_resolve(BOOTSTRAP[i].host, BOOTSTRAP[i].port, &addr)) {
            dht_ping_node((struct sockaddr*)&addr, sizeof(addr));
            log_msg("[dht] bootstrap -> %s:%u\n", BOOTSTRAP[i].host, BOOTSTRAP[i].port);
        }
    }
}

int dht_engine_fd(dht_engine_t *e) { return e->fd; }

void dht_engine_tick(dht_engine_t *e) {
    /* Read all pending UDP datagrams */
    uint8_t buf[4096];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    for (;;) {
        fromlen = sizeof(from);
        /* Leave one byte at end so we can null-terminate: jech requires buf[buflen]=='\0' */
        ssize_t n = recvfrom(e->fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &fromlen);
        if (n < 0) break;
        buf[n] = '\0';
        time_t tosend;
        dht_periodic(buf, (int)n, (struct sockaddr*)&from, (int)fromlen,
                     &tosend, dht_callback, NULL);
    }
    /* Also call with no data to drive timers */
    time_t tosend;
    dht_periodic(NULL, 0, NULL, 0, &tosend, dht_callback, NULL);

    /* Re-bootstrap if still 0 good nodes (every 30s) */
    {
        static time_t last_bootstrap = 0;
        int g = 0, d = 0, c = 0, in = 0;
        dht_nodes(AF_INET, &g, &d, &c, &in);
        time_t now = now_sec();
        if (g == 0 && now - last_bootstrap > 30) {
            log_msg("[dht] no good nodes, re-bootstrapping\n");
            for (size_t i = 0; i < sizeof(BOOTSTRAP)/sizeof(BOOTSTRAP[0]); i++) {
                struct sockaddr_in addr;
                if (net_resolve(BOOTSTRAP[i].host, BOOTSTRAP[i].port, &addr))
                    dht_ping_node((struct sockaddr*)&addr, sizeof(addr));
            }
            last_bootstrap = now;
        }
    }

    /* Re-issue search periodically */
    if (e->searching) {
        static time_t last_search = 0;
        time_t now = now_sec();
        if (now - last_search > 60) {
            dht_search(e->search_hash, e->listen_port, AF_INET, dht_callback, NULL);
            last_search = now;
        }
    }
}

void dht_engine_nodes(dht_engine_t *e __attribute__((unused)), int *good, int *dubious) {
    int g=0, d=0, c=0, in=0;
    dht_nodes(AF_INET, &g, &d, &c, &in);
    *good = g; *dubious = d;
}
