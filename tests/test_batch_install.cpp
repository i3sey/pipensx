#include "app/catalog_batch_installer.hpp"
#include "app/torbox_client.hpp"
#include "app/torbox_provider.hpp"

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

    {
        // TorBox prepare: one entry that resolves to a single .nsp package.
        std::vector<std::pair<std::string, std::string>> script = {
            {"createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":7}}"},
            {"mylist", "{\"success\":true,\"data\":{\"id\":7,\"name\":\"Game\","
                       "\"size\":1000,\"progress\":1.0,"
                       "\"download_state\":\"completed\","
                       "\"download_finished\":true,\"download_present\":true,"
                       "\"files\":[{\"id\":1,\"name\":\"game.nsp\","
                       "\"size\":1000}]}}"},
        };
        TorboxTransport transport =
            [script = std::make_shared<std::vector<std::pair<std::string,
                 std::string>>>(script)]
            (const TorboxHttpRequest& req, TorboxHttpResponse& res,
             std::string&) mutable {
                for (auto it = script->begin(); it != script->end(); ++it)
                    if (req.url.find(it->first) != std::string::npos) {
                        res.status = 200; res.body = it->second;
                        if (it->first == std::string("createtorrent"))
                            script->erase(it);
                        return true;
                    }
                res.status = 200;
                res.body = "{\"success\":false,\"detail\":\"unexpected\"}";
                return true;
            };

        CatalogEntry tb = entry;
        tb.title = "Game";
        tb.magnetUri = "magnet:?xt=urn:btih:deadbeef";

        CatalogBatchInstaller installer(
            root, [](const CatalogEntry&, const std::string&,
                     std::atomic<bool>&,
                     const MagnetResolver::ProgressCallback&,
                     std::vector<uint8_t>&, std::string&) { return false; });
        std::atomic<bool> cancelled{false};
        DebridBatchTiming fast{1, 1000};
        TorboxProvider provider("key", transport);
        BatchPreparation prepared = installer.prepareViaDebrid(
            {tb}, StreamSelection::PackagesOnly, provider, cancelled, {},
            fast);

        assert(prepared.failures().empty());
        assert(prepared.items().size() == 1);
        assert(prepared.items()[0].source == InstallSource::Debrid);
        assert(prepared.items()[0].debridId == "7");
        assert(prepared.items()[0].mode == TransferMode::StreamInstall);
        assert(prepared.items()[0].space.packageFiles == 1);
        assert(prepared.items()[0].preview.name == "Game");
        assert(prepared.items()[0].torrentPath.empty());
    }

    {
        // Debrid enqueue: one selected item imports; one deselected is removed.
        std::atomic<int> removeCalls{0};
        TorboxTransport transport =
            [&removeCalls](const TorboxHttpRequest& req,
                           TorboxHttpResponse& res, std::string&) {
                if (req.url.find("controltorrent") != std::string::npos)
                    ++removeCalls;
                res.status = 200; res.body = "{\"success\":true}";
                return true;
            };

        BatchPreparation prepared;
        {
            PreparedCatalogInstall keep;
            keep.entry = entry; keep.entry.infoHash = entry.infoHash;
            keep.preview.name = "Keep"; keep.preview.totalBytes = 1000;
            keep.mode = TransferMode::DownloadOnly;
            keep.source = InstallSource::Debrid; keep.debridId = "11";
            keep.selected = true;
            prepared.items().push_back(std::move(keep));

            PreparedCatalogInstall drop;
            drop.entry = entry; drop.entry.infoHash =
                "00112233445566778899aabbccddeeff00112233";
            drop.preview.name = "Drop"; drop.preview.totalBytes = 2000;
            drop.mode = TransferMode::DownloadOnly;
            drop.source = InstallSource::Debrid; drop.debridId = "22";
            drop.selected = false;
            prepared.items().push_back(std::move(drop));
        }

        const std::string appRoot = std::string(root) + "/tbapp";
        DownloadManager manager(appRoot, false);
        CatalogBatchInstaller installer(
            root, [](const CatalogEntry&, const std::string&,
                     std::atomic<bool>&,
                     const MagnetResolver::ProgressCallback&,
                     std::vector<uint8_t>&, std::string&) { return false; });
        TorboxProvider provider("key", transport);
        BatchEnqueueResult queued = installer.enqueueViaDebrid(
            prepared, manager, DebridProviderKind::TorBox, provider);

        assert(queued.taskIds.size() == 1);
        assert(queued.skipped == 1);
        assert(removeCalls.load() == 1);
        assert(manager.snapshot().size() == 1);
        assert(manager.snapshot()[0].source == TaskSource::Debrid);
        assert(manager.snapshot()[0].debridId == "11");

        std::string rmerr;
        assert(manager.remove(queued.taskIds[0], true, rmerr));
        unlink((appRoot + "/queue.bencode").c_str());
        rmdir((appRoot + "/torrents").c_str());
        rmdir((appRoot + "/downloads").c_str());
        rmdir(appRoot.c_str());
    }

    unlink(source.c_str());
    rmdir(root);
    std::puts("batch install tests passed");
    return 0;
}
