#include "seed_cache.h"
#include <stdlib.h>
#include <string.h>

seed_cache_t *seed_cache_create(uint32_t slots, uint32_t piece_length) {
    if (!slots || !piece_length || (uint64_t)slots * piece_length > SIZE_MAX ||
        (uint64_t)slots * sizeof(uint32_t) > SIZE_MAX ||
        (uint64_t)slots * sizeof(uint8_t *) > SIZE_MAX)
        return NULL;
    seed_cache_t *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;
    cache->indices = malloc((size_t)slots * sizeof(uint32_t));
    cache->slot_data = calloc(slots, sizeof(*cache->slot_data));
    cache->blocks = calloc(slots, sizeof(*cache->blocks));
    if (!cache->indices || !cache->slot_data || !cache->blocks) {
        seed_cache_destroy(cache);
        return NULL;
    }
    cache->piece_length = piece_length;
    const uint32_t block_slots = piece_length < 64u * 1024 * 1024
        ? (64u * 1024 * 1024) / piece_length : 1;
    while (cache->slots < slots) {
        uint32_t count = slots - cache->slots;
        if (count > block_slots) count = block_slots;
        uint8_t *block = NULL;
        while (count && !(block = malloc((size_t)count * piece_length)))
            count /= 2;
        if (!count) break;
        cache->blocks[cache->block_count++] = block;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t slot = cache->slots++;
            cache->slot_data[slot] = block + (size_t)i * piece_length;
            cache->indices[slot] = UINT32_MAX;
        }
    }
    if (!cache->slots) {
        seed_cache_destroy(cache);
        return NULL;
    }
    return cache;
}

void seed_cache_destroy(seed_cache_t *cache) {
    if (!cache) return;
    free(cache->indices);
    for (uint32_t i = 0; i < cache->block_count; ++i) free(cache->blocks[i]);
    free(cache->blocks);
    free(cache->slot_data);
    free(cache);
}

const uint8_t *seed_cache_get(const seed_cache_t *cache, uint32_t index) {
    if (!cache || index == UINT32_MAX) return NULL;
    for (uint32_t i = 0; i < cache->slots; ++i)
        if (cache->indices[i] == index)
            return cache->slot_data[i];
    return NULL;
}

uint32_t seed_cache_put(seed_cache_t *cache, uint32_t index,
                        const uint8_t *data, uint32_t length) {
    if (!cache || !data || index == UINT32_MAX ||
        !length || length > cache->piece_length)
        return UINT32_MAX;
    uint32_t slot = cache->head;
    int existing = 0;
    for (uint32_t i = 0; i < cache->slots; ++i) {
        if (cache->indices[i] == index) {
            slot = i;
            existing = 1;
            break;
        }
    }
    uint32_t evicted = cache->indices[slot];
    memcpy(cache->slot_data[slot], data, length);
    cache->indices[slot] = index;
    if (!existing) cache->head = (slot + 1) % cache->slots;
    return evicted == index ? UINT32_MAX : evicted;
}
