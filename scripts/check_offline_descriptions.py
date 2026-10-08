#!/usr/bin/env python3
"""Validate shipped offline description packs without loading a translation model."""
import argparse
import json
import struct
from pathlib import Path

import zstandard


def check_pack(path, expected):
    data = path.read_bytes()
    assert data[:8] == b"PNXDESC1", f"{path}: magic"
    version, count = struct.unpack_from("<II", data, 8)
    assert version == 1 and count == expected, f"{path}: version/count"
    index_end = 16 + count * 40
    previous = b""
    blocks = {}
    hashes = set()
    for i in range(count):
        digest, offset, compressed, decoded, begin, size = struct.unpack_from("<20sIIIII", data, 16 + i * 40)
        assert digest > previous, f"{path}: unordered or duplicate index"
        assert offset >= index_end and 0 < compressed <= 1048576
        assert offset + compressed <= len(data) and 0 < decoded <= 1048576
        assert 0 < size <= 65536 and begin + size <= decoded
        if offset not in blocks:
            blocks[offset] = zstandard.ZstdDecompressor().decompress(
                data[offset:offset + compressed], max_output_size=1048576)
        block = blocks[offset]
        assert len(block) == decoded, f"{path}: decoded block length"
        text = block[begin:begin + size].decode("utf-8", errors="strict")
        assert text.strip() and "▁" not in text and "\x00" not in text, f"{path}: invalid text"
        previous = digest
        hashes.add(digest)
    print(f"{path.name}: {count} valid descriptions, {len(data)} bytes")
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", default="resources/catalog/descriptions")
    args = parser.parse_args()
    root = Path(args.directory)
    manifest = json.loads((root / "coverage.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == 1 and not manifest["fixture"], "Incomplete/sample manifest"
    assert set(manifest["languages"]) == {"en-US", "pt-BR", "ru", "fr", "es", "it", "zh-CN", "ko"}
    reference = None
    for locale in manifest["languages"]:
        keys = check_pack(root / f"{locale}.pdx", manifest["source_count"])
        if reference is not None:
            assert keys == reference, "Languages have different source coverage"
        reference = keys
    print("Offline packs: all eight languages have identical complete coverage.")


if __name__ == "__main__":
    main()
