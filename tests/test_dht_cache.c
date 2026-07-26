// Validates the DHT node-cache codec: a write/read round-trip preserves the
// node ID and endpoint list, and malformed files (bad magic, truncation,
// oversized count, legacy raw-sockaddr dumps) are rejected instead of
// poisoning the routing table warm start.
#include "../src/core/dht.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *kPath = "test_dht_cache.bin";

static void fill_nodes(uint8_t (*nodes)[6], int count) {
    for (int i = 0; i < count; i++) {
        nodes[i][0] = 10;
        nodes[i][1] = 0;
        nodes[i][2] = (uint8_t)(i >> 8);
        nodes[i][3] = (uint8_t)i;
        nodes[i][4] = (uint8_t)(0x1a + (i >> 8));
        nodes[i][5] = (uint8_t)(0xe1 + i);
    }
}

static void test_roundtrip(void) {
    uint8_t id[20];
    for (int i = 0; i < 20; i++) id[i] = (uint8_t)(i * 7 + 1);
    uint8_t nodes[128][6];
    fill_nodes(nodes, 128);

    assert(dht_cache_write(kPath, id, nodes, 128) == 1);

    uint8_t id_back[20];
    uint8_t back[DHT_CACHE_MAX_NODES][6];
    int n = dht_cache_read(kPath, id_back, back, DHT_CACHE_MAX_NODES);
    assert(n == 128);
    assert(memcmp(id_back, id, 20) == 0);
    assert(memcmp(back, nodes, 128 * 6) == 0);

    // Caller with a smaller buffer gets a clamped, valid prefix.
    int clamped = dht_cache_read(kPath, id_back, back, 16);
    assert(clamped == 16);
    assert(memcmp(back, nodes, 16 * 6) == 0);
}

static void test_write_rejects_bad_counts(void) {
    uint8_t id[20] = {0};
    uint8_t nodes[1][6] = {{1, 2, 3, 4, 5, 6}};
    assert(dht_cache_write(kPath, id, nodes, 0) == 0);
    assert(dht_cache_write(kPath, id, nodes, -3) == 0);
    assert(dht_cache_write(kPath, id, nodes, DHT_CACHE_MAX_NODES + 1) == 0);
}

static void test_read_rejects_malformed(void) {
    uint8_t id[20];
    uint8_t nodes[DHT_CACHE_MAX_NODES][6];

    assert(dht_cache_read("no_such_file.bin", id, nodes,
                          DHT_CACHE_MAX_NODES) == -1);

    // Bad magic (a legacy raw-sockaddr dump starts with the int count).
    FILE *f = fopen(kPath, "wb");
    assert(f);
    int legacy_count = 42;
    fwrite(&legacy_count, sizeof(int), 1, f);
    fclose(f);
    assert(dht_cache_read(kPath, id, nodes, DHT_CACHE_MAX_NODES) == -1);

    // Truncated: header promises 4 nodes, file holds 2.
    uint8_t good_id[20] = {9};
    uint8_t two[2][6];
    fill_nodes(two, 2);
    f = fopen(kPath, "wb");
    assert(f);
    uint8_t cnt_le[2] = {4, 0};
    fwrite(DHT_CACHE_MAGIC, 1, 4, f);
    fwrite(good_id, 1, 20, f);
    fwrite(cnt_le, 1, 2, f);
    fwrite(two, 6, 2, f);
    fclose(f);
    assert(dht_cache_read(kPath, id, nodes, DHT_CACHE_MAX_NODES) == -1);

    // Count above the format limit.
    f = fopen(kPath, "wb");
    assert(f);
    uint8_t big_cnt[2] = {(uint8_t)((DHT_CACHE_MAX_NODES + 1) & 0xff),
                          (uint8_t)((DHT_CACHE_MAX_NODES + 1) >> 8)};
    fwrite(DHT_CACHE_MAGIC, 1, 4, f);
    fwrite(good_id, 1, 20, f);
    fwrite(big_cnt, 1, 2, f);
    fclose(f);
    assert(dht_cache_read(kPath, id, nodes, DHT_CACHE_MAX_NODES) == -1);
}

static void test_write_replaces_existing(void) {
    uint8_t id_a[20], id_b[20];
    memset(id_a, 0xaa, sizeof(id_a));
    memset(id_b, 0xbb, sizeof(id_b));
    uint8_t nodes[8][6];
    fill_nodes(nodes, 8);

    assert(dht_cache_write(kPath, id_a, nodes, 8) == 1);
    assert(dht_cache_write(kPath, id_b, nodes, 3) == 1);

    uint8_t id_back[20];
    uint8_t back[DHT_CACHE_MAX_NODES][6];
    int n = dht_cache_read(kPath, id_back, back, DHT_CACHE_MAX_NODES);
    assert(n == 3);
    assert(memcmp(id_back, id_b, 20) == 0);

    // The atomic write leaves no temp file behind.
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", kPath);
    assert(access(tmp, F_OK) != 0);
}

int main(void) {
    test_roundtrip();
    test_write_rejects_bad_counts();
    test_read_rejects_malformed();
    test_write_replaces_existing();
    unlink(kPath);
    printf("test_dht_cache: all tests passed\n");
    return 0;
}
