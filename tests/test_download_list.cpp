#include "../src/app/download_list_refresh.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

using pipensx::DownloadListUpdate;
using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::downloadListUpdate;

DownloadTask fetching(double progress) {
    DownloadTask task;
    task.id = "fetch";
    task.name = "Game";
    task.status = DownloadStatus::Fetching;
    task.fetchProgress = progress;
    return task;
}

DownloadListUpdate listed(const std::vector<DownloadTask>& previous,
                          const std::vector<DownloadTask>& next) {
    return downloadListUpdate(previous, next, 0, 0, "", "");
}

void testFetchingProgressRepaintsInPlace() {
    std::vector<DownloadTask> previous{fetching(0.10)};
    for (double progress : {0.40, 0.90}) {
        const std::vector<DownloadTask> next{fetching(progress)};
        assert(listed(previous, next) == DownloadListUpdate::Repaint);
        previous = next;
    }
    assert(listed(previous, previous) == DownloadListUpdate::None);
}

void testLaterRowStillRepaints() {
    DownloadTask quiet = fetching(0.10);
    quiet.id = "quiet";
    DownloadTask moving = fetching(0.10);
    moving.id = "moving";
    std::vector<DownloadTask> previous{quiet, moving};
    moving.fetchProgress = 0.40;
    assert(listed(previous, {quiet, moving}) == DownloadListUpdate::Repaint);
}

void testErrorSizeAndPackageCountRepaint() {
    DownloadTask task;
    task.id = "row";
    task.status = DownloadStatus::Error;
    task.error = "timeout";
    task.completedBytes = 10;
    task.totalBytes = 100;
    task.packagesInstalled = 0;
    task.packageCount = 1;
    std::vector<DownloadTask> previous{task};

    DownloadTask errorChanged = task;
    errorChanged.error = "no space";
    assert(listed(previous, {errorChanged}) == DownloadListUpdate::Repaint);

    DownloadTask totalChanged = task;
    totalChanged.totalBytes = 200;
    assert(listed(previous, {totalChanged}) == DownloadListUpdate::Repaint);

    DownloadTask wantedChanged = task;
    wantedChanged.wantedTotalBytes = 80;
    wantedChanged.wantedCompletedBytes = 40;
    assert(listed(previous, {wantedChanged}) == DownloadListUpdate::Repaint);

    DownloadTask packagesChanged = task;
    packagesChanged.packageCount = 3;
    assert(listed(previous, {packagesChanged}) == DownloadListUpdate::Repaint);

    DownloadTask installSizeChanged = task;
    installSizeChanged.installTotalBytes = 50;
    assert(listed(previous, {installSizeChanged}) ==
           DownloadListUpdate::Repaint);
}

void testCheckPercentAndTitleRepaint() {
    DownloadTask task;
    task.id = "check";
    task.status = DownloadStatus::Checking;
    task.piecesDone = 1;
    task.piecesTotal = 10;
    std::vector<DownloadTask> previous{task};

    DownloadTask piecesChanged = task;
    piecesChanged.piecesDone = 4;
    assert(listed(previous, {piecesChanged}) == DownloadListUpdate::Repaint);

    DownloadTask renamed = task;
    renamed.name = "Other";
    assert(listed(previous, {renamed}) == DownloadListUpdate::Repaint);
}

void testStatusOrIdentityReloads() {
    DownloadTask task = fetching(0.40);
    DownloadTask resumed = task;
    resumed.status = DownloadStatus::Downloading;
    assert(listed({task}, {resumed}) == DownloadListUpdate::Reload);

    DownloadTask other = task;
    other.id = "other";
    assert(listed({task}, {other}) == DownloadListUpdate::Reload);
    assert(listed({task}, {}) == DownloadListUpdate::Reload);

    // Progress on an earlier row must not hide a later status change: that
    // reshuffles sections and has to reload, not repaint in place.
    DownloadTask later = fetching(0.10);
    later.id = "later";
    DownloadTask laterMoved = later;
    laterMoved.status = DownloadStatus::Downloading;
    assert(listed({fetching(0.10), later}, {fetching(0.40), laterMoved}) ==
           DownloadListUpdate::Reload);
}

void testDeployGenerationRepaintsAndActiveTaskReloads() {
    const std::vector<DownloadTask> tasks{fetching(0.40)};
    assert(downloadListUpdate(tasks, tasks, 1, 2, "", "") ==
           DownloadListUpdate::Repaint);
    assert(downloadListUpdate(tasks, tasks, 1, 1, "", "fetch") ==
           DownloadListUpdate::Reload);
    assert(downloadListUpdate(tasks, tasks, 1, 1, "fetch", "fetch") ==
           DownloadListUpdate::None);
}

}  // namespace

int main() {
    testFetchingProgressRepaintsInPlace();
    testLaterRowStillRepaints();
    testErrorSizeAndPackageCountRepaint();
    testCheckPercentAndTitleRepaint();
    testStatusOrIdentityReloads();
    testDeployGenerationRepaintsAndActiveTaskReloads();
    return 0;
}
