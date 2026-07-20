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

/* ---- initiator handshake ---- */

enum {
    ST_READ_PUB = 0,
    ST_SYNC_VC,
    ST_READ_SELECT,
    ST_READ_PADD,
    ST_DONE,
    ST_FAIL = -1
};

/* HASH('reqN' | data) helper. */
static void req_hash(const char tag[4], const uint8_t *data, size_t len,
                     uint8_t out[20]) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, tag, 4);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

mse_status_t mse_client_start(mse_client_t *c, const uint8_t info_hash[20],
                              const uint8_t priv[MSE_DH_LEN],
                              const uint8_t *pad, size_t pad_len,
                              const uint8_t *ia, size_t ia_len,
                              uint8_t *out, size_t out_cap, size_t *produced) {
    *produced = 0;
    if (ia_len > MSE_MAX_IA || pad_len > MSE_MAX_PAD)
        return MSE_FAIL;
    memset(c, 0, sizeof(*c));
    memcpy(c->info_hash, info_hash, 20);
    memcpy(c->priv, priv, MSE_DH_LEN);
    memcpy(c->ia, ia, ia_len);
    c->ia_len = ia_len;

    if (out_cap < MSE_DH_LEN + pad_len)
        return MSE_FAIL;
    uint8_t pub[MSE_DH_LEN];
    mse_dh_public(priv, pub);
    memcpy(out, pub, MSE_DH_LEN);
    if (pad_len)
        memcpy(out + MSE_DH_LEN, pad, pad_len);
    *produced = MSE_DH_LEN + pad_len;
    c->state = ST_READ_PUB;
    return MSE_CONTINUE;
}

static mse_status_t emit_request(mse_client_t *c, const uint8_t secret[MSE_DH_LEN],
                                 uint8_t *out, size_t out_cap, size_t *produced) {
    // req1 | (req2 xor req3) | RC4( VC | crypto_provide | 00 00 | ia_len | IA )
    uint8_t req1[20], h2[20], h3[20];
    req_hash("req1", secret, MSE_DH_LEN, req1);
    req_hash("req2", c->info_hash, 20, h2);
    req_hash("req3", secret, MSE_DH_LEN, h3);

    uint8_t plain[MSE_VC_LEN + 4 + 2 + 2 + MSE_MAX_IA];
    size_t n = 0;
    memset(plain + n, 0, MSE_VC_LEN); n += MSE_VC_LEN;          // VC = 8 zeros
    plain[n++] = 0; plain[n++] = 0; plain[n++] = 0;
    plain[n++] = MSE_CRYPTO_RC4;                                // crypto_provide
    plain[n++] = 0; plain[n++] = 0;                             // len(padC) = 0
    plain[n++] = (uint8_t)(c->ia_len >> 8);
    plain[n++] = (uint8_t)(c->ia_len & 0xFF);                   // len(IA)
    memcpy(plain + n, c->ia, c->ia_len); n += c->ia_len;

    size_t total = 20 + 20 + n;
    if (out_cap < total)
        return MSE_FAIL;
    memcpy(out, req1, 20);
    for (int i = 0; i < 20; ++i)
        out[20 + i] = (uint8_t)(h2[i] ^ h3[i]);
    rc4_crypt(&c->send_rc4, plain, out + 40, n);               // encrypt block
    *produced = total;
    return MSE_CONTINUE;
}

mse_status_t mse_client_feed(mse_client_t *c, const uint8_t *in, size_t in_len,
                             size_t *consumed, uint8_t *out, size_t out_cap,
                             size_t *produced) {
    *consumed = 0;
    *produced = 0;

    if (c->state == ST_READ_PUB) {
        if (in_len < MSE_DH_LEN)
            return MSE_CONTINUE;
        uint8_t secret[MSE_DH_LEN];
        mse_dh_secret(c->priv, in, secret);
        *consumed += MSE_DH_LEN;

        mse_stream_key("keyA", secret, c->info_hash, &c->send_rc4);
        mse_stream_key("keyB", secret, c->info_hash, &c->recv_rc4);
        // The peer's encrypted VC == keyB keystream[0..7]; precompute to scan.
        rc4_t probe = c->recv_rc4;
        uint8_t zeros[MSE_VC_LEN] = {0};
        rc4_crypt(&probe, zeros, c->vc_expect, MSE_VC_LEN);

        mse_status_t r = emit_request(c, secret, out, out_cap, produced);
        if (r == MSE_FAIL) { c->state = ST_FAIL; return MSE_FAIL; }
        c->state = ST_SYNC_VC;
        c->skipped = 0;
    }

    if (c->state == ST_SYNC_VC) {
        const uint8_t *p = in + *consumed;
        size_t avail = in_len - *consumed;
        size_t k = 0;
        int found = 0;
        while (k + MSE_VC_LEN <= avail) {
            if (memcmp(p + k, c->vc_expect, MSE_VC_LEN) == 0) { found = 1; break; }
            ++k;
        }
        if (found) {
            *consumed += k + MSE_VC_LEN;
            rc4_discard(&c->recv_rc4, MSE_VC_LEN); // consume VC keystream
            c->skipped += (uint32_t)k;
            c->state = ST_READ_SELECT;
            c->select_have = 0;
        } else {
            // Everything except the trailing 7 bytes cannot start a VC match.
            size_t skip = avail > (MSE_VC_LEN - 1) ? avail - (MSE_VC_LEN - 1) : 0;
            *consumed += skip;
            c->skipped += (uint32_t)skip;
            if (c->skipped > MSE_MAX_PAD + MSE_DH_LEN)
                { c->state = ST_FAIL; return MSE_FAIL; }
            return MSE_CONTINUE;
        }
    }

    if (c->state == ST_READ_SELECT) {
        while (c->select_have < sizeof(c->select_buf) && *consumed < in_len) {
            rc4_crypt(&c->recv_rc4, in + *consumed,
                      &c->select_buf[c->select_have], 1);
            ++c->select_have;
            ++*consumed;
        }
        if (c->select_have < sizeof(c->select_buf))
            return MSE_CONTINUE;
        uint32_t select = ((uint32_t)c->select_buf[0] << 24) |
                          ((uint32_t)c->select_buf[1] << 16) |
                          ((uint32_t)c->select_buf[2] << 8) |
                          (uint32_t)c->select_buf[3];
        uint32_t padd = ((uint32_t)c->select_buf[4] << 8) | c->select_buf[5];
        if (!(select & MSE_CRYPTO_RC4) || padd > MSE_MAX_PAD)
            { c->state = ST_FAIL; return MSE_FAIL; }
        c->padd_remaining = padd;
        c->state = ST_READ_PADD;
    }

    if (c->state == ST_READ_PADD) {
        while (c->padd_remaining > 0 && *consumed < in_len) {
            rc4_discard(&c->recv_rc4, 1); // decrypt-and-drop padD
            ++*consumed;
            --c->padd_remaining;
        }
        if (c->padd_remaining > 0)
            return MSE_CONTINUE;
        c->state = ST_DONE;
        return MSE_DONE;
    }

    if (c->state == ST_DONE)
        return MSE_DONE;
    return c->state == ST_FAIL ? MSE_FAIL : MSE_CONTINUE;
}
