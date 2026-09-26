#pragma once

#include "download_manager.hpp"

namespace pipensx {

// Why a downloads command is refused. The names are the web API contract:
// a 409 body is {"error":"<name>"} and the task JSON repeats the same name.
enum class TaskActionReason {
    None,
    NotFound,
    Leased,
    Committing,
    NotPausable,
    NotResumable,
    NotCompleted,
    Debrid,
    NotQueued,
    Removing,
};

struct TaskAction {
    bool allowed = false;
    TaskActionReason reason = TaskActionReason::None;
};

struct TaskCapabilities {
    TaskAction pause;
    TaskAction resume;
    TaskAction verify;
    TaskAction remove;
    TaskAction move;
};

inline const char* taskActionReasonName(TaskActionReason reason) {
    switch (reason) {
    case TaskActionReason::None: return "";
    case TaskActionReason::NotFound: return "not_found";
    case TaskActionReason::Leased: return "leased";
    case TaskActionReason::Committing: return "committing";
    case TaskActionReason::NotPausable: return "not_pausable";
    case TaskActionReason::NotResumable: return "not_resumable";
    case TaskActionReason::NotCompleted: return "not_completed";
    case TaskActionReason::Debrid: return "debrid";
    case TaskActionReason::NotQueued: return "not_queued";
    case TaskActionReason::Removing: return "removing";
    }
    return "";
}

// What both downloads UIs may offer. DownloadManager checks the same rules
// again when the command arrives: the task can move on between the snapshot
// and the click.
//
// Committing is not pausable. That phase is a NAND commit already in flight;
// pause-all already left it alone, and a single pause must not either.
// Verify is refused for debrid even after Completed: there are no local
// pieces to rehash, and requeueing would download the file again.
inline TaskCapabilities taskCapabilities(const DownloadTask& task, bool leased) {
    TaskCapabilities caps;
    if (leased) {
        const TaskAction blocked{false, TaskActionReason::Leased};
        caps.pause = caps.resume = caps.verify = caps.remove = caps.move =
            blocked;
        return caps;
    }

    switch (task.status) {
    case DownloadStatus::Queued:
    case DownloadStatus::Checking:
    case DownloadStatus::Fetching:
    case DownloadStatus::Downloading:
    case DownloadStatus::Installing:
    case DownloadStatus::Verifying:
        caps.pause = {true, TaskActionReason::None};
        break;
    case DownloadStatus::Committing:
        caps.pause = {false, TaskActionReason::Committing};
        break;
    case DownloadStatus::Paused:
    case DownloadStatus::Completed:
    case DownloadStatus::Installed:
    case DownloadStatus::Error:
    case DownloadStatus::Removing:
        caps.pause = {false, TaskActionReason::NotPausable};
        break;
    }

    if (task.status == DownloadStatus::Paused ||
        task.status == DownloadStatus::Error)
        caps.resume = {true, TaskActionReason::None};
    else
        caps.resume = {false, TaskActionReason::NotResumable};

    if (task.status != DownloadStatus::Completed)
        caps.verify = {false, TaskActionReason::NotCompleted};
    else if (task.source == TaskSource::Debrid)
        caps.verify = {false, TaskActionReason::Debrid};
    else
        caps.verify = {true, TaskActionReason::None};

    if (task.status == DownloadStatus::Removing)
        caps.remove = {false, TaskActionReason::Removing};
    else
        caps.remove = {true, TaskActionReason::None};

    if (task.status == DownloadStatus::Queued)
        caps.move = {true, TaskActionReason::None};
    else
        caps.move = {false, TaskActionReason::NotQueued};
    return caps;
}

}  // namespace pipensx
