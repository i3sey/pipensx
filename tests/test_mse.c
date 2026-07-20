// Validates the MSE crypto primitives without any network: RC4 against a known
// answer, Diffie-Hellman for a matching shared secret both ways, and the stream
// key derivation for an encrypt/decrypt round-trip.
#include "../src/core/mse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_rc4_known_answer(void) {
    // Classic RC4 vector: key "Key", plaintext "Plaintext".
    const uint8_t expected[] = {0xBB, 0xF3, 0x16, 0xE8,
                                0xD9, 0x40, 0xAF, 0x0A, 0xD3};
    rc4_t rc4;
    rc4_init(&rc4, (const uint8_t *)"Key", 3);
    uint8_t out[9];
    rc4_crypt(&rc4, (const uint8_t *)"Plaintext", out, 9);
    assert(memcmp(out, expected, sizeof(expected)) == 0);

    // Decrypt path: fresh keystream over the ciphertext restores plaintext.
    rc4_t dec;
    rc4_init(&dec, (const uint8_t *)"Key", 3);
    uint8_t back[9];
    rc4_crypt(&dec, out, back, 9);
    assert(memcmp(back, "Plaintext", 9) == 0);
}

static void test_dh_shared_secret_matches(void) {
    uint8_t privA[MSE_DH_LEN], privB[MSE_DH_LEN];
    for (int i = 0; i < MSE_DH_LEN; ++i) {
        privA[i] = (uint8_t)(i * 7 + 3);
        privB[i] = (uint8_t)(i * 13 + 101);
    }
    // Keep the top bytes nonzero so the exponents are large.
    privA[0] |= 0x80;
    privB[0] |= 0x80;

    uint8_t pubA[MSE_DH_LEN], pubB[MSE_DH_LEN];
    mse_dh_public(privA, pubA);
    mse_dh_public(privB, pubB);
    assert(memcmp(pubA, pubB, MSE_DH_LEN) != 0);

    uint8_t secretA[MSE_DH_LEN], secretB[MSE_DH_LEN];
    mse_dh_secret(privA, pubB, secretA); // A computes pubB^a
    mse_dh_secret(privB, pubA, secretB); // B computes pubA^b
    assert(memcmp(secretA, secretB, MSE_DH_LEN) == 0);

    // Sanity: a shared secret is not trivially zero/one.
    int nonzero = 0;
    for (int i = 0; i < MSE_DH_LEN; ++i)
        nonzero |= secretA[i];
    assert(nonzero);
}

static void test_stream_key_roundtrip(void) {
    uint8_t secret[MSE_DH_LEN];
    uint8_t info_hash[20];
    for (int i = 0; i < MSE_DH_LEN; ++i)
        secret[i] = (uint8_t)(i * 3 + 5);
    for (int i = 0; i < 20; ++i)
        info_hash[i] = (uint8_t)(0xA0 + i);

    // Initiator send stream ("keyA"), receiver decrypts with the same "keyA".
    rc4_t send, recv;
    mse_stream_key("keyA", secret, info_hash, &send);
    mse_stream_key("keyA", secret, info_hash, &recv);

    const char *msg = "BitTorrent protocol handshake payload";
    size_t n = strlen(msg);
    uint8_t cipher[64], plain[64];
    rc4_crypt(&send, (const uint8_t *)msg, cipher, n);
    assert(memcmp(cipher, msg, n) != 0); // actually encrypted
    rc4_crypt(&recv, cipher, plain, n);
    assert(memcmp(plain, msg, n) == 0); // round-trips

    // "keyA" and "keyB" must be independent streams.
    rc4_t a, b;
    mse_stream_key("keyA", secret, info_hash, &a);
    mse_stream_key("keyB", secret, info_hash, &b);
    uint8_t ka[16], kb[16], zero[16] = {0};
    rc4_crypt(&a, zero, ka, 16);
    rc4_crypt(&b, zero, kb, 16);
    assert(memcmp(ka, kb, 16) != 0);
}

// A minimal MSE responder built from the same primitives, used only to drive
// the initiator state machine end to end. It assumes the initiator's fixed
// framing (padA = 0, padC = 0, IA = 68 bytes) — enough to prove interop,
// including the VC resync across a non-empty padB.
typedef struct {
    uint8_t priv[MSE_DH_LEN];
    uint8_t info_hash[20];
    rc4_t recv; // keyA: initiator's send stream
    rc4_t send; // keyB: initiator's recv stream
    uint8_t ia[MSE_MAX_IA];
    int have_ia;
} responder_t;

// Step 2: receive pubA, derive keys, emit pubB + padB.
static size_t resp_step2(responder_t *r, const uint8_t *pubA, uint8_t *out) {
    uint8_t secret[MSE_DH_LEN];
    mse_dh_secret(r->priv, pubA, secret);
    mse_stream_key("keyA", secret, r->info_hash, &r->recv);
    mse_stream_key("keyB", secret, r->info_hash, &r->send);
    uint8_t pubB[MSE_DH_LEN];
    mse_dh_public(r->priv, pubB);
    memcpy(out, pubB, MSE_DH_LEN);
    // padB — deliberately non-empty so the initiator must resync on the VC.
    const uint32_t padB = 30;
    for (uint32_t i = 0; i < padB; ++i)
        out[MSE_DH_LEN + i] = (uint8_t)(0xAA ^ i);
    return MSE_DH_LEN + padB;
}

// Step 4: consume the initiator's request (req1|req2^req3|enc), recover IA,
// then emit ENCRYPT(VC | crypto_select=RC4 | len(padD) | padD).
static size_t resp_step4(responder_t *r, const uint8_t *req, size_t req_len,
                         uint8_t *out) {
    assert(req_len == 124); // 20 + 20 + (8+4+2+0+2+68)
    const uint8_t *enc = req + 40;
    uint8_t block[84];
    rc4_crypt(&r->recv, enc, block, sizeof(block));
    // VC must decrypt to 8 zeros.
    for (int i = 0; i < 8; ++i)
        assert(block[i] == 0);
    // crypto_provide advertises RC4.
    assert(block[11] & MSE_CRYPTO_RC4);
    uint32_t pad_c = ((uint32_t)block[12] << 8) | block[13];
    assert(pad_c == 0);
    uint32_t ia_len = ((uint32_t)block[14] << 8) | block[15];
    assert(ia_len == MSE_MAX_IA);
    memcpy(r->ia, block + 16, ia_len);
    r->have_ia = 1;

    uint8_t plain[8 + 4 + 2];
    memset(plain, 0, 8);           // VC
    plain[8] = 0; plain[9] = 0; plain[10] = 0; plain[11] = MSE_CRYPTO_RC4;
    plain[12] = 0; plain[13] = 0;  // len(padD) = 0
    rc4_crypt(&r->send, plain, out, sizeof(plain));
    return sizeof(plain);
}

static void test_handshake_loopback(void) {
    uint8_t info_hash[20], privA[MSE_DH_LEN], privB[MSE_DH_LEN], ia[MSE_MAX_IA];
    for (int i = 0; i < 20; ++i)
        info_hash[i] = (uint8_t)(0x11 * i + 3);
    for (int i = 0; i < MSE_DH_LEN; ++i) {
        privA[i] = (uint8_t)(i * 5 + 1);
        privB[i] = (uint8_t)(i * 9 + 40);
    }
    privA[0] |= 0x80;
    privB[0] |= 0x80;
    for (int i = 0; i < MSE_MAX_IA; ++i)
        ia[i] = (uint8_t)(0x30 + i); // stand-in BT handshake

    responder_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.priv, privB, MSE_DH_LEN);
    memcpy(r.info_hash, info_hash, 20);

    mse_client_t cli;
    uint8_t out[1024];
    size_t produced = 0;

    // Step 1: initiator -> pubA (padA = 0).
    assert(mse_client_start(&cli, info_hash, privA, NULL, 0, ia, MSE_MAX_IA,
                            out, sizeof(out), &produced) == MSE_CONTINUE);
    assert(produced == MSE_DH_LEN);
    uint8_t pubA[MSE_DH_LEN];
    memcpy(pubA, out, MSE_DH_LEN);

    // Step 2: responder -> pubB + padB.
    uint8_t s2[MSE_DH_LEN + 64];
    size_t s2n = resp_step2(&r, pubA, s2);

    // Persistent input buffer for the initiator (unconsumed bytes carry over).
    uint8_t inbuf[2048];
    size_t in_len = 0;
    memcpy(inbuf, s2, s2n);
    in_len = s2n;

    uint8_t step3[512];
    size_t step3_len = 0;
    mse_status_t st = MSE_CONTINUE;

    // Feed step2; the initiator consumes pubB, emits step3, then stalls in the
    // VC resync (step4 not sent yet).
    size_t consumed = 0;
    st = mse_client_feed(&cli, inbuf, in_len, &consumed, out, sizeof(out),
                         &produced);
    assert(st == MSE_CONTINUE);
    assert(produced > 0);
    memcpy(step3, out, produced);
    step3_len = produced;
    memmove(inbuf, inbuf + consumed, in_len - consumed);
    in_len -= consumed;

    // Responder consumes step3, recovers IA, emits step4.
    uint8_t s4[64];
    size_t s4n = resp_step4(&r, step3, step3_len, s4);
    assert(r.have_ia && memcmp(r.ia, ia, MSE_MAX_IA) == 0);

    // Feed step4 (after the leftover padB tail); initiator resyncs -> DONE.
    memcpy(inbuf + in_len, s4, s4n);
    in_len += s4n;
    st = mse_client_feed(&cli, inbuf, in_len, &consumed, out, sizeof(out),
                         &produced);
    assert(st == MSE_DONE);

    // Post-handshake streams: initiator send -> responder recv (keyA), and
    // responder send -> initiator recv (keyB).
    const char *msg = "post-handshake bytes";
    size_t n = strlen(msg);
    uint8_t enc[64], dec[64];
    rc4_crypt(&cli.send_rc4, (const uint8_t *)msg, enc, n);
    rc4_crypt(&r.recv, enc, dec, n);
    assert(memcmp(dec, msg, n) == 0);

    const char *reply = "server reply bytes";
    size_t m = strlen(reply);
    rc4_crypt(&r.send, (const uint8_t *)reply, enc, m);
    rc4_crypt(&cli.recv_rc4, enc, dec, m);
    assert(memcmp(dec, reply, m) == 0);
}

int main(void) {
    test_rc4_known_answer();
    test_dh_shared_secret_matches();
    test_stream_key_roundtrip();
    test_handshake_loopback();
    puts("mse tests passed");
    return 0;
}
