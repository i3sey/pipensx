#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/core/torrent.c"

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
    sample_peer_rates(&torrent, 1000);
    assert(peer.dl_rate_bps < 4 * 1024 * 1024);

    /* Frozen: the EMA holds exactly and the interval's bytes are
       discarded from measurement, however small the trickle. */
    uint64_t held = peer.dl_rate_bps;
    torrent.rate_freeze = 1;
    for (int i = 0; i < 30; ++i) {
        peer.downloaded += 100 * 1024;
        sample_peer_rates(&torrent, 1000);
        assert(peer.dl_rate_bps == held);
        assert(peer.rate_last_downloaded == peer.downloaded);
    }

    /* Resume: only post-resume bytes enter the next sample, so a healthy
       interval moves the EMA up instead of averaging in the gated lull. */
    torrent.rate_freeze = 0;
    peer.downloaded += 8 * 1024 * 1024;
    sample_peer_rates(&torrent, 1000);
    assert(peer.dl_rate_bps > held);
    assert(peer.rate_last_downloaded == peer.downloaded);
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
    assert(queue_pop(&torrent, &ip, &port, &no_mse));
    assert(memcmp(&ip, compact, 4) == 0);
    assert(memcmp(&port, compact + 4, 2) == 0);
    assert(queue_pop(&torrent, &ip, &port, &no_mse));
    assert(memcmp(&ip, compact + 6, 4) == 0);
    assert(memcmp(&port, compact + 10, 2) == 0);
    assert(queue_pop(&torrent, &ip, &port, &no_mse));
    assert(ip == laterIp);
    assert(port == laterPort);
    assert(!queue_pop(&torrent, &ip, &port, &no_mse));

    // A plaintext-fallback re-queue carries the no_mse flag back out.
    assert(queue_insert(&torrent, laterIp, laterPort, 1, 1));
    ip = 0; port = 0; no_mse = 0;
    assert(queue_pop(&torrent, &ip, &port, &no_mse));
    assert(ip == laterIp && port == laterPort && no_mse == 1);
}

int main(void) {
    test_ema_update();
    test_last_piece_age_marks_missing_sample();
    test_adaptive_hedge_follows_median_latency();
    test_rate_freeze_preserves_peer_dl_rate();
    test_stat_counts_active_peers();
    test_blocklist_cooldown_and_wrap();
    test_initial_peers_keep_verified_order();
    puts("torrent tests passed");
    return 0;
}
