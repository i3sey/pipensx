#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx {

struct PortArchiveProbe {
    bool ok = false;
    uint64_t packedBytes = 0;
    uint64_t unpackBytes = 0;
    uint64_t maxSolidBlockBytes = 0;
    size_t switchFiles = 0;
    // Switch-relative destination of every extractable member, in archive
    // order (read from the archive headers, not by decompressing). Capped;
    // the extraction callback stays the authoritative source of written
    // files. Empty when the headers could not be walked.
    std::vector<std::string> files;
    std::string error;
};

// Solid 7z folders larger than ~32 MiB stream to disk (LZMA2 dictionary
// only). Small solids still decode in RAM via SzArEx_Extract.
inline constexpr uint64_t kPortArchiveSolidRamReserveBytes =
    512ull * 1024 * 1024;

inline bool portArchiveSolidFitsRam(uint64_t maxSolidBlockBytes,
                                    uint64_t availableHeapBytes) {
    if (maxSolidBlockBytes == 0)
        return true;
    if (availableHeapBytes <= kPortArchiveSolidRamReserveBytes)
        return false;
    return maxSolidBlockBytes <=
           availableHeapBytes - kPortArchiveSolidRamReserveBytes;
}

// Cheap header-only probe (no full decompress). ok=false fills error.
bool probePortArchive(const std::string& archivePath, PortArchiveProbe& out);

// Extract members whose path contains a /switch/ segment into targetRoot
// (everything after that segment). progress(bytes) is called as file data is
// written. Returns false and fills error on failure or cancel.
bool extractPortArchive(const std::string& archivePath,
                        const std::string& targetRoot,
                        const std::atomic<bool>& cancelled,
                        const std::function<void(uint64_t)>& progress,
                        const std::function<void(const std::string&)>& currentFile,
                        std::string& error);

} // namespace pipensx
