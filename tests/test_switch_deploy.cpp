#include "../src/app/install_space.hpp"
#include "../src/app/switch_deploy.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using namespace pipensx;

namespace {

std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

void writeFile(const std::string& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string nroBytes() {
    std::string bytes(32, '\0');
    std::memcpy(bytes.data() + 0x10, "NRO0", 4);
    return bytes;
}

void writeCompletedQueue(const std::string& root, const std::string& taskId,
                         const std::string& dataPath, uint64_t total,
                         const std::string& mode = "download",
                         const std::string& status = "completed") {
    std::string queue = "d5:tasksl";
    queue += "d9:completed" + std::string("i") + std::to_string(total) + "e";
    queue += "4:data" + bstr(dataPath);
    queue += "9:debrid-id" + bstr("ready");
    queue += "5:error" + bstr("");
    queue += "2:id" + bstr(taskId);
    queue += "8:metainfo" + bstr("");
    queue += "4:mode" + bstr(mode);
    queue += "4:name" + bstr("Port release");
    queue += "13:package-counti0e13:packages-donei0e";
    queue += "11:pieces-donei0e12:pieces-totali0e";
    queue += "8:provider" + bstr("torbox");
    queue += "9:selection" + bstr(std::string(2, '\1'));
    queue += "6:source" + bstr("debrid");
    queue += "6:status" + bstr(status);
    queue += "5:totali" + std::to_string(total) + "e";
    queue += "16:wanted-completedi" + std::to_string(total) + "e";
    queue += "12:wanted-totali" + std::to_string(total) + "e";
    queue += "ee7:versioni6ee";
    writeFile(root + "/queue.bencode", queue);
}

} // namespace

int main() {
    const std::string root = "/tmp/pipensx-switch-deploy";
    const std::string target = root + "/sd/switch";
    const std::string data = root + "/downloads/task";
    const std::string taskId = "0123456789012345678901234567890123456789";
    fs::remove_all(root);
    fs::create_directories(target);

    const std::string nro = nroBytes();
    const std::string asset = "PORT-DATA";
    const std::string package = "PACKAGE";
    const std::string external = "README";
    writeFile(data + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(data + "/Release/switch/MyPort/data.bin", asset);
    writeFile(data + "/Release/switch/MyPort/update.nsp", package);
    writeFile(data + "/Release/README.txt", external);
    writeCompletedQueue(root, taskId, data,
                        nro.size() + asset.size() + package.size() +
                            external.size());

    TaskFileManifest manifest;
    manifest.taskId = taskId;
    TaskFileRecord nroRecord;
    nroRecord.logicalPath = "Release/switch/MyPort/MyPort.nro";
    nroRecord.localPath = nroRecord.logicalPath;
    nroRecord.size = nro.size();
    manifest.files.push_back(nroRecord);
    TaskFileRecord assetRecord;
    assetRecord.logicalPath = "Release/switch/MyPort/data.bin";
    assetRecord.localPath = assetRecord.logicalPath;
    assetRecord.size = asset.size();
    manifest.files.push_back(assetRecord);
    TaskFileRecord packageRecord;
    packageRecord.logicalPath = "Release/switch/MyPort/update.nsp";
    packageRecord.localPath = packageRecord.logicalPath;
    packageRecord.size = package.size();
    packageRecord.package = true;
    manifest.files.push_back(packageRecord);
    TaskFileRecord externalRecord;
    externalRecord.logicalPath = "Release/README.txt";
    externalRecord.localPath = externalRecord.logicalPath;
    externalRecord.size = external.size();
    manifest.files.push_back(externalRecord);
    std::string error;
    assert(saveTaskFileManifest(root, manifest, error));

    StorageSpaceSnapshot storage;
    storage.available = true;
    storage.totalBytes = 1024 * 1024;
    storage.freeBytes = 1024 * 1024;
    setStorageSpaceOverride(&storage);

    DownloadManager manager(root, false);
    SwitchDeployService deploy(manager, root, target);
    SwitchDeployInspection inspection = deploy.inspect(taskId);
    assert(inspection.canStart());
    assert(inspection.plan.files.size() == 2);
    assert(inspection.plan.ignoredFiles == 2);
    assert(inspection.plan.bytesToCopy == nro.size() + asset.size());
    assert(inspection.plan.files[0].destinationRelativePath ==
               "MyPort/MyPort.nro" ||
           inspection.plan.files[1].destinationRelativePath ==
               "MyPort/MyPort.nro");

    storage.freeBytes = 1;
    setStorageSpaceOverride(&storage);
    inspection = deploy.inspect(taskId);
    assert(inspection.problem == SwitchDeployProblem::NoSpace);
    storage.freeBytes = 1024 * 1024;
    setStorageSpaceOverride(&storage);

    writeFile(data + "/Release/switch/MyPort/Data.bin", "A");
    writeFile(data + "/Release/switch/MyPort/data.bin", "B");
    TaskFileInventory collisionInventory;
    collisionInventory.taskId = "collision";
    collisionInventory.rootPath = data;
    collisionInventory.settled = true;
    auto addPresent = [&](TaskFileInventory& inventory,
                          const std::string& logical,
                          const std::string& absolute, uint64_t size) {
        TaskFileInfo file;
        file.logicalPath = logical;
        file.localPath = logical;
        file.absolutePath = absolute;
        file.size = size;
        file.action = TaskFileAction::Download;
        file.state = TaskFileState::Present;
        inventory.files.push_back(std::move(file));
    };
    addPresent(collisionInventory, "Release/switch/MyPort/MyPort.nro",
               data + "/Release/switch/MyPort/MyPort.nro", nro.size());
    addPresent(collisionInventory, "Release/switch/MyPort/Data.bin",
               data + "/Release/switch/MyPort/Data.bin", 1);
    addPresent(collisionInventory, "Release/switch/MyPort/data.bin",
               data + "/Release/switch/MyPort/data.bin", 1);
    TaskFileInventory incomplete = collisionInventory;
    incomplete.completeManifest = false;
    assert(inspectSwitchDeploy(std::move(incomplete), target).problem ==
           SwitchDeployProblem::NotReady);
    collisionInventory.completeManifest = true;
    SwitchDeployInspection collision = inspectSwitchDeploy(
        std::move(collisionInventory), target);
    assert(collision.problem == SwitchDeployProblem::UnsafePath);

    writeFile(data + "/sources/a", "A");
    writeFile(data + "/sources/b", "B");
    TaskFileInventory directoryCollision;
    directoryCollision.taskId = "directory-collision";
    directoryCollision.rootPath = data;
    directoryCollision.settled = true;
    directoryCollision.completeManifest = true;
    addPresent(directoryCollision, "Release/switch/MyPort/MyPort.nro",
               data + "/Release/switch/MyPort/MyPort.nro", nro.size());
    addPresent(directoryCollision, "Release/switch/MyPort/Data/a.bin",
               data + "/sources/a", 1);
    addPresent(directoryCollision, "Release/switch/myport/data/b.bin",
               data + "/sources/b", 1);
    assert(inspectSwitchDeploy(std::move(directoryCollision), target).problem ==
           SwitchDeployProblem::UnsafePath);

    TaskFileInventory prefixCollision;
    prefixCollision.taskId = "prefix-collision";
    prefixCollision.rootPath = data;
    prefixCollision.settled = true;
    prefixCollision.completeManifest = true;
    addPresent(prefixCollision, "Release/switch/MyPort/MyPort.nro",
               data + "/Release/switch/MyPort/MyPort.nro", nro.size());
    addPresent(prefixCollision, "Release/switch/MyPort/data",
               data + "/sources/a", 1);
    addPresent(prefixCollision, "Release/switch/MyPort/data/app.bin",
               data + "/sources/b", 1);
    assert(inspectSwitchDeploy(std::move(prefixCollision), target).problem ==
           SwitchDeployProblem::UnsafePath);

    writeFile(data + "/Other/switch/Other/Other.nro", nro);
    TaskFileInventory ambiguous;
    ambiguous.taskId = "ambiguous";
    ambiguous.rootPath = data;
    ambiguous.settled = true;
    ambiguous.completeManifest = true;
    addPresent(ambiguous, "Release/switch/MyPort/MyPort.nro",
               data + "/Release/switch/MyPort/MyPort.nro", nro.size());
    addPresent(ambiguous, "Other/switch/Other/Other.nro",
               data + "/Other/switch/Other/Other.nro", nro.size());
    assert(inspectSwitchDeploy(std::move(ambiguous), target).problem ==
           SwitchDeployProblem::AmbiguousLayout);

    // Restore the sidecar's source before the real copy.
    writeFile(data + "/Release/switch/MyPort/data.bin", asset);

    assert(deploy.start(taskId, error));
    for (int i = 0; i < 500 && deploy.snapshot().active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const SwitchDeploySnapshot finished = deploy.snapshot();
    assert(finished.phase == SwitchDeployPhase::Completed);
    assert(finished.filesCopied == 2);
    assert(fs::exists(target + "/MyPort/data.bin"));
    assert(fs::exists(target + "/MyPort/MyPort.nro"));
    assert(deploy.receiptState(taskId) == SwitchDeployReceiptState::Valid);

    inspection = deploy.inspect(taskId);
    assert(inspection.canStart());
    assert(inspection.plan.identicalFiles == 2);
    assert(inspection.plan.bytesToCopy == 0);

    writeFile(target + "/MyPort/data.bin", "DIFFERENT");
    inspection = deploy.inspect(taskId);
    assert(inspection.problem == SwitchDeployProblem::Conflict);
    assert(inspection.plan.conflictFiles == 1);
    assert(deploy.receiptState(taskId) == SwitchDeployReceiptState::Modified);

    deploy.shutdown();

    const std::string recoveryRoot = root + "/recovery";
    const std::string recoveryTarget = recoveryRoot + "/sd/switch";
    const std::string staleTemp =
        recoveryTarget + "/MyPort/file.pipensx-part-deadbeef";
    writeFile(staleTemp, "partial");
    std::string job = "d4:task" + bstr("deadbeef") +
                      "4:temp" + bstr(staleTemp) + "7:versioni1ee";
    writeFile(recoveryRoot + "/deploy-job.bencode", job);
    DownloadManager recoveryManager(recoveryRoot, false);
    {
        SwitchDeployService recovery(recoveryManager, recoveryRoot,
                                     recoveryTarget);
        assert(!fs::exists(staleTemp));
        assert(!fs::exists(recoveryRoot + "/deploy-job.bencode"));
    }

    const std::string streamRoot = root + "/stream";
    const std::string streamId = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";
    writeCompletedQueue(streamRoot, streamId, data, nro.size(), "install",
                        "installed");
    DownloadManager streamManager(streamRoot, false);
    SwitchDeployService streamDeploy(streamManager, streamRoot,
                                     streamRoot + "/sd/switch");
    assert(streamDeploy.inspect(streamId).problem ==
           SwitchDeployProblem::NotReady);
    assert(!streamManager.beginExternalDeploy(streamId, error));

    setStorageSpaceOverride(nullptr);
    fs::remove_all(root);
    std::cout << "switch deploy tests passed\n";
    return 0;
}
