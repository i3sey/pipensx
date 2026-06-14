#include "../src/app/download_manager.hpp"

extern "C" {
#include "../src/core/sha1.h"
}

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;

static std::string makeTorrent(const std::string& directory) {
    const std::string payload = "test payload";
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
         digest);

    std::string torrent = "d8:announce14:http://tracker4:infod6:lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e4:name8:test.bin12:piece lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e6:pieces20:";
    torrent.append(reinterpret_cast<const char*>(digest), 20);
    torrent += "ee";

    std::string path = directory + "/source.torrent";
    std::ofstream output(path, std::ios::binary);
    output.write(torrent.data(), static_cast<std::streamsize>(torrent.size()));
    output.close();
    return path;
}

int main() {
    char rootTemplate[] = "/tmp/pipensx-manager-XXXXXX";
    char* root = mkdtemp(rootTemplate);
    assert(root);
    std::string source = makeTorrent(root);
    std::string appRoot = std::string(root) + "/app";

    std::string taskId;
    std::string error;
    {
        DownloadManager manager(appRoot, false);
        pipensx::TorrentPreview preview;
        assert(DownloadManager::previewTorrent(source, preview, error));
        assert(preview.name == "test.bin");
        assert(preview.totalBytes == 12);
        assert(manager.importTorrent(source, taskId, error));
        assert(taskId.size() == 40);
        assert(!manager.importTorrent(source, taskId, error));

        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].status == DownloadStatus::Queued);
        assert(manager.pause(tasks[0].id));
        assert(manager.snapshot()[0].status == DownloadStatus::Paused);
        assert(manager.resume(tasks[0].id));
        assert(manager.snapshot()[0].status == DownloadStatus::Queued);
    }

    {
        DownloadManager manager(appRoot, false);
        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].id == taskId);
        assert(tasks[0].status == DownloadStatus::Queued);
        assert(manager.remove(taskId, true, error));
        assert(manager.snapshot().empty());
    }

    unlink(source.c_str());
    rmdir((appRoot + "/torrents").c_str());
    rmdir((appRoot + "/downloads").c_str());
    unlink((appRoot + "/queue.bencode").c_str());
    rmdir(appRoot.c_str());
    rmdir(root);
    std::puts("manager tests passed");
    return 0;
}
