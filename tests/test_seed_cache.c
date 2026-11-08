#include "core/seed_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static size_t max_allocation = SIZE_MAX;
static size_t allocation_budget = SIZE_MAX;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    if (size > max_allocation || size > allocation_budget) return NULL;
    void *result = __real_malloc(size);
    if (result && allocation_budget != SIZE_MAX) allocation_budget -= size;
    return result;
}

static void test_fragmented_heap_retains_partial_capacity(void) {
    const size_t MiB = 1024 * 1024;
    uint8_t *piece = __real_malloc(MiB);
    assert(piece);
    memset(piece, 42, MiB);
    max_allocation = MiB;
    seed_cache_t *cache = seed_cache_create(5, (uint32_t)MiB);
    assert(cache && cache->slots == 5 && cache->block_count == 5);
    seed_cache_destroy(cache);
    allocation_budget = 3 * MiB + 5 * sizeof(uint32_t);
    cache = seed_cache_create(5, (uint32_t)MiB);
    assert(cache && cache->slots == 3 && cache->block_count == 3);
    max_allocation = allocation_budget = SIZE_MAX;
    for (uint32_t i = 0; i < 4; ++i) {
        piece[0] = (uint8_t)i;
        uint32_t evicted = seed_cache_put(cache, i, piece, (uint32_t)MiB);
        assert(evicted == (i == 3 ? 0 : UINT32_MAX));
    }
    assert(!seed_cache_get(cache, 0));
    for (uint32_t i = 1; i < 4; ++i) {
        const uint8_t *stored = seed_cache_get(cache, i);
        assert(stored && stored[0] == i && stored[MiB - 1] == 42);
    }
    seed_cache_destroy(cache);
    free(piece);
}

int main(void) {
    test_fragmented_heap_retains_partial_capacity();
    uint8_t data[16];
    memset(data, 42, sizeof(data));
    assert(!seed_cache_create(0, 16));
    assert(!seed_cache_create(2, 0));
    seed_cache_t *cache = seed_cache_create(2, 16);
    assert(cache);
    assert(seed_cache_put(cache, 7, data, 16) == UINT32_MAX);
    assert(seed_cache_put(cache, 9, data, 3) == UINT32_MAX);
    assert(!memcmp(seed_cache_get(cache, 7), data, 16));
    data[0] = 99;
    assert(seed_cache_put(cache, 7, data, 16) == UINT32_MAX);
    assert(seed_cache_put(cache, 10, data, 16) == 7);
    assert(!seed_cache_get(cache, 7));
    assert(seed_cache_get(cache, 9)[0] == 42);
    assert(seed_cache_get(cache, 10)[0] == 99);
    assert(seed_cache_put(cache, 11, data, 16) == 9);
    assert(!seed_cache_get(cache, 9));
    assert(seed_cache_put(cache, 12, data, 17) == UINT32_MAX);
    assert(!seed_cache_get(cache, 12));
    seed_cache_destroy(cache);
    seed_cache_destroy(NULL);
    puts("seed cache tests passed");
}
