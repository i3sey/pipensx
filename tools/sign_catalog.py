#!/usr/bin/env python3
"""Sign the catalog with ed25519 for the client to verify (RF_ACCESS_PLAN П3.2).

The client bakes a 32-byte public key into the binary (kCatalogPublicKey in
src/app/catalog_service.cpp) and, once that key is non-zero, requires a valid
detached signature on every network refresh. This tool lives on the producer
side (the same CI that runs embed_catalog_infodicts.py): it generates the
keypair once, and signs each published catalog.json thereafter.

Signatures are plain ed25519 over the exact catalog bytes, emitted as a base64
".sig" sidecar uploaded next to catalog.json in the GitHub release. TweetNaCl
(client) and this tool are both RFC 8032 ed25519, so they interoperate.

Usage:
  # One-time: mint a keypair. Keep the private key secret (CI secret); paste
  # the printed C array into kCatalogPublicKey and rebuild the client.
  sign_catalog.py --gen-key

  # Per release: sign the catalog. Writes catalog.json.sig beside it.
  sign_catalog.py --key <private-hex> --sign catalog.json

  # Explicit output path:
  sign_catalog.py --key <private-hex> --sign catalog.json --out catalog.json.sig
"""

import argparse
import base64
import sys

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
        Ed25519PublicKey,
    )
    from cryptography.hazmat.primitives import serialization
except ImportError:
    sys.exit("error: the 'cryptography' package is required (pip install cryptography)")


def _raw_private(private_hex: str) -> Ed25519PrivateKey:
    seed = bytes.fromhex(private_hex.strip())
    if len(seed) != 32:
        sys.exit("error: private key must be 32 hex-encoded bytes (64 hex chars)")
    return Ed25519PrivateKey.from_private_bytes(seed)


def _public_raw(key) -> bytes:
    return key.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )


def _emit_c_array(pub: bytes) -> str:
    rows = []
    for i in range(0, 32, 8):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in pub[i : i + 8]) + ",")
    return "constexpr uint8_t kCatalogPublicKey[32] = {\n" + "\n".join(rows) + "\n};"


def gen_key() -> None:
    key = Ed25519PrivateKey.generate()
    seed = key.private_bytes(
        serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption()
    )
    pub = _public_raw(key.public_key())
    print("private key (hex, keep secret — store as a CI secret):")
    print(f"  {seed.hex()}")
    print("\npublic key (paste over kCatalogPublicKey in catalog_service.cpp):")
    print(_emit_c_array(pub))


def sign(private_hex: str, catalog_path: str, out_path: str) -> None:
    key = _raw_private(private_hex)
    with open(catalog_path, "rb") as handle:
        body = handle.read()
    signature = key.sign(body)
    # Sanity: verify locally before writing, so a broken sidecar never ships.
    _public_raw(key.public_key())
    key.public_key().verify(signature, body)
    encoded = base64.b64encode(signature).decode("ascii")
    with open(out_path, "w") as handle:
        handle.write(encoded + "\n")
    print(f"signed {catalog_path} ({len(body)} bytes) -> {out_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gen-key", action="store_true",
                        help="mint a new ed25519 keypair and exit")
    parser.add_argument("--key", help="private key as 64 hex chars")
    parser.add_argument("--sign", metavar="CATALOG",
                        help="path to catalog.json to sign")
    parser.add_argument("--out", metavar="SIG",
                        help="signature output path (default: <catalog>.sig)")
    args = parser.parse_args()

    if args.gen_key:
        gen_key()
        return
    if not args.sign or not args.key:
        parser.error("signing requires both --key and --sign (or use --gen-key)")
    out_path = args.out or (args.sign + ".sig")
    sign(args.key, args.sign, out_path)


if __name__ == "__main__":
    main()
