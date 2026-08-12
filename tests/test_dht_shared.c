/* White-box tests for the shared DHT engine. Includes dht.c as source (the
   test_torrent precedent) to reach g_dht, dht_callback and the session
   internals directly. */
#include "../src/core/dht.c"

#include <assert.h>
#include <stdio.h>

/* dht_callback routes by info-hash: peers land only in matching mailboxes,
   and every session attached to the same hash receives them. */
static void test_callback_routing(void) {
    struct dht_session a, b, b2;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&b2, 0, sizeof(b2));
    memset(a.info_hash, 0xAA, 20);
    memset(b.info_hash, 0xBB, 20);
    memcpy(b2.info_hash, b.info_hash, 20);
    a.next = &b;
    b.next = &b2;
    g_dht.sessions = &a;

    uint8_t peers[12];
    for (int i = 0; i < 12; i++)
        peers[i] = (uint8_t)(i + 1);
    dht_callback(NULL, DHT_EVENT_VALUES, b.info_hash, peers, sizeof(peers));
    assert(a.count == 0);
    assert(b.count == 2);
    assert(b2.count == 2);

    uint8_t out[4][6];
    assert(dht_session_poll(&b, out, 4) == 2);
    assert(memcmp(out[0], peers, 6) == 0);
    assert(memcmp(out[1], peers + 6, 6) == 0);
    assert(b.count == 0);
    assert(dht_session_poll(&b, out, 4) == 0);
    assert(dht_session_poll(&b2, out, 1) == 1);
    assert(memcmp(out[0], peers, 6) == 0);

    g_dht.sessions = NULL;
}

/* A full mailbox drops the newest peers (the re-search refills), and the
   ring wraps cleanly after a drain. */
static void test_mailbox_drop_newest_and_wrap(void) {
    struct dht_session s;
    memset(&s, 0, sizeof(s));
    memset(s.info_hash, 0xCC, 20);
    g_dht.sessions = &s;

    uint8_t peer[6];
    for (int i = 0; i < DHT_SESSION_MAILBOX + 10; i++) {
        memset(peer, (uint8_t)(i + 1), 6);
        dht_callback(NULL, DHT_EVENT_VALUES, s.info_hash, peer, 6);
    }
    assert(s.count == DHT_SESSION_MAILBOX);

    uint8_t out[1][6];
    assert(dht_session_poll(&s, out, 1) == 1);
    assert(out[0][0] == 1); /* oldest survived, overflow was dropped */
    int drained = 1;
    while (dht_session_poll(&s, out, 1) == 1)
        drained++;
    assert(drained == DHT_SESSION_MAILBOX);

    /* head is now mid-ring; a fresh push and pop must still line up */
    memset(peer, 0x7F, 6);
    dht_callback(NULL, DHT_EVENT_VALUES, s.info_hash, peer, 6);
    assert(s.count == 1);
    assert(dht_session_poll(&s, out, 1) == 1);
    assert(out[0][0] == 0x7F);

    g_dht.sessions = NULL;
}

/* Bind probe WITHOUT SO_REUSEADDR: the engine's socket sets it, and two
   UDP sockets that both set it may share a port on Linux — a plain bind
   against an existing REUSEADDR socket still fails with EADDRINUSE. */
static int port_is_free(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    int bound = bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
    close(fd);
    return bound;
}

/* Refcount lifecycle: first attach binds DHT_SHARED_PORT, the port stays
   bound while any session lives, the last detach releases it. */
static void test_lifecycle(void) {
    uint8_t hash_a[20], hash_b[20];
    memset(hash_a, 1, 20);
    memset(hash_b, 2, 20);

    dht_session_t *a = dht_attach(hash_a, 51413);
    if (!a) {
        /* Another process owns UDP 51413 on this machine; the interesting
           assertions below would test that process, not us. */
        puts("test_lifecycle skipped: UDP 51413 unavailable");
        return;
    }
    assert(!port_is_free(DHT_SHARED_PORT));

    dht_session_t *b = dht_attach(hash_b, 51414);
    assert(b != NULL);
    int good = 0, dubious = 0;
    dht_shared_nodes(&good, &dubious); /* must not deadlock with the thread */

    dht_detach(a);
    assert(!port_is_free(DHT_SHARED_PORT)); /* still one session attached */

    dht_detach(b);
    assert(port_is_free(DHT_SHARED_PORT)); /* engine stopped, port free */
}

/* Concurrency smoke: parallel attach/poll/detach cycles across threads,
   including engine restarts whenever the refcount touches zero. */
static void *smoke_worker(void *arg) {
    int idx = (int)(intptr_t)arg;
    uint8_t hash[20];
    memset(hash, 0x40 + idx, 20);
    for (int i = 0; i < 10; i++) {
        dht_session_t *s = dht_attach(hash, (uint16_t)(51413 + idx));
        if (!s)
            continue;
        uint8_t out[8][6];
        dht_session_poll(s, out, 8);
        dht_detach(s);
    }
    return NULL;
}

static void test_concurrent_attach_detach(void) {
    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, smoke_worker,
                              (void*)(intptr_t)i) == 0);
    for (int i = 0; i < 4; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    socket_t probe = net_udp_socket(DHT_SHARED_PORT);
    if (probe != INVALID_SOCK)
        net_close(probe);
}

int main(void) {
    test_callback_routing();
    test_mailbox_drop_newest_and_wrap();
    test_lifecycle();
    test_concurrent_attach_detach();
    puts("test_dht_shared: all tests passed");
    return 0;
}
