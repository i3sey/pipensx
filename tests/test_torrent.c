#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/bencode.h"
#include "../src/core/torrent.c"

/* Minimal multi-file torrent whose single file has the given "path" list. */
static int parse_with_path(const char *const *parts, size_t nparts,
                           metainfo_t *mi) {
    static uint8_t buf[8192];
    size_t n = 0;
#define PUT(...) \
    n += (size_t)snprintf((char *)buf + n, sizeof(buf) - n, __VA_ARGS__)
    PUT("d4:infod5:filesld6:lengthi100e4:pathl");
    for (size_t i = 0; i < nparts; i++)
        PUT("%zu:%s", strlen(parts[i]), parts[i]);
    PUT("eee4:name4:test12:piece lengthi16384e6:pieces20:");
#undef PUT
    memcpy(buf + n, "HHHHHHHHHHHHHHHHHHHH", 20);
    n += 20;
    memcpy(buf + n, "ee", 2);
    n += 2;
    return metainfo_parse(buf, n, mi);
}

/* A peer's extension handshake goes straight into be_dict_get (peer.c). A
   few thousand 'l' bytes used to recurse once per byte and take the thread
   stack with it; the parse must fail instead of crashing. */
static void test_bencode_rejects_deep_nesting(void) {
    enum { kDepth = 100000 };
    char *b = malloc(4 + kDepth * 2 + 1);
    assert(b);
    size_t n = 0;
    memcpy(b + n, "d1:x", 4);
    n += 4;
    memset(b + n, 'l', kDepth);
    n += kDepth;
    memset(b + n, 'e', kDepth);
    n += kDepth;
    b[n++] = 'e';
    be_node_t v;
    assert(!be_dict_get(b, b + n, "m", 1, &v));
    free(b);
}

/* A fake BEP-15 tracker on loopback. It answers the connect handshake
   honestly and then echoes `announce_txid_delta` added to the transaction id
   the client sent, so a reply that does not match can be simulated. */
struct fake_udp_tracker {
    int fd;
    uint16_t port;
    uint32_t announce_txid_delta;
};

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void *fake_udp_tracker_run(void *arg) {
    struct fake_udp_tracker *s = arg;
    uint8_t req[128], resp[26];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);

    /* connect: action 0, echo the transaction id, hand out a connection id */
    ssize_t n = recvfrom(s->fd, req, sizeof(req), 0, (struct sockaddr *)&peer,
                         &plen);
    if (n < 16) return NULL;
    memset(resp, 0, sizeof(resp));
    wr32(resp, 0);
    wr32(resp + 4, rd32(req + 12));
    memset(resp + 8, 0xAB, 8);
    sendto(s->fd, resp, 16, 0, (struct sockaddr *)&peer, plen);

    /* announce: action 1, one compact peer at 1.2.3.4:5678 */
    plen = sizeof(peer);
    n = recvfrom(s->fd, req, sizeof(req), 0, (struct sockaddr *)&peer, &plen);
    if (n < 16) return NULL;
    memset(resp, 0, sizeof(resp));
    wr32(resp, 1);
    wr32(resp + 4, rd32(req + 12) + s->announce_txid_delta);
    resp[20] = 1; resp[21] = 2; resp[22] = 3; resp[23] = 4;
    resp[24] = 0x16; resp[25] = 0x2E;
    sendto(s->fd, resp, 26, 0, (struct sockaddr *)&peer, plen);
    return NULL;
}

static uint32_t announce_against_fake(uint32_t txid_delta) {
    struct fake_udp_tracker s = {-1, 0, txid_delta};
    s.fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(s.fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(s.fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    socklen_t alen = sizeof(a);
    assert(getsockname(s.fd, (struct sockaddr *)&a, &alen) == 0);
    s.port = ntohs(a.sin_port);

    pthread_t th;
    assert(pthread_create(&th, NULL, fake_udp_tracker_run, &s) == 0);

    char url[64];
    snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)s.port);
    uint8_t info_hash[20], peer_id[20], compact[6 * 8];
    memset(info_hash, 0x11, sizeof(info_hash));
    memset(peer_id, 0x22, sizeof(peer_id));
    uint32_t got = tracker_announce_url_ex_cancel(
        url, info_hash, peer_id, 51413, 0, 100, compact, 8, NULL, NULL, NULL);

    pthread_join(th, NULL);
    close(s.fd);
    return got;
}

static void test_udp_tracker_checks_transaction_id(void) {
    /* A well-behaved tracker still works. */
    assert(announce_against_fake(0) == 1);
    /* A reply that does not echo the transaction id is not ours. Before the
       check, the peers in it were accepted. */
    assert(announce_against_fake(1) == 0);
}

/* Single-file torrent whose three numbers can be set independently, so the
   metadata can be made deliberately inconsistent. */
static int parse_single(long long pieceLen, long long fileLen, int pieces,
                        metainfo_t *mi) {
    static uint8_t buf[8192];
    size_t n = (size_t)snprintf(
        (char *)buf, sizeof(buf),
        "d4:infod6:lengthi%llde4:name4:test12:piece lengthi%llde6:pieces%d:",
        fileLen, pieceLen, pieces * 20);
    for (int i = 0; i < pieces * 20; i++) buf[n++] = 'H';
    memcpy(buf + n, "ee", 2);
    n += 2;
    return metainfo_parse(buf, n, mi);
}

static void test_metainfo_rejects_bad_numbers(void) {
    metainfo_t mi;
    assert(parse_single(16384, 100, 1, &mi)); /* ceil(100/16384) == 1 */
    metainfo_free(&mi);
    assert(parse_single(16384, 16385, 2, &mi)); /* one byte into piece 2 */
    metainfo_free(&mi);

    assert(!parse_single(0, 100, 1, &mi));          /* zero piece length */
    assert(!parse_single(-16384, 100, 1, &mi));     /* negative */
    assert(!parse_single(1LL << 40, 1LL << 40, 1, &mi)); /* absurd */
    assert(!parse_single(16384, -5, 1, &mi));       /* negative file length */
    assert(!parse_single(16384, 100, 7, &mi));      /* piece count mismatch */

    /* A 23-digit integer used to wrap into a plausible piece length. */
    const char ovf[] = "d4:infod6:lengthi99999999999999999999999e4:name4:test"
                       "12:piece lengthi99999999999999999999999e6:pieces20:"
                       "HHHHHHHHHHHHHHHHHHHHee";
    assert(!metainfo_parse((const uint8_t *)ovf, sizeof(ovf) - 1, &mi));
}

static void test_metainfo_path_join_bounds(void) {
    metainfo_t mi;
    const char *ok[] = {"dir", "file.nsp"};
    assert(parse_with_path(ok, 2, &mi));
    assert(strcmp(mi.files[0].path, "dir/file.nsp") == 0);
    metainfo_free(&mi);

    /* Each component fits on its own; together they do not. This used to
       walk off the end of a MAX_NAME_LEN stack buffer, one strncat per
       component, before the path was ever checked for safety. */
    char big[201];
    memset(big, 'A', 200);
    big[200] = 0;
    const char *toolong[] = {big, big, big};
    assert(!parse_with_path(toolong, 3, &mi));
}

static void test_ema_update(void) {
    uint64_t value = ema_update(0, 1000);
    assert(value == 300);

    uint64_t previous = value;
    for (int i = 0; i < 8; ++i) {
        value = ema_update(value, 1000);
        assert(value >= previous);
        assert(value <= 1000);
        previous = value;
    }

    value = ema_update(1000, 0);
    assert(value == 700);
    previous = value;
    for (int i = 0; i < 8; ++i) {
        value = ema_update(value, 0);
        assert(value <= previous);
        previous = value;
    }
}

static void test_last_piece_age_marks_missing_sample(void) {
    assert(last_piece_age_ms(10000, 0) == -1);
    assert(last_piece_age_ms(10000, 9500) == 500);
    assert(last_piece_age_ms(10000, 10001) == -1);
}

static void test_adaptive_hedge_follows_median_latency(void) {
    torrent_t torrent = {0};
    torrent.hedge_after_ms = 5000;
    peer_t peers[5] = {0};

    /* No sampled peers: static fallback. */
    assert(adaptive_hedge_after_ms(&torrent) == 5000);

    /* One sampled peer is below HEDGE_MIN_LATENCY_PEERS: still static. */
    peers[0].state = PS_ACTIVE;
    peers[0].block_lat_ema_ms = 200;
    torrent.peers[0] = &peers[0];
    assert(adaptive_hedge_after_ms(&torrent) == 5000);

    /* Median of {200, 300, 400} = 300 -> 4 * 300 = 1200. */
    peers[1].state = PS_ACTIVE;
    peers[1].block_lat_ema_ms = 400;
    peers[2].state = PS_ACTIVE;
    peers[2].block_lat_ema_ms = 300;
    torrent.peers[7] = &peers[1];
    torrent.peers[3] = &peers[2];
    assert(adaptive_hedge_after_ms(&torrent) == 1200);

    /* Peers without a sample or not active are ignored. */
    peers[3].state = PS_ACTIVE; /* no latency sample yet */
    peers[4].state = PS_CONNECTING;
    peers[4].block_lat_ema_ms = 9000;
    torrent.peers[10] = &peers[3];
    torrent.peers[11] = &peers[4];
    assert(adaptive_hedge_after_ms(&torrent) == 1200);

    /* Fast swarm clamps to the floor... */
    peers[0].block_lat_ema_ms = 50;
    peers[1].block_lat_ema_ms = 60;
    peers[2].block_lat_ema_ms = 70;
    assert(adaptive_hedge_after_ms(&torrent) == HEDGE_ADAPTIVE_MIN_MS);

    /* ...and a slow swarm never exceeds the static threshold. */
    peers[0].block_lat_ema_ms = 3000;
    peers[1].block_lat_ema_ms = 4000;
    peers[2].block_lat_ema_ms = 5000;
    assert(adaptive_hedge_after_ms(&torrent) == 5000);
}

static void test_rate_freeze_preserves_peer_dl_rate(void) {
    torrent_t torrent = {0};
    peer_t peer = {0};
    peer.state = PS_ACTIVE;
    peer.dl_rate_bps = 4 * 1024 * 1024;
    torrent.peers[0] = &peer;

    /* Unfrozen idle interval decays the EMA (pre-7.2 behaviour). */
    sample_peer_rates(&torrent, 1000, 0);
    assert(peer.dl_rate_bps < 4 * 1024 * 1024);

    /* Frozen: the EMA holds exactly and the interval's bytes are
       discarded from measurement, however small the trickle. */
    uint64_t held = peer.dl_rate_bps;
    torrent.rate_freeze = 1;
    for (int i = 0; i < 30; ++i) {
        peer.downloaded += 100 * 1024;
        sample_peer_rates(&torrent, 1000, 0);
        assert(peer.dl_rate_bps == held);
        assert(peer.rate_last_downloaded == peer.downloaded);
    }

    /* Resume: only post-resume bytes enter the next sample, so a healthy
       interval moves the EMA up instead of averaging in the gated lull. */
    torrent.rate_freeze = 0;
    peer.downloaded += 8 * 1024 * 1024;
    sample_peer_rates(&torrent, 1000, 0);
    assert(peer.dl_rate_bps > held);
    assert(peer.rate_last_downloaded == peer.downloaded);
}

static void test_probe_window_until_first_block(void) {
    torrent_t torrent = {0};
    torrent.request_pipeline_limit = 256;
    peer_t peer = {0};
    peer.state = PS_ACTIVE;
    peer.dl_rate_bps = 8 * 1024 * 1024;

    /* No block delivered yet: shallow probe window regardless of rate. */
    assert(peer_pipeline_limit(&torrent, &peer) == BOOTSTRAP_PIPELINE);

    /* First block arrived: window follows the rate estimate again
       (8 MiB/s wants 1024 in flight, clamped to the per-peer ceiling). */
    peer.last_piece_ms = 1;
    assert(peer_pipeline_limit(&torrent, &peer) ==
           torrent.request_pipeline_limit);
    peer.dl_rate_bps = 1024 * 1024;
    assert(peer_pipeline_limit(&torrent, &peer) ==
           1024ULL * 1024 * PIPELINE_TARGET_MS / 1000 / BLOCK_SIZE);
}

static void test_window_binding_growth(void) {
    torrent_t torrent = {0};
    torrent.request_pipeline_limit = 256;
    peer_t peer = {0};
    peer.state = PS_ACTIVE;
    peer.last_piece_ms = 1;
    peer.dl_rate_bps = 1024 * 1024;
    torrent.peers[0] = &peer;

    /* Window full, delivering, clean: the estimate grows multiplicatively
       past what the EMA alone would settle at. */
    peer.pipeline_len = (int)peer_pipeline_limit(&torrent, &peer);
    peer.downloaded = 1024 * 1024;
    sample_peer_rates(&torrent, 1000, 10000);
    assert(peer.dl_rate_bps == 1024 * 1024 * 3 / 2);

    /* Recent expiry blocks growth: plain EMA applies. */
    peer.pipeline_len = (int)peer_pipeline_limit(&torrent, &peer);
    peer.last_expiry_ms = 10500;
    uint64_t before = peer.dl_rate_bps;
    peer.downloaded += before;
    sample_peer_rates(&torrent, 1000, 11000);
    assert(peer.dl_rate_bps <= before);

    /* Idle window with an empty pipeline decays as before. */
    peer.pipeline_len = 0;
    peer.last_expiry_ms = 0;
    before = peer.dl_rate_bps;
    sample_peer_rates(&torrent, 1000, 12000);
    assert(peer.dl_rate_bps < before);
}

static void test_stat_counts_active_peers(void) {
    torrent_t torrent = {0};
    piece_mgr_t pm = {0};
    peer_t peers[3] = {0};
    torrent.pm = &pm;
    torrent.num_peers = 3;
    peers[0].state = PS_ACTIVE;
    peers[1].state = PS_CONNECTING;
    peers[2].state = PS_ACTIVE;
    torrent.peers[0] = &peers[0];
    torrent.peers[5] = &peers[1];
    torrent.peers[9] = &peers[2];

    torrent_stat_t stat;
    torrent_stat(&torrent, &stat);
    assert(stat.num_peers == 3);
    assert(stat.num_active_peers == 2);
}

static void test_copy_have_bitfield_guards(void) {
    torrent_t torrent = {0};
    piece_mgr_t pm = {0};
    uint8_t have_bf[2] = {0xa5, 0x30};
    pm.num_pieces = 12;
    pm.have_bf = have_bf;
    torrent.pm = &pm;

    assert(torrent_copy_have_bitfield(NULL, NULL, 0) == 0);
    assert(torrent_copy_have_bitfield(&torrent, NULL, 0) == 2);

    uint8_t out[2] = {0};
    assert(torrent_copy_have_bitfield(&torrent, out, sizeof(out)) == 2);
    assert(out[0] == 0xa5 && out[1] == 0x30);

    /* Undersized buffer refused. */
    assert(torrent_copy_have_bitfield(&torrent, out, 1) == 0);

    /* Mid-startup-scan the bitfield is incomplete: refuse both forms. */
    torrent.startup_verifying = 1;
    assert(torrent_copy_have_bitfield(&torrent, NULL, 0) == 0);
    assert(torrent_copy_have_bitfield(&torrent, out, sizeof(out)) == 0);
}

static void test_blocklist_cooldown_and_wrap(void) {
    torrent_t torrent = {0};
    uint32_t ip = htonl(0x5bd4c901u);
    uint16_t port = htons(6881);

    blocklist_add(&torrent, ip, port, 1000);
    assert(blocklist_blocked(&torrent, ip, port, 1000));
    assert(blocklist_blocked(&torrent, ip, port, 60999));
    assert(!blocklist_blocked(&torrent, ip, port, 61000));

    memset(&torrent, 0, sizeof(torrent));
    for (uint32_t i = 0; i <= 64; ++i)
        blocklist_add(&torrent, htonl(0x0b000001u + i), port, 2000 + i);
    assert(!blocklist_blocked(&torrent, htonl(0x0b000001u), port, 3000));
    assert(blocklist_blocked(&torrent, htonl(0x0b000041u), port, 3000));
}

static void test_initial_peers_keep_verified_order(void) {
    torrent_t torrent = {0};
    uint32_t laterIp = htonl(0x08080808u);
    uint16_t laterPort = htons(6881);
    assert(queue_push(&torrent, laterIp, laterPort));
    const uint8_t compact[] = {
        93, 184, 216, 34, 0x1a, 0xe1,
        1, 1, 1, 1, 0xc8, 0xd5,
        93, 184, 216, 34, 0x1a, 0xe1,
    };

    assert(torrent_add_initial_peers(&torrent, compact, 3) == 2);
    assert(torrent.qsize == 3);

    uint32_t ip = 0;
    uint16_t port = 0;
    uint8_t no_mse = 0;
    uint8_t use_utp = 1;
    assert(queue_pop(&torrent, &ip, &port, &no_mse, &use_utp));
    assert(memcmp(&ip, compact, 4) == 0);
    assert(memcmp(&port, compact + 4, 2) == 0);
    assert(no_mse == 0);  /* tracker/DHT peers dial TCP+MSE first */
    assert(use_utp == 0);
    assert(queue_pop(&torrent, &ip, &port, &no_mse, &use_utp));
    assert(memcmp(&ip, compact + 6, 4) == 0);
    assert(memcmp(&port, compact + 10, 2) == 0);
    assert(queue_pop(&torrent, &ip, &port, &no_mse, &use_utp));
    assert(ip == laterIp);
    assert(port == laterPort);
    assert(!queue_pop(&torrent, &ip, &port, &no_mse, &use_utp));

    /* A plaintext-fallback re-queue carries no_mse back out; a μTP-fallback
       re-queue carries use_utp. Both flags survive the queue independently. */
    assert(queue_insert(&torrent, laterIp, laterPort, 1, 1, 0));
    ip = 0; port = 0; no_mse = 0; use_utp = 1;
    assert(queue_pop(&torrent, &ip, &port, &no_mse, &use_utp));
    assert(ip == laterIp && port == laterPort && no_mse == 1 && use_utp == 0);
    assert(queue_insert(&torrent, laterIp, laterPort, 1, 0, 1));
    ip = 0; port = 0; no_mse = 1; use_utp = 0;
    assert(queue_pop(&torrent, &ip, &port, &no_mse, &use_utp));
    assert(ip == laterIp && port == laterPort && no_mse == 0 && use_utp == 1);
}

int main(void) {
    test_ema_update();
    test_last_piece_age_marks_missing_sample();
    test_adaptive_hedge_follows_median_latency();
    test_rate_freeze_preserves_peer_dl_rate();
    test_probe_window_until_first_block();
    test_window_binding_growth();
    test_stat_counts_active_peers();
    test_copy_have_bitfield_guards();
    test_blocklist_cooldown_and_wrap();
    test_initial_peers_keep_verified_order();
    test_bencode_rejects_deep_nesting();
    test_metainfo_path_join_bounds();
    test_metainfo_rejects_bad_numbers();
    test_udp_tracker_checks_transaction_id();
    puts("torrent tests passed");
    return 0;
}
