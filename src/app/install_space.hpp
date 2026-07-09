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

StorageSpaceSnapshot queryStorageSpace(const std::string& path);

} // namespace pipensx
