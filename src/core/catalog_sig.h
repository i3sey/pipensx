#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ed25519 detached-signature verification for the catalog (RF_ACCESS_PLAN
   П3.2). Returns 1 when `sig` (64 bytes) is a valid signature of the `len`
   message bytes under `pubkey` (32 bytes), 0 otherwise. Constant work
   regardless of where the mismatch is; no allocation on failure paths beyond
   the single verify scratch buffer. */
int catalog_sig_verify(const uint8_t pubkey[32], const uint8_t *msg,
                       size_t len, const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
