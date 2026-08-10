#pragma once

#include "download_manager.hpp"
#include "port_archive.hpp"
#include "task_files.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pipensx {

enum class SwitchDeployProblem {
    None,
    TaskNotFound,
    NotReady,
    LayoutNotFound,
    AmbiguousLayout,
    UnsafePath,
    MissingSource,
    Conflict,
    NoSpace,
    NoRam,
    Busy,
    Io,
};

enum class SwitchDeployEntryState {
    Missing,
    ExistingIdentical,
    ExistingConflict,
};

struct SwitchDeployEntry {
    std::string sourcePath;
    std::string sourceRelativePath;
    std::string destinationPath;
    std::string destinationRelativePath;
    uint64_t size = 0;
    SwitchDeployEntryState state = SwitchDeployEntryState::Missing;
    std::array<uint8_t, 32> sha256 {};
    bool nro = false;
};

struct SwitchDeployArchive {
    std::string sourcePath;
    std::string sourceRelativePath;
    uint64_t size = 0;
    uint64_t unpackBytes = 0;
    uint64_t maxSolidBlockBytes = 0;
    size_t switchFiles = 0;
    bool extractable = true;
    std::string detail;
};

struct SwitchDeployPlan {
    std::string taskId;
    std::string targetRoot;
    std::vector<SwitchDeployEntry> files;
    std::vector<SwitchDeployArchive> archives;
    uint64_t totalBytes = 0;
    uint64_t bytesToCopy = 0;
    uint64_t freeBytes = 0;
    size_t ignoredFiles = 0;
    size_t identicalFiles = 0;
    size_t conflictFiles = 0;
};

struct SwitchDeployInspection {
    TaskFileInventory inventory;
    SwitchDeployPlan plan;
    SwitchDeployProblem problem = SwitchDeployProblem::None;
    std::string detail;

    bool canStart() const { return problem == SwitchDeployProblem::None; }
};

enum class SwitchDeployPhase {
    Idle,
    Preparing,
    Copying,
    Extracting,
    Completed,
    Failed,
    Cancelled,
};

struct SwitchDeploySnapshot {
    SwitchDeployPhase phase = SwitchDeployPhase::Idle;
    SwitchDeployProblem problem = SwitchDeployProblem::None;
    std::string taskId;
    std::string currentPath;
    std::string detail;
    uint64_t bytesCopied = 0;
    uint64_t totalBytes = 0;
    size_t filesCopied = 0;
    size_t totalFiles = 0;
    size_t identicalFiles = 0;
    uint64_t generation = 0;

    bool active() const {
        return phase == SwitchDeployPhase::Preparing ||
               phase == SwitchDeployPhase::Copying ||
               phase == SwitchDeployPhase::Extracting;
    }
};

enum class SwitchDeployReceiptState { None, Valid, Modified };

SwitchDeployInspection inspectSwitchDeploy(
    TaskFileInventory inventory, const std::string& targetRoot);

class SwitchDeployService {
public:
    SwitchDeployService(DownloadManager& manager, std::string appRoot,
                        std::string targetRoot);
    ~SwitchDeployService();

    SwitchDeployService(const SwitchDeployService&) = delete;
    SwitchDeployService& operator=(const SwitchDeployService&) = delete;

    SwitchDeployInspection inspect(const std::string& taskId) const;
    bool inventory(const std::string& taskId, TaskFileInventory& inventory,
                   std::string& error) const;
    bool start(const std::string& taskId, std::string& error,
               bool includeArchives = true);
    void cancel();
    void shutdown();
    SwitchDeploySnapshot snapshot() const;
    SwitchDeployReceiptState receiptState(const std::string& taskId) const;
    // Background scan for stream-install ports ready to copy. Never starts a
    // copy — takePendingDeployOffer() hands the inspected plan to the UI prompt.
    struct PendingOffer {
        std::string taskId;
        SwitchDeployInspection inspection;
    };
    void scheduleDeployOfferPoll();
    std::optional<PendingOffer> takePendingDeployOffer();
    void dismissDeployOffer(const std::string& taskId);

private:
    void pollDeployOffers();
    bool considerDeployOffer(const std::string& taskId);
    void run(DownloadManager::ExternalDeployLease lease,
             bool includeArchives);
    void finish(SwitchDeployPhase phase, SwitchDeployProblem problem,
                std::string detail);
    void cleanupInterruptedJob();

    DownloadManager& manager_;
    std::string appRoot_;
    std::string targetRoot_;
    mutable std::mutex mutex_;
    SwitchDeploySnapshot snapshot_;
    std::thread worker_;
    std::thread pollWorker_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> pollInFlight_{false};
    std::mutex offerMutex_;
    std::unordered_set<std::string> offerHandled_;
    std::optional<PendingOffer> pendingOffer_;
};

} // namespace pipensx
