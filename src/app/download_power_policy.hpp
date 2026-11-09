#pragma once

#include <algorithm>
#include <unordered_set>
#include "download_manager.hpp"

namespace pipensx {

inline bool downloadPowerActive(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Queued:
        case DownloadStatus::Checking:
        case DownloadStatus::Fetching:
        case DownloadStatus::Downloading:
        case DownloadStatus::Verifying:
        case DownloadStatus::Installing:
        case DownloadStatus::Committing: return true;
        default: return false;
    }
}

class QueueSleepPolicy {
public:
    bool update(const std::vector<DownloadPowerTask>& tasks, bool enabled,
                bool externalActive) {
        std::unordered_set<std::string> current;
        for (const auto& task : tasks) {
            current.insert(task.id);
            // Also catch tiny downloads that finish between two UI polls.
            if (enabled && (downloadPowerActive(task.status) ||
                (initialized_ && !known_.count(task.id) &&
                 (task.status == DownloadStatus::Completed ||
                  task.status == DownloadStatus::Installed))))
                pending_.insert(task.id);
        }
        known_ = std::move(current);
        initialized_ = true;
        if (!enabled) {
            pending_.clear();
            return false;
        }
        if (pending_.empty()) return false;
        bool allDone = true;
        bool removed = false;
        for (const auto& id : pending_) {
            auto found = std::find_if(tasks.begin(), tasks.end(),
                [&](const auto& task) { return task.id == id; });
            if (found == tasks.end() || found->status == DownloadStatus::Removing) {
                removed = true;
                break;
            }
            if (found->status != DownloadStatus::Completed &&
                found->status != DownloadStatus::Installed) allDone = false;
        }
        // Deleting work is not a successful queue completion.
        if (removed) { pending_.clear(); return false; }
        if (!allDone || externalActive) return false;
        for (const auto& task : tasks)
            if (task.status != DownloadStatus::Completed &&
                task.status != DownloadStatus::Installed) return false;
        pending_.clear();
        return true;
    }
private:
    std::unordered_set<std::string> pending_;
    std::unordered_set<std::string> known_;
    bool initialized_ = false;
};

} // namespace pipensx
