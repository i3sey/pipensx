#include "app/catalog_batch_installer.hpp"

extern "C" {
#include "core/sha1.h"
}

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace pipensx;

static std::string makeTorrent(const std::string& directory) {
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

    std::string path = directory + "/source.torrent";
    std::ofstream output(path, std::ios::binary);
    output.write(torrent.data(), static_cast<std::streamsize>(torrent.size()));
    return path;
}

static bool copyFile(const std::string& source, const std::string& target) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(target, std::ios::binary);
    output << input.rdbuf();
    return input.good() || input.eof();
}

int main() {
    char rootTemplate[] = "/tmp/pipensx-batch-XXXXXX";
    char* root = mkdtemp(rootTemplate);
    assert(root);
    std::string source = makeTorrent(root);

    TorrentPreview sourcePreview;
    std::string error;
    assert(DownloadManager::previewTorrent(source, sourcePreview, error));

    CatalogEntry entry;
    entry.title = "Game";
    entry.infoHash = sourcePreview.infoHash;
    entry.magnetUri = "magnet:test";

    {
        CatalogBatchInstaller installer(
            root,
            [source](const CatalogEntry&, const std::string& target,
                     std::atomic<bool>&,
                     const MagnetResolver::ProgressCallback&,
                     std::vector<uint8_t>&,
                     std::string&) { return copyFile(source, target); });
        std::atomic<bool> cancelled{false};
        BatchPreparation prepared = installer.prepare(
            {entry}, StreamSelection::PackagesOnly, cancelled, {});

        assert(!prepared.cancelled());
        assert(prepared.failures().empty());
        assert(prepared.items().size() == 1);
        assert(prepared.items()[0].mode == TransferMode::StreamInstall);
        assert(prepared.items()[0].selection.empty());
        assert(prepared.items()[0].space.packageFiles == 1);
    }

    {
        CatalogEntry bad = entry;
        bad.title = "Unavailable";
        bad.magnetUri = "magnet:bad";
        CatalogBatchInstaller installer(
            root,
            [source](const CatalogEntry& entry, const std::string& target,
                     std::atomic<bool>&,
                     const MagnetResolver::ProgressCallback&,
                     std::vector<uint8_t>&,
                     std::string& error) {
                if (entry.magnetUri == "magnet:bad") {
                    error = "No usable peers.";
                    return false;
                }
                return copyFile(source, target);
            });
        std::atomic<bool> cancelled{false};
        BatchPreparation prepared = installer.prepare(
            {bad, entry}, StreamSelection::PackagesOnly, cancelled, {});

        assert(prepared.items().size() == 1);
        assert(prepared.items()[0].entry.title == "Game");
        assert(prepared.failures().size() == 1);
        assert(prepared.failures()[0].entry.title == "Unavailable");
        assert(prepared.failures()[0].error == "No usable peers.");
    }

    {
        const std::vector<uint8_t> initialPeers{
            93, 184, 216, 34, 0x1a, 0xe1,
            1, 1, 1, 1, 0xc8, 0xd5,
        };
        CatalogBatchInstaller installer(
            root,
            [source, initialPeers](
                const CatalogEntry&, const std::string& target,
                std::atomic<bool>&,
                const MagnetResolver::ProgressCallback&,
                std::vector<uint8_t>& peers, std::string&) {
                peers = initialPeers;
                return copyFile(source, target);
            });
        std::atomic<bool> cancelled{false};
        BatchPreparation prepared = installer.prepare(
            {entry}, StreamSelection::PackagesOnly, cancelled, {});
        assert(prepared.items().size() == 1);
        assert(prepared.items()[0].initialPeers == initialPeers);
        const std::string temporary = prepared.items()[0].torrentPath;
        assert(access(temporary.c_str(), F_OK) == 0);

        const std::string appRoot = std::string(root) + "/app";
        DownloadManager manager(appRoot, false);
        BatchEnqueueResult queued = installer.enqueue(prepared, manager);
        assert(queued.failures.empty());
        assert(queued.taskIds.size() == 1);
        assert(queued.queuedInfoHashes ==
               std::vector<std::string>{entry.infoHash});
        assert(access(temporary.c_str(), F_OK) != 0);
        assert(manager.snapshot().size() == 1);
        assert(manager.snapshot()[0].mode == TransferMode::StreamInstall);
        assert(manager.snapshot()[0].initialPeers == initialPeers);

        assert(manager.remove(queued.taskIds[0], true, error));
        unlink((appRoot + "/queue.bencode").c_str());
        rmdir((appRoot + "/torrents").c_str());
        rmdir((appRoot + "/downloads").c_str());
        rmdir(appRoot.c_str());
    }

    {
        CatalogBatchInstaller installer(
            root,
            [source](const CatalogEntry&, const std::string& target,
                     std::atomic<bool>& cancelled,
                     const MagnetResolver::ProgressCallback&,
                     std::vector<uint8_t>&,
                     std::string&) {
                const bool copied = copyFile(source, target);
                cancelled.store(true);
                return copied;
            });
        std::atomic<bool> cancelled{false};
        BatchPreparation prepared = installer.prepare(
            {entry}, StreamSelection::PackagesOnly, cancelled, {});
        assert(!prepared.cancelled());
        assert(prepared.failures().empty());
        assert(prepared.items().size() == 1);
    }

    {
        std::string temporary;
        CatalogBatchInstaller installer(
            root,
            [&temporary](const CatalogEntry&, const std::string& target,
                         std::atomic<bool>& cancelled,
                         const MagnetResolver::ProgressCallback&,
                         std::vector<uint8_t>&,
                         std::string& error) {
                temporary = target;
                std::ofstream partial(target, std::ios::binary);
                partial << "partial";
                partial.close();
                cancelled.store(true);
                error = "Cancelled.";
                return false;
            });
        std::atomic<bool> cancelled{false};
        BatchPreparation prepared = installer.prepare(
            {entry}, StreamSelection::PackagesOnly, cancelled, {});
        assert(prepared.cancelled());
        assert(prepared.items().empty());
        assert(prepared.failures().empty());
        assert(!temporary.empty());
        assert(access(temporary.c_str(), F_OK) != 0);
    }

    unlink(source.c_str());
    rmdir(root);
    std::puts("batch install tests passed");
    return 0;
}
