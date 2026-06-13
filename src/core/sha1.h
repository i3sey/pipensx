#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buf[64];
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]);

/* One-shot helper */
void sha1(const void *data, size_t len, uint8_t digest[20]);
