#include "../src/app/download_manager.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "../src/core/sha1.h"
}

namespace {

std::string tempRoot() {
    return "/tmp/pipensx-queue-" +
           std::to_string(static_cast<long long>(getpid()));
}

std::string makeTorrent(const std::string& directory, const std::string& name,
                        const std::string& payload) {
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
         digest);
    std::string torrent = "d8:announce14:http://tracker4:infod6:lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e4:name";
    torrent += std::to_string(name.size()) + ":" + name;
    torrent += "12:piece lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e6:pieces20:";
    torrent.append(reinterpret_cast<const char*>(digest), 20);
    torrent += "ee";
    std::string path = directory + "/" + name + ".torrent";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(torrent.data(), static_cast<std::streamsize>(torrent.size()));
    return path;
}

using pipensx::DownloadStatus;
using pipensx::DownloadTask;

DownloadTask task(DownloadStatus status) {
    DownloadTask t;
    t.id = "id-" + std::to_string(static_cast<int>(status));
    t.status = status;
    return t;
}

void testSummarizeQueueCounts() {
    std::vector<DownloadTask> tasks;
    tasks.push_back(task(DownloadStatus::Downloading));
    tasks.push_back(task(DownloadStatus::Downloading));
    tasks.push_back(task(DownloadStatus::Installing));
    tasks.push_back(task(DownloadStatus::Queued));
    tasks.push_back(task(DownloadStatus::Queued));
    tasks.push_back(task(DownloadStatus::Queued));
    tasks.push_back(task(DownloadStatus::Queued));
    tasks.push_back(task(DownloadStatus::Paused));
    tasks.push_back(task(DownloadStatus::Completed));
    tasks.push_back(task(DownloadStatus::Error));
    const pipensx::QueueSummary s = pipensx::summarizeQueue(tasks, 0);
    assert(s.downloading == 2);
    assert(s.installing == 1);
    assert(s.queued == 4);
    assert(s.paused == 1);
    assert(s.completed == 1);
    assert(s.errors == 1);
}

void testSummarizeQueueSpeedAndEta() {
    std::vector<DownloadTask> tasks;
    DownloadTask a = task(DownloadStatus::Downloading);
    a.totalBytes = 1000;
    a.completedBytes = 400; // remaining 600
    a.speedBytesPerSecond = 100;
    tasks.push_back(a);
    DownloadTask b = task(DownloadStatus::Downloading);
    b.totalBytes = 500;
    b.completedBytes = 100; // remaining 400
    b.speedBytesPerSecond = 200;
    tasks.push_back(b);
    DownloadTask q = task(DownloadStatus::Queued);
    q.totalBytes = 300; // remaining 300
    tasks.push_back(q);
    const pipensx::QueueSummary s = pipensx::summarizeQueue(tasks, 0);
    assert(s.downloadSpeedBps == 300);
    assert(s.installSpeedBps == 0);
    assert(s.totalRemainingBytes == 600 + 400 + 300);
    assert(s.etaSeconds == 1300 / 300); // 4 (integer division)
}

void testSummarizeQueueNoThroughputNoEta() {
    std::vector<DownloadTask> tasks;
    tasks.push_back(task(DownloadStatus::Queued));
    const pipensx::QueueSummary s = pipensx::summarizeQueue(tasks, 0);
    assert(s.queued == 1);
    assert(s.etaSeconds == 0);
}

void testMoveTask() {
    const std::string root = tempRoot();
    mkdir(root.c_str(), 0755);
    const std::string source =
        makeTorrent(root, "a.bin", "aaaa");
    const std::string source2 =
        makeTorrent(root, "b.bin", "bbbb");
    const std::string source3 =
        makeTorrent(root, "c.bin", "cccc");
    std::string queueRoot = root + "/queue";
    {
        pipensx::DownloadManager manager(queueRoot, false);
        std::string error;
        std::string first, second, third;
        assert(manager.importTorrent(source, pipensx::TransferMode::DownloadOnly,
                                     first, error));
        assert(manager.importTorrent(source2, pipensx::TransferMode::DownloadOnly,
                                     second, error));
        assert(manager.importTorrent(source3, pipensx::TransferMode::DownloadOnly,
                                     third, error));
        auto tasks = manager.snapshot();
        assert(tasks.size() == 3);

        // Move the last task up one: order becomes first, third, second.
        assert(manager.moveTask(third, true, error));
        tasks = manager.snapshot();
        assert(tasks[0].id == first && tasks[1].id == third &&
               tasks[2].id == second);

        // Move the first task down one: order becomes third, first, second.
        assert(manager.moveTask(first, false, error));
        tasks = manager.snapshot();
        assert(tasks[0].id == third && tasks[1].id == first &&
               tasks[2].id == second);

        // Move past the top is a no-op success.
        assert(manager.moveTask(third, true, error));
        assert(manager.snapshot()[0].id == third);

        // Unknown id fails.
        error.clear();
        assert(!manager.moveTask("nope", true, error));
        assert(!error.empty());

        // Pausing removes the task from the queue; it can no longer move.
        assert(manager.pause(third));
        error.clear();
        assert(!manager.moveTask(third, true, error));
        assert(!error.empty());
    }
    unlink(source.c_str());
    unlink(source2.c_str());
    unlink(source3.c_str());
    // Clean the queue app directory tree.
    unlink((queueRoot + "/queue.bencode").c_str());
    rmdir((queueRoot + "/torrents").c_str());
    rmdir((queueRoot + "/downloads").c_str());
    rmdir(queueRoot.c_str());
    rmdir(root.c_str());
}

} // namespace

int main() {
    testSummarizeQueueCounts();
    testSummarizeQueueSpeedAndEta();
    testSummarizeQueueNoThroughputNoEta();
    testMoveTask();
    std::puts("queue controls tests passed");
    return 0;
}
