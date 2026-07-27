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
    std::string v5Root = std::string(root) + "/v5-app";
    std::string fastResumeRoot = std::string(root) + "/fast-resume-app";
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
    std::puts("manager tests passed");
    return 0;
}
