#include "catalog_batch_installer.hpp"
#include "download_manager.hpp"
#include "nx_file_types.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
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

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool sleepAbortable(std::atomic<bool>& cancelled, uint32_t ms) {
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (cancelled.load()) return false;
        uint32_t slice = ms - elapsed < 50 ? ms - elapsed : 50;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        elapsed += slice;
    }
    return !cancelled.load();
}

TorrentPreview previewFromDebrid(const DebridInfo& info) {
    TorrentPreview p;
    p.name = info.name;
    p.totalBytes = info.bytes;
    p.fileCount = static_cast<uint32_t>(info.files.size());
    for (const DebridFile& f : info.files) {
        TorrentPreview::File pf;
        pf.path = f.path;
        pf.length = f.bytes;
        pf.package = isPackageName(f.path);
        pf.compressed = isCompressedName(f.path);
        pf.cartridge = isCartridgeName(f.path);
        if (pf.package) ++p.packageCount;
        if (pf.cartridge) ++p.cartridgeCount;
        p.files.push_back(std::move(pf));
    }
    return p;
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

BatchPreparation CatalogBatchInstaller::prepareViaDebrid(
    const std::vector<CatalogEntry>& entries,
    StreamSelection selection,
    DebridProvider& provider,
    std::atomic<bool>& cancelled,
    const ProgressCallback& progress,
    DebridBatchTiming timing) const {
    BatchPreparation result;

    struct Pending {
        CatalogEntry entry;
        std::string id;
        DebridInfo info;
        bool ready = false;
        bool haveInfo = false;
    };
    std::vector<Pending> pending;

    auto removeAll = [&]() {
        for (const Pending& p : pending) {
            std::string ignored;
            provider.remove(p.id, ignored);
        }
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        if (cancelled.load()) { result.cancelled_ = true; removeAll(); return result; }
        if (progress) progress({i + 1, entries.size(), entries[i].title, {}});
        std::string id;
        std::string err;
        if (!provider.createFromMagnet(entries[i].magnetUri, id, err)) {
            result.failures_.push_back(
                {entries[i], err.empty() ? "Debrid service rejected the magnet." : err});
            continue;
        }
        Pending p; p.entry = entries[i]; p.id = id;
        pending.push_back(std::move(p));
    }

    uint64_t start = nowMs();
    while (!pending.empty()) {
        if (cancelled.load()) { result.cancelled_ = true; removeAll(); return result; }
        bool allReady = true;
        size_t readyCount = 0;
        for (Pending& p : pending) {
            if (p.ready) { ++readyCount; continue; }
            DebridInfo info;
            std::string err;
            if (provider.fetchInfo(p.id, info, err)) {
                p.info = info; p.haveInfo = true;
                if (info.phase >= DebridInfo::Phase::AwaitingSelection &&
                    !info.files.empty()) {
                    p.ready = true; ++readyCount;
                } else {
                    allReady = false;
                }
            } else {
                allReady = false;
            }
        }
        if (allReady) break;
        if (progress)
            progress({readyCount, pending.size(),
                      "Fetching file lists from debrid service", {}});
        if (nowMs() - start >= timing.resolveWindowMs) break;
        if (!sleepAbortable(cancelled, timing.pollIntervalMs)) {
            result.cancelled_ = true; removeAll(); return result;
        }
    }

    for (Pending& p : pending) {
        if (p.ready) {
            TorrentPreview preview = previewFromDebrid(p.info);
            TransferMode mode = TransferMode::StreamInstall;
            std::vector<uint8_t> mask =
                defaultInstallSelection(preview, mode, selection);
            InstallSpaceEstimate space =
                estimateInstallSpace(preview, mask, mode);
            if (space.packageFiles == 0) {
                if (selection == StreamSelection::PackagesOnly) {
                    std::string ignored; provider.remove(p.id, ignored);
                    result.failures_.push_back(
                        {p.entry, "No package files match the current "
                                  "Settings selection."});
                    continue;
                }
                mode = TransferMode::DownloadOnly;
                space = estimateInstallSpace(preview, mask, mode);
            }
            if (space.selectedFiles == 0 || space.overflow) {
                std::string ignored; provider.remove(p.id, ignored);
                result.failures_.push_back(
                    {p.entry, space.overflow ? "Selected size is too large."
                                             : "No files were selected."});
                continue;
            }
            PreparedCatalogInstall item;
            item.entry = p.entry;
            item.preview = std::move(preview);
            item.selection = std::move(mask);
            item.mode = mode;
            item.space = space;
            item.source = InstallSource::Debrid;
            item.debridId = p.id;
            result.items_.push_back(std::move(item));
        } else {
            PreparedCatalogInstall item;
            item.entry = p.entry;
            item.preview.name = p.haveInfo && !p.info.name.empty()
                                    ? p.info.name : p.entry.title;
            item.preview.totalBytes =
                p.haveInfo && p.info.bytes ? p.info.bytes : p.entry.size;
            // Metadata can still arrive in the queue. Preserve batch-install
            // intent; DebridTransfer selects and installs package files once
            // the provider exposes them.
            item.mode = TransferMode::StreamInstall;
            item.space.selectedBytes = item.preview.totalBytes;
            item.space.downloadBytes = item.preview.totalBytes;
            item.space.requiredBytes = item.preview.totalBytes;
            item.space.selectedFiles = 1;
            item.space.certainty = SpaceEstimateCertainty::Conservative;
            item.source = InstallSource::Debrid;
            item.debridId = p.id;
            result.items_.push_back(std::move(item));
        }
    }
    return result;
}

BatchEnqueueResult CatalogBatchInstaller::enqueueViaDebrid(
    BatchPreparation& prepared,
    DownloadManager& manager,
    DebridProviderKind providerKind,
    DebridProvider& provider) const {
    BatchEnqueueResult result;
    for (PreparedCatalogInstall& item : prepared.items_) {
        if (!item.selected) {
            std::string ignored;
            provider.remove(item.debridId, ignored);
            ++result.skipped;
            continue;
        }
        DebridImport import;
        import.infoHash = lowerAscii(item.entry.infoHash);
        import.name = item.preview.name.empty() ? item.entry.title
                                                 : item.preview.name;
        import.totalBytes = item.preview.totalBytes;
        import.debridId = item.debridId;
        import.provider = providerKind;
        import.mode = item.mode;
        import.fileSelection = item.selection;
        import.packageCount = item.mode == TransferMode::StreamInstall
                                  ? item.space.packageFiles : 0;
        std::string taskId, error;
        if (manager.importDebrid(import, taskId, error)) {
            result.taskIds.push_back(std::move(taskId));
            result.queuedInfoHashes.push_back(item.entry.infoHash);
        } else {
            std::string ignored;
            provider.remove(item.debridId, ignored);
            result.failures.push_back({item.entry, std::move(error)});
        }
    }
    return result;
}

} // namespace pipensx
