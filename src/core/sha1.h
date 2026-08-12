#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __SWITCH__
/* libnx hardware SHA-1 (ARMv8 Crypto Extensions) */
#include <switch/crypto/sha1.h>
typedef Sha1Context sha1_ctx_t;
#else
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buf[64];
} sha1_ctx_t;
#endif

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]);

/* One-shot helper */
void sha1(const void *data, size_t len, uint8_t digest[20]);
