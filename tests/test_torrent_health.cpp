#include "../src/app/download_manager.hpp"

#include <cassert>
#include <cstdio>

namespace {

using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::TorrentHealth;

void testNotActive() {
    DownloadTask t;
    t.status = DownloadStatus::Queued;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::NotActive);
    t.status = DownloadStatus::Completed;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::NotActive);
    t.status = DownloadStatus::Installing;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::NotActive);
}

void testPoor() {
    DownloadTask t;
    t.status = DownloadStatus::Downloading;
    t.speedBytesPerSecond = 0;
    t.peers = 0;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::Poor);
}

void testSlow() {
    DownloadTask t;
    t.status = DownloadStatus::Downloading;
    // Connected but no data yet.
    t.speedBytesPerSecond = 0;
    t.peers = 3;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::Slow);
    // Flowing but under the 512 KB/s threshold.
    t.speedBytesPerSecond = 256 * 1024;
    t.peers = 1;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::Slow);
}

void testExcellent() {
    DownloadTask t;
    t.status = DownloadStatus::Downloading;
    t.speedBytesPerSecond = 1024 * 1024;
    t.peers = 5;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::Excellent);
    t.speedBytesPerSecond = 512 * 1024;
    assert(pipensx::torrentHealth(t, 0) == TorrentHealth::Excellent);
}

} // namespace

int main() {
    testNotActive();
    testPoor();
    testSlow();
    testExcellent();
    std::puts("torrent health tests passed");
    return 0;
}
