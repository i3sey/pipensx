#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t slots, piece_length, head;
    uint32_t *indices;
    uint8_t **slot_data;
    uint8_t **blocks;
    uint32_t block_count;
} seed_cache_t;

/* May return fewer slots when even a single further piece cannot fit. */
seed_cache_t *seed_cache_create(uint32_t slots, uint32_t piece_length);
void seed_cache_destroy(seed_cache_t *cache);
uint32_t seed_cache_put(seed_cache_t *cache, uint32_t index,
                        const uint8_t *data, uint32_t length);
const uint8_t *seed_cache_get(const seed_cache_t *cache, uint32_t index);
