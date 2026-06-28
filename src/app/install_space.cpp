#include "install_space.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/statvfs.h>
#endif

namespace pipensx {
namespace {

void addBytes(uint64_t& target, uint64_t value, bool& overflow) {
    if (value > std::numeric_limits<uint64_t>::max() - target) {
        target = std::numeric_limits<uint64_t>::max();
        overflow = true;
        return;
    }
    target += value;
}

} // namespace

std::vector<uint8_t> defaultInstallSelection(
    const TorrentPreview& preview,
    TransferMode mode,
    StreamSelection selection) {
    if (mode != TransferMode::StreamInstall ||
        selection == StreamSelection::AllFiles) {
        return {};
    }

    std::vector<uint8_t> mask;
    mask.reserve(preview.files.size());
    bool allSelected = true;
    for (const TorrentPreview::File& file : preview.files) {
        const uint8_t selected = file.package ? 1 : 0;
        mask.push_back(selected);
        allSelected = allSelected && selected != 0;
    }
    return allSelected ? std::vector<uint8_t>() : mask;
}

InstallSpaceEstimate estimateInstallSpace(
    const TorrentPreview& preview,
    const std::vector<uint8_t>& selectedFiles,
    TransferMode mode) {
    InstallSpaceEstimate result;
    const bool useSelection = !selectedFiles.empty();
    const size_t count = preview.files.size();

    if (useSelection && selectedFiles.size() != count) {
        result.overflow = true;
        return result;
    }

    bool streamedPackage = false;
    bool compressedPackage = false;
    for (size_t i = 0; i < count; ++i) {
        if (useSelection && selectedFiles[i] == 0)
            continue;
        const TorrentPreview::File& file = preview.files[i];
        ++result.selectedFiles;
        addBytes(result.selectedBytes, file.length, result.overflow);
        if (mode == TransferMode::StreamInstall && file.package) {
            ++result.packageFiles;
            streamedPackage = true;
            compressedPackage = compressedPackage || file.compressed;
            addBytes(result.packageBytes, file.length, result.overflow);
        } else {
            addBytes(result.downloadBytes, file.length, result.overflow);
        }
    }

    result.requiredBytes = result.downloadBytes;
    addBytes(result.requiredBytes, result.packageBytes, result.overflow);
    if (compressedPackage)
        result.certainty = SpaceEstimateCertainty::CompressedUnknown;
    else if (streamedPackage)
        result.certainty = SpaceEstimateCertainty::Conservative;
    return result;
}

InstallSpaceCheck assessInstallSpace(
    const InstallSpaceEstimate& estimate,
    const StorageSpaceSnapshot& storage) {
    InstallSpaceCheck result;
    if (!storage.available)
        return result;
    if (estimate.overflow || estimate.requiredBytes > storage.freeBytes) {
        result.status = InstallSpaceCheckStatus::Insufficient;
        result.shortfallBytes = estimate.overflow
            ? std::numeric_limits<uint64_t>::max()
            : estimate.requiredBytes - storage.freeBytes;
        return result;
    }
    result.status = InstallSpaceCheckStatus::Enough;
    return result;
}

StorageSpaceSnapshot queryStorageSpace(const std::string& path) {
    StorageSpaceSnapshot result;
#ifdef __SWITCH__
    (void)path;
    s64 total = 0;
    s64 free = 0;
    Result rc = nsGetStorageSize(NcmStorageId_SdCard, &total, &free);
    if (R_FAILED(rc) || total < 0 || free < 0) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer),
                      "Unable to query SD storage (0x%08x).", rc);
        result.error = buffer;
        return result;
    }
    result.totalBytes = static_cast<uint64_t>(total);
    result.freeBytes = static_cast<uint64_t>(free);
#else
    struct statvfs info {};
    if (statvfs(path.c_str(), &info) != 0) {
        result.error = std::string("Unable to query storage: ") +
                       std::strerror(errno);
        return result;
    }
    const uint64_t blockSize = static_cast<uint64_t>(info.f_frsize);
    result.totalBytes = blockSize * static_cast<uint64_t>(info.f_blocks);
    result.freeBytes = blockSize * static_cast<uint64_t>(info.f_bavail);
#endif
    result.available = true;
    return result;
}

} // namespace pipensx
