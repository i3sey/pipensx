# TweetNaCl (vendored)

Canonical TweetNaCl 20140427 from https://tweetnacl.cr.yp.to/ — public domain.
Files `tweetnacl.c` / `tweetnacl.h` are byte-for-byte the upstream release.

Used verify-only for ed25519 catalog signatures (RF_ACCESS_PLAN П3.2) via
`src/core/catalog_sig.c`. `randombytes()` is stubbed there because the
signing/keygen paths are never linked into the client.
