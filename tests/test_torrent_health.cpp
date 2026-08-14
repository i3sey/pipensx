#include "../src/app/download_manager.hpp"

#include <cassert>
#include <cstdio>

namespace {

using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::TaskSource;
using pipensx::TorrentHealth;
using pipensx::kProgressRateStaleMs;

DownloadTask downloading() {
    DownloadTask t;
    t.status = DownloadStatus::Downloading;
    t.downloadProgressUpdatedAtMs = 1000;
    return t;
}

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
    DownloadTask t = downloading();
    t.speedBytesPerSecond = 0;
    t.peers = 0;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Poor);
}

void testSlow() {
    DownloadTask t = downloading();
    t.speedBytesPerSecond = 0;
    t.peers = 3;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Slow);
    t.speedBytesPerSecond = 256 * 1024;
    t.peers = 1;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Slow);
}

void testExcellent() {
    DownloadTask t = downloading();
    t.speedBytesPerSecond = 1024 * 1024;
    t.peers = 5;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Excellent);
    t.speedBytesPerSecond = 512 * 1024;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Excellent);
}

void testStaleTorrent() {
    DownloadTask t = downloading();
    t.speedBytesPerSecond = 1024 * 1024;
    t.peers = 5;
    assert(pipensx::torrentHealth(t, 1000 + kProgressRateStaleMs + 1) ==
           TorrentHealth::Slow);
    t.peers = 0;
    assert(pipensx::torrentHealth(t, 1000 + kProgressRateStaleMs + 1) ==
           TorrentHealth::Poor);
}

void testDebrid() {
    DownloadTask t = downloading();
    t.source = TaskSource::Debrid;
    t.peers = 0;
    t.speedBytesPerSecond = 1024 * 1024;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Excellent);
    t.speedBytesPerSecond = 256 * 1024;
    assert(pipensx::torrentHealth(t, 1000) == TorrentHealth::Slow);
    assert(pipensx::torrentHealth(t, 1000 + kProgressRateStaleMs + 1) ==
           TorrentHealth::Poor);
}

} // namespace

int main() {
    testNotActive();
    testPoor();
    testSlow();
    testExcellent();
    testStaleTorrent();
    testDebrid();
    std::puts("torrent health tests passed");
    return 0;
}
