#include "catalog_batch_installer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <utility>
#include <unistd.h>

namespace pipensx {
namespace {

std::atomic<uint64_t> gBatchTempSerial{1};

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

void addEstimate(uint64_t& target, uint64_t value, bool& overflow) {
    if (value > std::numeric_limits<uint64_t>::max() - target) {
        target = std::numeric_limits<uint64_t>::max();
        overflow = true;
    } else {
        target += value;
    }
}

} // namespace

BatchPreparation::~BatchPreparation() {
    for (const PreparedCatalogInstall& item : items_)
        if (!item.torrentPath.empty())
            ::unlink(item.torrentPath.c_str());
}

InstallSpaceEstimate BatchPreparation::selectedSpace() const {
    InstallSpaceEstimate total;
    bool streamed = false;
    bool compressed = false;
    for (const PreparedCatalogInstall& item : items_) {
        if (!item.selected)
            continue;
        addEstimate(total.selectedBytes, item.space.selectedBytes,
                    total.overflow);
        addEstimate(total.downloadBytes, item.space.downloadBytes,
                    total.overflow);
        addEstimate(total.packageBytes, item.space.packageBytes,
                    total.overflow);
        addEstimate(total.requiredBytes, item.space.requiredBytes,
                    total.overflow);
        total.selectedFiles += item.space.selectedFiles;
        total.packageFiles += item.space.packageFiles;
        streamed = streamed ||
                   item.space.certainty == SpaceEstimateCertainty::Conservative;
        compressed = compressed || item.space.certainty ==
                                      SpaceEstimateCertainty::CompressedUnknown;
    }
    if (compressed)
        total.certainty = SpaceEstimateCertainty::CompressedUnknown;
    else if (streamed)
        total.certainty = SpaceEstimateCertainty::Conservative;
    return total;
}

CatalogBatchInstaller::CatalogBatchInstaller(std::string rootPath,
                                             ResolveTorrent resolver)
    : rootPath_(std::move(rootPath)), resolver_(std::move(resolver)) {}

BatchPreparation CatalogBatchInstaller::prepare(
    const std::vector<CatalogEntry>& entries,
    StreamSelection selection,
    std::atomic<bool>& cancelled,
    const ProgressCallback& progress) const {
    BatchPreparation result;
    if (!resolver_) {
        for (const CatalogEntry& entry : entries)
            result.failures_.push_back({entry, "Torrent resolver is unavailable."});
        return result;
    }

    for (size_t index = 0; index < entries.size(); ++index) {
        const CatalogEntry& entry = entries[index];
        if (cancelled.load()) {
            result.cancelled_ = true;
            break;
        }

        const uint64_t serial = gBatchTempSerial.fetch_add(1);
        const std::string hash = lowerAscii(entry.infoHash);
        const std::string path = rootPath_ + "/_catalog_batch_" +
                                 (hash.empty() ? "unknown" : hash) + "_" +
                                 std::to_string(serial) + ".torrent";
        auto forwardProgress = [&, index](const MagnetProgress& magnet) {
            if (progress)
                progress({index + 1, entries.size(), entry.title, magnet});
        };
        if (progress)
            progress({index + 1, entries.size(), entry.title, {}});

        std::string error;
        std::vector<uint8_t> initialPeers;
        if (!resolver_(entry, path, cancelled, forwardProgress,
                       initialPeers, error)) {
            ::unlink(path.c_str());
            if (cancelled.load()) {
                result.cancelled_ = true;
                break;
            }
            result.failures_.push_back(
                {entry, error.empty() ? "Unable to resolve torrent metadata."
                                      : error});
            continue;
        }
        TorrentPreview preview;
        if (!DownloadManager::previewTorrent(path, preview, error)) {
            ::unlink(path.c_str());
            result.failures_.push_back({entry, error});
            continue;
        }
        if (!entry.infoHash.empty() &&
            lowerAscii(entry.infoHash) != lowerAscii(preview.infoHash)) {
            ::unlink(path.c_str());
            result.failures_.push_back(
                {entry, "Resolved torrent does not match the catalog entry."});
            continue;
        }

        TransferMode mode = TransferMode::StreamInstall;
        std::vector<uint8_t> mask = defaultInstallSelection(
            preview, mode, selection);
        InstallSpaceEstimate space = estimateInstallSpace(preview, mask, mode);
        if (space.packageFiles == 0) {
            if (selection == StreamSelection::PackagesOnly) {
                ::unlink(path.c_str());
                result.failures_.push_back(
                    {entry, "No package files match the current Settings selection."});
                continue;
            }
            mode = TransferMode::DownloadOnly;
            space = estimateInstallSpace(preview, mask, mode);
        }
        if (space.selectedFiles == 0 || space.overflow) {
            ::unlink(path.c_str());
            result.failures_.push_back(
                {entry, space.overflow ? "Selected size is too large."
                                       : "No files were selected."});
            continue;
        }

        PreparedCatalogInstall item;
        item.entry = entry;
        item.torrentPath = path;
        item.preview = std::move(preview);
        item.selection = std::move(mask);
        item.initialPeers = std::move(initialPeers);
        item.mode = mode;
        item.space = space;
        result.items_.push_back(std::move(item));
    }
    return result;
}

BatchEnqueueResult CatalogBatchInstaller::enqueue(
    BatchPreparation& prepared,
    DownloadManager& manager) const {
    BatchEnqueueResult result;
    for (PreparedCatalogInstall& item : prepared.items_) {
        if (!item.selected) {
            ++result.skipped;
            continue;
        }
        std::string taskId;
        std::string error;
        if (manager.importTorrent(item.torrentPath, item.mode, item.selection,
                                  taskId, error, item.initialPeers)) {
            result.taskIds.push_back(std::move(taskId));
            result.queuedInfoHashes.push_back(item.entry.infoHash);
        } else {
            result.failures.push_back({item.entry, std::move(error)});
        }
        ::unlink(item.torrentPath.c_str());
        item.torrentPath.clear();
    }
    return result;
}

} // namespace pipensx
