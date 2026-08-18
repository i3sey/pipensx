// Progress-fraction semantics for DownloadTask, including the stream-install
// bar. Regression: a single-package port whose forwarder NSP commits early
// must not show 100% while the bulk of the data is still downloading.

#include "app/download_manager.hpp"

#include <cassert>
#include <cmath>

using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::TransferMode;
using pipensx::streamInstallProgressOf;

static void approx(float got, float want) {
    assert(std::fabs(got - want) < 0.001f);
}

// Set the download axis to wantPercent of a 100-byte selection.
static DownloadTask portTask(float wantPercent) {
    DownloadTask task;
    task.mode = TransferMode::StreamInstall;
    task.packageCount = 1;
    task.wantedTotalBytes = 100;
    task.wantedCompletedBytes =
        static_cast<uint64_t>(wantPercent * 100.0f);
    task.totalBytes = 100;
    task.completedBytes = task.wantedCompletedBytes;
    return task;
}

int main() {
    // Forwarder already committed, data still downloading: the bar tracks the
    // download, not the installed package.
    {
        DownloadTask task = portTask(0.8f);
        task.packagesInstalled = 1;
        task.status = DownloadStatus::Downloading;
        approx(streamInstallProgressOf(task), 0.8f);
    }
    // Fresh port: forwarder commits almost immediately, so the bar must stay
    // at the download fraction for the rest of the transfer.
    {
        DownloadTask task = portTask(0.05f);
        task.packagesInstalled = 1;
        task.status = DownloadStatus::Downloading;
        approx(streamInstallProgressOf(task), 0.05f);
    }
    // While the package is installing, the bar is the max of the download and
    // package axes — it must not drop below the download fraction.
    {
        DownloadTask task = portTask(0.8f);
        task.status = DownloadStatus::Installing;
        task.installTotalBytes = 100;
        task.installedBytes = 50;
        approx(streamInstallProgressOf(task), 0.8f);

        task.wantedCompletedBytes = 30;
        task.completedBytes = 30;
        approx(streamInstallProgressOf(task), 0.5f);
    }
    // Multi-package: package axis only counts while installing/committing.
    {
        DownloadTask task = portTask(0.8f);
        task.packageCount = 5;
        task.packagesInstalled = 4;
        task.status = DownloadStatus::Downloading;
        approx(streamInstallProgressOf(task), 0.8f);
    }
    // Download-only tasks ignore the package axis entirely.
    {
        DownloadTask task = portTask(0.8f);
        task.mode = TransferMode::DownloadOnly;
        task.packagesInstalled = 1;
        task.status = DownloadStatus::Downloading;
        approx(streamInstallProgressOf(task), 0.8f);
    }
    // Fully downloaded still reads 100%.
    {
        DownloadTask task = portTask(1.0f);
        task.packagesInstalled = 1;
        task.status = DownloadStatus::Downloading;
        approx(streamInstallProgressOf(task), 1.0f);
    }
    return 0;
}
