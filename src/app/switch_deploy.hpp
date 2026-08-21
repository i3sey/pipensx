#pragma once

#include "download_manager.hpp"
#include "port_archive.hpp"
#include "task_files.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
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

// Copy-to-/switch is offered for a real port layout, including recoverable
// destination problems. LayoutNotFound is the normal NSP/NSZ case — not an error.
inline bool switchDeployOffersCopy(SwitchDeployProblem problem) {
    return problem == SwitchDeployProblem::None ||
           problem == SwitchDeployProblem::Conflict ||
           problem == SwitchDeployProblem::NoSpace ||
           problem == SwitchDeployProblem::NoRam;
}

// True when every deployable file already sits on the destination byte-for-byte
// identical and there is no archive left to extract — the port is fully
// installed and a copy offer would be a no-op.
inline bool switchDeployFullyInstalled(
    const SwitchDeployInspection& inspection) {
    return inspection.problem == SwitchDeployProblem::None &&
           !inspection.plan.files.empty() &&
           inspection.plan.identicalFiles == inspection.plan.files.size() &&
           inspection.plan.archives.empty();
}

inline bool switchDeployCopiesAtmosphere(const SwitchDeployPlan& plan) {
    for (const SwitchDeployEntry& entry : plan.files) {
        const std::string& path = entry.destinationRelativePath;
        if (path.size() < 11)
            continue;
        const char prefix[] = "atmosphere/";
        bool match = true;
        for (int i = 0; i < 11; ++i) {
            char c = path[static_cast<size_t>(i)];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != prefix[i]) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

inline const char* switchDeployCopyActionKey(const SwitchDeployPlan& plan) {
    return switchDeployCopiesAtmosphere(plan)
               ? "pipensx/deploy/install_mod"
               : "pipensx/deploy/install_port";
}

inline const char* switchDeployWarningKey(const SwitchDeployPlan& plan) {
    return switchDeployCopiesAtmosphere(plan)
               ? "pipensx/deploy/warning_mod"
               : "pipensx/deploy/warning";
}

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
    // Background scan for ports ready to copy. Stream-install tasks without
    // an auto-copy marker become a UI prompt. Armed one-tap ports set
    // autoStart so the UI can start the copy without a second question.
    struct PendingOffer {
        std::string taskId;
        SwitchDeployInspection inspection;
        bool autoStart = false;
    };
    bool armAutoCopy(const std::string& taskId);
    void clearAutoCopy(const std::string& taskId);
    bool autoCopyArmed(const std::string& taskId) const;
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

// What removing one installed port touches, computed up front so the
// confirmation dialog can show the breakdown. One title maps to one or more
// infohashes (task ids); every hash with a deployment receipt is cleaned.
struct PortUninstallPlan {
    std::string titleId;
    std::vector<std::string> taskIds;
    // Exact mode: destination-relative paths under /switch to delete
    // (receipt copies plus unpacked archive members).
    std::vector<std::string> switchFiles;
    uint64_t switchBytes = 0;
    // Folder mode: a v1 receipt whose archive is gone from the task data, so
    // the unpacked list cannot be rebuilt — these top-level folders under
    // /switch are removed entirely instead.
    std::vector<std::string> wholeFolders;
    bool hasTask = false;
    bool taskHasData = false;
};

struct PortUninstallReport {
    bool shortcutRemoved = false;
    bool filesDeleted = false;
    bool tasksRemoved = false;
    size_t filesRemoved = 0;
    size_t filesMissing = 0;
    size_t filesFailed = 0;
    std::string error;

    bool complete() const {
        return shortcutRemoved && filesDeleted && tasksRemoved;
    }
};

// Stateless helper for full port removal from "My Games": deployed files →
// download task + data → the ncm shortcut (injected, keeps this service free
// of InstalledTitleService so it builds on PC) → receipts and auto-copy
// markers. Best-effort: already-missing files are fine, and the receipts
// survive a failed run so tapping Uninstall again is safe.
class PortUninstallService {
public:
    PortUninstallService(DownloadManager& manager, std::string appRoot,
                         std::string targetRoot);

    bool receiptExists(const std::string& taskId) const;

    // Builds the removal breakdown for the dialog. Receipts are matched to
    // the title by the recorded title ids (v3) or the forwarder package name
    // in the task manifest (older receipts); `taskIds` (metadata-index
    // infohashes) is only a fallback for ordinary NSP titles. False when no
    // matching receipt could be read (caller falls back to a plain shortcut
    // uninstall, leaving the deployment untouched).
    bool plan(const std::string& titleId,
              const std::vector<std::string>& taskIds,
              PortUninstallPlan& plan) const;

    // Runs the removal pipeline synchronously — call from a worker thread.
    // uninstallShortcut(error) removes the ncm shortcut (existing installed
    // title uninstall); false and a non-empty error marks the run failed.
    // Returns report.complete(); on full success the receipts and auto-copy
    // markers are deleted.
    bool uninstallPort(
        const PortUninstallPlan& plan,
        const std::function<bool(std::string&)>& uninstallShortcut,
        PortUninstallReport& report) const;

private:
    bool deleteDeployed(const PortUninstallPlan& plan,
                        PortUninstallReport& report) const;
    bool removeTasks(const PortUninstallPlan& plan,
                     PortUninstallReport& report) const;

    DownloadManager& manager_;
    std::string appRoot_;
    std::string targetRoot_;
};

} // namespace pipensx
