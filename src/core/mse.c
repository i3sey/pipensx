#include "mse.h"
#include "sha1.h"

#include <string.h>

/* Canonical MSE/PE 768-bit prime (Vuze/libtorrent), big-endian, g = 2. */
const uint8_t MSE_PRIME[MSE_DH_LEN] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC9,0x0F,0xDA,0xA2,
    0x21,0x68,0xC2,0x34,0xC4,0xC6,0x62,0x8B,0x80,0xDC,0x1C,0xD1,
    0x29,0x02,0x4E,0x08,0x8A,0x67,0xCC,0x74,0x02,0x0B,0xBE,0xA6,
    0x3B,0x13,0x9B,0x22,0x51,0x4A,0x08,0x79,0x8E,0x34,0x04,0xDD,
    0xEF,0x95,0x19,0xB3,0xCD,0x3A,0x43,0x1B,0x30,0x2B,0x0A,0x6D,
    0xF2,0x5F,0x14,0x37,0x4F,0xE1,0x35,0x6D,0x6D,0x51,0xC2,0x45,
    0xE4,0x85,0xB5,0x76,0x62,0x5E,0x7E,0xC6,0xF4,0x4C,0x42,0xE9,
    0xA6,0x3A,0x36,0x21,0x00,0x00,0x00,0x00,0x00,0x09,0x05,0x63
};

/* ---- RC4 ---- */
void rc4_init(rc4_t *rc4, const uint8_t *key, size_t key_len) {
    for (int n = 0; n < 256; ++n)
        rc4->s[n] = (uint8_t)n;
    rc4->i = rc4->j = 0;
    if (!key_len)
        return;
    uint8_t j = 0;
    for (int n = 0; n < 256; ++n) {
        j = (uint8_t)(j + rc4->s[n] + key[n % key_len]);
        uint8_t t = rc4->s[n];
        rc4->s[n] = rc4->s[j];
        rc4->s[j] = t;
    }
}

void rc4_crypt(rc4_t *rc4, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t i = rc4->i, j = rc4->j;
    for (size_t n = 0; n < len; ++n) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + rc4->s[i]);
        uint8_t t = rc4->s[i];
        rc4->s[i] = rc4->s[j];
        rc4->s[j] = t;
        uint8_t k = rc4->s[(uint8_t)(rc4->s[i] + rc4->s[j])];
        out[n] = (uint8_t)(in[n] ^ k);
    }
    rc4->i = i;
    rc4->j = j;
}

void rc4_discard(rc4_t *rc4, size_t n) {
    uint8_t i = rc4->i, j = rc4->j;
    for (size_t k = 0; k < n; ++k) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + rc4->s[i]);
        uint8_t t = rc4->s[i];
        rc4->s[i] = rc4->s[j];
        rc4->s[j] = t;
    }
    rc4->i = i;
    rc4->j = j;
}

/* ---- Diffie-Hellman ---- */

#ifdef USE_MBEDTLS
#include <mbedtls/bignum.h>

static void dh_modexp(const uint8_t *base, size_t base_len,
                      const uint8_t exp[MSE_DH_LEN],
                      uint8_t out[MSE_DH_LEN]) {
    mbedtls_mpi b, e, p, r;
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_read_binary(&b, base, base_len);
    mbedtls_mpi_read_binary(&e, exp, MSE_DH_LEN);
    mbedtls_mpi_read_binary(&p, MSE_PRIME, MSE_DH_LEN);
    mbedtls_mpi_exp_mod(&r, &b, &e, &p, NULL);
    mbedtls_mpi_write_binary(&r, out, MSE_DH_LEN);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&r);
}
#else
#include <openssl/bn.h>

static void dh_modexp(const uint8_t *base, size_t base_len,
                      const uint8_t exp[MSE_DH_LEN],
                      uint8_t out[MSE_DH_LEN]) {
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *b = BN_bin2bn(base, (int)base_len, NULL);
    BIGNUM *e = BN_bin2bn(exp, MSE_DH_LEN, NULL);
    BIGNUM *p = BN_bin2bn(MSE_PRIME, MSE_DH_LEN, NULL);
    BIGNUM *r = BN_new();
    BN_mod_exp(r, b, e, p, ctx);
    BN_bn2binpad(r, out, MSE_DH_LEN);
    BN_free(b);
    BN_free(e);
    BN_free(p);
    BN_free(r);
    BN_CTX_free(ctx);
}
#endif

void mse_dh_public(const uint8_t priv[MSE_DH_LEN], uint8_t pub[MSE_DH_LEN]) {
    static const uint8_t g[1] = {2};
    dh_modexp(g, sizeof(g), priv, pub);
}

void mse_dh_secret(const uint8_t priv[MSE_DH_LEN],
                   const uint8_t peer_pub[MSE_DH_LEN],
                   uint8_t secret[MSE_DH_LEN]) {
    dh_modexp(peer_pub, MSE_DH_LEN, priv, secret);
}

void mse_stream_key(const char label[4], const uint8_t secret[MSE_DH_LEN],
                    const uint8_t info_hash[20], rc4_t *out) {
    uint8_t key[20];
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, label, 4);
    sha1_update(&ctx, secret, MSE_DH_LEN);
    sha1_update(&ctx, info_hash, 20);
    sha1_final(&ctx, key);
    rc4_init(out, key, sizeof(key));
    rc4_discard(out, 1024); /* MSE: throw away the first 1024 keystream bytes */
}
