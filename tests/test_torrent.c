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

int main(void) {
    test_ema_update();
    test_blocklist_cooldown_and_wrap();
    puts("torrent tests passed");
    return 0;
}
