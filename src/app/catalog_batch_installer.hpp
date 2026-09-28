#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "add_release.hpp"
#include "catalog_service.hpp"
#include "debrid_provider.hpp"
#include "install_space.hpp"
#include "magnet_resolver.hpp"

namespace pipensx {

enum class InstallSource { Torrent, Debrid };

struct PreparedCatalogInstall {
    CatalogEntry entry;
    std::string torrentPath;
    TorrentPreview preview;
    std::vector<uint8_t> selection;
    std::vector<uint8_t> initialPeers;
    TransferMode mode = TransferMode::DownloadOnly;
    InstallSpaceEstimate space;
    bool selected = true;
    InstallSource source = InstallSource::Torrent;
    std::string debridId;
};

struct BatchItemFailure {
    CatalogEntry entry;
    std::string error;
};

// One "Update all" target (bugfix guide F4): the version-matched release
// bundle of an already-installed title plus the one-tap facts the update
// mask needs. Targets are built from the installed list, so the base game
// is present by construction and selectSmartInstallFiles can only add the
// missing patch and DLC — bulk queueing never installs an update for a
// title without its base (F7) and never re-downloads an installed base.
struct UpdateQueueTarget {
    CatalogEntry entry;
    OneTapContext oneTap;
};

struct BatchPrepareProgress {
    size_t index = 0;
    size_t total = 0;
    std::string title;
    MagnetProgress magnet;
};

class BatchPreparation {
public:
    BatchPreparation() = default;
    ~BatchPreparation();
    BatchPreparation(const BatchPreparation&) = delete;
    BatchPreparation& operator=(const BatchPreparation&) = delete;
    BatchPreparation(BatchPreparation&&) noexcept = default;
    BatchPreparation& operator=(BatchPreparation&&) = default;

    const std::vector<PreparedCatalogInstall>& items() const { return items_; }
    std::vector<PreparedCatalogInstall>& items() { return items_; }
    const std::vector<BatchItemFailure>& failures() const { return failures_; }
    bool cancelled() const { return cancelled_; }
    InstallSpaceEstimate selectedSpace() const;

private:
    friend class CatalogBatchInstaller;
    std::vector<PreparedCatalogInstall> items_;
    std::vector<BatchItemFailure> failures_;
    bool cancelled_ = false;
};

struct BatchEnqueueResult {
    std::vector<std::string> taskIds;
    std::vector<std::string> queuedInfoHashes;
    std::vector<BatchItemFailure> failures;
    size_t skipped = 0;
};

struct DebridBatchTiming {
    uint32_t pollIntervalMs = 2000;
    uint32_t resolveWindowMs = 60000;
};

class CatalogBatchInstaller {
public:
    using ResolveTorrent = std::function<bool(
        const CatalogEntry&, const std::string&, std::atomic<bool>&,
        const MagnetResolver::ProgressCallback&, std::vector<uint8_t>&,
        std::string&)>;
    using ProgressCallback = std::function<void(const BatchPrepareProgress&)>;

    CatalogBatchInstaller(std::string rootPath, ResolveTorrent resolver);

    BatchPreparation prepare(const std::vector<CatalogEntry>& entries,
                             StreamSelection selection,
                             std::atomic<bool>& cancelled,
                             const ProgressCallback& progress) const;
    BatchEnqueueResult enqueue(BatchPreparation& prepared,
                               DownloadManager& manager) const;

    // F4 "Update all": prepare every target the way the detail-card one-tap
    // does — resolve, then the update-aware smart mask instead of the
    // settings-driven batch mask. A release whose update package cannot be
    // identified is reported as a failure (the chooser explanation), never
    // guessed into a wrong install.
    BatchPreparation prepareUpdates(
        const std::vector<UpdateQueueTarget>& targets,
        std::atomic<bool>& cancelled,
        const ProgressCallback& progress) const;

    // Debrid twin of prepareUpdates: one transfer per target, planned with
    // the same update-aware mask. Transfers whose metadata does not arrive
    // inside the window are removed and reported.
    BatchPreparation prepareUpdatesViaDebrid(
        const std::vector<UpdateQueueTarget>& targets,
        DebridProvider& provider,
        std::atomic<bool>& cancelled,
        const ProgressCallback& progress,
        DebridBatchTiming timing = {}) const;

    // Update enqueue: torrent items go through importTorrentActions, so a
    // repeated "Update all" lands in the merge path and a repeat that adds
    // nothing new refuses with "already in the download manager" — counted
    // as skipped, not failed. Debrid items go through importDebrid.
    BatchEnqueueResult enqueueUpdates(
        BatchPreparation& prepared,
        DownloadManager& manager,
        DebridProviderKind providerKind = DebridProviderKind::TorBox,
        DebridProvider* provider = nullptr) const;

    BatchPreparation prepareViaDebrid(
        const std::vector<CatalogEntry>& entries,
        StreamSelection selection,
        DebridProvider& provider,
        std::atomic<bool>& cancelled,
        const ProgressCallback& progress,
        DebridBatchTiming timing = {}) const;

    BatchEnqueueResult enqueueViaDebrid(
        BatchPreparation& prepared,
        DownloadManager& manager,
        DebridProviderKind providerKind,
        DebridProvider& provider) const;

private:
    std::string rootPath_;
    ResolveTorrent resolver_;
};

} // namespace pipensx
