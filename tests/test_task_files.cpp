#include "../src/app/download_manager.hpp"
#include "../src/app/task_files.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace pipensx;

int main() {
    const std::string root = "/tmp/pipensx-task-files";
    fs::remove_all(root);
    fs::create_directories(root + "/downloads/task/Release/switch/MyPort");

    TorrentPreview preview;
    preview.name = "Release";
    preview.infoHash = "0123456789012345678901234567890123456789";
    preview.multi = true;
    TorrentPreview::File nro;
    nro.path = "switch/MyPort/MyPort.nro";
    nro.length = 4;
    preview.files.push_back(nro);
    TorrentPreview::File skipped;
    skipped.path = "README.txt";
    skipped.length = 7;
    preview.files.push_back(skipped);

    const std::vector<uint8_t> actions{
        static_cast<uint8_t>(FileAction::Download),
        static_cast<uint8_t>(FileAction::Skip),
    };
    const TaskFileManifest source = makeTaskFileManifest(
        preview.infoHash, preview, actions);
    std::string error;
    assert(saveTaskFileManifest(root, source, error));

    TaskFileManifest loaded;
    assert(loadTaskFileManifest(root, preview.infoHash, loaded, error));
    assert(loaded.files.size() == 2);
    assert(loaded.files[0].logicalPath ==
           "Release/switch/MyPort/MyPort.nro");
    assert(loaded.files[0].action == TaskFileAction::Download);
    assert(loaded.files[1].action == TaskFileAction::Skip);

    {
        std::ofstream file(root +
                           "/downloads/task/Release/switch/MyPort/MyPort.nro",
                           std::ios::binary);
        file.write("NRO0", 4);
    }
    DownloadTask task;
    task.id = preview.infoHash;
    task.dataPath = root + "/downloads/task";
    task.status = DownloadStatus::Completed;
    task.source = TaskSource::Debrid;

    TaskFileInventory inventory;
    assert(buildTaskFileInventory(root, task, inventory, error));
    assert(inventory.completeManifest);
    assert(inventory.files.size() == 2);
    assert(inventory.files[0].state == TaskFileState::Skipped);
    assert(inventory.files[0].staysInDownloads);
    assert(inventory.files[1].state == TaskFileState::Present);
    assert(inventory.files[1].kind == SwitchPathKind::Nro);
    assert(inventory.files[1].destinationRoot == "/switch");
    assert(inventory.files[1].destinationExample == "MyPort/MyPort.nro");
    assert(inventory.presentBytes == 4);

    {
        TaskFileInventory dest;
        dest.settled = true;
        auto add = [&](const std::string& path, TaskFileAction action,
                       bool package = false) {
            TaskFileInfo file;
            file.logicalPath = path;
            file.action = action;
            file.package = package;
            file.state = action == TaskFileAction::Skip
                ? TaskFileState::Skipped : TaskFileState::Present;
            dest.files.push_back(std::move(file));
        };
        add("Game.nro", TaskFileAction::Download);
        add("data/x.bin", TaskFileAction::Download);
        add("readme.txt", TaskFileAction::Skip);
        add("game.nsp", TaskFileAction::Install, true);
        add("atmosphere/contents/0100B00B51230000/romfs/loc.txt",
            TaskFileAction::Download);
        add("rus.zip", TaskFileAction::Download);
        annotateTaskFileDestinations(dest);
        assert(dest.files[0].kind == SwitchPathKind::Nro);
        assert(dest.files[0].destinationRoot == "/switch");
        assert(dest.files[0].destinationExample == "Game.nro");
        assert(dest.files[1].kind == SwitchPathKind::Nro);
        assert(dest.files[1].destinationRoot == "/switch");
        assert(dest.files[1].destinationExample == "data/x.bin");
        assert(!dest.files[1].staysInDownloads);
        assert(dest.files[2].staysInDownloads);
        assert(dest.files[3].kind == SwitchPathKind::Package);
        assert(!dest.files[3].staysInDownloads);
        assert(dest.files[3].destinationRoot.empty());
        assert(dest.files[4].kind == SwitchPathKind::LayeredFsRomfs);
        assert(dest.files[4].destinationRoot == "/atmosphere");
        assert(dest.files[4].destinationExample.find(
                   "atmosphere/contents/") == 0);
        assert(dest.files[5].kind == SwitchPathKind::Archive);
        assert(!dest.files[5].staysInDownloads);
        assert(dest.files[5].destinationCount == 0);
    }

    assert(taskFilePathIsSafe("Release/switch/MyPort/file.dat"));
    assert(!taskFilePathIsSafe("../switch/MyPort/file.dat"));
    assert(taskFilePathIsValidUtf8("switch/игра/file.dat"));
    assert(!taskFilePathIsValidUtf8(std::string("switch/\xc0\x80", 9)));
    assert(!taskFilePathIsFatCompatible("switch/Game/bad:name"));
    assert(!taskFilePathIsFatCompatible("switch/" + std::string(256, 'a')));

    TaskFileManifest slashManifest;
    slashManifest.taskId = "slashpathhash0123456789012345678901234";
    TaskFileRecord slashRecord;
    slashRecord.logicalPath = "Example/file.bin";
    slashRecord.localPath = "Example/file.bin";
    slashRecord.size = 1000;
    slashRecord.action = TaskFileAction::Download;
    slashManifest.files.push_back(slashRecord);
    assert(saveTaskFileManifest(root, slashManifest, error));
    slashRecord.size = 2000;
    slashManifest.files[0] = slashRecord;
    assert(saveTaskFileManifest(root, slashManifest, error));
    TaskFileManifest reloaded;
    assert(loadTaskFileManifest(root, slashManifest.taskId, reloaded, error));
    assert(reloaded.files[0].size == 2000);

    removeTaskFileManifest(root, preview.infoHash);
    assert(!loadTaskFileManifest(root, preview.infoHash, loaded, error));
    fs::remove_all(root);
    std::cout << "task file tests passed\n";
    return 0;
}
