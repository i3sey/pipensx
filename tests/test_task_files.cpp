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
    assert(inventory.files[1].state == TaskFileState::Present);
    assert(inventory.presentBytes == 4);

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

    removeTaskFileManifest(root, preview.infoHash);
    assert(!loadTaskFileManifest(root, preview.infoHash, loaded, error));
    fs::remove_all(root);
    std::cout << "task file tests passed\n";
    return 0;
}
