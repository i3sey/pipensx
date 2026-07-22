#include "../src/app/download_manager.hpp"

extern "C" {
#include "../src/core/sha1.h"
}

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;
using pipensx::FileAction;
using pipensx::TransferMode;

static std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

static std::string makeTorrent(const std::string& directory,
                               const std::string& name,
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
    std::ofstream output(path, std::ios::binary);
    output.write(torrent.data(), static_cast<std::streamsize>(torrent.size()));
    output.close();
    return path;
}

static std::string makeSelectiveTorrent(const std::string& directory) {
    const std::string payload = "aaaabbbbcccc";
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
         digest);

    std::string torrent = "d8:announce18:http://127.0.0.1:14:infod5:filesl";
    for (const char* name : {"unselected-a.bin", "selected.7z",
                             "unselected-b.bin"}) {
        torrent += "d6:lengthi4e4:pathl" + bstr(name) + "ee";
    }
    torrent += "e4:name9:selection12:piece lengthi12e6:pieces20:";
    torrent.append(reinterpret_cast<const char*>(digest), 20);
    torrent += "ee";

    std::string path = directory + "/selective.torrent";
    std::ofstream output(path, std::ios::binary);
    output.write(torrent.data(), static_cast<std::streamsize>(torrent.size()));
    output.close();
    return path;
}

static void copyFile(const std::string& source, const std::string& destination) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary);
    output << input.rdbuf();
}

int main() {
    char rootTemplate[] = "/tmp/pipensx-manager-XXXXXX";
    char* root = mkdtemp(rootTemplate);
    assert(root);
    std::string source = makeTorrent(root, "package.nsp", "test payload");
    std::string downloadOnlySource =
        makeTorrent(root, "download-only.nsp", "download payload");
    std::string readmeSource =
        makeTorrent(root, "readme.txt", "readme payload");
    std::string appRoot = std::string(root) + "/app";
    std::string actionsRoot = std::string(root) + "/actions-app";
    std::string invalidRoot = std::string(root) + "/invalid-app";
    std::string legacyRoot = std::string(root) + "/legacy-app";
    std::string activeRoot = std::string(root) + "/active-app";
    std::string queueRoot = std::string(root) + "/queue-app";
    std::string selectiveSource = makeSelectiveTorrent(root);

    std::string taskId;
    std::string error;
    {
        DownloadManager manager(appRoot, false);
        assert(!manager.hasActiveTransfer());
        pipensx::TorrentPreview preview;
        assert(DownloadManager::previewTorrent(source, preview, error));
        assert(preview.name == "package.nsp");
        assert(preview.totalBytes == 12);
        assert(preview.packageCount == 1);
        assert(manager.importTorrent(
            source, TransferMode::StreamInstall, taskId, error));
        assert(taskId.size() == 40);
        assert(!manager.importTorrent(source, taskId, error));

        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].status == DownloadStatus::Queued);
        assert(tasks[0].mode == TransferMode::StreamInstall);
        assert(tasks[0].packageCount == 1);
        assert(manager.hasActiveTransfer());
        assert(manager.pause(tasks[0].id));
        assert(manager.snapshot()[0].status == DownloadStatus::Paused);
        assert(!manager.hasActiveTransfer());
        assert(manager.resume(tasks[0].id));
        assert(manager.snapshot()[0].status == DownloadStatus::Queued);
        assert(manager.hasActiveTransfer());
    }

    {
        DownloadManager manager(actionsRoot, false);
        std::vector<uint8_t> actions{
            static_cast<uint8_t>(FileAction::Download),
        };
        std::string downloadTaskId;
        assert(manager.importTorrentActions(
            downloadOnlySource, actions, downloadTaskId, error));
        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].mode == TransferMode::DownloadOnly);
        assert(tasks[0].packageCount == 0);
        assert((tasks[0].fileSelection == actions));
        assert(manager.remove(downloadTaskId, true, error));
    }

    {
        DownloadManager manager(invalidRoot, false);
        std::vector<uint8_t> actions{
            static_cast<uint8_t>(FileAction::Install),
        };
        std::string ignoredTaskId;
        assert(!manager.importTorrentActions(
            readmeSource, actions, ignoredTaskId, error));
        assert(error == "Only NSP/NSZ package files can be installed.");
        assert(manager.snapshot().empty());
    }

    {
        DownloadManager manager(activeRoot, true);
        std::vector<uint8_t> actions{
            static_cast<uint8_t>(FileAction::Skip),
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Skip),
        };
        std::string selectiveTaskId;
        assert(manager.importTorrentActions(
            selectiveSource, actions, selectiveTaskId, error));
        const std::string dataPath = manager.snapshot()[0].dataPath + "/selection";
        const std::string selected = dataPath + "/selected.7z";
        for (int i = 0; i < 500 && access(selected.c_str(), F_OK) != 0; ++i)
            usleep(10000);
        assert(access(selected.c_str(), F_OK) == 0);
        assert(access((dataPath + "/unselected-a.bin").c_str(), F_OK) != 0);
        assert(access((dataPath + "/unselected-b.bin").c_str(), F_OK) != 0);
        manager.shutdown();
        assert(manager.remove(selectiveTaskId, true, error));
    }

    {
        {
            DownloadManager createDirs(legacyRoot, false);
        }
        pipensx::TorrentPreview preview;
        assert(DownloadManager::previewTorrent(source, preview, error));
        std::string metainfoPath =
            legacyRoot + "/torrents/" + preview.infoHash + ".torrent";
        std::string dataPath = legacyRoot + "/downloads/package.nsp-" +
                               preview.infoHash.substr(0, 8);
        copyFile(source, metainfoPath);

        std::string legacySelection(1, '\1');
        std::string queue = "d5:tasksl";
        queue += "d";
        queue += "4:data" + bstr(dataPath);
        queue += "5:error" + bstr("");
        queue += "2:id" + bstr(preview.infoHash);
        queue += "8:metainfo" + bstr(metainfoPath);
        queue += "4:mode" + bstr("install");
        queue += "4:name" + bstr(preview.name);
        queue += "13:package-counti1e";
        queue += "13:packages-donei0e";
        queue += "9:selection" + bstr(legacySelection);
        queue += "6:status" + bstr("queued");
        queue += "5:totali12e";
        queue += "e";
        queue += "e7:versioni3ee";
        std::ofstream output(legacyRoot + "/queue.bencode",
                             std::ios::binary | std::ios::trunc);
        output << queue;
        output.close();

        DownloadManager manager(legacyRoot, false);
        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].mode == TransferMode::StreamInstall);
        assert((tasks[0].fileSelection == std::vector<uint8_t>{
            static_cast<uint8_t>(FileAction::Install),
        }));
        assert(manager.remove(tasks[0].id, true, error));
    }

    {
        DownloadManager manager(appRoot, false);
        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].id == taskId);
        assert(tasks[0].status == DownloadStatus::Queued);
        assert(tasks[0].mode == TransferMode::StreamInstall);
        assert(tasks[0].packageCount == 1);
        assert(manager.remove(taskId, true, error));
        assert(manager.snapshot().empty());
    }

    // moveToFront: the worker claims the first Queued entry in list order, so
    // promoting a task is a reorder of tasks_, not a priority flag.
    {
        DownloadManager manager(queueRoot, false);
        std::string first, second, third;
        assert(manager.importTorrent(source, TransferMode::DownloadOnly, first,
                                     error));
        assert(manager.importTorrent(downloadOnlySource,
                                     TransferMode::DownloadOnly, second,
                                     error));
        assert(manager.importTorrent(readmeSource, TransferMode::DownloadOnly,
                                     third, error));
        auto tasks = manager.snapshot();
        assert(tasks.size() == 3);
        assert(tasks[0].id == first && tasks[2].id == third);

        // Last to front, and the two it jumped keep their relative order.
        assert(manager.moveToFront(third, error));
        tasks = manager.snapshot();
        assert(tasks[0].id == third);
        assert(tasks[1].id == first);
        assert(tasks[2].id == second);

        // Already next up: a no-op that still reports success.
        assert(manager.moveToFront(third, error));
        assert(manager.snapshot()[0].id == third);

        // A paused task is not in the queue, so it cannot be promoted, and the
        // order is left untouched.
        assert(manager.pause(third));
        error.clear();
        assert(!manager.moveToFront(third, error));
        assert(!error.empty());
        tasks = manager.snapshot();
        assert(tasks[0].id == third && tasks[1].id == first);

        // Promotion lands ahead of the first *queued* task, not at index 0:
        // the paused entry at the head keeps its place.
        assert(manager.moveToFront(second, error));
        tasks = manager.snapshot();
        assert(tasks[0].id == third); // paused, untouched
        assert(tasks[1].id == second);
        assert(tasks[2].id == first);

        error.clear();
        assert(!manager.moveToFront("nope", error));
        assert(!error.empty());

        assert(manager.remove(first, true, error));
        assert(manager.remove(second, true, error));
        assert(manager.remove(third, true, error));
    }

    unlink(source.c_str());
    unlink(downloadOnlySource.c_str());
    unlink(readmeSource.c_str());
    unlink(selectiveSource.c_str());
    rmdir((queueRoot + "/torrents").c_str());
    rmdir((queueRoot + "/downloads").c_str());
    unlink((queueRoot + "/queue.bencode").c_str());
    rmdir(queueRoot.c_str());
    rmdir((activeRoot + "/torrents").c_str());
    rmdir((activeRoot + "/downloads").c_str());
    unlink((activeRoot + "/queue.bencode").c_str());
    rmdir(activeRoot.c_str());
    rmdir((actionsRoot + "/torrents").c_str());
    rmdir((actionsRoot + "/downloads").c_str());
    unlink((actionsRoot + "/queue.bencode").c_str());
    rmdir(actionsRoot.c_str());
    rmdir((invalidRoot + "/torrents").c_str());
    rmdir((invalidRoot + "/downloads").c_str());
    rmdir(invalidRoot.c_str());
    rmdir((legacyRoot + "/torrents").c_str());
    rmdir((legacyRoot + "/downloads").c_str());
    unlink((legacyRoot + "/queue.bencode").c_str());
    rmdir(legacyRoot.c_str());
    rmdir((appRoot + "/torrents").c_str());
    rmdir((appRoot + "/downloads").c_str());
    unlink((appRoot + "/queue.bencode").c_str());
    rmdir(appRoot.c_str());
    rmdir(root);
    std::puts("manager tests passed");
    return 0;
}
