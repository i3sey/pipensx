#include "app/download_manager.hpp"
#include "app/retro_rom.hpp"
#include "app/task_files.hpp"
extern "C" {
#include "core/sha1.h"
}
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace pipensx;
namespace fs = std::filesystem;
int main() {
    char temporary[] = "/tmp/pipensx-retro-queue-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path root = temporary;
    const auto app = (root / "app").string();
    fs::create_directories(app);
    fs::create_directories(root / "sd");
    const auto torrent = root / "source.torrent";
    const std::string payload = "download only";
    uint8_t digest[20];
    sha1(payload.data(), payload.size(), digest);
    const std::string info =
        "d6:lengthi13e4:name14:unexpected.nsp12:piece lengthi16384e6:pieces20:" +
        std::string(reinterpret_cast<char *>(digest), 20) + "e";
    std::ofstream(torrent, std::ios::binary) << "d4:info" << info << "e";
    TorrentPreview preview;
    std::string error, id;
    assert(DownloadManager::previewTorrent(torrent.string(), preview, error));
    RetroRomStore store(app, (root / "sd").string());
    RetroRelease release;
    release.platform = "gba";
    release.topicId = "100";
    release.title = "Retro torrent";
    release.infoHash = preview.infoHash;
    RetroRomOperation operation;
    std::string protectedTask;
    {
        DownloadManager manager(app, false);
        assert(store.plan(release, "roms/gba", "", operation, error));
        assert(manager.importTorrentActions(torrent.string(), {1}, id, error, {},
                                            TransferMode::DownloadOnly, {}, true));
        auto task = manager.snapshot(id);
        assert(task && task->mode == TransferMode::DownloadOnly && task->packageCount == 0);
        assert(task->fileSelection == std::vector<uint8_t>{1});
        assert(!manager.importTorrentActions(torrent.string(), {2}, id, error, {},
                                             TransferMode::StreamInstall, {}, true));
        task = manager.snapshot(release.infoHash);
        assert(task && task->mode == TransferMode::DownloadOnly &&
               task->fileSelection == std::vector<uint8_t>{1});
        int index = 0;
        for (const auto provider :
             {DebridProviderKind::TorBox, DebridProviderKind::TorrServer,
              DebridProviderKind::RealDebrid, DebridProviderKind::AllDebrid}) {
            release.topicId = std::to_string(101 + index);
            release.infoHash = std::string(40, char('b' + index));
            assert(store.plan(release, "roms/gba", "", operation, error));
            DebridImport import;
            import.infoHash = release.infoHash;
            import.name = "ROM bundle";
            import.totalBytes = 512;
            import.provider = provider;
            import.debridId = "fixture-" + std::to_string(index);
            import.mode = TransferMode::DownloadOnly;
            import.fileSelection = {1, 0};
            assert(manager.importDebrid(import, id, error, true));
            task = manager.snapshot(id);
            assert(task && task->source == TaskSource::Debrid && task->debridProvider == provider);
            assert(task->mode == TransferMode::DownloadOnly && task->packageCount == 0 &&
                   task->fileSelection == import.fileSelection);
            import.mode = TransferMode::StreamInstall;
            import.fileSelection = {2, 2};
            import.packageCount = 2;
            assert(!manager.importDebrid(import, id, error, true));
            task = manager.snapshot(release.infoHash);
            assert(task && task->mode == TransferMode::DownloadOnly &&
                   task->fileSelection == std::vector<uint8_t>({1, 0}));
            ++index;
        }
        auto installInfo = info;
        installInfo[installInfo.find("unexpected.nsp") + 11] = 'N';
        const auto installTorrent = root / "install.torrent";
        std::ofstream(installTorrent, std::ios::binary) << "d4:info" << installInfo << "e";
        assert(manager.importTorrentActions(installTorrent.string(), {2}, protectedTask, error, {},
                                            TransferMode::StreamInstall, {}, true));
        assert(!manager.importTorrentActions(installTorrent.string(), {1}, id, error, {},
                                             TransferMode::DownloadOnly, {}, true));
        const auto protectedSnapshot = manager.snapshot(protectedTask);
        assert(protectedSnapshot && protectedSnapshot->mode == TransferMode::StreamInstall &&
               protectedSnapshot->fileSelection == std::vector<uint8_t>{2});
        assert(manager.snapshot().size() == 6);
    }
    {
        DownloadManager restarted(app, false);
        assert(restarted.snapshot().size() == 6);
        for (const auto &task : restarted.snapshot())
            if (task.id != protectedTask)
                assert(task.mode == TransferMode::DownloadOnly && task.packageCount == 0);
        std::vector<RetroRomOperation> operations;
        assert(store.list(operations, error) && operations.size() == 5);
        for (const auto &task : restarted.snapshot()) {
            if (task.id != protectedTask) {
                assert(isRetroDownloadTask(app, task.id));
                assert(!taskHasPortOrArchiveFiles(app, task));
            } else {
                assert(!isRetroDownloadTask(app, task.id));
            }
        }
        DownloadTask dummyPortTask;
        dummyPortTask.id = std::string(40, 'f');
        dummyPortTask.mode = TransferMode::DownloadOnly;
        assert(!isRetroDownloadTask(app, dummyPortTask.id));
        assert(taskHasPortOrArchiveFiles(app, dummyPortTask));

        // Fallback scan of retro/operations/*.json when by-task marker is missing
        const auto marker = fs::path(app) / "retro/by-task" / preview.infoHash;
        assert(fs::exists(marker));
        fs::remove(marker);
        assert(isRetroDownloadTask(app, preview.infoHash));
    }
    fs::remove_all(root);
    std::cout << "retro direct and four-provider queue routing: OK\n";
}
