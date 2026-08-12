#ifndef PIPENSX_SHA256_H
#define PIPENSX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t data[64];
    uint32_t state[8];
    uint64_t bit_len;
    size_t data_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);
void sha256(const void *data, size_t len, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif
