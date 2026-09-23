#include "../src/app/task_actions.hpp"

#include <cassert>
#include <string>

namespace {

using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::TaskAction;
using pipensx::TaskActionReason;
using pipensx::TaskCapabilities;
using pipensx::TaskSource;
using pipensx::taskActionReasonName;
using pipensx::taskCapabilities;

void expect(const TaskAction& action, bool allowed, TaskActionReason reason) {
    assert(action.allowed == allowed);
    assert(action.reason == reason);
    const std::string name = taskActionReasonName(action.reason);
    if (allowed)
        assert(name.empty());
    else
        assert(!name.empty());
}

bool pausable(DownloadStatus status) {
    return status == DownloadStatus::Queued ||
           status == DownloadStatus::Checking ||
           status == DownloadStatus::Fetching ||
           status == DownloadStatus::Downloading ||
           status == DownloadStatus::Installing ||
           status == DownloadStatus::Verifying;
}

// One row per status, source, and copy-lease. Both UIs read this table, and
// the manager refuses a command with the same reason.
void testCapabilityTable() {
    const DownloadStatus statuses[] = {
        DownloadStatus::Queued,      DownloadStatus::Checking,
        DownloadStatus::Fetching,    DownloadStatus::Downloading,
        DownloadStatus::Paused,      DownloadStatus::Verifying,
        DownloadStatus::Completed,   DownloadStatus::Installing,
        DownloadStatus::Committing,  DownloadStatus::Installed,
        DownloadStatus::Error,       DownloadStatus::Removing,
    };
    const TaskSource sources[] = {TaskSource::Torrent, TaskSource::Debrid};
    for (DownloadStatus status : statuses) {
        for (TaskSource source : sources) {
            for (bool leased : {false, true}) {
                DownloadTask task;
                task.status = status;
                task.source = source;
                const TaskCapabilities caps = taskCapabilities(task, leased);
                if (leased) {
                    expect(caps.pause, false, TaskActionReason::Leased);
                    expect(caps.resume, false, TaskActionReason::Leased);
                    expect(caps.verify, false, TaskActionReason::Leased);
                    expect(caps.remove, false, TaskActionReason::Leased);
                    expect(caps.move, false, TaskActionReason::Leased);
                    continue;
                }

                if (status == DownloadStatus::Committing)
                    expect(caps.pause, false, TaskActionReason::Committing);
                else if (pausable(status))
                    expect(caps.pause, true, TaskActionReason::None);
                else
                    expect(caps.pause, false, TaskActionReason::NotPausable);

                if (status == DownloadStatus::Paused ||
                    status == DownloadStatus::Error)
                    expect(caps.resume, true, TaskActionReason::None);
                else
                    expect(caps.resume, false, TaskActionReason::NotResumable);

                if (status != DownloadStatus::Completed)
                    expect(caps.verify, false, TaskActionReason::NotCompleted);
                else if (source == TaskSource::Debrid)
                    expect(caps.verify, false, TaskActionReason::Debrid);
                else
                    expect(caps.verify, true, TaskActionReason::None);

                if (status == DownloadStatus::Removing)
                    expect(caps.remove, false, TaskActionReason::Removing);
                else
                    expect(caps.remove, true, TaskActionReason::None);

                if (status == DownloadStatus::Queued)
                    expect(caps.move, true, TaskActionReason::None);
                else
                    expect(caps.move, false, TaskActionReason::NotQueued);
            }
        }
    }
}

void testReasonNamesAreStable() {
    assert(std::string(taskActionReasonName(TaskActionReason::None)).empty());
    assert(std::string(taskActionReasonName(TaskActionReason::NotFound)) ==
           "not_found");
    assert(std::string(taskActionReasonName(TaskActionReason::Leased)) ==
           "leased");
    assert(std::string(taskActionReasonName(TaskActionReason::Committing)) ==
           "committing");
    assert(std::string(taskActionReasonName(TaskActionReason::NotPausable)) ==
           "not_pausable");
    assert(std::string(taskActionReasonName(TaskActionReason::NotResumable)) ==
           "not_resumable");
    assert(std::string(taskActionReasonName(TaskActionReason::NotCompleted)) ==
           "not_completed");
    assert(std::string(taskActionReasonName(TaskActionReason::Debrid)) ==
           "debrid");
    assert(std::string(taskActionReasonName(TaskActionReason::NotQueued)) ==
           "not_queued");
    assert(std::string(taskActionReasonName(TaskActionReason::Removing)) ==
           "removing");
}

}  // namespace

int main() {
    testCapabilityTable();
    testReasonNamesAreStable();
    return 0;
}
