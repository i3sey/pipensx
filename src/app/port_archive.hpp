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
    size_t layeredFiles = 0;
    // Source member and destination pairs, in archive order (read from
    // headers, not by decompressing). NRO members are /switch-relative;
    // LayeredFS members are SD-root relative (atmosphere/contents/…).
    std::vector<std::string> sourceFiles;
    std::vector<std::string> files;
    std::vector<uint8_t> destinationSdRoot;
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

// Extract mapped members. NRO / switch/ destinations land under targetRoot
// (/switch). LayeredFS destinations land under sdRoot (SD card root). When
// sdRoot is omitted it equals targetRoot, which is what the archive unit
// tests use with a single output directory.
bool extractPortArchive(const std::string& archivePath,
                        const std::string& targetRoot,
                        const std::atomic<bool>& cancelled,
                        const std::function<void(uint64_t)>& progress,
                        const std::function<void(const std::string&)>& currentFile,
                        std::string& error);
bool extractPortArchive(const std::string& archivePath,
                        const std::string& targetRoot,
                        const std::string& sdRoot,
                        const std::atomic<bool>& cancelled,
                        const std::function<void(uint64_t)>& progress,
                        const std::function<void(const std::string&)>& currentFile,
                        std::string& error);

} // namespace pipensx
