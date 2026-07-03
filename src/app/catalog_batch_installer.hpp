#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "catalog_service.hpp"
#include "install_space.hpp"
#include "magnet_resolver.hpp"

namespace pipensx {

struct PreparedCatalogInstall {
    CatalogEntry entry;
    std::string torrentPath;
    TorrentPreview preview;
    std::vector<uint8_t> selection;
    std::vector<uint8_t> initialPeers;
    TransferMode mode = TransferMode::DownloadOnly;
    InstallSpaceEstimate space;
    bool selected = true;
};

struct BatchItemFailure {
    CatalogEntry entry;
    std::string error;
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
    BatchPreparation& operator=(BatchPreparation&&) = delete;

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

private:
    std::string rootPath_;
    ResolveTorrent resolver_;
};

} // namespace pipensx
