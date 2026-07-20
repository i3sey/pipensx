#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * Message Stream Encryption / Protocol Encryption (MSE/PE) primitives.
 *
 * Self-contained crypto for the outgoing-connection (initiator) side of the
 * MSE handshake: RC4 keystream, the fixed 768-bit Diffie-Hellman group, and the
 * SHA-1 key derivation. The stateful handshake framing that uses these lives in
 * the peer layer; this file is pure, side-effect-free, and unit-tested against
 * OpenSSL so the bignum path can be trusted before any network integration.
 *
 * Portability: the modular exponentiation delegates to mbedtls on Switch
 * (USE_MBEDTLS) and OpenSSL on the host test build. RC4 and SHA-1 are our own,
 * so the protocol logic is identical on every target.
 */

#define MSE_DH_LEN   96   /* 768-bit DH public key / shared secret, big-endian */
#define MSE_VC_LEN   8    /* verification constant: 8 zero bytes */

/* crypto_provide / crypto_select bitfield (BEP-unofficial MSE). */
#define MSE_CRYPTO_PLAINTEXT 0x01u
#define MSE_CRYPTO_RC4       0x02u

/* ---- RC4 ---- */
typedef struct {
    uint8_t s[256];
    uint8_t i, j;
} rc4_t;

void rc4_init(rc4_t *rc4, const uint8_t *key, size_t key_len);
/* XOR `len` keystream bytes over in->out (in==out allowed). */
void rc4_crypt(rc4_t *rc4, const uint8_t *in, uint8_t *out, size_t len);
/* Discard `n` keystream bytes (MSE requires discarding the first 1024). */
void rc4_discard(rc4_t *rc4, size_t n);

/* ---- Diffie-Hellman (g = 2, P = MSE 768-bit prime) ---- */

/* The MSE prime, big-endian, 96 bytes. */
extern const uint8_t MSE_PRIME[MSE_DH_LEN];

/*
 * Fill `priv` with a fresh private exponent (caller supplies randomness via
 * rand_bytes). pub = 2^priv mod P. Both outputs are MSE_DH_LEN big-endian.
 */
void mse_dh_public(const uint8_t priv[MSE_DH_LEN], uint8_t pub[MSE_DH_LEN]);

/* secret = peer_pub^priv mod P, MSE_DH_LEN big-endian. */
void mse_dh_secret(const uint8_t priv[MSE_DH_LEN],
                   const uint8_t peer_pub[MSE_DH_LEN],
                   uint8_t secret[MSE_DH_LEN]);

/*
 * Derive an RC4 stream keyed by HASH(label | S | SKEY), where label is "keyA"
 * (initiator's send stream) or "keyB" (initiator's receive stream), S is the
 * DH shared secret, and SKEY is the 20-byte info hash. The returned stream has
 * already discarded its first 1024 keystream bytes, per MSE.
 */
void mse_stream_key(const char label[4], const uint8_t secret[MSE_DH_LEN],
                    const uint8_t info_hash[20], rc4_t *out);
