#include "../src/app/install_space.hpp"
#include "../src/app/nx_file_types.hpp"
#include "../src/app/port_archive.hpp"
#include "../src/app/port_selection.hpp"
#include "../src/app/switch_deploy.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <zlib.h>

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

// Minimal stored (method 0) ZIP writer for the deploy fixture. The harness
// links zlib already (extractZip), so CRC-32 comes from there.
std::string writeStoredZip(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    auto crcOf = [](const std::string& data) {
        return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(
                                                  data.data()),
                                          static_cast<uInt>(data.size())));
    };
    std::string out;
    auto put16 = [&out](uint16_t value) {
        out.push_back(static_cast<char>(value & 0xff));
        out.push_back(static_cast<char>(value >> 8));
    };
    auto put32 = [&out](uint32_t value) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    };
    std::vector<uint32_t> offsets;
    for (const auto& entry : entries) {
        offsets.push_back(static_cast<uint32_t>(out.size()));
        put32(0x04034b50); // local header
        put16(20);         // version needed
        put16(0);          // flags
        put16(0);          // method: stored
        put16(0);          // mtime
        put16(0);          // mdate
        put32(crcOf(entry.second));
        put32(static_cast<uint32_t>(entry.second.size()));
        put32(static_cast<uint32_t>(entry.second.size()));
        put16(static_cast<uint16_t>(entry.first.size()));
        put16(0); // extra
        out += entry.first;
        out += entry.second;
    }
    const uint32_t directoryOffset = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        put32(0x02014b50); // central directory
        put16(20);         // version made by
        put16(20);         // version needed
        put16(0);          // flags
        put16(0);          // method: stored
        put16(0);          // mtime
        put16(0);          // mdate
        put32(crcOf(entries[i].second));
        put32(static_cast<uint32_t>(entries[i].second.size()));
        put32(static_cast<uint32_t>(entries[i].second.size()));
        put16(static_cast<uint16_t>(entries[i].first.size()));
        put16(0); // extra
        put16(0); // comment
        put16(0); // disk
        put16(0); // internal attrs
        put32(0); // external attrs
        put32(offsets[i]);
        out += entries[i].first;
    }
    const uint32_t directorySize =
        static_cast<uint32_t>(out.size()) - directoryOffset;
    put32(0x06054b50); // end of central directory
    put16(0);          // disk
    put16(0);          // directory disk
    put16(static_cast<uint16_t>(entries.size()));
    put16(static_cast<uint16_t>(entries.size()));
    put32(directorySize);
    put32(directoryOffset);
    put16(0); // comment
    return out;
}

// Writes a v1 receipt (no unpacked member list) for the loose files.
void writeV1Receipt(const std::string& root, const std::string& taskId,
                    const std::vector<std::pair<std::string, uint64_t>>& files) {
    std::string blob = "d5:filesl";
    for (const auto& file : files) {
        blob += "d6:digest" + bstr(std::string(32, '\0'));
        blob += "4:path" + bstr(file.first);
        blob += "4:size" + std::string("i") + std::to_string(file.second) +
                "ee";
    }
    blob += "e4:task" + bstr(taskId);
    blob += "7:versioni1ee";
    writeFile(root + "/deployments/" + taskId + ".bencode", blob);
}

// Writes a v2 receipt (unpacked member list, no recorded titles).
void writeV2Receipt(const std::string& root, const std::string& taskId,
                    const std::vector<std::pair<std::string, uint64_t>>& files,
                    const std::vector<std::string>& unpacked) {
    std::string blob = "d5:filesl";
    for (const auto& file : files) {
        blob += "d6:digest" + bstr(std::string(32, '\0'));
        blob += "4:path" + bstr(file.first);
        blob += "4:size" + std::string("i") + std::to_string(file.second) +
                "ee";
    }
    blob += "e4:task" + bstr(taskId);
    blob += "8:unpackedl";
    for (const std::string& path : unpacked)
        blob += bstr(path);
    blob += "e7:versioni2ee";
    writeFile(root + "/deployments/" + taskId + ".bencode", blob);
}

} // namespace

int main() {
    assert(isPortArchiveName("switch.7z"));
    assert(isPortArchiveName("path/to/switch.7z"));
    assert(isPortArchiveName("SWITCH.ZIP"));
    assert(!isPortArchiveName("myswitch.7z"));
    assert(!isPortArchiveName("switch.rar"));

    {
        TorrentPreview preview;
        preview.multi = true;
        preview.name = "Release";
        preview.files = {
            {"switch/MyPort/MyPort.nro", 32, false, false, false},
            {"switch/MyPort/data.bin", 9, false, false, false},
            {"readme.txt", 4, false, false, false},
        };
        assert(candidatePortRoot(preview) == "release/switch");
        assert(torrentPortLayoutDetected(preview));
        const auto mask = selectPortInstallActions(preview);
        assert(mask.size() == 3);
        assert(mask[0] == static_cast<uint8_t>(FileAction::Download));
        assert(mask[1] == static_cast<uint8_t>(FileAction::Download));
        assert(mask[2] == static_cast<uint8_t>(FileAction::Skip));
    }
    {
        TorrentPreview preview;
        preview.files = {
            {"game.nsp", 100, true, false, false},
            {"switch.zip", 50, false, false, false},
            {"notes.txt", 3, false, false, false},
        };
        assert(torrentHasPortArchive(preview));
        const auto mask = selectPortInstallActions(preview);
        assert(mask[0] == static_cast<uint8_t>(FileAction::Install));
        assert(mask[1] == static_cast<uint8_t>(FileAction::Download));
        assert(mask[2] == static_cast<uint8_t>(FileAction::Skip));
    }
    {
        TorrentPreview preview;
        preview.files = {{"readme.txt", 4, false, false, false}};
        assert(!torrentPortLayoutDetected(preview));
        const auto mask = selectPortInstallActions(preview);
        assert(mask.size() == 1);
        assert(mask[0] == static_cast<uint8_t>(FileAction::Download));
    }

    assert(portArchiveSolidFitsRam(0, 0));
    assert(portArchiveSolidFitsRam(100, kPortArchiveSolidRamReserveBytes + 100));
    assert(!portArchiveSolidFitsRam(100, kPortArchiveSolidRamReserveBytes + 99));
    assert(!portArchiveSolidFitsRam(1, kPortArchiveSolidRamReserveBytes));

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
    assert(!switchDeployFullyInstalled(inspection));
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
    assert(switchDeployFullyInstalled(inspection));

    writeFile(target + "/MyPort/data.bin", "DIFFERENT");
    inspection = deploy.inspect(taskId);
    assert(inspection.problem == SwitchDeployProblem::Conflict);
    assert(inspection.plan.conflictFiles == 1);
    assert(!switchDeployFullyInstalled(inspection));
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
    const std::string streamData = streamRoot + "/downloads/task";
    const std::string streamTarget = streamRoot + "/sd/switch";
    const std::string streamId = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";
    fs::create_directories(streamTarget);
    writeFile(streamData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(streamData + "/Release/switch/MyPort/data.bin", asset);
    writeFile(streamData + "/Release/switch/MyPort/update.nsp", package);
    writeFile(streamData + "/Release/README.txt", external);
    writeCompletedQueue(streamRoot, streamId, streamData,
                        nro.size() + asset.size() + package.size() +
                            external.size(),
                        "install", "installed");
    TaskFileManifest streamManifest = manifest;
    streamManifest.taskId = streamId;
    assert(saveTaskFileManifest(streamRoot, streamManifest, error));
    DownloadManager streamManager(streamRoot, false);
    SwitchDeployService streamDeploy(streamManager, streamRoot, streamTarget);
    SwitchDeployInspection streamInspection = streamDeploy.inspect(streamId);
    assert(streamInspection.canStart());
    assert(streamInspection.plan.files.size() == 2);
    assert(streamManager.beginExternalDeploy(streamId, error));

    const std::string autoRoot = root + "/auto";
    const std::string autoData = autoRoot + "/downloads/task";
    const std::string autoTarget = autoRoot + "/sd/switch";
    const std::string autoId = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    fs::create_directories(autoTarget);
    writeFile(autoData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(autoData + "/Release/switch/MyPort/data.bin", asset);
    writeFile(autoData + "/Release/switch/MyPort/update.nsp", package);
    writeFile(autoData + "/Release/README.txt", external);
    writeCompletedQueue(autoRoot, autoId, autoData,
                        nro.size() + asset.size() + package.size() +
                            external.size(),
                        "install", "installed");
    TaskFileManifest autoManifest = manifest;
    autoManifest.taskId = autoId;
    assert(saveTaskFileManifest(autoRoot, autoManifest, error));
    DownloadManager autoManager(autoRoot, false);
    SwitchDeployService autoDeploy(autoManager, autoRoot, autoTarget);
    autoDeploy.scheduleDeployOfferPoll();
    std::optional<SwitchDeployService::PendingOffer> offered;
    for (int i = 0; i < 500 && !offered; ++i) {
        offered = autoDeploy.takePendingDeployOffer();
        if (!offered)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(offered);
    assert(offered->taskId == autoId);
    assert(!offered->autoStart);
    assert(offered->inspection.canStart());
    autoDeploy.dismissDeployOffer(autoId);
    assert(!autoDeploy.takePendingDeployOffer());
    // Copy only after explicit confirmation (user accepted the prompt).
    assert(autoDeploy.start(autoId, error, false));
    for (int i = 0; i < 500 && autoDeploy.snapshot().active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(autoDeploy.snapshot().phase == SwitchDeployPhase::Completed);
    assert(autoDeploy.receiptState(autoId) == SwitchDeployReceiptState::Valid);
    autoDeploy.shutdown();

    const std::string oneTapRoot = root + "/onetap";
    const std::string oneTapData = oneTapRoot + "/downloads/task";
    const std::string oneTapTarget = oneTapRoot + "/sd/switch";
    const std::string oneTapId = "cccccccccccccccccccccccccccccccccccccccc";
    fs::create_directories(oneTapTarget);
    writeFile(oneTapData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(oneTapData + "/Release/switch/MyPort/data.bin", asset);
    writeFile(oneTapData + "/Release/switch/MyPort/update.nsp", package);
    writeFile(oneTapData + "/Release/README.txt", external);
    writeCompletedQueue(oneTapRoot, oneTapId, oneTapData,
                        nro.size() + asset.size() + package.size() +
                            external.size(),
                        "download", "completed");
    TaskFileManifest oneTapManifest = manifest;
    oneTapManifest.taskId = oneTapId;
    assert(saveTaskFileManifest(oneTapRoot, oneTapManifest, error));
    DownloadManager oneTapManager(oneTapRoot, false);
    SwitchDeployService oneTapDeploy(oneTapManager, oneTapRoot, oneTapTarget);
    assert(oneTapDeploy.armAutoCopy(oneTapId));
    assert(oneTapDeploy.autoCopyArmed(oneTapId));
    oneTapDeploy.scheduleDeployOfferPoll();
    std::optional<SwitchDeployService::PendingOffer> oneTapOffer;
    for (int i = 0; i < 500 && !oneTapOffer; ++i) {
        oneTapOffer = oneTapDeploy.takePendingDeployOffer();
        if (!oneTapOffer)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(oneTapOffer);
    assert(oneTapOffer->taskId == oneTapId);
    assert(oneTapOffer->autoStart);
    assert(oneTapOffer->inspection.canStart());
    oneTapDeploy.dismissDeployOffer(oneTapId);
    oneTapDeploy.clearAutoCopy(oneTapId);
    assert(!oneTapDeploy.autoCopyArmed(oneTapId));
    oneTapDeploy.shutdown();

    // Deleting the installed files must not restart the copy. A one-tap
    // auto-copy that already ran leaves a receipt; when the user removes the
    // files from /switch afterwards, the next app start must not offer or
    // auto-start the copy again — restoring them is a manual choice.
    const std::string rearmRoot = root + "/rearm";
    const std::string rearmData = rearmRoot + "/downloads/task";
    const std::string rearmTarget = rearmRoot + "/sd/switch";
    const std::string rearmId = "dddddddddddddddddddddddddddddddddddddddd";
    fs::create_directories(rearmTarget);
    writeFile(rearmData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(rearmData + "/Release/switch/MyPort/data.bin", asset);
    writeFile(rearmData + "/Release/switch/MyPort/update.nsp", package);
    writeFile(rearmData + "/Release/README.txt", external);
    writeCompletedQueue(rearmRoot, rearmId, rearmData,
                        nro.size() + asset.size() + package.size() +
                            external.size(),
                        "install", "installed");
    TaskFileManifest rearmManifest = manifest;
    rearmManifest.taskId = rearmId;
    assert(saveTaskFileManifest(rearmRoot, rearmManifest, error));
    DownloadManager rearmManager(rearmRoot, false);
    SwitchDeployService rearmDeploy(rearmManager, rearmRoot, rearmTarget);
    assert(rearmDeploy.armAutoCopy(rearmId));
    rearmDeploy.scheduleDeployOfferPoll();
    std::optional<SwitchDeployService::PendingOffer> rearmOffer;
    for (int i = 0; i < 500 && !rearmOffer; ++i) {
        rearmOffer = rearmDeploy.takePendingDeployOffer();
        if (!rearmOffer)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(rearmOffer);
    assert(rearmOffer->autoStart);
    assert(rearmOffer->inspection.canStart());
    rearmDeploy.dismissDeployOffer(rearmId);
    assert(rearmDeploy.start(rearmId, error, false));
    for (int i = 0; i < 500 && rearmDeploy.snapshot().active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(rearmDeploy.snapshot().phase == SwitchDeployPhase::Completed);
    assert(rearmDeploy.receiptState(rearmId) == SwitchDeployReceiptState::Valid);
    // The one-tap marker survives until the next offer poll, like on console.
    assert(rearmDeploy.autoCopyArmed(rearmId));
    // User deletes the installed port files from /switch.
    fs::remove_all(rearmTarget + "/MyPort");
    assert(rearmDeploy.receiptState(rearmId) ==
           SwitchDeployReceiptState::Modified);
    rearmDeploy.shutdown();
    {
        // Fresh service = fresh offer bookkeeping, as after an app restart.
        SwitchDeployService fresh(rearmManager, rearmRoot, rearmTarget);
        fresh.scheduleDeployOfferPoll();
        std::optional<SwitchDeployService::PendingOffer> rearmAgain;
        for (int i = 0; i < 500 && !rearmAgain; ++i) {
            rearmAgain = fresh.takePendingDeployOffer();
            if (!rearmAgain)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(!rearmAgain);
        assert(!fresh.autoCopyArmed(rearmId));
    }

    // ------------------------------------------------------------------
    // Receipt v2 + PortUninstallService: a port with loose files and a
    // switch.zip archive.
    const std::string portRoot = root + "/port";
    const std::string portData = portRoot + "/downloads/task";
    const std::string portTarget = portRoot + "/sd/switch";
    const std::string portId = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    fs::create_directories(portTarget);
    const std::string extra1 = "EXTRA-ONE";
    const std::string extra2 = std::string(512, 'E');
    const std::string skipInZip = "SKIP-ME";
    const std::string zipBytes = writeStoredZip({
        {"switch/MyPort/extra1.bin", extra1},
        {"switch/MyPort/sub/extra2.bin", extra2},
        {"other/skip.bin", skipInZip},
    });
    writeFile(portData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(portData + "/Release/switch/MyPort/data.bin", asset);
    writeFile(portData + "/Release/switch.zip", zipBytes);
    writeCompletedQueue(portRoot, portId, portData,
                        nro.size() + asset.size() + zipBytes.size());
    TaskFileManifest portManifest;
    portManifest.taskId = portId;
    TaskFileRecord portNro = nroRecord;
    portManifest.files.push_back(portNro);
    TaskFileRecord portAsset = assetRecord;
    portManifest.files.push_back(portAsset);
    TaskFileRecord portZip;
    portZip.logicalPath = "Release/switch.zip";
    portZip.localPath = portZip.logicalPath;
    portZip.size = zipBytes.size();
    portManifest.files.push_back(portZip);
    assert(saveTaskFileManifest(portRoot, portManifest, error));
    DownloadManager portManager(portRoot, false);
    SwitchDeployService portDeploy(portManager, portRoot, portTarget);
    PortUninstallService portUninstall(portManager, portRoot, portTarget);
    assert(portUninstall.receiptExists(portId) == false);
    SwitchDeployInspection portInspection = portDeploy.inspect(portId);
    assert(portInspection.canStart());
    assert(portInspection.plan.files.size() == 2);
    assert(portInspection.plan.archives.size() == 1);
    assert(portInspection.plan.archives[0].extractable);
    assert(portDeploy.start(portId, error));
    for (int i = 0; i < 500 && portDeploy.snapshot().active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(portDeploy.snapshot().phase == SwitchDeployPhase::Completed);
    assert(fs::exists(portTarget + "/MyPort/MyPort.nro"));
    assert(fs::exists(portTarget + "/MyPort/data.bin"));
    assert(fs::exists(portTarget + "/MyPort/extra1.bin"));
    assert(fs::exists(portTarget + "/MyPort/sub/extra2.bin"));
    assert(!fs::exists(portTarget + "/other/skip.bin"));
    // v2 receipt roundtrip: unpacked members are saved and verified on load.
    assert(portDeploy.receiptState(portId) == SwitchDeployReceiptState::Valid);
    assert(portUninstall.receiptExists(portId));
    fs::remove(portTarget + "/MyPort/extra1.bin");
    assert(portDeploy.receiptState(portId) == SwitchDeployReceiptState::Modified);
    writeFile(portTarget + "/MyPort/extra1.bin", extra1);
    assert(portDeploy.receiptState(portId) == SwitchDeployReceiptState::Valid);
    portDeploy.shutdown();

    // The plan drives the dialog: 2 receipt copies + 2 extracted members,
    // the task and its data are still present.
    PortUninstallPlan portPlan;
    assert(portUninstall.plan("0100000000eeee00", {portId}, portPlan));
    assert(portPlan.taskIds == std::vector<std::string>({portId}));
    assert(portPlan.switchFiles.size() == 4);
    assert(portPlan.switchBytes ==
           nro.size() + asset.size() + extra1.size() + extra2.size());
    assert(portPlan.wholeFolders.empty());
    assert(portPlan.hasTask);
    assert(portPlan.taskHasData);

    // Files outside the receipt are not touched: an unknown file inside the
    // port folder survives, the folder itself stays, empty subfolders go.
    writeFile(portTarget + "/MyPort/user-save.bin", "KEEP");
    writeFile(portTarget + "/KeepMe/keep.txt", "KEEP");
    PortUninstallReport portReport;
    assert(portUninstall.uninstallPort(
        portPlan,
        [](std::string&) { return true; },
        portReport));
    assert(portReport.complete());
    assert(portReport.shortcutRemoved);
    assert(portReport.filesRemoved == 4);
    assert(portReport.filesMissing == 0);
    assert(portReport.filesFailed == 0);
    assert(!fs::exists(portTarget + "/MyPort/MyPort.nro"));
    assert(!fs::exists(portTarget + "/MyPort/data.bin"));
    assert(!fs::exists(portTarget + "/MyPort/extra1.bin"));
    assert(!fs::exists(portTarget + "/MyPort/sub"));
    assert(fs::exists(portTarget + "/MyPort/user-save.bin"));
    assert(fs::exists(portTarget + "/KeepMe/keep.txt"));
    assert(!portManager.snapshot(portId));
    assert(!fs::exists(portData));
    assert(!portUninstall.receiptExists(portId));
    assert(!fs::exists(portRoot + "/deployments/" + portId + ".auto"));
    portManager.shutdown();

    // ------------------------------------------------------------------
    // v1 receipt with the archive still in the task data: the unpacked list
    // is rebuilt from the archive headers.
    const std::string v1Root = root + "/v1";
    const std::string v1Data = v1Root + "/downloads/task";
    const std::string v1Target = v1Root + "/sd/switch";
    const std::string v1Id = "ffffffffffffffffffffffffffffffffffffffff";
    fs::create_directories(v1Target);
    writeFile(v1Data + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(v1Data + "/Release/switch/MyPort/data.bin", asset);
    writeFile(v1Data + "/Release/switch.zip", zipBytes);
    writeCompletedQueue(v1Root, v1Id, v1Data,
                        nro.size() + asset.size() + zipBytes.size());
    TaskFileManifest v1Manifest = portManifest;
    v1Manifest.taskId = v1Id;
    assert(saveTaskFileManifest(v1Root, v1Manifest, error));
    writeV1Receipt(v1Root, v1Id,
                   {{"MyPort/MyPort.nro", nro.size()},
                    {"MyPort/data.bin", asset.size()}});
    // Simulate the deployment the receipt describes.
    writeFile(v1Target + "/MyPort/MyPort.nro", nro);
    writeFile(v1Target + "/MyPort/data.bin", asset);
    writeFile(v1Target + "/MyPort/extra1.bin", extra1);
    writeFile(v1Target + "/MyPort/sub/extra2.bin", extra2);
    DownloadManager v1Manager(v1Root, false);
    PortUninstallService v1Uninstall(v1Manager, v1Root, v1Target);
    assert(v1Uninstall.receiptExists(v1Id));
    PortUninstallPlan v1Plan;
    assert(v1Uninstall.plan("0100000000ffff00", {v1Id}, v1Plan));
    assert(v1Plan.wholeFolders.empty());
    assert(v1Plan.switchFiles.size() == 4);
    assert(v1Plan.switchBytes ==
           nro.size() + asset.size() + extra1.size() + extra2.size());
    assert(v1Uninstall.uninstallPort(
        v1Plan, [](std::string&) { return true; }, portReport));
    assert(portReport.complete());
    assert(!fs::exists(v1Target + "/MyPort"));
    assert(!v1Manager.snapshot(v1Id));
    assert(!v1Uninstall.receiptExists(v1Id));
    v1Manager.shutdown();

    // ------------------------------------------------------------------
    // Old v1 receipt + the archive deleted from the task data: the exact
    // unpacked list is unknowable, so the plan switches to removing the
    // receipt's top-level folders entirely.
    const std::string goneRoot = root + "/v1-gone";
    const std::string goneData = goneRoot + "/downloads/task";
    const std::string goneTarget = goneRoot + "/sd/switch";
    const std::string goneId = "abababababababababababababababababababab";
    fs::create_directories(goneTarget);
    writeFile(goneData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeFile(goneData + "/Release/switch/MyPort/data.bin", asset);
    // No switch.zip: it was deleted from the task data after the copy.
    writeCompletedQueue(goneRoot, goneId, goneData,
                        nro.size() + asset.size());
    TaskFileManifest goneManifest;
    goneManifest.taskId = goneId;
    TaskFileRecord goneNro = nroRecord;
    goneManifest.files.push_back(goneNro);
    TaskFileRecord goneAsset = assetRecord;
    goneManifest.files.push_back(goneAsset);
    TaskFileRecord goneZip;
    goneZip.logicalPath = "Release/switch.zip";
    goneZip.localPath = goneZip.logicalPath;
    goneZip.size = zipBytes.size();
    goneManifest.files.push_back(goneZip);
    assert(saveTaskFileManifest(goneRoot, goneManifest, error));
    writeV1Receipt(goneRoot, goneId,
                   {{"MyPort/MyPort.nro", nro.size()},
                    {"MyPort/data.bin", asset.size()}});
    // The deployed state has files the receipt cannot know about.
    writeFile(goneTarget + "/MyPort/MyPort.nro", nro);
    writeFile(goneTarget + "/MyPort/data.bin", asset);
    writeFile(goneTarget + "/MyPort/mystery.dat", "MYSTERY");
    writeFile(goneTarget + "/KeepMe/keep.txt", "KEEP");
    DownloadManager goneManager(goneRoot, false);
    PortUninstallService goneUninstall(goneManager, goneRoot, goneTarget);
    PortUninstallPlan gonePlan;
    assert(goneUninstall.plan("0100000000abab00", {goneId}, gonePlan));
    assert(gonePlan.wholeFolders ==
           std::vector<std::string>({"MyPort"}));
    assert(gonePlan.switchFiles.empty());
    assert(gonePlan.hasTask);
    // A failing shortcut keeps the receipt: repeating the removal is safe.
    assert(!goneUninstall.uninstallPort(
        gonePlan, [](std::string& error) {
            error = "ns delete failed";
            return false;
        },
        portReport));
    assert(!portReport.complete());
    assert(!portReport.shortcutRemoved);
    assert(fs::exists(goneRoot + "/deployments/" + goneId + ".bencode"));
    assert(!fs::exists(goneTarget + "/MyPort"));
    assert(!goneManager.snapshot(goneId));
    // Second pass: files and task are already gone, shortcut now succeeds —
    // the receipt is dropped only after the full success.
    assert(goneUninstall.plan("0100000000abab00", {goneId}, gonePlan));
    assert(gonePlan.wholeFolders ==
           std::vector<std::string>({"MyPort"}));
    assert(goneUninstall.uninstallPort(
        gonePlan, [](std::string&) { return true; }, portReport));
    assert(portReport.complete());
    assert(fs::exists(goneTarget + "/KeepMe/keep.txt"));
    assert(!goneUninstall.receiptExists(goneId));
    goneManager.shutdown();

    // ------------------------------------------------------------------
    // Receipt v3: the deploy records the bracketed title id of the forwarder
    // package, so Uninstall links the title to its receipt without the
    // metadata index (which covers retail releases only — the very reason a
    // port uninstall used to fall back to a plain shortcut removal).
    const std::string titledRoot = root + "/titled";
    const std::string titledData = titledRoot + "/downloads/task";
    const std::string titledTarget = titledRoot + "/sd/switch";
    const std::string titledId = "f99434b99c431b6229609c2f5755e5d1e5b890ab";
    fs::create_directories(titledTarget);
    writeFile(titledData + "/Release/switch/MyPort/MyPort.nro", nro);
    writeCompletedQueue(titledRoot, titledId, titledData, nro.size());
    TaskFileManifest titledManifest;
    titledManifest.taskId = titledId;
    TaskFileRecord titledNro;
    titledNro.logicalPath = "Release/switch/MyPort/MyPort.nro";
    titledNro.localPath = titledNro.logicalPath;
    titledNro.size = nro.size();
    titledManifest.files.push_back(titledNro);
    TaskFileRecord titledForwarder;
    titledForwarder.logicalPath =
        "Subway Surfers (port) [01d2c0b236000000].nsp";
    titledForwarder.localPath = titledForwarder.logicalPath;
    titledForwarder.size = 2048;
    titledForwarder.package = true;
    titledForwarder.action = TaskFileAction::Install;
    titledManifest.files.push_back(titledForwarder);
    assert(saveTaskFileManifest(titledRoot, titledManifest, error));
    DownloadManager titledManager(titledRoot, false);
    SwitchDeployService titledDeploy(titledManager, titledRoot, titledTarget);
    PortUninstallService titledUninstall(titledManager, titledRoot,
                                         titledTarget);
    SwitchDeployInspection titledInspection = titledDeploy.inspect(titledId);
    assert(titledInspection.canStart());
    assert(titledDeploy.start(titledId, error));
    for (int i = 0; i < 500 && titledDeploy.snapshot().active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(titledDeploy.snapshot().phase == SwitchDeployPhase::Completed);
    assert(titledDeploy.receiptState(titledId) ==
           SwitchDeployReceiptState::Valid);
    // The receipt carries the title: the plan matches with no metadata
    // hashes, and a different title id does not match.
    PortUninstallPlan titledPlan;
    assert(titledUninstall.plan("01d2c0b236000000", {}, titledPlan));
    assert(titledPlan.taskIds == std::vector<std::string>({titledId}));
    assert(titledPlan.switchFiles.size() == 1);
    PortUninstallPlan otherPlan;
    assert(!titledUninstall.plan("0100000000000000", {}, otherPlan));
    // A full removal through the title-linked plan cleans everything.
    PortUninstallReport titledReport;
    assert(titledUninstall.uninstallPort(
        titledPlan, [](std::string&) { return true; }, titledReport));
    assert(titledReport.complete());
    assert(!fs::exists(titledTarget + "/MyPort"));
    assert(!titledManager.snapshot(titledId));
    assert(!titledUninstall.receiptExists(titledId));
    titledDeploy.shutdown();
    titledManager.shutdown();

    // ------------------------------------------------------------------
    // Receipts written before titles were recorded (v2): the link title →
    // task is recovered from the bracketed id in the forwarder package name
    // inside the task manifest — the exact shape of a deployed port on
    // builds before this fix.
    const std::string legacyRoot = root + "/legacy";
    const std::string legacyData = legacyRoot + "/downloads/task";
    const std::string legacyTarget = legacyRoot + "/sd/switch";
    const std::string legacyId = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    fs::create_directories(legacyTarget);
    writeFile(legacyTarget + "/MyPort/MyPort.nro", nro);
    writeCompletedQueue(legacyRoot, legacyId, legacyData, nro.size());
    TaskFileManifest legacyManifest;
    legacyManifest.taskId = legacyId;
    TaskFileRecord legacyNro;
    legacyNro.logicalPath = "Release/switch/MyPort/MyPort.nro";
    legacyNro.localPath = legacyNro.logicalPath;
    legacyNro.size = nro.size();
    legacyManifest.files.push_back(legacyNro);
    TaskFileRecord legacyForwarder;
    legacyForwarder.logicalPath =
        "Subway Surfers (port) [01d2c0b236000000].nsp";
    legacyForwarder.localPath = legacyForwarder.logicalPath;
    legacyForwarder.size = 2048;
    legacyForwarder.package = true;
    legacyForwarder.action = TaskFileAction::Install;
    legacyManifest.files.push_back(legacyForwarder);
    assert(saveTaskFileManifest(legacyRoot, legacyManifest, error));
    writeV2Receipt(legacyRoot, legacyId,
                   {{"MyPort/MyPort.nro", nro.size()}},
                   {"MyPort/MyPort.nro"});
    DownloadManager legacyManager(legacyRoot, false);
    PortUninstallService legacyUninstall(legacyManager, legacyRoot,
                                         legacyTarget);
    // Matched with no metadata hashes: the forwarder name carries the id.
    PortUninstallPlan legacyPlan;
    assert(legacyUninstall.plan("01D2C0B236000000", {}, legacyPlan));
    assert(legacyPlan.taskIds == std::vector<std::string>({legacyId}));
    assert(legacyPlan.switchFiles.size() == 1);
    // A different title id does not match.
    assert(!legacyUninstall.plan("0100000000000000", {}, legacyPlan));
    legacyManager.shutdown();

    setStorageSpaceOverride(nullptr);
    fs::remove_all(root);

    {
        assert(isLayeredFsPayloadPath(
            "Russian Language Mod/atmopshere/contents/"
            "0100A3D008C5C000/romfs/a.bin"));
        assert(!isLayeredFsPayloadPath(
            "Russian Language Mod/atmopshere/contents/"
            "0100000000000042/romfs/a.bin"));

        const std::string modRoot = "/tmp/pipensx-layeredfs-deploy";
        const std::string modTarget = modRoot + "/sd/switch";
        const std::string modData = modRoot + "/downloads/task";
        fs::remove_all(modRoot);
        fs::create_directories(modTarget);
        const std::string payload = "RUS-TEXT";
        const std::string scarlet =
            "Russian Language Mod/atmopshere/contents/"
            "0100A3D008C5C000/romfs/message.txt";
        const std::string sysmod =
            "Russian Language Mod/atmopshere/contents/"
            "0100000000000042/romfs/sys.bin";
        writeFile(modData + "/" + scarlet, payload);
        writeFile(modData + "/" + sysmod, "SYS");
        writeFile(modData + "/readme.txt", "HI");

        TaskFileInventory inv;
        inv.taskId = "layeredfs";
        inv.rootPath = modData;
        inv.settled = true;
        inv.completeManifest = true;
        auto addMod = [&](const std::string& logical, uint64_t size) {
            TaskFileInfo file;
            file.logicalPath = logical;
            file.localPath = logical;
            file.absolutePath = modData + "/" + logical;
            file.size = size;
            file.action = TaskFileAction::Download;
            file.state = TaskFileState::Present;
            inv.files.push_back(std::move(file));
        };
        addMod(scarlet, payload.size());
        addMod(sysmod, 3);
        addMod("readme.txt", 2);

        StorageSpaceSnapshot modStorage;
        modStorage.available = true;
        modStorage.totalBytes = 1024 * 1024;
        modStorage.freeBytes = 1024 * 1024;
        setStorageSpaceOverride(&modStorage);
        SwitchDeployInspection got =
            inspectSwitchDeploy(std::move(inv), modTarget);
        assert(got.canStart());
        assert(got.plan.files.size() == 1);
        assert(got.plan.files[0].destinationRelativePath ==
               "atmosphere/contents/0100A3D008C5C000/romfs/message.txt");
        assert(got.plan.files[0].destinationPath ==
               modRoot + "/sd/atmosphere/contents/0100A3D008C5C000/"
                         "romfs/message.txt");
        assert(switchDeployCopiesAtmosphere(got.plan));

        TaskFileInventory sysOnly;
        sysOnly.taskId = "sysmodule";
        sysOnly.rootPath = modData;
        sysOnly.settled = true;
        sysOnly.completeManifest = true;
        TaskFileInfo sysFile;
        sysFile.logicalPath = sysmod;
        sysFile.localPath = sysmod;
        sysFile.absolutePath = modData + "/" + sysmod;
        sysFile.size = 3;
        sysFile.action = TaskFileAction::Download;
        sysFile.state = TaskFileState::Present;
        sysOnly.files.push_back(std::move(sysFile));
        assert(inspectSwitchDeploy(std::move(sysOnly), modTarget).problem ==
               SwitchDeployProblem::LayoutNotFound);

        setStorageSpaceOverride(nullptr);
        fs::remove_all(modRoot);
    }

    std::cout << "switch deploy tests passed\n";
    return 0;
}
