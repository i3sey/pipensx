#include "catalog_batch_installer.hpp"
#include "add_release.hpp"
#include "download_manager.hpp"
#include "game_update_install.hpp"
#include "nx_file_types.hpp"
#include "torrent_metainfo_fetch.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <unistd.h>

namespace pipensx {
namespace {

std::atomic<uint64_t> gBatchTempSerial{1};

// Keep in sync with pipensx/debrid/magnet_unavailable (en-US).
constexpr char kMagnetUnavailableError[] =
    "The debrid service could not open this torrent. Try another release.";

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

TorrentPreview previewFromDebrid(const DebridInfo& info) {
    return previewFromDebridFiles(info);
}

// Fills a DebridImport for an already-created provider transfer, mirroring
// enqueueViaDebrid's shape for the stream-install mask the caller carries.
DebridImport debridImportFor(const PreparedCatalogInstall& item,
                             DebridProviderKind providerKind) {
    DebridImport import;
    import.infoHash = lowerAscii(item.entry.infoHash);
    import.name = item.preview.name.empty() ? item.entry.title
                                            : item.preview.name;
    import.totalBytes = item.preview.totalBytes;
    import.debridId = item.debridId;
    import.provider = providerKind;
    import.mode = item.mode;
    import.fileSelection = item.selection;
    import.packageCount = item.space.packageFiles;
    return import;
}

// Number of package files the mask actually installs. The update mask must
// carry at least one, otherwise the release carries nothing identifiable.
uint32_t countInstalledPackages(const TorrentPreview& preview,
                                const std::vector<uint8_t>& mask) {
    uint32_t count = 0;
    for (size_t i = 0; i < preview.files.size() && i < mask.size(); ++i)
        if (preview.files[i].package &&
            mask[i] == static_cast<uint8_t>(FileAction::Install))
            ++count;
    return count;
}

std::string updateNothingSelectedError() {
    OneTapPlan plan;
    plan.block = OneTapBlock::NothingSelected;
    return oneTapBlockedMessage(plan);
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

        const SettingsInstallPlan planned = planSettingsInstall(
            preview, TransferMode::StreamInstall, selection);
        TransferMode mode = planned.mode;
        std::vector<uint8_t> mask = planned.fileSelection;
        InstallSpaceEstimate space = planned.space;
        if (space.packageFiles == 0 && mode != TransferMode::PortInstall) {
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

BatchPreparation CatalogBatchInstaller::prepareUpdates(
    const std::vector<UpdateQueueTarget>& targets,
    std::atomic<bool>& cancelled,
    const ProgressCallback& progress) const {
    BatchPreparation result;
    if (!resolver_) {
        for (const UpdateQueueTarget& target : targets)
            result.failures_.push_back(
                {target.entry, "Torrent resolver is unavailable."});
        return result;
    }

    for (size_t index = 0; index < targets.size(); ++index) {
        const UpdateQueueTarget& target = targets[index];
        if (cancelled.load()) {
            result.cancelled_ = true;
            break;
        }

        // Mirrors the detail-card flow: the metadata index is RuTracker-
        // derived, so a bundle without a magnet still resolves through the
        // canonical mirror announce.
        CatalogEntry entry = target.entry;
        if (entry.magnetUri.empty())
            entry.magnetUri =
                updateMagnetFor(lowerAscii(entry.infoHash), nullptr);
        const uint64_t serial = gBatchTempSerial.fetch_add(1);
        const std::string hash = lowerAscii(entry.infoHash);
        const std::string path = rootPath_ + "/_catalog_batch_" +
                                 (hash.empty() ? "unknown" : hash) + "_" +
                                 std::to_string(serial) + ".torrent";
        auto forwardProgress = [&, index](const MagnetProgress& magnet) {
            if (progress)
                progress({index + 1, targets.size(), entry.title, magnet});
        };
        if (progress)
            progress({index + 1, targets.size(), entry.title, {}});

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
        if (!hash.empty() && hash != lowerAscii(preview.infoHash)) {
            ::unlink(path.c_str());
            result.failures_.push_back(
                {entry, "Resolved torrent does not match the catalog entry."});
            continue;
        }

        // Same mask the one-tap update installs: the bundled patch above the
        // installed version plus missing DLC. Base is installed by
        // construction, so it is never selected again (F7).
        const OneTapContext& oneTap = target.oneTap;
        const std::vector<uint8_t> mask = selectSmartInstallFiles(
            preview, oneTap.titleInstalled, oneTap.installedVersion,
            oneTap.latestVersion, oneTap.titleId, oneTap.installedDlcIds);
        if (countInstalledPackages(preview, mask) == 0) {
            ::unlink(path.c_str());
            result.failures_.push_back(
                {entry, updateNothingSelectedError()});
            continue;
        }
        const InstallSpaceEstimate space = estimateInstallSpace(
            preview, mask, TransferMode::StreamInstall);
        if (space.overflow) {
            ::unlink(path.c_str());
            result.failures_.push_back(
                {entry, "Selected size is too large."});
            continue;
        }

        PreparedCatalogInstall item;
        item.entry = entry;
        item.torrentPath = path;
        item.preview = std::move(preview);
        item.selection = mask;
        item.initialPeers = std::move(initialPeers);
        item.mode = TransferMode::StreamInstall;
        item.space = space;
        result.items_.push_back(std::move(item));
    }
    return result;
}

BatchPreparation CatalogBatchInstaller::prepareUpdatesViaDebrid(
    const std::vector<UpdateQueueTarget>& targets,
    DebridProvider& provider,
    std::atomic<bool>& cancelled,
    const ProgressCallback& progress,
    DebridBatchTiming timing) const {
    BatchPreparation result;
    for (size_t index = 0; index < targets.size(); ++index) {
        const UpdateQueueTarget& target = targets[index];
        if (cancelled.load()) {
            result.cancelled_ = true;
            return result;
        }
        if (progress)
            progress({index + 1, targets.size(), target.entry.title, {}});
        const std::string hash = lowerAscii(target.entry.infoHash);
        const uint64_t serial = gBatchTempSerial.fetch_add(1);
        const std::string tmp = rootPath_ + "/_debrid_batch_" +
                                (hash.empty() ? "unknown" : hash) + "_" +
                                std::to_string(serial) + ".torrent";
        const std::string magnet = target.entry.magnetUri.empty()
            ? updateMagnetFor(hash, nullptr)
            : target.entry.magnetUri;
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timing.resolveWindowMs);
        std::string id;
        DebridInfo info;
        std::string err;
        if (!createDebridWithMetainfoFallback(
                provider, magnet, hash, target.entry.infoDict, tmp, cancelled,
                deadline, id, info, err)) {
            if (cancelled.load()) {
                result.cancelled_ = true;
                return result;
            }
            result.failures_.push_back(
                {target.entry, err.empty() ? kMagnetUnavailableError : err});
            continue;
        }
        if (info.phase == DebridInfo::Phase::Failed) {
            std::string ignored;
            provider.remove(id, ignored);
            result.failures_.push_back({target.entry, kMagnetUnavailableError});
            continue;
        }

        TorrentPreview preview =
            previewFromDebridFiles(info, target.entry.title,
                                   target.entry.size);
        const OneTapContext& oneTap = target.oneTap;
        const std::vector<uint8_t> mask = selectSmartInstallFiles(
            preview, oneTap.titleInstalled, oneTap.installedVersion,
            oneTap.latestVersion, oneTap.titleId, oneTap.installedDlcIds);
        if (countInstalledPackages(preview, mask) == 0) {
            std::string ignored;
            provider.remove(id, ignored);
            result.failures_.push_back(
                {target.entry, updateNothingSelectedError()});
            continue;
        }

        PreparedCatalogInstall item;
        item.entry = target.entry;
        item.preview = std::move(preview);
        item.selection = mask;
        item.mode = TransferMode::StreamInstall;
        item.space = estimateInstallSpace(preview, item.selection,
                                          TransferMode::StreamInstall);
        item.source = InstallSource::Debrid;
        item.debridId = id;
        result.items_.push_back(std::move(item));
    }
    return result;
}

BatchEnqueueResult CatalogBatchInstaller::enqueueUpdates(
    BatchPreparation& prepared,
    DownloadManager& manager,
    DebridProviderKind providerKind,
    DebridProvider* provider) const {
    BatchEnqueueResult result;
    for (PreparedCatalogInstall& item : prepared.items_) {
        if (!item.selected) {
            ++result.skipped;
            continue;
        }
        std::string taskId;
        std::string error;
        bool ok = false;
        if (item.source == InstallSource::Debrid) {
            if (!provider) {
                result.failures.push_back(
                    {item.entry, "Debrid provider is unavailable."});
                continue;
            }
            ok = manager.importDebrid(
                debridImportFor(item, providerKind), taskId, error);
            if (!ok) {
                std::string ignored;
                provider->remove(item.debridId, ignored);
            }
        } else {
            ok = manager.importTorrentActions(item.torrentPath, item.selection,
                                              taskId, error,
                                              item.initialPeers);
            ::unlink(item.torrentPath.c_str());
            item.torrentPath.clear();
            // F3: a repeated import that adds nothing new refuses with
            // "already in the download manager" — that is a skip, not a
            // failure, so "Update all" stays idempotent.
            if (!ok && lowerAscii(error).find(
                           "already in the download manager") !=
                           std::string::npos) {
                ++result.skipped;
                continue;
            }
        }
        if (ok) {
            result.taskIds.push_back(std::move(taskId));
            result.queuedInfoHashes.push_back(item.entry.infoHash);
        } else {
            result.failures.push_back({item.entry, std::move(error)});
        }
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
        const CatalogEntry& entry = entries[i];
        const uint64_t serial = gBatchTempSerial.fetch_add(1);
        const std::string hash = lowerAscii(entry.infoHash);
        const std::string tmp = rootPath_ + "/_debrid_batch_" +
                                (hash.empty() ? "unknown" : hash) + "_" +
                                std::to_string(serial) + ".torrent";
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timing.resolveWindowMs);
        std::string id;
        DebridInfo info;
        std::string err;
        if (!createDebridWithMetainfoFallback(
                provider, entry.magnetUri, hash, entry.infoDict, tmp, cancelled,
                deadline, id, info, err)) {
            if (cancelled.load()) {
                result.cancelled_ = true;
                removeAll();
                return result;
            }
            result.failures_.push_back(
                {entry, err.empty() ? kMagnetUnavailableError : err});
            continue;
        }
        Pending p;
        p.entry = entry;
        p.id = id;
        p.info = std::move(info);
        p.ready = true;
        p.haveInfo = true;
        pending.push_back(std::move(p));
    }

    for (Pending& p : pending) {
        if (p.haveInfo && p.info.phase == DebridInfo::Phase::Failed) {
            std::string ignored;
            provider.remove(p.id, ignored);
            result.failures_.push_back({p.entry, kMagnetUnavailableError});
            continue;
        }
        if (p.ready) {
            TorrentPreview preview = previewFromDebrid(p.info);
            const SettingsInstallPlan planned = planSettingsInstall(
                preview, TransferMode::StreamInstall, selection);
            TransferMode mode = planned.mode;
            std::vector<uint8_t> mask = planned.fileSelection;
            InstallSpaceEstimate space = planned.space;
            if (space.packageFiles == 0 &&
                mode != TransferMode::PortInstall) {
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
        import.packageCount =
            (item.mode == TransferMode::StreamInstall ||
             item.mode == TransferMode::PortInstall)
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
