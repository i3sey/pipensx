#include "app/archive_service.hpp"
#include "app/app_settings.hpp"
#include "app/autorun_service.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "install/install_backend.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "core/sha1.h"
}

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <borealis/extern/nanovg/stb_image.h>
#include "../vendor/borealis/library/lib/extern/glfw/deps/stb_image_write.h"
#pragma GCC diagnostic pop

namespace {

using namespace pipensx;

std::string bs(const std::string& s) {
    return std::to_string(s.size()) + ":" + s;
}

std::string makeTorrentBytes(const std::string& filename,
                             const std::string& payload,
                             const std::string& tracker = "http://bt.t-ru.org/ann?uk=test") {
    const std::string info =
        "d6:lengthi" + std::to_string(payload.size()) + "e4:name" +
        bs(filename) + "12:piece lengthi16384e6:pieces20:" +
        std::string(20, '\x11') + "e";
    return "d8:announce" + bs(tracker) + "4:info" + info + "e";
}

std::string infoHashHexOfTorrent(const std::string& torrentBytes) {
    const size_t pos = torrentBytes.find("4:info");
    assert(pos != std::string::npos);
    const size_t start = pos + 6;
    const size_t len = torrentBytes.size() - start - 1;
    uint8_t digest[20]{};
    sha1(reinterpret_cast<const uint8_t*>(torrentBytes.data() + start),
         len, digest);
    static const char kHex[] = "0123456789abcdef";
    std::string out(40, '0');
    for (int i = 0; i < 20; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

std::vector<uint8_t> makeSolidJpeg(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w * h * 3));
    for (size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = r;
        rgb[i + 1] = g;
        rgb[i + 2] = b;
    }
    std::vector<uint8_t> out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* ptr = static_cast<const uint8_t*>(data);
        vec->insert(vec->end(), ptr, ptr + size);
    };
    const int ok = stbi_write_jpg_to_func(cb, &out, w, h, 3, rgb.data(), 90);
    assert(ok && !out.empty());
    return out;
}

void writeFile(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.good());
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    assert(out.good());
}

void writeSimFootprint(const std::string& root,
                       const std::string& upperId,
                       uint64_t totalBytes) {
    const std::string simRoot = root + "/install-sim";
    const std::string titlesDir = simRoot + "/titles";
    mkdir(simRoot.c_str(), 0755);
    mkdir(titlesDir.c_str(), 0755);
    std::ofstream f(titlesDir + "/" + upperId + ".sim", std::ios::trunc);
    assert(f.good());
    f << "total_bytes=" << totalBytes << "\n"
      << "control_bytes=131072\ncontent_count=5\nmeta_count=2\nsd=1\nnand=0\n";
}

void testIconBadging() {
    const std::vector<uint8_t> src = makeSolidJpeg(256, 256, 200, 220, 240);
    std::vector<uint8_t> badged;
    std::string err;
    assert(badgeArchiveIconJpeg(src.data(), src.size(), badged, err));
    assert(!badged.empty() && badged.size() <= 120 * 1024);

    int w = 0, h = 0, ch = 0;
    uint8_t* pixels = stbi_load_from_memory(
        badged.data(), static_cast<int>(badged.size()), &w, &h, &ch, 3);
    assert(pixels && w == 256 && h == 256);
    // Top-left pixel was dimmed (~12% darker than ~200,220,240).
    assert(pixels[0] < 190 && pixels[2] < 225);
    // Bottom-right badge edge pixel around (196, 200) sits in dark #1A1D22 pill.
    const size_t badgeCorner = (200 * 256 + 194) * 3;
    assert(pixels[badgeCorner] < 70 && pixels[badgeCorner + 1] < 70);
    // Bright white arrow/cloud center around (216, 208) has high luminance.
    const size_t badgeCenter = (208 * 256 + 216) * 3;
    assert(pixels[badgeCenter] > 200 && pixels[badgeCenter + 1] > 200);
    stbi_image_free(pixels);
}

void testRecipePersistence(const std::string& root) {
    ArchiveRecipe r;
    r.titleId = "0100111122220000";
    r.applicationId = 0x0100111122220000ULL;
    r.name = "Metroid";
    r.publisher = "Nintendo";
    r.displayVersion = "1.0.0";
    r.installedVersion = "65536";
    r.sourceKind = ArchiveSourceKind::Catalog;
    r.infoHash = std::string(40, 'a');
    r.magnetUri = "magnet:?xt=urn:btih:" + r.infoHash;
    r.hasTorrentFile = true;
    r.freedBytes = 4ULL * 1024 * 1024 * 1024;
    r.archivedAt = 1700000000;

    std::string err;
    assert(saveArchiveRecipe(root, r, err));
    ArchiveRecipe loaded;
    assert(loadArchiveRecipe(root, r.titleId, loaded, err));
    assert(loaded.titleId == r.titleId);
    assert(loaded.name == r.name);
    assert(loaded.sourceKind == ArchiveSourceKind::Catalog);
    assert(loaded.infoHash == r.infoHash);
    assert(loaded.freedBytes == r.freedBytes);

    const auto list = listArchiveRecipes(root);
    assert(list.size() == 1 && list[0].titleId == r.titleId);
    removeArchiveArtifacts(root, r.titleId);
    assert(listArchiveRecipes(root).empty());
}

void testArchiveInspectArchiveAndRestore(const std::string& root) {
    mkdir(root.c_str(), 0755);

    const std::string gameId = "0100ABCDEF012000";
    const std::string extId = "0100FEDCBA986000";
    const std::string autorunId = "0100BEEF00010000";
    const std::string portId = "0100555566660000";

    const std::string gameTorrent = makeTorrentBytes(
        "Cool Game [" + gameId + "][v0].nsp", "sample-package-payload");
    const std::string gameHash = infoHashHexOfTorrent(gameTorrent);

    const std::string torDir = root + "/torrents";
    mkdir(torDir.c_str(), 0755);
    const std::string torPath = torDir + "/" + gameHash + ".torrent";
    writeFile(torPath, gameTorrent.data(), gameTorrent.size());

    // Pre-populate a finished historical Installed task in queue.bencode before
    // constructing DownloadManager (matches post-install Switch state).
    const std::string taskName = "Cool Game";
    const std::string dataDir = root + "/downloads/cool";
    const std::string stateBencode =
        "d5:tasksl" +
        (std::string("d4:data") + bs(dataDir) +
         "2:id" + bs(gameHash) +
         "8:metainfo" + bs(torPath) +
         "4:mode" + bs("install") +
         "4:name" + bs(taskName) +
         "6:status" + bs("installed") +
         "5:totali22e" + "e") +
        "e7:versioni6ee";
    writeFile(root + "/queue.bencode", stateBencode.data(), stateBencode.size());

    DownloadManager manager(root, false);
    manager.setTorrentingEnabled(true);
    AppSettings settings(root + "/settings.json");
    std::string err;
    settings.load(err);
    InstalledTitleService installed(root);
    CatalogService catalog(root);
    GameMetadataService metadata(root);

    CatalogEntry catEntry;
    catEntry.titleId = gameId;
    catEntry.title = "Cool Game [NSP]";
    catEntry.infoHash = gameHash;
    catEntry.magnetUri = "magnet:?xt=urn:btih:" + gameHash;
    catEntry.packageType = "nsp";
    CatalogSnapshot catSnap;
    catSnap.entries = {catEntry};
    catalog.adopt(std::move(catSnap), "test");

    // Pre-write original icon for gameId into installed-icons cache.
    const std::string iconDir = root + "/installed-icons";
    mkdir(iconDir.c_str(), 0755);
    const std::vector<uint8_t> origIcon = makeSolidJpeg(256, 256, 40, 120, 200);
    writeFile(iconDir + "/" + gameId + ".jpg", origIcon.data(), origIcon.size());

    // Pre-write heavy footprint in PC install-sim.
    writeSimFootprint(root, gameId, 3ULL * 1024 * 1024 * 1024);

    InstalledTitle tGame;
    tGame.applicationId = 0x0100ABCDEF012000ULL;
    tGame.titleId = gameId;
    tGame.name = "Cool Game";
    tGame.publisher = "Studio";
    tGame.displayVersion = "1.2.0";
    tGame.version = "65536";
    tGame.iconPath = iconDir + "/" + gameId + ".jpg";

    InstalledTitle tExternal;
    tExternal.applicationId = 0x0100FEDCBA986000ULL;
    tExternal.titleId = extId;
    tExternal.name = "External Cartridge Dump";
    tExternal.version = "0";

    InstalledTitle tAutorun;
    tAutorun.applicationId = 0x0100BEEF00010000ULL;
    tAutorun.titleId = autorunId;
    tAutorun.name = "Wine Forwarder";

    InstalledTitle tPort;
    tPort.applicationId = 0x0100555566660000ULL;
    tPort.titleId = portId;
    tPort.name = "Port Forwarder";

    installed.injectTitles({tGame, tExternal, tAutorun, tPort});
    assert(manager.snapshot(gameHash) &&
           manager.snapshot(gameHash)->status == DownloadStatus::Installed);

    ArchiveService archive(
        root, &installed, &manager, &catalog, &metadata, &settings,
        [&](const std::string& tid) { return tid == portId; },
        "sdmc:/switch/pipensx/pipensx.nro",
        [](uint64_t tid, const std::string& titleName,
           const std::string& publisher, const std::string& displayVersion,
           const std::string& nroPath, const std::string& args,
           const std::vector<uint8_t>& badgedIcon,
           std::vector<uint8_t>& outNsp, std::string& buildErr) {
            (void)tid;
            (void)publisher;
            (void)displayVersion;
            assert(titleName == "Cool Game");
            assert(titleName.find("\xE2\x98\x81") == std::string::npos);
            assert(nroPath == "sdmc:/switch/pipensx/pipensx.nro");
            assert(args == "sdmc:/switch/pipensx/pipensx.nro --restore-archive 0100ABCDEF012000");
            assert(!badgedIcon.empty());
            outNsp = buildPfs0({{"program.nca", std::vector<uint8_t>(256, 1)},
                                {"control.nca", badgedIcon}});
            if (outNsp.empty()) {
                buildErr = "empty stub";
                return false;
            }
            return true;
        });

    // Batch inspect verification:
    const auto libMap = archive.inspectLibrary(installed.titles());
    assert(libMap.at(gameId).canArchive == true);
    assert(libMap.at(gameId).sourceKind == ArchiveSourceKind::TaskHistory);
    assert(libMap.at(gameId).installedBytes == 3ULL * 1024 * 1024 * 1024);
    assert(libMap.at(extId).canArchive == false);
    assert(libMap.at(extId).blockReason == ArchiveBlockReason::NoRestoreSource);
    assert(libMap.at(autorunId).blockReason == ArchiveBlockReason::HomebrewOrForwarder);
    assert(libMap.at(portId).blockReason == ArchiveBlockReason::HomebrewOrForwarder);

    ArchiveActionResult res;
    assert(!archive.archiveTitle(extId, res, err));
    assert(archive.archiveTitle(gameId, res, err));
    assert(res.freedBytes > 3ULL * 1024 * 1024 * 1024 - 4096);
    struct stat st {};
    assert(stat(archiveRecipePath(root, gameId).c_str(), &st) == 0);
    assert(stat(archiveBackupIconPath(root, gameId).c_str(), &st) == 0);
    assert(stat(archiveBackupTorrentPath(root, gameId).c_str(), &st) == 0);
    assert(stat((iconDir + "/" + gameId + "-arch.jpg").c_str(), &st) == 0);

    // Simulate Switch refresh marking tGame as archived (pnxarch).
    tGame.isArchived = true;
    tGame.version = "0";
    tGame.iconPath = iconDir + "/" + gameId + "-arch.jpg";
    installed.injectTitles({tGame, tExternal});
    assert(!installed.contains(gameId));
    assert(installed.isArchived(gameId));
    assert(installed.containsInstalledOrArchived(gameId));
    assert(archive.inspectArchive(gameId).blockReason ==
           ArchiveBlockReason::AlreadyArchived);

    // Verify restoreTitle queues from backup .torrent (and reuses/resets task).
    std::atomic<bool> cancelled{false};
    RestoreStartResult restoreRes;
    assert(archive.restoreTitle(gameId, cancelled, restoreRes, err, true));
    assert(restoreRes.queuedImmediately);
    assert(restoreRes.taskId == gameHash);
    assert(manager.hasTask(gameHash));

    // Removing the stub is blocked while a restore download is active/queued;
    // once paused, removeArchivedStub clears stub record + archive artifacts.
    assert(!archive.removeArchivedStub(gameId, err));
    assert(manager.pause(gameHash));
    assert(archive.removeArchivedStub(gameId, err));
    assert(stat(archiveRecipePath(root, gameId).c_str(), &st) != 0);

    manager.shutdown();
}

} // namespace

int main() {
    testIconBadging();
    const std::string root = "/tmp/pipensx-test-archive-" + std::to_string(getpid());
    mkdir(root.c_str(), 0755);
    testRecipePersistence(root + "/recipe");
    testArchiveInspectArchiveAndRestore(root + "/service");
    std::printf("test_archive_service: all passed\n");
    return 0;
}
