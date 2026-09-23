#pragma once

#include "download_manager.hpp"

#include <string>
#include <vector>

namespace pipensx {

// Fields the downloads row paints. Id and status are excluded: those change
// section membership, so the list reloads instead of repainting. Progress
// timestamps are excluded too — ETA is derived from bytes, speed, and the
// clock, and a timestamp tick by itself is not a visible change.
struct DownloadRowPaint {
    std::string name;
    std::string error;
    std::string currentPackage;
    TransferMode mode = TransferMode::DownloadOnly;
    TaskSource source = TaskSource::Torrent;
    double fetchProgress = 0.0;
    uint64_t totalBytes = 0;
    uint64_t completedBytes = 0;
    uint64_t wantedTotalBytes = 0;
    uint64_t wantedCompletedBytes = 0;
    uint64_t speedBytesPerSecond = 0;
    uint32_t peers = 0;
    uint32_t piecesDone = 0;
    uint32_t piecesTotal = 0;
    uint32_t packageCount = 0;
    uint32_t packagesInstalled = 0;
    uint64_t installedBytes = 0;
    uint64_t installTotalBytes = 0;
    uint64_t installSpeedBytesPerSecond = 0;

    static DownloadRowPaint from(const DownloadTask& task) {
        DownloadRowPaint paint;
        paint.name = task.name;
        paint.error = task.error;
        paint.currentPackage = task.currentPackage;
        paint.mode = task.mode;
        paint.source = task.source;
        paint.fetchProgress = task.fetchProgress;
        paint.totalBytes = task.totalBytes;
        paint.completedBytes = task.completedBytes;
        paint.wantedTotalBytes = task.wantedTotalBytes;
        paint.wantedCompletedBytes = task.wantedCompletedBytes;
        paint.speedBytesPerSecond = task.speedBytesPerSecond;
        paint.peers = task.peers;
        paint.piecesDone = task.piecesDone;
        paint.piecesTotal = task.piecesTotal;
        paint.packageCount = task.packageCount;
        paint.packagesInstalled = task.packagesInstalled;
        paint.installedBytes = task.installedBytes;
        paint.installTotalBytes = task.installTotalBytes;
        paint.installSpeedBytesPerSecond = task.installSpeedBytesPerSecond;
        return paint;
    }

    bool operator==(const DownloadRowPaint& other) const {
        return name == other.name && error == other.error &&
               currentPackage == other.currentPackage && mode == other.mode &&
               source == other.source && fetchProgress == other.fetchProgress &&
               totalBytes == other.totalBytes &&
               completedBytes == other.completedBytes &&
               wantedTotalBytes == other.wantedTotalBytes &&
               wantedCompletedBytes == other.wantedCompletedBytes &&
               speedBytesPerSecond == other.speedBytesPerSecond &&
               peers == other.peers && piecesDone == other.piecesDone &&
               piecesTotal == other.piecesTotal &&
               packageCount == other.packageCount &&
               packagesInstalled == other.packagesInstalled &&
               installedBytes == other.installedBytes &&
               installTotalBytes == other.installTotalBytes &&
               installSpeedBytesPerSecond == other.installSpeedBytesPerSecond;
    }

    bool operator!=(const DownloadRowPaint& other) const {
        return !(*this == other);
    }
};

inline bool downloadRowProgressChanged(const DownloadTask& previous,
                                       const DownloadTask& next) {
    return DownloadRowPaint::from(previous) != DownloadRowPaint::from(next);
}

// What MainView::refresh does with a new snapshot. Repaint keeps the same
// rows (no reloadData), so focus and scroll stay put.
enum class DownloadListUpdate {
    None,
    Repaint,
    Reload,
};

inline DownloadListUpdate downloadListUpdate(
    const std::vector<DownloadTask>& previous,
    const std::vector<DownloadTask>& next,
    uint64_t previousDeployGeneration, uint64_t nextDeployGeneration,
    const std::string& previousActiveDeploy,
    const std::string& nextActiveDeploy) {
    if (previous.size() != next.size() ||
        previousActiveDeploy != nextActiveDeploy)
        return DownloadListUpdate::Reload;
    // Scan every row. Stopping at the first progress delta used to hide a
    // later status change, and the list then jumped under the cursor.
    bool repaint = previousDeployGeneration != nextDeployGeneration;
    for (size_t i = 0; i < next.size(); ++i) {
        if (previous[i].id != next[i].id ||
            previous[i].status != next[i].status)
            return DownloadListUpdate::Reload;
        if (downloadRowProgressChanged(previous[i], next[i]))
            repaint = true;
    }
    return repaint ? DownloadListUpdate::Repaint : DownloadListUpdate::None;
}

}  // namespace pipensx
