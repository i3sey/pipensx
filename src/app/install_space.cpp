#include "install_space.hpp"

#include "nx_file_types.hpp"
#include "port_selection.hpp"

#include <algorithm>
#include <cctype>
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

bool parseNszInstalledBytes(const std::string& path, uint64_t& bytes) {
    if (!isCompressedName(path) || path.size() < 6)
        return false;

    const size_t extension = path.size() - 4;
    if (extension == 0 || path[extension - 1] != ')')
        return false;
    const size_t open = path.rfind('(', extension - 1);
    if (open == std::string::npos)
        return false;

    size_t begin = open + 1;
    size_t end = extension - 1;
    while (begin < end && std::isspace(
                              static_cast<unsigned char>(path[begin])))
        ++begin;
    while (end > begin && std::isspace(
                            static_cast<unsigned char>(path[end - 1])))
        --end;

    size_t unitBegin = end;
    while (unitBegin > begin && !std::isspace(
                                    static_cast<unsigned char>(
                                        path[unitBegin - 1])))
        --unitBegin;
    if (unitBegin == begin)
        return false;
    size_t numberEnd = unitBegin;
    while (numberEnd > begin && std::isspace(
                                   static_cast<unsigned char>(
                                       path[numberEnd - 1])))
        --numberEnd;

    std::string unit = path.substr(unitBegin, end - unitBegin);
    for (char& ch : unit)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    uint64_t multiplier = 0;
    if (unit == "KB")
        multiplier = 1ULL << 10;
    else if (unit == "MB")
        multiplier = 1ULL << 20;
    else if (unit == "GB")
        multiplier = 1ULL << 30;
    else if (unit == "TB")
        multiplier = 1ULL << 40;
    else
        return false;

    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint64_t fractionScale = 1;
    bool separatorSeen = false;
    bool digitSeen = false;
    for (size_t i = begin; i < numberEnd; ++i) {
        const unsigned char ch = static_cast<unsigned char>(path[i]);
        if (ch == '.' || ch == ',') {
            if (separatorSeen)
                return false;
            separatorSeen = true;
            continue;
        }
        if (!std::isdigit(ch))
            return false;
        digitSeen = true;
        const uint64_t digit = ch - '0';
        if (!separatorSeen) {
            if (whole > (std::numeric_limits<uint64_t>::max() - digit) / 10)
                return false;
            whole = whole * 10 + digit;
        } else {
            // More precision is not useful for a byte count and makes the
            // fixed-point multiplication needlessly prone to overflow.
            if (fractionScale >= 1000000)
                return false;
            fraction = fraction * 10 + digit;
            fractionScale *= 10;
        }
    }
    if (!digitSeen || (separatorSeen && fractionScale == 1) ||
        (whole == 0 && fraction == 0) ||
        whole > std::numeric_limits<uint64_t>::max() / multiplier)
        return false;

    const uint64_t wholeBytes = whole * multiplier;
    const uint64_t fractionBytes =
        (fraction * multiplier + fractionScale / 2) / fractionScale;
    if (fractionBytes > std::numeric_limits<uint64_t>::max() - wholeBytes)
        return false;
    bytes = wholeBytes + fractionBytes;
    return true;
}

uint64_t compressedInstallBytes(const TorrentPreview::File& file,
                                bool& sizeKnown, bool& overflow) {
    uint64_t namedBytes = 0;
    if (parseNszInstalledBytes(file.path, namedBytes)) {
        sizeKnown = true;
        return std::max(file.length, namedBytes);
    }

    sizeKnown = false;
    constexpr uint64_t kCompressedExpansionFallback = 3;
    if (file.length > std::numeric_limits<uint64_t>::max() /
                          kCompressedExpansionFallback) {
        overflow = true;
        return std::numeric_limits<uint64_t>::max();
    }
    return file.length * kCompressedExpansionFallback;
}

} // namespace

TransferMode defaultTransferMode(const TorrentPreview& preview,
                                 TransferMode requestedMode) {
    if (torrentPortLayoutDetected(preview))
        return TransferMode::PortInstall;
    return requestedMode;
}

std::vector<uint8_t> defaultInstallSelection(
    const TorrentPreview& preview,
    TransferMode mode,
    StreamSelection selection) {
    if (selection == StreamSelection::PackagesOnly &&
        torrentPortLayoutDetected(preview))
        return selectPortInstallActions(preview);
    if (mode != TransferMode::StreamInstall ||
        selection == StreamSelection::AllFiles) {
        return {};
    }

    std::vector<uint8_t> mask;
    mask.reserve(preview.files.size());
    bool allSelected = true;
    for (const TorrentPreview::File& file : preview.files) {
        uint8_t action = static_cast<uint8_t>(FileAction::Skip);
        if (file.package)
            action = static_cast<uint8_t>(FileAction::Install);
        else if (!file.cartridge && isPortPayloadName(file.path))
            action = static_cast<uint8_t>(FileAction::Download);
        mask.push_back(action);
        allSelected = allSelected &&
                      action != static_cast<uint8_t>(FileAction::Skip);
    }
    return allSelected ? std::vector<uint8_t>() : mask;
}

InstallSpaceEstimate estimateInstallSpace(
    const TorrentPreview& preview,
    const std::vector<uint8_t>& fileActions,
    TransferMode mode) {
    InstallSpaceEstimate result;
    const bool useSelection = !fileActions.empty();
    const size_t count = preview.files.size();

    if (useSelection && fileActions.size() != count) {
        result.overflow = true;
        return result;
    }

    bool streamedPackage = false;
    bool unknownCompressedPackage = false;
    for (size_t i = 0; i < count; ++i) {
        const TorrentPreview::File& file = preview.files[i];
        uint8_t action = useSelection
            ? fileActions[i]
            : (mode == TransferMode::StreamInstall && file.package
                   ? static_cast<uint8_t>(FileAction::Install)
                   : static_cast<uint8_t>(FileAction::Download));
        if (action == static_cast<uint8_t>(FileAction::Skip))
            continue;
        if (action != static_cast<uint8_t>(FileAction::Download) &&
            action != static_cast<uint8_t>(FileAction::Install)) {
            result.overflow = true;
            return result;
        }
        ++result.selectedFiles;
        addBytes(result.selectedBytes, file.length, result.overflow);
        const bool packageInstall = file.package &&
            ((mode == TransferMode::StreamInstall &&
              action == static_cast<uint8_t>(FileAction::Install)) ||
             mode == TransferMode::PortInstall);
        if (packageInstall) {
            ++result.packageFiles;
            streamedPackage = true;
            uint64_t installBytes = file.length;
            if (file.compressed) {
                bool sizeKnown = false;
                installBytes = compressedInstallBytes(file, sizeKnown,
                                                       result.overflow);
                unknownCompressedPackage = unknownCompressedPackage ||
                                           !sizeKnown;
            }
            addBytes(result.packageBytes, installBytes, result.overflow);
            // Deferred port packages must also remain as local files until
            // payload deployment succeeds; stream installs do not.
            if (mode == TransferMode::PortInstall)
                addBytes(result.downloadBytes, file.length, result.overflow);
        } else {
            addBytes(result.downloadBytes, file.length, result.overflow);
        }
    }

    result.requiredBytes = result.downloadBytes;
    addBytes(result.requiredBytes, result.packageBytes, result.overflow);
    if (unknownCompressedPackage)
        result.certainty = SpaceEstimateCertainty::CompressedUnknown;
    else if (streamedPackage)
        result.certainty = SpaceEstimateCertainty::Conservative;
    return result;
}

InstallSpaceCheck assessInstallSpace(
    const InstallSpaceEstimate& estimate,
    const StorageSpaceSnapshot& storage) {
    return assessTransferSpace(estimate, storage, storage);
}

InstallSpaceCheck assessTransferSpace(
    const InstallSpaceEstimate& estimate,
    const StorageSpaceSnapshot& downloadStorage,
    const StorageSpaceSnapshot& packageStorage) {
    InstallSpaceCheck result;
    if (estimate.overflow) {
        result.status = InstallSpaceCheckStatus::Insufficient;
        result.shortfallBytes = std::numeric_limits<uint64_t>::max();
        return result;
    }
    uint64_t shortfall = 0;
    bool checked = false;
    const bool sharedPool = downloadStorage.available &&
        packageStorage.available &&
        downloadStorage.totalBytes == packageStorage.totalBytes &&
        downloadStorage.freeBytes == packageStorage.freeBytes;
    if (sharedPool) {
        uint64_t combined = estimate.downloadBytes;
        if (combined > std::numeric_limits<uint64_t>::max() -
                           estimate.packageBytes)
            combined = std::numeric_limits<uint64_t>::max();
        else
            combined += estimate.packageBytes;
        if (combined > downloadStorage.freeBytes) {
            result.status = InstallSpaceCheckStatus::Insufficient;
            result.shortfallBytes = combined - downloadStorage.freeBytes;
            return result;
        }
        result.status = InstallSpaceCheckStatus::Enough;
        return result;
    }
    auto checkPool = [&](uint64_t need, const StorageSpaceSnapshot& storage) {
        if (need == 0)
            return;
        if (!storage.available)
            return;
        checked = true;
        if (need > storage.freeBytes)
            shortfall = std::max(shortfall, need - storage.freeBytes);
    };
    checkPool(estimate.downloadBytes, downloadStorage);
    checkPool(estimate.packageBytes, packageStorage);
    if (!checked &&
        ((estimate.downloadBytes > 0 && !downloadStorage.available) ||
         (estimate.packageBytes > 0 && !packageStorage.available)))
        return result;
    if (shortfall > 0) {
        result.status = InstallSpaceCheckStatus::Insufficient;
        result.shortfallBytes = shortfall;
        return result;
    }
    if (!checked && estimate.requiredBytes == 0) {
        result.status = InstallSpaceCheckStatus::Enough;
        return result;
    }
    if (!checked)
        return result;
    result.status = InstallSpaceCheckStatus::Enough;
    return result;
}

bool catalogEntryFitsFreeSpace(uint64_t entrySizeBytes,
                               const StorageSpaceSnapshot& storage) {
    // Never hide what we cannot measure, and never hide an entry whose size the
    // catalog does not carry.
    if (!storage.available || entrySizeBytes == 0)
        return true;
    return entrySizeBytes <= storage.freeBytes;
}

namespace {
bool gStorageOverrideSet = false;
StorageSpaceSnapshot gStorageOverride;
} // namespace

void setStorageSpaceOverride(const StorageSpaceSnapshot* snapshot) {
    gStorageOverrideSet = snapshot != nullptr;
    gStorageOverride = snapshot ? *snapshot : StorageSpaceSnapshot{};
}

StorageSpaceSnapshot queryStorageSpace(const std::string& path) {
    return queryInstallStorageSpace(install::InstallStorageTarget::SdCard,
                                    path);
}

StorageSpaceSnapshot queryInstallStorageSpace(
    install::InstallStorageTarget target, const std::string& fallbackPath) {
    if (gStorageOverrideSet)
        return gStorageOverride;
    StorageSpaceSnapshot result;
#ifdef __SWITCH__
    (void)fallbackPath;
    s64 total = 0;
    s64 free = 0;
    const NcmStorageId storageId = target == install::InstallStorageTarget::Nand
        ? NcmStorageId_BuiltInUser
        : NcmStorageId_SdCard;
    Result rc = nsGetStorageSize(storageId, &total, &free);
    if (R_FAILED(rc) || total < 0 || free < 0) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer),
                      target == install::InstallStorageTarget::Nand
                          ? "Unable to query system memory (0x%08x)."
                          : "Unable to query SD storage (0x%08x).",
                      rc);
        result.error = buffer;
        return result;
    }
    result.totalBytes = static_cast<uint64_t>(total);
    result.freeBytes = static_cast<uint64_t>(free);
#else
    (void)target;
    struct statvfs info {};
    if (statvfs(fallbackPath.c_str(), &info) != 0) {
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
