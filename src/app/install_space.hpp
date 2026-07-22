#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app_settings.hpp"
#include "download_manager.hpp"

namespace pipensx {

enum class SpaceEstimateCertainty {
    Exact,
    Conservative,
    CompressedUnknown,
};

struct InstallSpaceEstimate {
    uint64_t selectedBytes = 0;
    uint64_t downloadBytes = 0;
    uint64_t packageBytes = 0;
    uint64_t requiredBytes = 0;
    uint32_t selectedFiles = 0;
    uint32_t packageFiles = 0;
    SpaceEstimateCertainty certainty = SpaceEstimateCertainty::Exact;
    bool overflow = false;
};

struct StorageSpaceSnapshot {
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    bool available = false;
    std::string error;
};

enum class InstallSpaceCheckStatus {
    Unknown,
    Enough,
    Insufficient,
};

struct InstallSpaceCheck {
    InstallSpaceCheckStatus status = InstallSpaceCheckStatus::Unknown;
    uint64_t shortfallBytes = 0;
};

InstallSpaceEstimate estimateInstallSpace(
    const TorrentPreview& preview,
    const std::vector<uint8_t>& fileActions,
    TransferMode mode);

std::vector<uint8_t> defaultInstallSelection(
    const TorrentPreview& preview,
    TransferMode mode,
    StreamSelection selection);

InstallSpaceCheck assessInstallSpace(
    const InstallSpaceEstimate& estimate,
    const StorageSpaceSnapshot& storage);

// Coarse guard for the catalog's "fits on SD" filter. The catalog size is the
// download size, not the install size (NSZ packages expand on commit), hence
// "fits" rather than a promise. True when the storage figure is unavailable or
// the entry size is unknown (0) — the filter never hides what it cannot judge.
bool catalogEntryFitsFreeSpace(uint64_t entrySizeBytes,
                               const StorageSpaceSnapshot& storage);

StorageSpaceSnapshot queryStorageSpace(const std::string& path);

// Test seam: makes queryStorageSpace return a fixed snapshot instead of hitting
// nsGetStorageSize/statvfs, so the golden screenshot runner renders the storage
// meters deterministically. Pass nullptr to restore the real query.
void setStorageSpaceOverride(const StorageSpaceSnapshot* snapshot);

} // namespace pipensx
