#pragma once

#include "debrid_provider.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include "../core/torrent.h"
}

#include "../install/install_backend.hpp"
#include "stream_budget_arbiter.hpp"

namespace pipensx {

// Supported range for setMaxActiveDownloads(). Inline because the settings
// layer validates against it and not every binary links download_manager.cpp.
inline constexpr uint32_t kMinActiveDownloads = 1;
inline constexpr uint32_t kMaxActiveDownloads = 4;
inline constexpr uint64_t kInstallRateWindowMs = 1000;
inline constexpr uint64_t kProgressRateStaleMs = 3000;

inline uint32_t clampMaxActiveDownloads(uint64_t value) {
    if (value < kMinActiveDownloads)
        return kMinActiveDownloads;
    if (value > kMaxActiveDownloads)
        return kMaxActiveDownloads;
    return static_cast<uint32_t>(value);
}

enum class DownloadStatus {
    Queued,
    Checking,
    Fetching,
    Downloading,
    Paused,
    Verifying,
    Completed,
    Installing,
    Committing,
    Installed,
    Error,
    Removing,
};

enum class TaskSource { Torrent, Debrid };

enum class TransferMode {
    DownloadOnly,
    StreamInstall,
};

enum class FileAction : uint8_t {
    Skip = 0,
    Download = 1,
    Install = 2,
};

struct DownloadTask {
    std::string id;
    std::string name;
    std::string metainfoPath;
    std::string dataPath;
    std::string error;
    DownloadStatus status = DownloadStatus::Queued;
    TransferMode mode = TransferMode::DownloadOnly;
    TaskSource source = TaskSource::Torrent;
    DebridProviderKind debridProvider = DebridProviderKind::TorBox;
    std::string debridId;
    double fetchProgress = 0.0;
    uint64_t totalBytes = 0;
    uint64_t completedBytes = 0;
    /* Progress denominators excluding skipped storage ranges (unselected
       files, consumed resume prefixes): those pieces are pre-marked done by
       the engine's startup scan, so completedBytes/totalBytes alone would
       overstate progress. Transient — set by the torrent poll loop, zero
       before the engine reports (fall back to total/completed then). */
    uint64_t wantedTotalBytes = 0;
    uint64_t wantedCompletedBytes = 0;
    uint64_t speedBytesPerSecond = 0;
    uint64_t downloadProgressUpdatedAtMs = 0;
    uint32_t peers = 0;
    uint32_t dhtGood = 0;
    uint32_t dhtDubious = 0;
    uint32_t piecesDone = 0;
    uint32_t piecesTotal = 0;
    uint32_t piecesVerified = 0;
    uint32_t packageCount = 0;
    uint32_t packagesInstalled = 0;
    uint64_t installedBytes = 0;
    uint64_t installTotalBytes = 0;
    uint64_t installSpeedBytesPerSecond = 0;
    uint64_t installSpeedUpdatedAtMs = 0;
    // Transient baseline for the shared install-rate estimator. Rate state is
    // intentionally omitted from queue persistence.
    uint64_t installRateBaseBytes = 0;
    uint64_t installRateBaseAtMs = 0;
    std::string currentPackage;
    std::vector<uint8_t> fileSelection;
    /* Compact IPv4 endpoints verified during magnet resolution. Ephemeral:
       queued before tracker/DHT results and intentionally not persisted. */
    std::vector<uint8_t> initialPeers;
    /* Fast resume: have-bitfield from the last orderly torrent teardown.
       Empty = untrusted (crash or mid-run) and the next start does a full
       hash scan. */
    std::vector<uint8_t> resumeBitfield;
};

// (done, total) download-progress byte pair. Falls back to the raw engine
// numbers until the poll loop fills the wanted fields (queued task, debrid
// source, or engine not yet reporting).
inline std::pair<uint64_t, uint64_t> downloadProgressBytes(
    const DownloadTask& task) {
    if (task.wantedTotalBytes) {
        const uint64_t done = task.wantedCompletedBytes > task.wantedTotalBytes
            ? task.wantedTotalBytes : task.wantedCompletedBytes;
        return {done, task.wantedTotalBytes};
    }
    return {task.completedBytes, task.totalBytes};
}

struct TorrentPreview {
    std::string name;
    std::string infoHash;
    bool multi = false;
    uint64_t totalBytes = 0;
    uint32_t fileCount = 0;
    uint32_t trackerCount = 0;
    uint32_t packageCount = 0;
    uint32_t cartridgeCount = 0;
    uint32_t pieceCount = 0;
    struct File {
        std::string path;
        uint64_t length = 0;
        bool package = false;
        bool compressed = false;
        bool cartridge = false;
    };
    std::vector<File> files;
};

struct DebridImport {
    std::string infoHash;               // lowercase hex, becomes task id
    std::string name;
    uint64_t totalBytes = 0;
    DebridProviderKind provider = DebridProviderKind::TorBox;
    std::string debridId;              // nonzero when already created (catalog)
    std::string torrentPath;           // local .torrent to copy+upload ("" for catalog)
    TransferMode mode = TransferMode::DownloadOnly;
    std::vector<uint8_t> fileSelection;
    uint32_t packageCount = 0;         // selected packages (0 if mode DownloadOnly)
};

class DownloadManager {
public:
    class ExternalDeployLease {
    public:
        ExternalDeployLease() = default;
        ExternalDeployLease(ExternalDeployLease&& other) noexcept;
        ExternalDeployLease& operator=(ExternalDeployLease&& other) noexcept;
        ~ExternalDeployLease();

        ExternalDeployLease(const ExternalDeployLease&) = delete;
        ExternalDeployLease& operator=(const ExternalDeployLease&) = delete;

        const DownloadTask& task() const { return task_; }
        explicit operator bool() const { return owner_ != nullptr; }

    private:
        friend class DownloadManager;
        ExternalDeployLease(DownloadManager* owner, DownloadTask task)
            : owner_(owner), task_(std::move(task)) {}
        void release();

        DownloadManager* owner_ = nullptr;
        DownloadTask task_;
    };

    explicit DownloadManager(std::string rootPath, bool startWorker = true);
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    static bool previewTorrent(const std::string& path, TorrentPreview& preview,
                               std::string& error);

    bool importTorrent(const std::string& path, TransferMode mode,
                       const std::vector<uint8_t>& selectedFiles,
                       std::string& taskId, std::string& error,
                       const std::vector<uint8_t>& initialPeers = {});
    bool importTorrentActions(const std::string& path,
                              const std::vector<uint8_t>& fileActions,
                              std::string& taskId, std::string& error,
                              const std::vector<uint8_t>& initialPeers = {});
    bool importTorrent(const std::string& path, TransferMode mode,
                       std::string& taskId, std::string& error) {
        std::vector<uint8_t> selectedFiles;
        return importTorrent(path, mode, selectedFiles, taskId, error);
    }
    bool importTorrent(const std::string& path, std::string& taskId,
                       std::string& error) {
        return importTorrent(path, TransferMode::DownloadOnly, taskId, error);
    }
    bool importDebrid(const DebridImport& import,
                      std::string& taskId, std::string& error);
    void setTorboxApiKey(const std::string& key);
    void setTorrserverUrl(const std::string& url);
    void setRealdebridApiKey(const std::string& key);
    std::string torboxApiKey() const;
    void setTorrentingEnabled(bool enabled);
    bool torrentingEnabled() const;
    bool pause(const std::string& taskId);
    bool resume(const std::string& taskId);
    bool retry(const std::string& taskId);
    bool verify(const std::string& taskId);
    bool remove(const std::string& taskId, bool deleteData,
                std::string& error);

    // Make a queued task the next one to start. The scheduler always claims
    // the first claimable Queued entry in list order, so "next up" is a
    // position, not a priority field — this moves the task ahead of every
    // other queued one and leaves the order behind it alone. Note: while the
    // single install token is held, a stream-install task at the front can
    // still be passed by download-only tasks behind it.
    bool moveToFront(const std::string& taskId, std::string& error);

    // How many torrents may download concurrently (clamped to
    // [kMinActiveDownloads, kMaxActiveDownloads]).
    // Shrinking takes effect as running tasks finish; nothing is preempted.
    void setMaxActiveDownloads(uint32_t count);

    // Where stream installs commit content (PERF_PLAN 7.4). Applied to
    // coordinators started after the call; a transfer in flight keeps the
    // target it began with. Default is SD.
    void setInstallTarget(install::InstallStorageTarget target) {
        installTarget_.store(target, std::memory_order_relaxed);
    }

    install::InstallStorageTarget installTarget() const {
        return installTarget_.load(std::memory_order_relaxed);
    }

    bool hasActiveTransfer() const;
    // Existence check without the full deep copy snapshot() makes.
    bool hasTask(const std::string& id) const;
    std::vector<DownloadTask> snapshot() const;
    std::optional<DownloadTask> snapshot(const std::string& id) const;
    std::optional<ExternalDeployLease> beginExternalDeploy(
        const std::string& taskId, std::string& error);
    bool externalDeployActive() const;
    std::string externalDeployTaskId() const;
    bool save(std::string& error) const;
    void shutdown();

    const std::string& rootPath() const { return rootPath_; }
    const std::string& downloadRoot() const { return downloadRoot_; }
    const std::string& torrentRoot() const { return torrentRoot_; }

private:
    // Everything the worker copies out of a task under mutex_ when it claims
    // it; runTask then runs lock-free against these until it re-locks to
    // publish progress.
    struct ClaimedTask {
        std::string id;
        std::string name;
        std::string metainfoPath;
        std::string dataPath;
        TransferMode mode = TransferMode::DownloadOnly;
        TaskSource source = TaskSource::Torrent;
        DebridProviderKind debridProvider = DebridProviderKind::TorBox;
        std::string debridId;
        uint32_t packagesInstalled = 0;
        std::vector<uint8_t> fileSelection;
        std::vector<uint8_t> initialPeers;
        std::vector<uint8_t> resumeBitfield;
    };

    // One active torrent slot: a runner thread owning one engine instance.
    // The slot index picks the listen port (base + index), so N=1 always
    // runs on the classic port.
    struct RunnerSlot {
        std::thread thread;
        std::string taskId;
        uint32_t slotIndex = 0;
        bool holdsInstallToken = false;
        std::atomic<bool> done{false};
    };

    void load();
    void schedulerMain();
    void runTask(RunnerSlot* slot, ClaimedTask claim);
    // The debrid half of runTask: no engine, no peers, no arbiter slot — the
    // provider fetches the torrent and we pull the result over HTTPS.
    void runDebridTask(const ClaimedTask& claim);
    DownloadTask* claimableLocked();
    void reapRunnersLocked(std::unique_lock<std::mutex>& lock);
    bool saveLocked(std::string& error) const;
    DownloadTask* findLocked(const std::string& id);
    const DownloadTask* findLocked(const std::string& id) const;
    void endExternalDeploy(const std::string& taskId);
    bool externallyLeasedLocked(const std::string& taskId) const;
    bool removeLocked(const std::string& id, bool deleteData,
                      std::string& error);
    // Fires a detached thread, so it must not touch *this: the manager can be
    // torn down while a provider call is still in flight.
    static void removeFromDebridAsync(DebridProviderKind provider,
                                      const std::string& apiKey,
                                      const std::string& debridId);
    std::string apiKeyFor(DebridProviderKind provider) const;
    static std::unique_ptr<class DebridProvider> makeProvider(
        DebridProviderKind provider, const std::string& key);

    std::string rootPath_;
    std::string torrentRoot_;
    std::string downloadRoot_;
    std::string statePath_;
    std::string torboxApiKey_;
    std::string torrserverUrl_;
    std::string realdebridApiKey_;
    // Off until someone opts in. The constructor starts the worker before any
    // caller can configure the manager, so a restored Queued torrent task is
    // eligible for pickup during that window — defaulting to true would let it
    // reach a tracker on a console where the user has torrenting disabled.
    std::atomic<bool> torrentingEnabled_{false};

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    StreamBudgetArbiter arbiter_;
    std::vector<DownloadTask> tasks_;
    std::thread worker_; // scheduler thread
    std::vector<std::unique_ptr<RunnerSlot>> runners_; // guarded by mutex_
    uint32_t maxActive_ = 1;          // guarded by mutex_
    // Single install token: only one stream-install task may write to NCM
    // at a time; download-only tasks pass token-blocked stream tasks.
    bool installTokenHeld_ = false;   // guarded by mutex_
    std::string externalDeployTaskId_; // guarded by mutex_
    uint32_t slotBitmap_ = 0;         // guarded by mutex_
    std::atomic<bool> stopping_{false};
    std::atomic<install::InstallStorageTarget> installTarget_{
        install::InstallStorageTarget::SdCard};
    bool workerStarted_ = false;
};

const char* statusName(DownloadStatus status);

void updateTaskInstallProgress(DownloadTask& task, uint64_t installedBytes,
                               uint64_t installTotalBytes,
                               DownloadStatus status, uint64_t nowMs);
void updateTaskDownloadProgress(DownloadTask& task, uint64_t completedBytes,
                                uint64_t nowMs);
uint64_t currentInstallSpeed(const DownloadTask& task, uint64_t nowMs);
std::optional<uint64_t> taskEtaSeconds(const DownloadTask& task,
                                       uint64_t nowMs);

// The scheduler's claim rule, exposed for tests: a Queued task may start
// unless it is a stream install while another one holds the install token.
bool taskClaimableUnderInstallToken(const DownloadTask& task,
                                     bool installTokenHeld);

// User-facing torrent health for an active download, derived from peer count
// and current throughput. NotActive for anything that is not downloading.
enum class TorrentHealth {
    NotActive,
    Poor,
    Slow,
    Excellent,
};

// Peer/throughput-derived health, kept inline so tests need no download
// engine. NotActive for anything that is not downloading.
inline TorrentHealth torrentHealth(const DownloadTask& task, uint64_t nowMs) {
    (void)nowMs;
    if (task.status != DownloadStatus::Downloading)
        return TorrentHealth::NotActive;
    const uint64_t speed = task.speedBytesPerSecond;
    constexpr uint64_t kSlowThreshold = 512ull * 1024ull;
    if (speed == 0)
        return task.peers == 0 ? TorrentHealth::Poor : TorrentHealth::Slow;
    if (speed < kSlowThreshold)
        return TorrentHealth::Slow;
    return TorrentHealth::Excellent;
}

} // namespace pipensx
