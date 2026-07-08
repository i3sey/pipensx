#ifndef PIPENSX_SHA256_H
#define PIPENSX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sha256(const void *data, size_t len, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif
