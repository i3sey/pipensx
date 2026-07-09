#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../core/torrent.h"
}

#include "../install/install_backend.hpp"

namespace pipensx {

enum class DownloadStatus {
    Queued,
    Checking,
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
    uint64_t totalBytes = 0;
    uint64_t completedBytes = 0;
    uint64_t speedBytesPerSecond = 0;
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
    std::string currentPackage;
    std::vector<uint8_t> fileSelection;
    /* Compact IPv4 endpoints verified during magnet resolution. Ephemeral:
       queued before tracker/DHT results and intentionally not persisted. */
    std::vector<uint8_t> initialPeers;
};

struct TorrentPreview {
    std::string name;
    std::string infoHash;
    uint64_t totalBytes = 0;
    uint32_t fileCount = 0;
    uint32_t trackerCount = 0;
    uint32_t packageCount = 0;
    uint32_t cartridgeCount = 0;
    struct File {
        std::string path;
        uint64_t length = 0;
        bool package = false;
        bool compressed = false;
        bool cartridge = false;
    };
    std::vector<File> files;
};

class DownloadManager {
public:
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
    bool pause(const std::string& taskId);
    bool resume(const std::string& taskId);
    bool retry(const std::string& taskId);
    bool verify(const std::string& taskId);
    bool remove(const std::string& taskId, bool deleteData,
                std::string& error);

    // Where stream installs commit content (PERF_PLAN 7.4). Applied to
    // coordinators started after the call; a transfer in flight keeps the
    // target it began with. Default is SD.
    void setInstallTarget(install::InstallStorageTarget target) {
        installTarget_.store(target, std::memory_order_relaxed);
    }

    bool hasActiveTransfer() const;
    std::vector<DownloadTask> snapshot() const;
    bool save(std::string& error) const;
    void shutdown();

    const std::string& rootPath() const { return rootPath_; }
    const std::string& downloadRoot() const { return downloadRoot_; }
    const std::string& torrentRoot() const { return torrentRoot_; }

private:
    void load();
    void workerMain();
    bool saveLocked(std::string& error) const;
    DownloadTask* findLocked(const std::string& id);
    const DownloadTask* findLocked(const std::string& id) const;
    bool removeLocked(const std::string& id, bool deleteData,
                      std::string& error);

    std::string rootPath_;
    std::string torrentRoot_;
    std::string downloadRoot_;
    std::string statePath_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<DownloadTask> tasks_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<install::InstallStorageTarget> installTarget_{
        install::InstallStorageTarget::SdCard};
    bool workerStarted_ = false;
};

const char* statusName(DownloadStatus status);

} // namespace pipensx
