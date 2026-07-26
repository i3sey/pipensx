#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

// The bundled romfs catalog snapshots ship zstd-compressed (they are ~80% of
// the NRO uncompressed); these helpers let the load paths accept either form.

// True when `path` names a zstd-compressed snapshot (".zst" suffix).
bool isZstdPath(const std::string& path);

// Decompress a single zstd frame in place, rejecting anything whose declared
// content size is missing or larger than maxBytes.
bool decompressZstd(std::string& data, size_t maxBytes, std::string& error);
bool decompressZstd(std::vector<uint8_t>& data, size_t maxBytes,
                    std::string& error);

}  // namespace pipensx
