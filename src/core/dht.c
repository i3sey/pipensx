/*
 * Glue layer for jech's dht.c (MIT).
 * Implements the user-provided callbacks and wraps the public API.
 */
#include "dht.h"
#include "util.h"
/* dht.h needs stdio.h for FILE* declaration */
#include <stdio.h>
#include "../../vendor/dht/dht.h"
#include <poll.h>
#include <pthread.h>
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

/* ---- shared engine ---- */

/* Per-session mailbox size: 256 compact endpoints. Full mailbox drops the
   newest peers; the periodic re-search refills them. */
#define DHT_SESSION_MAILBOX 256
/* Datagrams the engine thread handles per jech_mu hold. See the drain loop. */
#define DHT_DRAIN_PER_PASS  32

struct dht_session {
    uint8_t  info_hash[20];
    uint16_t announce_port;
    time_t   last_search;             /* engine thread only */
    /* Mailbox ring; guarded by g_dht.jech_mu. */
    uint8_t  peers[DHT_SESSION_MAILBOX][6];
    int      head;
    int      count;
    struct dht_session *next;
};

/*
 * Lock order: lifecycle_mu -> jech_mu.
 *
 * lifecycle_mu (never taken by the engine thread) guards the refcount and
 * the whole first-attach-start / last-detach-stop transitions, so an attach
 * racing a last detach blocks until teardown finishes and then does a fresh
 * start — warm from the cache that detach just persisted.
 *
 * jech_mu serialises every call into vendored jech code plus the session
 * list and mailboxes. dht_callback only ever runs inside jech calls, i.e.
 * with jech_mu already held: it must NOT re-lock it.
 */
static struct {
    pthread_mutex_t lifecycle_mu;
    pthread_mutex_t jech_mu;
    pthread_t  thread;
    atomic_int stop;
    socket_t   fd;
    uint8_t    node_id[20];
    int        refcount;              /* under lifecycle_mu */
    struct dht_session *sessions;     /* under jech_mu */
    /* Engine-thread-only state. */
    int        bootstrapped;
    time_t     last_bootstrap;
} g_dht = {
    .lifecycle_mu = PTHREAD_MUTEX_INITIALIZER,
    .jech_mu = PTHREAD_MUTEX_INITIALIZER,
    /* the rest zero-initialized */
};

static void dht_callback(void *closure, int event,
                         const uint8_t *info_hash,
                         const void *data, size_t data_len) {
    (void)closure;
    /* Runs inside dht_periodic/dht_search with g_dht.jech_mu held by the
       caller — do not lock anything here. */
    if (event == DHT_EVENT_VALUES) {
        /* data is a list of compact IPv4 peers (6 bytes each). jech tags
           the event with the search's info-hash; deliver to every session
           attached to that hash. */
        const uint8_t *p = (const uint8_t*)data;
        int count = (int)(data_len / 6);
        for (struct dht_session *s = g_dht.sessions; s; s = s->next) {
            if (memcmp(s->info_hash, info_hash, 20) != 0)
                continue;
            for (int i = 0; i < count && s->count < DHT_SESSION_MAILBOX; i++) {
                int tail = (s->head + s->count) % DHT_SESSION_MAILBOX;
                memcpy(s->peers[tail], p + i * 6, 6);
                s->count++;
            }
        }
    } else if (event == DHT_EVENT_SEARCH_DONE) {
        log_msg("[dht] search done\n");
    }
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
#define BOOTSTRAP_COUNT (sizeof(BOOTSTRAP)/sizeof(BOOTSTRAP[0]))

/* Resolve outside jech_mu (DNS blocks), ping under it. */
static void dht_bootstrap_ping(void) {
    struct sockaddr_in addrs[BOOTSTRAP_COUNT];
    int resolved[BOOTSTRAP_COUNT];
    for (size_t i = 0; i < BOOTSTRAP_COUNT; i++) {
        resolved[i] = net_resolve(BOOTSTRAP[i].host, BOOTSTRAP[i].port,
                                  &addrs[i]);
        if (resolved[i])
            log_msg("[dht] bootstrap -> %s:%u\n",
                    BOOTSTRAP[i].host, BOOTSTRAP[i].port);
    }
    pthread_mutex_lock(&g_dht.jech_mu);
    for (size_t i = 0; i < BOOTSTRAP_COUNT; i++)
        if (resolved[i])
            dht_ping_node((struct sockaddr*)&addrs[i], sizeof(addrs[i]));
    pthread_mutex_unlock(&g_dht.jech_mu);
}

static void *dht_thread_main(void *arg) {
    (void)arg;
    time_t tosleep = 0;
    while (!atomic_load(&g_dht.stop)) {
        if (!g_dht.bootstrapped) {
            /* DNS resolves live here so attach never blocks on them. */
            dht_bootstrap_ping();
            g_dht.bootstrapped = 1;
            g_dht.last_bootstrap = now_sec();
        }

        /* Cap the poll timeout so the stop flag and the 30s/60s cadences
           below stay responsive regardless of jech's tosleep. */
        int timeout_ms = 250;
        if (tosleep <= 0)
            timeout_ms = 250;
        else if (tosleep * 1000 < timeout_ms)
            timeout_ms = (int)(tosleep * 1000);
        struct pollfd pfd;
        pfd.fd = g_dht.fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, timeout_ms);

        pthread_mutex_lock(&g_dht.jech_mu);
        if (r > 0 && (pfd.revents & POLLIN)) {
            /* Read all pending UDP datagrams. Leave one byte at the end so
               we can null-terminate: jech requires buf[buflen]=='\0'. */
            uint8_t buf[4096];
            struct sockaddr_in from;
            socklen_t fromlen;
            /* Bounded per pass: jech_mu is also taken by the torrent threads
               (dht_session_poll), so an unbounded drain lets a DHT burst park
               a peer event loop for as long as the socket keeps yielding
               datagrams. Leftovers stay queued for the next poll(), which
               returns immediately while the socket is still readable. */
            for (int drained = 0; drained < DHT_DRAIN_PER_PASS; drained++) {
                fromlen = sizeof(from);
                ssize_t n = recvfrom(g_dht.fd, buf, sizeof(buf) - 1, 0,
                                     (struct sockaddr*)&from, &fromlen);
                if (n < 0) break;
                buf[n] = '\0';
                dht_periodic(buf, (int)n, (struct sockaddr*)&from,
                             (int)fromlen, &tosleep, dht_callback, NULL);
            }
        }
        /* Drive timers even when no datagram arrived. */
        dht_periodic(NULL, 0, NULL, 0, &tosleep, dht_callback, NULL);

        int g = 0, d = 0, c = 0, in = 0;
        dht_nodes(AF_INET, &g, &d, &c, &in);
        time_t now = now_sec();
        int rebootstrap = g == 0 && now - g_dht.last_bootstrap > 30;

        /* Re-issue every session's search periodically. A pure-lookup
           session (announce port 0) rides a port-carrying session's search
           for the same hash instead of clobbering its announce port. */
        for (struct dht_session *s = g_dht.sessions; s; s = s->next) {
            if (now - s->last_search <= 60)
                continue;
            if (s->announce_port == 0) {
                int covered = 0;
                for (struct dht_session *o = g_dht.sessions; o; o = o->next)
                    if (o != s && o->announce_port != 0 &&
                        memcmp(o->info_hash, s->info_hash, 20) == 0) {
                        covered = 1;
                        break;
                    }
                if (covered) {
                    s->last_search = now;
                    continue;
                }
            }
            dht_search(s->info_hash, s->announce_port, AF_INET,
                       dht_callback, NULL);
            s->last_search = now;
        }
        pthread_mutex_unlock(&g_dht.jech_mu);

        if (rebootstrap) {
            log_msg("[dht] no good nodes, re-bootstrapping\n");
            dht_bootstrap_ping();
            g_dht.last_bootstrap = now;
        }
    }
    return NULL;
}

/* Both engine_start and engine_stop run under lifecycle_mu with no engine
   thread alive, so they may call jech directly. */
static int dht_engine_start(void) {
    uint8_t node_id[20];
    rand_bytes(node_id, 20);

    /* Warm start: a cached node list from the last orderly teardown. The
       stored node ID is reused so remote buckets that still remember us keep
       us as the same node. */
    uint8_t cached_nodes[DHT_CACHE_MAX_NODES][6];
    int cached_count = -1;
    if (g_cache_path[0]) {
        uint8_t cached_id[20];
        cached_count = dht_cache_read(g_cache_path, cached_id, cached_nodes,
                                      DHT_CACHE_MAX_NODES);
        if (cached_count >= 0)
            memcpy(node_id, cached_id, 20);
    }

    g_dht.fd = net_udp_socket(DHT_SHARED_PORT);
    if (g_dht.fd == INVALID_SOCK)
        return 0;
    if (dht_init(g_dht.fd, -1, node_id, (const uint8_t*)"PIPENSX1") < 0) {
        net_close(g_dht.fd);
        return 0;
    }
    memcpy(g_dht.node_id, node_id, 20);

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

    atomic_store(&g_dht.stop, 0);
    g_dht.bootstrapped = 0;
    g_dht.last_bootstrap = 0;
    if (pthread_create(&g_dht.thread, NULL, dht_thread_main, NULL) != 0) {
        dht_uninit();
        net_close(g_dht.fd);
        return 0;
    }
    log_msg("[dht] init port=%u\n", DHT_SHARED_PORT);
    return 1;
}

static void dht_engine_stop(void) {
    atomic_store(&g_dht.stop, 1);
    pthread_join(g_dht.thread, NULL);

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
            if (dht_cache_write(g_cache_path, g_dht.node_id, nodes, num))
                log_msg("[dht] saved %d nodes to %s\n", num, g_cache_path);
        }
    }
    dht_uninit();
    net_close(g_dht.fd);
}

dht_session_t *dht_attach(const uint8_t info_hash[20],
                          uint16_t announce_port) {
    struct dht_session *s =
        (struct dht_session*)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    memcpy(s->info_hash, info_hash, 20);
    s->announce_port = announce_port;

    pthread_mutex_lock(&g_dht.lifecycle_mu);
    if (g_dht.refcount == 0 && !dht_engine_start()) {
        pthread_mutex_unlock(&g_dht.lifecycle_mu);
        free(s);
        return NULL;
    }
    g_dht.refcount++;
    pthread_mutex_lock(&g_dht.jech_mu);
    s->next = g_dht.sessions;
    g_dht.sessions = s;
    /* Kick off the search immediately; found peers may land in the mailbox
       before this even returns (jech serves local storage inline).
       last_search stays 0 so the engine thread re-issues once shortly after
       the bootstrap pings have had a chance to reply, then every 60 s. */
    dht_search(s->info_hash, s->announce_port, AF_INET, dht_callback, NULL);
    pthread_mutex_unlock(&g_dht.jech_mu);
    int sessions = g_dht.refcount;
    pthread_mutex_unlock(&g_dht.lifecycle_mu);

    log_msg("[dht] attach announce_port=%u sessions=%d\n",
            announce_port, sessions);
    return s;
}

void dht_detach(dht_session_t *s) {
    if (!s)
        return;
    pthread_mutex_lock(&g_dht.lifecycle_mu);
    pthread_mutex_lock(&g_dht.jech_mu);
    for (struct dht_session **it = &g_dht.sessions; *it; it = &(*it)->next) {
        if (*it == s) {
            *it = s->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_dht.jech_mu);
    g_dht.refcount--;
    if (g_dht.refcount == 0)
        dht_engine_stop();
    int sessions = g_dht.refcount;
    pthread_mutex_unlock(&g_dht.lifecycle_mu);

    free(s);
    log_msg("[dht] detach sessions=%d\n", sessions);
}

int dht_session_poll(dht_session_t *s, uint8_t (*out)[6], int max) {
    if (!s || max <= 0)
        return 0;
    pthread_mutex_lock(&g_dht.jech_mu);
    int n = 0;
    while (n < max && s->count > 0) {
        memcpy(out[n], s->peers[s->head], 6);
        s->head = (s->head + 1) % DHT_SESSION_MAILBOX;
        s->count--;
        n++;
    }
    pthread_mutex_unlock(&g_dht.jech_mu);
    return n;
}

void dht_shared_nodes(int *good, int *dubious) {
    int g = 0, d = 0, c = 0, in = 0;
    pthread_mutex_lock(&g_dht.jech_mu);
    dht_nodes(AF_INET, &g, &d, &c, &in);
    pthread_mutex_unlock(&g_dht.jech_mu);
    *good = g;
    *dubious = d;
}
