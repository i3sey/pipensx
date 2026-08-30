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

void append32(std::string& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<char>(value >> (i * 8)));
}

void append64(std::string& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<char>(value >> (i * 8)));
}

std::string makeTestNsp(const std::string& nca) {
    const std::string name = "00112233445566778899aabbccddeeff.nca";
    std::string out = "PFS0";
    append32(out, 1);
    append32(out, static_cast<uint32_t>(name.size() + 1));
    append32(out, 0);
    append64(out, 0);
    append64(out, nca.size());
    append32(out, 0);
    append32(out, 0);
    out += name;
    out.push_back('\0');
    out += nca;
    return out;
}

void writeCompletedQueue(const std::string& root, const std::string& taskId,
                         const std::string& dataPath, uint64_t total,
                         const std::string& mode = "download",
                         const std::string& status = "completed",
                         uint32_t packageCount = 0,
                         uint32_t packagesDone = 0) {
    std::string queue = "d5:tasksl";
    queue += "d9:completed" + std::string("i") + std::to_string(total) + "e";
    queue += "4:data" + bstr(dataPath);
    queue += "9:debrid-id" + bstr("ready");
    queue += "5:error" + bstr("");
    queue += "2:id" + bstr(taskId);
    queue += "8:metainfo" + bstr("");
    queue += "4:mode" + bstr(mode);
    queue += "4:name" + bstr("Port release");
    queue += "13:package-counti" + std::to_string(packageCount) +
             "e13:packages-donei" + std::to_string(packagesDone) + "e";
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
    assert(isPortArchiveName("myswitch.7z"));
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
        assert(mask[0] == static_cast<uint8_t>(FileAction::Download));
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
    {
        TorrentPreview preview;
        preview.multi = true;
        preview.name = "GTA V port";
        preview.files = {
            {"atmosphere/contents/0100B00B51230000/romfs/common.rpf",
             100, false, false, false},
            {"atmosphere/contents/0100B00B51230000/romfs/switch/x64.rpf",
             200, false, false, false},
            {"atmosphere/contents/0100B00B51230000/exefs/main.npdm",
             10, false, false, false},
            {"Grand Theft Auto V [0100b00b51230000].nsp",
             50, true, false, false},
            {"readme.txt", 5, false, false, false},
        };
        assert(torrentHasLayeredFsPayload(preview));
        assert(torrentPortLayoutDetected(preview));
        const auto mask = selectPortInstallActions(preview);
        assert(mask.size() == 5);
        assert(mask[0] == static_cast<uint8_t>(FileAction::Download));
        assert(mask[1] == static_cast<uint8_t>(FileAction::Download));
        assert(mask[2] == static_cast<uint8_t>(FileAction::Skip));
        assert(mask[3] == static_cast<uint8_t>(FileAction::Download));
        assert(mask[4] == static_cast<uint8_t>(FileAction::Skip));
        assert(isLayeredFsRomfsPath(
            "prefix/atmosphere/contents/0100b00b51230000/romfs/a"));
        assert(!isLayeredFsRomfsPath(
            "atmosphere/contents/not-a-title/romfs/a"));
        assert(!isLayeredFsRomfsPath(
            "atmosphere/contents/0100b00b51230000/exefs/a"));
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
    SwitchDeployInspection multiPort =
        inspectSwitchDeploy(std::move(ambiguous), target);
    assert(multiPort.canStart());
    assert(multiPort.plan.files.size() == 2);

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
    assert(!autoDeploy.snapshot().active());
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
    assert(oneTapDeploy.snapshot().phase == SwitchDeployPhase::Preparing);
    assert(oneTapDeploy.snapshot().taskId == oneTapId);
    assert(oneTapDeploy.start(oneTapId, error, false));
    oneTapDeploy.dismissDeployOffer(oneTapId);
    assert(oneTapDeploy.snapshot().active());
    for (int i = 0; i < 500 && oneTapDeploy.snapshot().active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(oneTapDeploy.snapshot().phase == SwitchDeployPhase::Completed);
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

    {
        const std::string remapRoot = root + "-remap";
        const std::string remapTarget = remapRoot + "/sd/switch";
        const std::string remapData = remapRoot + "/downloads/remap-task";
        fs::remove_all(remapRoot);
        fs::create_directories(remapTarget);
        const std::string remapNro = nroBytes();
        writeFile(remapData + "/_files/000001_MyPort.nro", remapNro);
        TaskFileInventory remapInventory;
        remapInventory.taskId = "remap-task-id";
        remapInventory.rootPath = remapData;
        remapInventory.settled = true;
        remapInventory.completeManifest = true;
        TaskFileInfo remapFile;
        remapFile.logicalPath =
            "VeryLongReleaseName/switch/MyPort/MyPort.nro";
        remapFile.localPath = "_files/000001_MyPort.nro";
        remapFile.absolutePath = remapData + "/_files/000001_MyPort.nro";
        remapFile.size = remapNro.size();
        remapFile.action = TaskFileAction::Download;
        remapFile.state = TaskFileState::Present;
        remapInventory.files.push_back(std::move(remapFile));
        SwitchDeployInspection remapInspection =
            inspectSwitchDeploy(std::move(remapInventory), remapTarget);
        assert(remapInspection.canStart());
        assert(remapInspection.plan.files.size() == 1);
        assert(remapInspection.plan.files[0].destinationRelativePath ==
               "MyPort/MyPort.nro");
        fs::remove_all(remapRoot);
    }

    {
        const std::string cosmoRoot = root + "-cosmo";
        const std::string cosmoTarget = cosmoRoot + "/sd/switch";
        const std::string cosmoData = cosmoRoot + "/downloads/lain";
        fs::remove_all(cosmoRoot);
        fs::create_directories(cosmoTarget);
        const std::string cosmoNro = nroBytes();
        writeFile(cosmoData + "/_files/000001_LainNX.nro", cosmoNro);
        TaskFileInventory cosmoInventory;
        cosmoInventory.taskId = "lain-cosmo";
        cosmoInventory.rootPath = cosmoData;
        cosmoInventory.settled = true;
        cosmoInventory.completeManifest = true;
        TaskFileInfo ghost;
        ghost.logicalPath =
            "Lain [NSP]/switch/LainNX/.cosmo";
        ghost.localPath = "_files/000000_.cosmo";
        ghost.absolutePath = cosmoData + "/_files/000000_.cosmo";
        ghost.size = 0;
        ghost.action = TaskFileAction::Download;
        ghost.state = TaskFileState::Present;
        cosmoInventory.files.push_back(std::move(ghost));
        TaskFileInfo nro;
        nro.logicalPath = "Lain [NSP]/switch/LainNX/LainNX.nro";
        nro.localPath = "_files/000001_LainNX.nro";
        nro.absolutePath = cosmoData + "/_files/000001_LainNX.nro";
        nro.size = cosmoNro.size();
        nro.action = TaskFileAction::Download;
        nro.state = TaskFileState::Present;
        cosmoInventory.files.push_back(std::move(nro));
        SwitchDeployInspection cosmoInspection =
            inspectSwitchDeploy(std::move(cosmoInventory), cosmoTarget);
        assert(cosmoInspection.canStart());
        assert(cosmoInspection.plan.files.size() == 1);
        assert(cosmoInspection.plan.ignoredFiles == 1);
        assert(cosmoInspection.plan.files[0].destinationRelativePath ==
               "LainNX/LainNX.nro");
        fs::remove_all(cosmoRoot);
    }

    {
        const std::string bonusArchive = data + "/bonus.zip";
        writeFile(bonusArchive, "not a port archive");
        TaskFileInventory archiveInventory;
        archiveInventory.taskId = "archive-without-port";
        archiveInventory.rootPath = data;
        archiveInventory.settled = true;
        archiveInventory.completeManifest = true;
        addPresent(archiveInventory, "bonus.zip", bonusArchive,
                   fs::file_size(bonusArchive));
        SwitchDeployInspection archiveInspection =
            inspectSwitchDeploy(std::move(archiveInventory), target);
        assert(archiveInspection.problem == SwitchDeployProblem::NotAPort);
    }

    {
        TaskFileInventory nszInventory;
        nszInventory.taskId = "nsz-only";
        nszInventory.rootPath = data;
        nszInventory.settled = true;
        nszInventory.completeManifest = true;
        TaskFileInfo nszFile;
        nszFile.logicalPath = "Plants_vs_Zombies.nsz";
        nszFile.localPath = "_files/000000_Plants_vs_Zombies.nsz";
        nszFile.absolutePath = data + "/_files/000000_Plants_vs_Zombies.nsz";
        nszFile.size = 1024;
        nszFile.package = true;
        nszFile.action = TaskFileAction::Download;
        nszFile.state = TaskFileState::Present;
        nszInventory.files.push_back(std::move(nszFile));
        SwitchDeployInspection nszInspection =
            inspectSwitchDeploy(std::move(nszInventory), target);
        assert(nszInspection.problem == SwitchDeployProblem::NotAPort);
    }

    {
        TaskFileInventory mixedInventory;
        mixedInventory.taskId = "nsp-plus-data";
        mixedInventory.rootPath = data;
        mixedInventory.settled = true;
        mixedInventory.completeManifest = true;
        TaskFileInfo nspFile;
        nspFile.logicalPath = "Laingame.nsp";
        nspFile.localPath = "_files/000000_Laingame.nsp";
        nspFile.size = 4096;
        nspFile.package = true;
        nspFile.action = TaskFileAction::Install;
        nspFile.state = TaskFileState::Installed;
        mixedInventory.files.push_back(std::move(nspFile));
        TaskFileInfo dataFile;
        dataFile.logicalPath = "roms/LAIN.BIN";
        dataFile.localPath = "_files/000001_LAIN.BIN";
        dataFile.size = 1024;
        dataFile.action = TaskFileAction::Download;
        dataFile.state = TaskFileState::Present;
        mixedInventory.files.push_back(std::move(dataFile));
        SwitchDeployInspection mixedInspection =
            inspectSwitchDeploy(std::move(mixedInventory), target);
        assert(mixedInspection.problem == SwitchDeployProblem::LayoutNotFound);
    }

    // Unified port transaction: packages are downloaded to disk, payload is
    // committed first, and only then is the local NSP fed to the installer.
    {
        const std::string txRoot = root + "/transaction";
        const std::string txData = txRoot + "/downloads/task";
        const std::string txTarget = txRoot + "/sd/switch";
        const std::string txId =
            "1234512345123451234512345123451234512345";
        fs::create_directories(txTarget);
        const std::string txNro = nroBytes();
        const std::string txPackage = makeTestNsp(std::string(4096, 'N'));
        const std::string badPackage(txPackage.size(), 'X');
        writeFile(txData + "/Release/Game/Game.nro", txNro);
        writeFile(txData + "/Release/Forwarder.nsp", badPackage);
        writeCompletedQueue(txRoot, txId, txData,
                            txNro.size() + txPackage.size(),
                            "port", "completed", 1, 0);
        TaskFileManifest txManifest;
        txManifest.taskId = txId;
        TaskFileRecord txNroFile;
        txNroFile.logicalPath = "Release/Game/Game.nro";
        txNroFile.localPath = txNroFile.logicalPath;
        txNroFile.size = txNro.size();
        txManifest.files.push_back(txNroFile);
        TaskFileRecord txPackageFile;
        txPackageFile.logicalPath = "Release/Forwarder.nsp";
        txPackageFile.localPath = txPackageFile.logicalPath;
        txPackageFile.size = txPackage.size();
        txPackageFile.package = true;
        txPackageFile.action = TaskFileAction::Download;
        txManifest.files.push_back(txPackageFile);
        assert(saveTaskFileManifest(txRoot, txManifest, error));
        DownloadManager txManager(txRoot, false);
        assert(txManager.snapshot(txId)->mode == TransferMode::PortInstall);
        SwitchDeployService txDeploy(txManager, txRoot, txTarget);
        SwitchDeployInspection txInspection = txDeploy.inspect(txId);
        assert(txInspection.canStart());
        assert(txInspection.plan.packages.size() == 1);

        // Package failure keeps the deployed payload and its receipt. A retry
        // must skip payload deployment and resume at the package stage.
        assert(txDeploy.start(txId, error));
        for (int i = 0; i < 500 && txDeploy.snapshot().active(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(txDeploy.snapshot().phase == SwitchDeployPhase::Failed);
        auto txTask = txManager.snapshot(txId);
        assert(txTask && txTask->status == DownloadStatus::Completed);
        assert(txTask->packagesInstalled == 0);
        assert(!txTask->error.empty());
        assert(fs::exists(txTarget + "/Game/Game.nro"));
        assert(txDeploy.receiptState(txId) == SwitchDeployReceiptState::Valid);

        writeFile(txData + "/Release/Forwarder.nsp", txPackage);
        txInspection = txDeploy.inspect(txId);
        assert(txInspection.canStart());
        assert(txInspection.plan.files.empty());
        assert(txInspection.plan.archives.empty());
        assert(txInspection.plan.packages.size() == 1);
        assert(txDeploy.start(txId, error));
        for (int i = 0; i < 500 && txDeploy.snapshot().active(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(txDeploy.snapshot().phase == SwitchDeployPhase::Completed);
        txTask = txManager.snapshot(txId);
        assert(txTask && txTask->status == DownloadStatus::Installed);
        assert(txTask->packagesInstalled == 1);
        txDeploy.shutdown();
        txManager.shutdown();
    }

    // FAT32 stores logical files above 4 GiB as DBI split folders. The
    // LayeredFS planner accepts the folder as one payload file (the atomic
    // rename preserves Horizon's archive attribute) rather than rejecting it
    // as a directory or deploying its 00/01 parts as separate RomFS files.
    {
        constexpr uint64_t splitPart = 0xFFFF0000u;
        constexpr uint64_t splitSize = 0x100000000ull;
        const std::string splitRoot = root + "/layered-split";
        const std::string splitTarget = splitRoot + "/sd/switch";
        const std::string splitSource = splitRoot +
            "/downloads/task/Release/atmosphere/contents/"
            "0100B00B51230000/romfs/common.rpf";
        fs::create_directories(splitSource);
        writeFile(splitSource + "/00", "");
        writeFile(splitSource + "/01", "");
        fs::resize_file(splitSource + "/00", splitPart);
        fs::resize_file(splitSource + "/01", splitSize - splitPart);
        fs::create_directories(splitTarget);
        TaskFileInventory splitInventory;
        splitInventory.taskId = "layered-split";
        splitInventory.rootPath = splitRoot + "/downloads/task";
        splitInventory.settled = true;
        splitInventory.completeManifest = true;
        addPresent(splitInventory,
                   "Release/atmosphere/contents/0100B00B51230000/"
                   "romfs/common.rpf",
                   splitSource, splitSize);
        SwitchDeployInspection splitInspection =
            inspectSwitchDeploy(std::move(splitInventory), splitTarget);
        assert(splitInspection.canStart());
        assert(splitInspection.plan.files.size() == 1);
        assert(splitInspection.plan.files[0].moveSource);
        assert(splitInspection.plan.bytesToMove == splitSize);
        fs::remove_all(splitRoot);
    }

    // Atmosphere LayeredFS + NSP transaction: every RomFS member is selected,
    // moved (not copied) to the SD root, tied to the package CNMT title id,
    // and recorded with its destination root for exact uninstall.
    {
        const std::string layeredRoot = root + "/layered";
        const std::string layeredData = layeredRoot + "/downloads/task";
        const std::string layeredTarget = layeredRoot + "/sd/switch";
        const std::string layeredSd = layeredRoot + "/sd";
        const std::string layeredId =
            "9876598765987659876598765987659876598765";
        const std::string titleId = "0100B00B51230000";
        const std::string common = "COMMON-RPF";
        const std::string switchAsset = "SWITCH-ASSET";
        const std::string layeredPackage =
            makeTestNsp(std::string(1024, 'L'));
        fs::create_directories(layeredTarget);
        writeFile(layeredData +
                      "/Release/atmosphere/contents/" + titleId +
                      "/romfs/common.rpf",
                  common);
        writeFile(layeredData +
                      "/Release/atmosphere/contents/" + titleId +
                      "/romfs/switch/data.rpf",
                  switchAsset);
        writeFile(layeredData + "/GTA V [" + titleId + "].nsp",
                  layeredPackage);
        writeCompletedQueue(layeredRoot, layeredId, layeredData,
                            common.size() + switchAsset.size() +
                                layeredPackage.size(),
                            "port", "completed", 1, 0);
        TaskFileManifest layeredManifest;
        layeredManifest.taskId = layeredId;
        auto addLayered = [&](const std::string& logical, uint64_t size,
                              bool packageFile = false) {
            TaskFileRecord record;
            record.logicalPath = logical;
            record.localPath = logical;
            record.size = size;
            record.package = packageFile;
            record.action = TaskFileAction::Download;
            layeredManifest.files.push_back(std::move(record));
        };
        addLayered("Release/atmosphere/contents/" + titleId +
                       "/romfs/common.rpf",
                   common.size());
        addLayered("Release/atmosphere/contents/" + titleId +
                       "/romfs/switch/data.rpf",
                   switchAsset.size());
        addLayered("GTA V [" + titleId + "].nsp",
                   layeredPackage.size(), true);
        assert(saveTaskFileManifest(layeredRoot, layeredManifest, error));
        writeFile(layeredSd + "/config/sys-clk/config.ini",
                  "[0100B00B51230000]\nhandheld_cpu=1785\n");

        DownloadManager layeredManager(layeredRoot, false);
        SwitchDeployService layeredDeploy(layeredManager, layeredRoot,
                                           layeredTarget);
        PortUninstallService layeredUninstall(layeredManager, layeredRoot,
                                               layeredTarget);
        storage.freeBytes = 0; // Moving needs no second payload-sized pool.
        setStorageSpaceOverride(&storage);
        SwitchDeployInspection layeredInspection =
            layeredDeploy.inspect(layeredId);
        assert(layeredInspection.canStart());
        assert(layeredInspection.plan.layeredFs);
        assert(layeredInspection.plan.files.size() == 2);
        assert(layeredInspection.plan.bytesToCopy == 0);
        assert(layeredInspection.plan.bytesToMove ==
               common.size() + switchAsset.size());
        assert(layeredInspection.plan.performanceToolDetected);
        assert(layeredInspection.plan.performanceProfileDetected);
        for (const SwitchDeployEntry& entry : layeredInspection.plan.files) {
            assert(entry.target == SwitchDeployTarget::SdRoot);
            assert(entry.moveSource);
            assert(entry.destinationRelativePath.rfind(
                       "atmosphere/contents/" + titleId + "/romfs/", 0) == 0);
        }

        // Unknown files mean a mixed LayeredFS version/mod set. Installation
        // is blocked rather than deleting user mods or merging versions.
        const std::string unknown = layeredSd + "/atmosphere/contents/" +
            titleId + "/romfs/user-mod.rpf";
        writeFile(unknown, "KEEP-MOD");
        layeredInspection = layeredDeploy.inspect(layeredId);
        assert(layeredInspection.problem == SwitchDeployProblem::Conflict);
        fs::remove(unknown);

        assert(layeredDeploy.start(layeredId, error));
        for (int i = 0; i < 500 && layeredDeploy.snapshot().active(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(layeredDeploy.snapshot().phase == SwitchDeployPhase::Completed);
        const std::string deployedCommon =
            layeredSd + "/atmosphere/contents/" + titleId +
            "/romfs/common.rpf";
        const std::string deployedSwitch =
            layeredSd + "/atmosphere/contents/" + titleId +
            "/romfs/switch/data.rpf";
        assert(fs::exists(deployedCommon));
        assert(fs::exists(deployedSwitch));
        assert(!fs::exists(layeredData +
                           "/Release/atmosphere/contents/" + titleId +
                           "/romfs/common.rpf"));
        assert(fs::exists(layeredData + "/GTA V [" + titleId + "].nsp"));
        assert(layeredDeploy.receiptState(layeredId) ==
               SwitchDeployReceiptState::Valid);
        const auto layeredTask = layeredManager.snapshot(layeredId);
        assert(layeredTask && layeredTask->status == DownloadStatus::Installed);
        assert(layeredTask->packagesInstalled == 1);

        PortUninstallPlan layeredPlan;
        assert(layeredUninstall.plan(titleId, {}, layeredPlan));
        assert(layeredPlan.switchFiles.empty());
        assert(layeredPlan.sdRootFiles.size() == 2);
        assert(layeredPlan.sdRootBytes == common.size() + switchAsset.size());
        writeFile(layeredSd + "/atmosphere/contents/" + titleId +
                      "/romfs/user-mod.rpf",
                  "KEEP-MOD");
        PortUninstallReport layeredReport;
        assert(layeredUninstall.uninstallPort(
            layeredPlan, [](std::string&) { return true; }, layeredReport));
        assert(layeredReport.complete());
        assert(!fs::exists(deployedCommon));
        assert(!fs::exists(deployedSwitch));
        assert(fs::exists(layeredSd + "/atmosphere/contents/" + titleId +
                          "/romfs/user-mod.rpf"));
        layeredDeploy.shutdown();
        layeredManager.shutdown();
    }

    // Crash recovery reverses an atomic move that landed before its receipt.
    {
        const std::string recoveryRoot = root + "/move-recovery";
        const std::string recoveryTarget = recoveryRoot + "/sd/switch";
        const std::string recoverySource =
            recoveryRoot + "/downloads/task/Release/file.rpf";
        const std::string recoveryDestination =
            recoveryRoot + "/sd/atmosphere/contents/0100B00B51230000/"
                           "romfs/file.rpf";
        fs::create_directories(recoveryTarget);
        writeFile(recoveryDestination, "RECOVER");
        std::string moveJob = "d5:filesl";
        moveJob += "d4:dest" + bstr(recoveryDestination);
        moveJob += "4:sizei7e";
        moveJob += "6:source" + bstr(recoverySource) + "ee";
        moveJob += "4:task" + bstr("move-recovery");
        moveJob += "7:versioni1ee";
        writeFile(recoveryRoot + "/deploy-move-job.bencode", moveJob);
        DownloadManager recoveryManager(recoveryRoot, false);
        {
            SwitchDeployService recovery(recoveryManager, recoveryRoot,
                                         recoveryTarget);
            assert(fs::exists(recoverySource));
            assert(!fs::exists(recoveryDestination));
            assert(!fs::exists(recoveryRoot +
                               "/deploy-move-job.bencode"));
        }
        recoveryManager.shutdown();
    }

    storage.freeBytes = 1024 * 1024;
    setStorageSpaceOverride(nullptr);
    fs::remove_all(root);
    std::cout << "switch deploy tests passed\n";
    return 0;
}
