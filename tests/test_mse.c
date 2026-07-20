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

int main(void) {
    test_rc4_known_answer();
    test_dh_shared_secret_matches();
    test_stream_key_roundtrip();
    puts("mse tests passed");
    return 0;
}
