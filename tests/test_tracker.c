#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define UDP_RETRY_TIMEOUT_MS 100
#include "../src/core/tracker.c"

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64be(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

static void test_http_started_event_only_when_requested(void) {
    uint8_t info_hash[20];
    uint8_t peer_id[20];
    memset(info_hash, 0x11, sizeof(info_hash));
    memset(peer_id, 0x22, sizeof(peer_id));

    char url[1024];
    assert(http_build_announce_url(url, sizeof(url),
                                   "http://tracker.example/announce",
                                   info_hash, peer_id, 51413, 123, 456, 1));
    assert(strstr(url, "&event=started") != NULL);

    assert(http_build_announce_url(url, sizeof(url),
                                   "http://tracker.example/announce",
                                   info_hash, peer_id, 51413, 123, 456, 0));
    assert(strstr(url, "event=started") == NULL);
    assert(strstr(url, "&numwant=200") != NULL);
}

static void test_udp_started_event_only_when_requested(void) {
    uint8_t info_hash[20];
    uint8_t peer_id[20];
    uint8_t ann[98];
    memset(info_hash, 0x33, sizeof(info_hash));
    memset(peer_id, 0x44, sizeof(peer_id));

    udp_build_announce_packet(ann, 0x0102030405060708ULL, 0x0a0b0c0d,
                              info_hash, peer_id, 51413, 123, 456, 1);
    assert(rd32be(ann + 80) == 2);
    assert(rd64be(ann + 56) == 123);
    assert(rd64be(ann + 64) == 456);

    udp_build_announce_packet(ann, 0x0102030405060708ULL, 0x0a0b0c0d,
                              info_hash, peer_id, 51413, 123, 456, 0);
    assert(rd32be(ann + 80) == 0);
}

struct retry_udp_tracker {
    int fd;
    uint16_t port;
    int drop_connect;
    int drop_announce;
    int connect_requests;
    int announce_requests;
};

static void *retry_udp_tracker_run(void *arg) {
    struct retry_udp_tracker *s = arg;
    uint8_t req[128], resp[26];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);

    for (;;) {
        ssize_t n = recvfrom(s->fd, req, sizeof(req), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 16)
            return NULL;
        assert(rd32be(req + 8) == UDP_CONNECT);
        s->connect_requests++;
        if (s->connect_requests <= s->drop_connect)
            continue;
        memset(resp, 0, sizeof(resp));
        wr32be(resp, UDP_CONNECT);
        wr32be(resp + 4, rd32be(req + 12));
        memset(resp + 8, 0xAB, 8);
        sendto(s->fd, resp, 16, 0, (struct sockaddr *)&peer, plen);
        break;
    }

    for (;;) {
        plen = sizeof(peer);
        ssize_t n = recvfrom(s->fd, req, sizeof(req), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 16)
            return NULL;
        assert(rd32be(req + 8) == UDP_ANNOUNCE);
        s->announce_requests++;
        if (s->announce_requests <= s->drop_announce)
            continue;
        memset(resp, 0, sizeof(resp));
        wr32be(resp, UDP_ANNOUNCE);
        wr32be(resp + 4, rd32be(req + 12));
        resp[20] = 1; resp[21] = 2; resp[22] = 3; resp[23] = 4;
        resp[24] = 0x16; resp[25] = 0x2E;
        sendto(s->fd, resp, 26, 0, (struct sockaddr *)&peer, plen);
        return NULL;
    }
}

static uint32_t announce_against_retry_fake(int drop_connect,
                                            int drop_announce,
                                            struct retry_udp_tracker *s) {
    memset(s, 0, sizeof(*s));
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(s->fd >= 0);
    s->drop_connect = drop_connect;
    s->drop_announce = drop_announce;
    struct timeval timeout = {2, 0};
    assert(setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      sizeof(timeout)) == 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(s->fd, (struct sockaddr *)&addr, &alen) == 0);
    s->port = ntohs(addr.sin_port);

    pthread_t th;
    assert(pthread_create(&th, NULL, retry_udp_tracker_run, s) == 0);

    char url[64];
    snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)s->port);
    uint8_t info_hash[20], peer_id[20], compact[6 * 8];
    memset(info_hash, 0x11, sizeof(info_hash));
    memset(peer_id, 0x22, sizeof(peer_id));
    uint32_t got = tracker_announce_url_ex_cancel_event(
        url, info_hash, peer_id, 51413, 0, 100, compact, 8, NULL, 1,
        NULL, NULL);

    pthread_join(th, NULL);
    close(s->fd);
    return got;
}

static void test_udp_retries_dropped_datagrams(void) {
    struct retry_udp_tracker s;
    assert(announce_against_retry_fake(1, 1, &s) == 1);
    assert(s.connect_requests == 2);
    assert(s.announce_requests == 2);
}

int main(void) {
    test_http_started_event_only_when_requested();
    test_udp_started_event_only_when_requested();
    test_udp_retries_dropped_datagrams();
    puts("tracker tests passed");
    return 0;
}
