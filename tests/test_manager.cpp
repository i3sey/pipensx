#include "../src/app/download_manager.hpp"

extern "C" {
#include "../src/core/sha1.h"
}

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <unistd.h>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;
using pipensx::DownloadTask;
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

// Two files, one piece each (piece length = file length): the skipped file's
// piece is fully inside a STORAGE_FILE_SKIP range, so the startup scan can
// pre-mark it done without downloading. Used to prove a skipped file is not
// wanted.
static std::string makeSelectiveScanTorrent(const std::string& directory) {
    const std::string a = "AAAABBBB";
    const std::string b = "CCCCDDDD";
    uint8_t digesta[20], digestb[20];
    sha1(reinterpret_cast<const uint8_t*>(a.data()), a.size(), digesta);
    sha1(reinterpret_cast<const uint8_t*>(b.data()), b.size(), digestb);

    std::string torrent = "d8:announce18:http://127.0.0.1:14:infod5:filesl";
    torrent += "d6:lengthi8e4:pathl12:selected.binee";
    torrent += "d6:lengthi8e4:pathl14:unselected.binee";
    torrent += "e4:name14:selective-scan12:piece lengthi8e6:pieces40:";
    torrent.append(reinterpret_cast<const char*>(digesta), 20);
    torrent.append(reinterpret_cast<const char*>(digestb), 20);
    torrent += "ee";

    std::string path = directory + "/selective-scan.torrent";
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

// Encode a C++ string as a bencode string ("len:value")
static std::string bencodeStr(const std::string& s) {
    return std::to_string(s.size()) + ":" + s;
}

// Recursively remove a directory tree (test cleanup only)
static void removeAll(const std::string& path) {
    system(("rm -rf " + path).c_str());
}

// Create a single directory (ignores EEXIST)
static void makeDir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

// Write binary content to a file
static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static void testTorboxTaskPersistenceRoundTrip() {
    const std::string root = "/tmp/pipensx-manager-torbox-test";
    removeAll(root);
    makeDir(root);
    makeDir(root + "/downloads");
    makeDir(root + "/downloads/example-aabbccdd");

    std::string dataPath = root + "/downloads/example-aabbccdd";
    std::string state =
        "d5:tasksl"
        "d"
        "4:data" + bencodeStr(dataPath) +
        "5:error0:"
        "2:id40:aabbccddaabbccddaabbccddaabbccddaabbccdd"
        "8:metainfo0:"
        "4:mode7:install"
        "4:name7:Example"
        "13:package-counti1e"
        "13:packages-donei0e"
        "9:selection0:"
        "6:source6:torbox"
        "6:status6:queued"
        "9:torbox-idi297464e"
        "5:totali1000000e"
        "e"
        "e"
        "7:versioni4e"
        "e";
    writeFile(root + "/queue.bencode", state);

    pipensx::DownloadManager manager(root, false);
    auto tasks = manager.snapshot();
    assert(tasks.size() == 1);
    assert(tasks[0].source == pipensx::TaskSource::Debrid);
    assert(tasks[0].debridProvider == pipensx::DebridProviderKind::TorBox);
    assert(tasks[0].debridId == "297464");
    assert(tasks[0].status == pipensx::DownloadStatus::Queued);
    assert(tasks[0].metainfoPath.empty());

    // Round trip: save and reload
    std::string error;
    assert(manager.save(error));
    pipensx::DownloadManager reloaded(root, false);
    auto again = reloaded.snapshot();
    assert(again.size() == 1);
    assert(again[0].source == pipensx::TaskSource::Debrid);
    assert(again[0].debridProvider == pipensx::DebridProviderKind::TorBox);
    assert(again[0].debridId == "297464");

    removeAll(root);
}

static void testLegacyQueueLoadsAsTorrentSource() {
    const std::string root = "/tmp/pipensx-manager-legacy-test";
    removeAll(root);
    makeDir(root);
    makeDir(root + "/torrents");
    makeDir(root + "/downloads");
    makeDir(root + "/downloads/example-aabbccdd");

    // Create a real .torrent file using the existing fixture helper
    std::string torrentPath = makeTorrent(
        root + "/torrents", "example.bin", "payload");

    std::string dataPath = root + "/downloads/example-aabbccdd";

    // Write a v3 state (no source/torbox-id keys — legacy torrent task)
    std::string state =
        "d5:tasksl"
        "d"
        "4:data" + bencodeStr(dataPath) +
        "5:error0:"
        "2:id40:aabbccddaabbccddaabbccddaabbccddaabbccdd"
        "8:metainfo" + bencodeStr(torrentPath) +
        "4:mode8:download"
        "4:name7:Example"
        "13:package-counti0e"
        "13:packages-donei0e"
        "9:selection0:"
        "6:status6:queued"
        "5:totali1000000e"
        "e"
        "e"
        "7:versioni3e"
        "e";
    writeFile(root + "/queue.bencode", state);

    pipensx::DownloadManager manager(root, false);
    auto tasks = manager.snapshot();
    assert(tasks.size() == 1);
    assert(tasks[0].source == pipensx::TaskSource::Torrent);
    assert(tasks[0].debridId.empty());
    assert(tasks[0].status == pipensx::DownloadStatus::Queued);

    removeAll(root);
}

static void testV4TorrentActionsRemainTriState() {
    const std::string root = "/tmp/pipensx-manager-v4-actions-test";
    removeAll(root);
    makeDir(root);
    makeDir(root + "/torrents");
    makeDir(root + "/downloads");
    makeDir(root + "/downloads/example-aabbccdd");
    const std::string torrentPath = makeTorrent(
        root + "/torrents", "package.nsp", "payload");
    const std::string selection(
        1, static_cast<char>(pipensx::FileAction::Install));
    const std::string state =
        "d5:tasksl"
        "d"
        "4:data" + bencodeStr(root + "/downloads/example-aabbccdd") +
        "5:error0:"
        "2:id40:aabbccddaabbccddaabbccddaabbccddaabbccdd"
        "8:metainfo" + bencodeStr(torrentPath) +
        "4:mode7:install"
        "4:name7:Example"
        "13:package-counti1e"
        "13:packages-donei0e"
        "9:selection" + bencodeStr(selection) +
        "6:status6:queued"
        "5:totali1000000e"
        "e"
        "e"
        "7:versioni4e"
        "e";
    writeFile(root + "/queue.bencode", state);

    {
        pipensx::DownloadManager manager(root, false);
        const auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].source == pipensx::TaskSource::Torrent);
        assert(tasks[0].fileSelection.size() == 1);
        assert(tasks[0].fileSelection[0] ==
               static_cast<uint8_t>(pipensx::FileAction::Install));
    }
    removeAll(root);
}

static void testTorrentTaskWithEmptyMetainfoStillErrors() {
    const std::string root = "/tmp/pipensx-manager-empty-metainfo-test";
    removeAll(root);
    makeDir(root);
    makeDir(root + "/downloads");
    makeDir(root + "/downloads/example-aabbccdd");

    std::string dataPath = root + "/downloads/example-aabbccdd";

    // Write a v3 state (no source field — inferred as torrent) with empty
    // metainfo path. This should load as Error: torrent tasks require a
    // real metainfo file.
    std::string state =
        "d5:tasksl"
        "d"
        "4:data" + bencodeStr(dataPath) +
        "5:error0:"
        "2:id40:aabbccddaabbccddaabbccddaabbccddaabbccdd"
        "8:metainfo0:"
        "4:mode8:download"
        "4:name7:Example"
        "13:package-counti0e"
        "13:packages-donei0e"
        "9:selection0:"
        "6:status6:queued"
        "5:totali1000000e"
        "e"
        "e"
        "7:versioni3e"
        "e";
    writeFile(root + "/queue.bencode", state);

    pipensx::DownloadManager manager(root, false);
    auto tasks = manager.snapshot();
    assert(tasks.size() == 1);
    assert(tasks[0].source == pipensx::TaskSource::Torrent);
    assert(tasks[0].status == pipensx::DownloadStatus::Error);

    removeAll(root);
}

static void testImportDebridCatalogTask() {
    const std::string root = "/tmp/pipensx-manager-debrid-import";
    removeAll(root);

    pipensx::DownloadManager manager(root, false);
    pipensx::DebridImport import;
    import.infoHash = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    import.name = "Example Game";
    import.totalBytes = 1000000;
    import.debridId = "297464";
    import.provider = pipensx::DebridProviderKind::TorBox;
    import.mode = pipensx::TransferMode::StreamInstall;
    import.fileSelection = {1, 0};
    import.packageCount = 1;

    std::string taskId, error;
    assert(manager.importDebrid(import, taskId, error));
    assert(taskId == import.infoHash);
    auto tasks = manager.snapshot();
    assert(tasks.size() == 1);
    assert(tasks[0].source == pipensx::TaskSource::Debrid);
    assert(tasks[0].debridId == "297464");
    assert(tasks[0].metainfoPath.empty());
    assert(tasks[0].status == pipensx::DownloadStatus::Queued);
    assert(tasks[0].packageCount == 1);

    // Duplicate rejected by info hash.
    assert(!manager.importDebrid(import, taskId, error));
    assert(!error.empty());

    removeAll(root);
}

static void testImportDebridRejectsEmptySelection() {
    const std::string root = "/tmp/pipensx-manager-debrid-empty-selection";
    removeAll(root);

    pipensx::DownloadManager manager(root, false);
    pipensx::DebridImport import;
    import.infoHash = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    import.name = "Example Game";
    import.fileSelection = {
        static_cast<uint8_t>(pipensx::FileAction::Skip),
        static_cast<uint8_t>(pipensx::FileAction::Skip),
    };

    std::string taskId, error;
    assert(!manager.importDebrid(import, taskId, error));
    assert(error == "Select at least one file.");
    assert(manager.snapshot().empty());

    removeAll(root);
}

static void testImportDebridPickerCopiesTorrent() {
    const std::string root = "/tmp/pipensx-manager-debrid-picker";
    removeAll(root);
    makeDir(root);
    makeDir(root + "/src");

    std::string torrentPath = makeTorrent(
        root + "/src", "example.bin", "payload");

    std::string error;
    pipensx::TorrentPreview preview;
    assert(DownloadManager::previewTorrent(torrentPath, preview, error));
    assert(preview.infoHash.size() == 40);

    // DebridImport with torrentPath set, debridId empty.
    pipensx::DownloadManager manager(root + "/app", false);
    pipensx::DebridImport import;
    import.infoHash = preview.infoHash;
    import.name = preview.name;
    import.totalBytes = preview.totalBytes;
    import.debridId = "";
    import.provider = pipensx::DebridProviderKind::TorBox;
    import.torrentPath = torrentPath;
    import.mode = pipensx::TransferMode::DownloadOnly;

    std::string taskId;
    assert(manager.importDebrid(import, taskId, error));
    assert(taskId == preview.infoHash);

    auto tasks = manager.snapshot();
    assert(tasks.size() == 1);

    std::string expectedMetainfoPath =
        root + "/app/torrents/" + preview.infoHash + ".torrent";
    assert(tasks[0].metainfoPath == expectedMetainfoPath);

    struct stat st;
    assert(stat(expectedMetainfoPath.c_str(), &st) == 0);

    removeAll(root);
}

static void testTaskEtaUsesFreshProgressDomain() {
    DownloadTask task;
    task.status = DownloadStatus::Downloading;
    task.completedBytes = 1000;
    task.totalBytes = 3500;
    task.speedBytesPerSecond = 1000;
    task.downloadProgressUpdatedAtMs = 1000;
    auto eta = pipensx::taskEtaSeconds(task, 1000);
    assert(eta && *eta == 3);

    assert(!pipensx::taskEtaSeconds(
        task, 1001 + pipensx::kProgressRateStaleMs));
    pipensx::updateTaskDownloadProgress(task, 1500, 5000);
    eta = pipensx::taskEtaSeconds(task, 5000);
    assert(eta && *eta == 2);
    // Payload activity keeps ETA fresh before the next piece completes.
    pipensx::updateTaskDownloadProgress(task, 1500, 8000);
    eta = pipensx::taskEtaSeconds(task, 8000);
    assert(eta && *eta == 2);

    task.speedBytesPerSecond = 0;
    assert(!pipensx::taskEtaSeconds(task, 1000));

    task.status = DownloadStatus::Queued;
    pipensx::updateTaskInstallProgress(
        task, 100, 2100, DownloadStatus::Installing, 1000);
    assert(!pipensx::taskEtaSeconds(task, 1000));

    // A partial window is not enough to publish a noisy rate.
    pipensx::updateTaskInstallProgress(
        task, 600, 2100, DownloadStatus::Installing, 1500);
    assert(!pipensx::taskEtaSeconds(task, 1500));

    pipensx::updateTaskInstallProgress(
        task, 1100, 2100, DownloadStatus::Installing, 2000);
    assert(pipensx::currentInstallSpeed(task, 2000) == 1000);
    eta = pipensx::taskEtaSeconds(task, 2000);
    assert(eta && *eta == 1);

    assert(pipensx::currentInstallSpeed(
               task, 2000 + pipensx::kProgressRateStaleMs) == 1000);
    assert(!pipensx::taskEtaSeconds(
        task, 2001 + pipensx::kProgressRateStaleMs));

    pipensx::updateTaskInstallProgress(
        task, 2100, 2100, DownloadStatus::Committing, 6000);
    assert(!pipensx::taskEtaSeconds(task, 6000));
    assert(task.installSpeedBytesPerSecond == 0);

    pipensx::updateTaskInstallProgress(
        task, 0, 0, DownloadStatus::Installing, 7000);
    assert(!pipensx::taskEtaSeconds(task, 7000));
}

int main() {
    testTaskEtaUsesFreshProgressDomain();
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
    std::string v5Root = std::string(root) + "/v5-app";
    std::string fastResumeRoot = std::string(root) + "/fast-resume-app";
    std::string selectiveSource = makeSelectiveTorrent(root);
    std::string selectiveScanSource = makeSelectiveScanTorrent(root);

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
        // Fresh import into an empty data directory arms an all-zero trusted
        // bitfield (12-byte payload = 1 piece = 1 byte).
        assert(tasks[0].resumeBitfield == std::vector<uint8_t>(1, 0));
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
        manager.setTorrentingEnabled(true);  // torrenting is off by default
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

    // A skipped file must not be wanted. The trusted all-zero fast-resume
    // bitfield would skip the startup scan that pre-marks skipped ranges'
    // pieces done (storage_range_skipped), so the engine would download the
    // whole torrent and discard everything but the selection. With the scan
    // running, the skipped piece is done and only the selected piece stays
    // wanted.
    {
        std::string scanRoot = std::string(root) + "/selective-scan-app";
        DownloadManager manager(scanRoot, true);
        manager.setTorrentingEnabled(true);
        std::vector<uint8_t> actions{
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Skip),
        };
        std::string scanTaskId;
        assert(manager.importTorrentActions(
            selectiveScanSource, actions, scanTaskId, error));
        assert(manager.snapshot()[0].resumeBitfield.empty());
        DownloadTask task;
        bool downloading = false;
        for (int i = 0; i < 500 && !downloading; ++i) {
            task = manager.snapshot()[0];
            downloading = task.status == DownloadStatus::Downloading;
            if (!downloading)
                usleep(10000);
        }
        assert(downloading);
        assert(task.piecesTotal == 2);
        // The skipped piece was pre-marked done by the startup scan; the
        // selected piece is still wanted (no peers in this test).
        assert(task.piecesDone == 1);
        const std::string dataPath = task.dataPath + "/selective-scan";
        assert(access((dataPath + "/selected.bin").c_str(), F_OK) == 0);
        assert(access((dataPath + "/unselected.bin").c_str(), F_OK) != 0);
        manager.shutdown();
        assert(manager.remove(scanTaskId, true, error));
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
        assert(tasks[0].resumeBitfield == std::vector<uint8_t>(1, 0));
        assert(manager.remove(taskId, true, error));
        assert(manager.snapshot().empty());
    }

    // Fast resume: a version-5 queue with a resume-bf blob loads, and a user
    // recheck (verify) drops the trusted bitfield persistently.
    {
        {
            DownloadManager createDirs(v5Root, false);
        }
        pipensx::TorrentPreview preview;
        assert(DownloadManager::previewTorrent(source, preview, error));
        std::string metainfoPath =
            v5Root + "/torrents/" + preview.infoHash + ".torrent";
        std::string dataPath = v5Root + "/downloads/package.nsp-" +
                               preview.infoHash.substr(0, 8);
        copyFile(source, metainfoPath);

        std::string bitfield(1, '\x80');
        std::string queue = "d5:tasksl";
        queue += "d";
        queue += "4:data" + bstr(dataPath);
        queue += "5:error" + bstr("");
        queue += "2:id" + bstr(preview.infoHash);
        queue += "8:metainfo" + bstr(metainfoPath);
        queue += "4:mode" + bstr("download");
        queue += "4:name" + bstr(preview.name);
        queue += "13:package-counti0e";
        queue += "13:packages-donei0e";
        queue += "9:resume-bf" + bstr(bitfield);
        queue += "9:selection" + bstr(std::string(1, '\1'));
        queue += "6:status" + bstr("completed");
        queue += "5:totali12e";
        queue += "e";
        queue += "e7:versioni5ee";
        std::ofstream output(v5Root + "/queue.bencode",
                             std::ios::binary | std::ios::trunc);
        output << queue;
        output.close();

        DownloadManager manager(v5Root, false);
        auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        assert(tasks[0].status == DownloadStatus::Completed);
        assert((tasks[0].resumeBitfield == std::vector<uint8_t>{0x80}));
        assert(manager.verify(tasks[0].id));
        assert(manager.snapshot()[0].resumeBitfield.empty());
        {
            DownloadManager reloaded(v5Root, false);
            assert(reloaded.snapshot()[0].resumeBitfield.empty());
        }
        assert(manager.remove(tasks[0].id, true, error));
    }

    // Fast resume with a live worker: claiming the task persists the disarmed
    // state, an orderly teardown (pause) arms it again.
    {
        DownloadManager manager(fastResumeRoot, true);
        manager.setTorrentingEnabled(true);  // torrenting is off by default
        std::string frId;
        assert(manager.importTorrent(downloadOnlySource,
                                     TransferMode::DownloadOnly, frId, error));
        // Wait for Downloading, not merely "not Queued": the claim sets
        // Checking before the worker has polled the torrent even once, and
        // torrent_copy_have_bitfield() refuses to arm while startup_verifying
        // is still set. Pausing on Checking therefore races the first poll —
        // win it and the teardown arms, lose it and resumeBitfield stays
        // empty. Downloading is set only once stat.verifying has cleared,
        // which is exactly the precondition arming needs.
        bool disarmed = false;
        for (int i = 0; i < 500; ++i) {
            auto task = manager.snapshot()[0];
            if (task.status == DownloadStatus::Downloading &&
                task.resumeBitfield.empty()) {
                disarmed = true;
                break;
            }
            usleep(10000);
        }
        assert(disarmed);
        assert(manager.pause(frId));
        bool armed = false;
        for (int i = 0; i < 500; ++i) {
            auto task = manager.snapshot()[0];
            if (task.status == DownloadStatus::Paused &&
                !task.resumeBitfield.empty()) {
                armed = true;
                break;
            }
            usleep(10000);
        }
        assert(armed);
        manager.shutdown();
        {
            DownloadManager reloaded(fastResumeRoot, false);
            assert(reloaded.snapshot()[0].resumeBitfield ==
                   std::vector<uint8_t>(1, 0));
        }
        assert(manager.remove(frId, true, error));
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

    {
        // The scheduler's claim rule: a download-only task passes a
        // stream-install task blocked on the install token.
        pipensx::DownloadTask stream;
        stream.status = DownloadStatus::Queued;
        stream.mode = TransferMode::StreamInstall;
        assert(pipensx::taskClaimableUnderInstallToken(stream, false));
        assert(!pipensx::taskClaimableUnderInstallToken(stream, true));
        pipensx::DownloadTask plain = stream;
        plain.mode = TransferMode::DownloadOnly;
        assert(pipensx::taskClaimableUnderInstallToken(plain, true));
        pipensx::DownloadTask finished = plain;
        finished.status = DownloadStatus::Completed;
        assert(!pipensx::taskClaimableUnderInstallToken(finished, false));
    }

    {
        // Two download-only tasks leave Queued concurrently with two slots.
        std::string parallelRoot = std::string(root) + "/parallel-app";
        std::string firstSource =
            makeTorrent(root, "parallel-a.bin", "parallel payload a");
        std::string secondSource =
            makeTorrent(root, "parallel-b.bin", "parallel payload bb");
        DownloadManager manager(parallelRoot, true);
        manager.setTorrentingEnabled(true);  // torrenting is off by default
        manager.setMaxActiveDownloads(2);
        std::string firstId, secondId;
        assert(manager.importTorrent(
            firstSource, TransferMode::DownloadOnly, firstId, error));
        assert(manager.importTorrent(
            secondSource, TransferMode::DownloadOnly, secondId, error));
        auto activeCount = [&manager] {
            int active = 0;
            for (const auto& task : manager.snapshot())
                if (task.status == DownloadStatus::Checking ||
                    task.status == DownloadStatus::Downloading ||
                    task.status == DownloadStatus::Verifying)
                    ++active;
            return active;
        };
        bool both = false;
        for (int i = 0; i < 500 && !(both = activeCount() == 2); ++i)
            usleep(10000);
        assert(both);
        manager.shutdown();
        assert(manager.remove(firstId, true, error));
        assert(manager.remove(secondId, true, error));
        unlink(firstSource.c_str());
        unlink(secondSource.c_str());
        rmdir((parallelRoot + "/torrents").c_str());
        rmdir((parallelRoot + "/downloads").c_str());
        unlink((parallelRoot + "/queue.bencode").c_str());
        rmdir(parallelRoot.c_str());
    }

    {
        // Install token: with a stream install running, a second stream
        // install stays Queued while a download-only task passes it.
        std::string tokenRoot = std::string(root) + "/token-app";
        std::string streamB =
            makeTorrent(root, "package-b.nsp", "second package payload");
        DownloadManager manager(tokenRoot, true);
        manager.setTorrentingEnabled(true);  // torrenting is off by default
        manager.setMaxActiveDownloads(2);
        std::string streamAId, streamBId, plainId;
        assert(manager.importTorrent(
            source, TransferMode::StreamInstall, streamAId, error));
        assert(manager.importTorrent(
            streamB, TransferMode::StreamInstall, streamBId, error));
        assert(manager.importTorrent(
            downloadOnlySource, TransferMode::DownloadOnly, plainId, error));
        auto statusOf = [&manager](const std::string& id) {
            for (const auto& task : manager.snapshot())
                if (task.id == id)
                    return task.status;
            return DownloadStatus::Error;
        };
        auto started = [](DownloadStatus status) {
            return status == DownloadStatus::Checking ||
                   status == DownloadStatus::Downloading ||
                   status == DownloadStatus::Verifying;
        };
        bool ok = false;
        for (int i = 0; i < 500; ++i) {
            if ((ok = started(statusOf(streamAId)) &&
                      started(statusOf(plainId))))
                break;
            usleep(10000);
        }
        assert(ok);
        // The second stream install is behind the download-only task in
        // list order yet still waiting: only the token can block it.
        assert(statusOf(streamBId) == DownloadStatus::Queued);
        manager.shutdown();
        assert(manager.remove(streamAId, true, error));
        assert(manager.remove(streamBId, true, error));
        assert(manager.remove(plainId, true, error));
        unlink(streamB.c_str());
        rmdir((tokenRoot + "/torrents").c_str());
        rmdir((tokenRoot + "/downloads").c_str());
        unlink((tokenRoot + "/queue.bencode").c_str());
        rmdir(tokenRoot.c_str());
    }

    unlink(source.c_str());
    unlink(downloadOnlySource.c_str());
    unlink(readmeSource.c_str());
    unlink(selectiveSource.c_str());
    unlink(selectiveScanSource.c_str());
    rmdir((queueRoot + "/torrents").c_str());
    rmdir((queueRoot + "/downloads").c_str());
    unlink((queueRoot + "/queue.bencode").c_str());
    rmdir(queueRoot.c_str());
    rmdir((activeRoot + "/torrents").c_str());
    rmdir((activeRoot + "/downloads").c_str());
    unlink((activeRoot + "/queue.bencode").c_str());
    rmdir(activeRoot.c_str());
    rmdir((v5Root + "/torrents").c_str());
    rmdir((v5Root + "/downloads").c_str());
    unlink((v5Root + "/queue.bencode").c_str());
    rmdir(v5Root.c_str());
    rmdir((fastResumeRoot + "/torrents").c_str());
    rmdir((fastResumeRoot + "/downloads").c_str());
    unlink((fastResumeRoot + "/queue.bencode").c_str());
    rmdir(fastResumeRoot.c_str());
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

    testTorboxTaskPersistenceRoundTrip();
    testLegacyQueueLoadsAsTorrentSource();
    testV4TorrentActionsRemainTriState();
    testTorrentTaskWithEmptyMetainfoStillErrors();
    testImportDebridCatalogTask();
    testImportDebridRejectsEmptySelection();
    testImportDebridPickerCopiesTorrent();

    // --- Torrenting gate: a torrent task is refused while torrenting is off ---
    {
        char gateTemplate[] = "/tmp/pipensx-gate-XXXXXX";
        char* gateRoot = mkdtemp(gateTemplate);
        assert(gateRoot);

        // Minimal single-file .torrent (package) so importTorrent succeeds.
        const std::string payload = "test payload";
        uint8_t digest[20];
        sha1(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
             digest);
        std::string torrent = "d8:announce14:http://tracker4:infod6:lengthi";
        torrent += std::to_string(payload.size());
        torrent += "e4:name11:package.nsp12:piece lengthi";
        torrent += std::to_string(payload.size());
        torrent += "e6:pieces20:";
        torrent.append(reinterpret_cast<const char*>(digest), 20);
        torrent += "ee";
        std::string tpath = std::string(gateRoot) + "/g.torrent";
        std::ofstream(tpath, std::ios::binary)
            .write(torrent.data(),
                   static_cast<std::streamsize>(torrent.size()));

        DownloadManager manager(std::string(gateRoot) + "/app", true);
        manager.setTorrentingEnabled(false);
        std::string id, err;
        assert(manager.importTorrent(tpath, id, err));

        // Worker should mark it Error (gate fires before any network use).
        bool sawError = false;
        for (int i = 0; i < 100 && !sawError; ++i) {
            for (const DownloadTask& t : manager.snapshot())
                if (t.id == id && t.status == DownloadStatus::Error &&
                    t.error.find("Torrenting disabled") != std::string::npos)
                    sawError = true;
            if (!sawError)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(sawError);
        manager.shutdown();
    }

    std::puts("manager tests passed");
    return 0;
}
