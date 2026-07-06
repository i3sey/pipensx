#include "catalog_sig.h"

#include <stdlib.h>
#include <string.h>

#include "../../vendor/tweetnacl/tweetnacl.h"

/* TweetNaCl declares randombytes() extern and references it from the keygen /
   signing entry points. The client only ever verifies, so those paths are dead
   code — but the symbol must resolve at link time. Abort if it is ever reached:
   a live call means someone wired signing into the client, which is a bug. */
void randombytes(unsigned char *buf, unsigned long long len) {
    (void)buf;
    (void)len;
    abort();
}

int catalog_sig_verify(const uint8_t pubkey[32], const uint8_t *msg,
                       size_t len, const uint8_t sig[64]) {
    if (!pubkey || !sig || (!msg && len))
        return 0;

    /* crypto_sign_open consumes the combined form sig(64) || message and
       writes the recovered message into a same-sized scratch buffer. */
    unsigned long long signedLen = (unsigned long long)len + 64u;
    unsigned char *signedMsg = (unsigned char *)malloc(signedLen);
    unsigned char *recovered = (unsigned char *)malloc(signedLen);
    int ok = 0;
    if (signedMsg && recovered) {
        memcpy(signedMsg, sig, 64);
        if (len)
            memcpy(signedMsg + 64, msg, len);
        unsigned long long recoveredLen = 0;
        ok = crypto_sign_ed25519_tweet_open(recovered, &recoveredLen, signedMsg,
                                            signedLen, pubkey) == 0;
    }
    free(signedMsg);
    free(recovered);
    return ok;
}
