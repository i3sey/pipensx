#include "app/catalog_service.hpp"
#include "app/catalog_refresh.hpp"
#include "app/catalog_presentation.hpp"
#include "app/game_metadata_service.hpp"
#include "app/magnet_resolver.hpp"

extern "C" {
#include "core/sha1.h"
#include "core/tracker.h"
}

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using pipensx::CatalogEntry;
using pipensx::CatalogHealth;
using pipensx::CatalogPresentation;
using pipensx::CatalogRefreshBatch;
using pipensx::CatalogService;
using pipensx::catalogEntryIsGame;
using pipensx::catalogEntryHasMatchedTitle;
using pipensx::adoptCatalogRefresh;
using pipensx::GameMetadata;
using pipensx::GameMetadataService;
using pipensx::MetadataSnapshot;
using pipensx::MagnetResolver;
using pipensx::MagnetSpec;
using pipensx::mergeScreenshotUrls;
using pipensx::resolveCatalogPresentation;

namespace {

std::string hexHash(const uint8_t hash[20]) {
    static const char digits[] = "0123456789ABCDEF";
    std::string result(40, '0');
    for (size_t i = 0; i < 20; ++i) {
        result[i * 2] = digits[hash[i] >> 4];
        result[i * 2 + 1] = digits[hash[i] & 15];
    }
    return result;
}

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output << text;
    assert(output.good());
}

void testMagnetParsing() {
    MagnetSpec spec;
    std::string error;
    assert(MagnetResolver::parse(
        "magnet:?xt=urn:btih:E21269D03D34B557F63CE915DEA14F765C9C9798"
        "&tr=http%3A%2F%2Fbt3.t-ru.org%2Fann%3Fmagnet",
        spec, error));
    assert(spec.infoHashHex ==
           "E21269D03D34B557F63CE915DEA14F765C9C9798");
    assert(spec.trackerUrl == "http://bt3.t-ru.org/ann?magnet");

    assert(!MagnetResolver::parse(
        "magnet:?xt=urn:btih:E21269D03D34B557F63CE915DEA14F765C9C9798"
        "&tr=http://tracker.example/announce",
        spec, error));
    assert(!MagnetResolver::parse("https://rutracker.org/", spec, error));
}

void testCatalogParsing() {
    const char* json =
        "["
        "{\"title\":\"Older\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "E21269D03D34B557F63CE915DEA14F765C9C9798&tr="
        "http://bt.t-ru.org/ann?magnet\",\"size\":100,"
        "\"published_date\":10},"
        "{\"title\":\"Newer\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "EF47FD2FAD86A00074507991E1907AA097FC40F5&tr="
        "http://bt4.t-ru.org/ann?magnet\",\"size\":200,"
        "\"published_date\":20},"
        "{\"title\":\"Duplicate\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "EF47FD2FAD86A00074507991E1907AA097FC40F5&tr="
        "http://bt4.t-ru.org/ann?magnet\"},"
        "{\"title\":\"Bad\",\"magnetURI\":\"invalid\"}"
        "]";
    std::vector<CatalogEntry> entries;
    std::string error;
    assert(CatalogService::parseJson(json, entries, error));
    assert(entries.size() == 2);
    assert(entries[0].title == "Newer");
    assert(entries[1].title == "Older");
    assert(entries[0].size == 200);
    assert(!CatalogService::parseJson("{}", entries, error));
    assert(!CatalogService::parseJson("[]", entries, error));
}

void testCatalogV2HealthParsing() {
    const char* json =
        "["
        "{\"title\":\"Alive\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "1111111111111111111111111111111111111111&tr="
        "http://bt.t-ru.org/ann?magnet\",\"topic_id\":123,"
        "\"forum_id\":1605,\"tracker_id\":2,\"source_updated_at\":1000,"
        "\"catalog_generated_at\":2000,\"last_checked_at\":3000,"
        "\"peer_count\":12,\"metadata_ok\":true,\"health\":\"ok\","
        "\"screenshots\":[\"https://example/s1.jpg\","
        "\"https://example/s2.jpg\"]},"
        "{\"title\":\"Dead\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "2222222222222222222222222222222222222222&tr="
        "http://bt2.t-ru.org/ann?magnet\",\"health\":\"tracker_not_registered\","
        "\"failure_reason\":\"Torrent not registered\"}"
        "]";
    std::vector<CatalogEntry> entries;
    std::string error;
    assert(CatalogService::parseJson(json, entries, error));
    assert(entries.size() == 2);
    assert(entries[0].health == CatalogHealth::Ok);
    assert(entries[0].topicId == 123);
    assert(entries[0].forumId == 1605);
    assert(entries[0].trackerId == 2);
    assert(entries[0].sourceUpdatedAt == 1000);
    assert(entries[0].catalogGeneratedAt == 2000);
    assert(entries[0].lastCheckedAt == 3000);
    assert(entries[0].peerCount == 12);
    assert(entries[0].metadataOk);
    assert(entries[0].screenshots.size() == 2);
    assert(entries[0].screenshots[1] == "https://example/s2.jpg");
    assert(!entries[0].isHiddenByDefault());
    assert(entries[1].health == CatalogHealth::TrackerNotRegistered);
    assert(entries[1].healthReason == "Torrent not registered");
    assert(entries[1].isHiddenByDefault());
}

/* Base64 of the info dictionary used by testTorrentConstruction; its SHA-1
   is 9E2B6F8ACD7B3DA966E5FF4BA4C0E00750AC2595. */
constexpr const char* kInfoDictB64 =
    "ZDY6bGVuZ3RoaTFlNDpuYW1lODp0ZXN0Lm5zcDEyOnBpZWNlIGxlbmd0aGkxNjM4NGU2On"
    "BpZWNlczIwOjAxMjM0NTY3ODkwMTIzNDU2Nzg5ZQ==";

void testInfoDictParsing() {
    const std::string good =
        "[{\"title\":\"Preset\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "9E2B6F8ACD7B3DA966E5FF4BA4C0E00750AC2595&tr="
        "http://bt.t-ru.org/ann?magnet\",\"info_dict\":\"" +
        std::string(kInfoDictB64) + "\"}]";
    std::vector<CatalogEntry> entries;
    std::string error;
    assert(CatalogService::parseJson(good, entries, error));
    assert(entries.size() == 1);
    assert(entries[0].infoDict.size() == 82);
    assert(entries[0].infoDict.front() == 'd');
    assert(entries[0].infoDict.back() == 'e');

    /* A dictionary that hashes to a different btih must be dropped while
       the entry itself survives for the network resolve. */
    const std::string mismatched =
        "[{\"title\":\"Mismatch\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "E21269D03D34B557F63CE915DEA14F765C9C9798&tr="
        "http://bt.t-ru.org/ann?magnet\",\"info_dict\":\"" +
        std::string(kInfoDictB64) + "\"}]";
    assert(CatalogService::parseJson(mismatched, entries, error));
    assert(entries.size() == 1);
    assert(entries[0].infoDict.empty());

    const char* invalid =
        "[{\"title\":\"BadB64\",\"magnetURI\":\"magnet:?xt=urn:btih:"
        "E21269D03D34B557F63CE915DEA14F765C9C9798&tr="
        "http://bt.t-ru.org/ann?magnet\",\"info_dict\":\"@@not base64@@\"}]";
    assert(CatalogService::parseJson(invalid, entries, error));
    assert(entries.size() == 1);
    assert(entries[0].infoDict.empty());
}

void testResolveFromPresetInfoDict() {
    const std::string info =
        "d6:lengthi1e4:name8:test.nsp12:piece lengthi16384e6:pieces20:"
        "01234567890123456789e";
    uint8_t digest[20];
    sha1(info.data(), info.size(), digest);
    std::string magnet = "magnet:?xt=urn:btih:" + hexHash(digest) +
                         "&tr=http://bt.t-ru.org/ann?magnet";
    std::vector<uint8_t> preset(info.begin(), info.end());

    char pathTemplate[] = "/tmp/pipensx-preset-XXXXXX";
    int fd = mkstemp(pathTemplate);
    assert(fd >= 0);
    close(fd);
    std::string path = pathTemplate;

    /* Offline: a valid preset dictionary resolves without any network. */
    MagnetResolver resolver;
    std::atomic<bool> cancelled{false};
    std::string error;
    assert(resolver.resolveToFile(magnet, path, cancelled, nullptr, error,
                                  nullptr, &preset));
    std::ifstream input(path, std::ios::binary);
    std::string torrent((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    assert(torrent.rfind("d8:announce", 0) == 0);
    assert(torrent.find(info) != std::string::npos);

    /* A preset that does not match the magnet hash must fall through to
       the network resolve; with cancellation pre-set that path exits
       without producing a file. */
    std::string wrongMagnet =
        "magnet:?xt=urn:btih:E21269D03D34B557F63CE915DEA14F765C9C9798"
        "&tr=http://bt.t-ru.org/ann?magnet";
    std::atomic<bool> stop{true};
    unlink(path.c_str());
    assert(!resolver.resolveToFile(wrongMagnet, path, stop, nullptr, error,
                                   nullptr, &preset));
    assert(access(path.c_str(), F_OK) != 0);
    unlink(path.c_str());
}

void testTorrentConstruction() {
    const std::string info =
        "d6:lengthi1e4:name8:test.nsp12:piece lengthi16384e6:pieces20:"
        "01234567890123456789e";
    uint8_t digest[20];
    sha1(info.data(), info.size(), digest);

    MagnetSpec spec;
    std::memcpy(spec.infoHash, digest, sizeof(digest));
    spec.infoHashHex = hexHash(digest);
    spec.trackerUrl = "http://bt.t-ru.org/ann?magnet";

    std::vector<uint8_t> torrent;
    std::string error;
    assert(MagnetResolver::buildTorrent(
        spec, std::vector<uint8_t>(info.begin(), info.end()), torrent, error));
    assert(!torrent.empty());

    spec.infoHash[0] ^= 1;
    assert(!MagnetResolver::buildTorrent(
        spec, std::vector<uint8_t>(info.begin(), info.end()), torrent, error));
}

void testMetadataIndexParsing() {
    const char* json =
        "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
        "\"titleId\":\"0100230005A52000\",\"match\":\"exact\","
        "\"name\":\"Lovers in a Dangerous Spacetime\","
        "\"description\":\"desc\",\"publisher\":\"Asteroid Base\","
        "\"releaseDate\":\"2017-10-03\",\"iconUrl\":\"https://example/icon.jpg\","
        "\"bannerUrl\":\"https://example/banner.jpg\","
        "\"screenshots\":[\"https://example/s1.jpg\"],"
        "\"categories\":[\"Action\",\"Party\"]},"
        "{\"infoHash\":\"bad\",\"titleId\":\"0100000000000000\","
        "\"name\":\"Bad\"}]";
    std::vector<GameMetadata> items;
    std::string error;
    assert(GameMetadataService::parseIndex(json, items, error));
    assert(items.size() == 1);
    assert(items[0].infoHash == "E21269D03D34B557F63CE915DEA14F765C9C9798");
    assert(items[0].titleId == "0100230005A52000");
    assert(items[0].screenshots.size() == 1);
    assert(items[0].categories.size() == 2);
    assert(!GameMetadataService::parseIndex("{}", items, error));
}

void testMetadataSnapshotAcceptsVerifiedIndex() {
    const std::string index =
        "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
        "\"titleId\":\"0100230005A52000\",\"name\":\"Runtime\"}]";
    const std::string manifest =
        "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
        "\"langegenCommit\":\"langegen-sha\",\"titledbCommit\":\"titledb-sha\","
        "\"index\":{\"url\":\"https://github.com/i3sey/"
        "pipensx-metadata/releases/download/metadata-test/"
        "game_metadata_index.json\","
        "\"bytes\":103,\"sha256\":\"75b92238836d44279c24060d060e49f5"
        "b76e8049ca7fade0b736078433c4de80\",\"entries\":1}}";
    MetadataSnapshot snapshot;
    std::string error;
    assert(GameMetadataService::prepareSnapshot(manifest, index, snapshot,
                                                error));
    assert(snapshot.items.size() == 1);
    assert(snapshot.items[0].name == "Runtime");
    assert(snapshot.manifest.indexBytes == index.size());
    const std::string manifestWithoutUrl =
        "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
        "\"langegenCommit\":\"langegen-sha\",\"titledbCommit\":\"titledb-sha\","
        "\"index\":{\"bytes\":103,\"sha256\":\"75b92238836d44279c24060d060e49f5"
        "b76e8049ca7fade0b736078433c4de80\",\"entries\":1}}";
    assert(GameMetadataService::prepareSnapshot(manifestWithoutUrl, index,
                                                snapshot, error));
    assert(snapshot.manifest.indexUrl.find(
        "/releases/latest/download/game_metadata_index.json") !=
           std::string::npos);
}

void testMetadataSnapshotRejectsTamperedIndex() {
    const std::string index =
        "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
        "\"titleId\":\"0100230005A52000\",\"name\":\"Runtimf\"}]";
    const std::string manifest =
        "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
        "\"langegenCommit\":\"langegen-sha\",\"titledbCommit\":\"titledb-sha\","
        "\"index\":{\"url\":\"https://raw.githubusercontent.com/"
        "i3sey/pipensx-metadata/data/game_metadata_index.json\","
        "\"bytes\":103,\"sha256\":\"75b92238836d44279c24060d060e49f5"
        "b76e8049ca7fade0b736078433c4de80\",\"entries\":1}}";
    MetadataSnapshot snapshot;
    std::string error;
    assert(!GameMetadataService::prepareSnapshot(manifest, index, snapshot,
                                                 error));
    assert(error.find("SHA-256") != std::string::npos);
}

void testMetadataLoadPrefersVerifiedRuntimeCache() {
    const std::string root = "/tmp/pipensx-metadata-cache-" +
        std::to_string(static_cast<long long>(getpid()));
    const std::string bundled = root + "/bundled.json";
    mkdir(root.c_str(), 0755);
    writeTextFile(
        bundled,
        "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
        "\"titleId\":\"0100230005A52000\",\"name\":\"Bundled\"}]");

    {
        GameMetadataService metadata(root, bundled);
        const std::string index =
            "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
            "\"titleId\":\"0100230005A52000\",\"name\":\"Runtime\"}]";
        const std::string sha =
            "75b92238836d44279c24060d060e49f5b76e8049ca7fade0b736078433c4de80";
        const std::string manifest =
            "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
            "\"langegenCommit\":\"langegen-sha\","
            "\"titledbCommit\":\"titledb-sha\",\"index\":{"
            "\"url\":\"https://raw.githubusercontent.com/i3sey/"
            "pipensx-metadata/data/game_metadata_index.json\","
            "\"bytes\":103,\"sha256\":\"" + sha +
            "\",\"entries\":1}}";
        writeTextFile(root + "/catalog/metadata/" + sha + ".json", index);
        writeTextFile(root + "/catalog/metadata/manifest.json", manifest);

        std::string error;
        assert(metadata.load(error));
        const GameMetadata* found = metadata.findByInfoHash(
            "E21269D03D34B557F63CE915DEA14F765C9C9798");
        assert(found);
        assert(found->name == "Runtime");
    }

    unlink((root + "/catalog/metadata/manifest.json").c_str());
    unlink((root + "/catalog/metadata/"
            "75b92238836d44279c24060d060e49f5b76e8049ca7fade0b736078433c4de80"
            ".json").c_str());
    unlink(bundled.c_str());
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

void testMetadataFetchVerifiesBeforeAdoptAndFallsBackToCache() {
    const std::string root = "/tmp/pipensx-metadata-fetch-" +
        std::to_string(static_cast<long long>(getpid()));
    mkdir(root.c_str(), 0755);
    const std::string manifestUrl =
        "https://raw.githubusercontent.com/i3sey/pipensx-metadata/"
        "data/manifest.json";
    const std::string indexUrl =
        "https://raw.githubusercontent.com/i3sey/pipensx-metadata/"
        "data/game_metadata_index.json";
    const std::string index =
        "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
        "\"titleId\":\"0100230005A52000\",\"name\":\"Runtime\"}]";
    const std::string manifest =
        "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
        "\"langegenCommit\":\"langegen-sha\",\"titledbCommit\":\"titledb-sha\","
        "\"index\":{\"url\":\"" + indexUrl +
        "\",\"bytes\":103,\"sha256\":\"75b92238836d44279c24060d060e49f5"
        "b76e8049ca7fade0b736078433c4de80\",\"entries\":1}}";
    bool failIndex = false;
    GameMetadataService::MetadataFetcher fetcher =
        [&](const std::string& url, size_t, std::vector<uint8_t>& bytes,
            std::string& error) {
            if (url == manifestUrl) {
                bytes.assign(manifest.begin(), manifest.end());
                return true;
            }
            if (url == indexUrl && !failIndex) {
                bytes.assign(index.begin(), index.end());
                return true;
            }
            error = "simulated index failure";
            return false;
        };

    {
        GameMetadataService metadata(root, root + "/missing.json",
                                     manifestUrl, fetcher);
        MetadataSnapshot snapshot;
        std::string error;
        assert(metadata.fetchLatest(snapshot, error));
        assert(metadata.size() == 0);

        failIndex = true;
        MetadataSnapshot cached;
        assert(metadata.fetchLatest(cached, error));
        assert(metadata.size() == 0);
        assert(cached.items.size() == 1);
        assert(cached.items[0].name == "Runtime");
        metadata.adopt(std::move(cached));
        assert(metadata.size() == 1);
        assert(metadata.findByInfoHash(
            "E21269D03D34B557F63CE915DEA14F765C9C9798"));
    }

    unlink((root + "/catalog/metadata/manifest.json").c_str());
    unlink((root + "/catalog/metadata/"
            "75b92238836d44279c24060d060e49f5b76e8049ca7fade0b736078433c4de80"
            ".json").c_str());
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

void testMetadataLoadFallsBackWhenRuntimeCacheIsCorrupt() {
    const std::string root = "/tmp/pipensx-metadata-corrupt-" +
        std::to_string(static_cast<long long>(getpid()));
    const std::string bundled = root + "/bundled.json";
    mkdir(root.c_str(), 0755);
    writeTextFile(
        bundled,
        "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
        "\"titleId\":\"0100230005A52000\",\"name\":\"Bundled\"}]");
    const std::string sha(64, '0');
    {
        GameMetadataService metadata(root, bundled);
        writeTextFile(root + "/catalog/metadata/" + sha + ".json", "[]");
        writeTextFile(
            root + "/catalog/metadata/manifest.json",
            "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
            "\"langegenCommit\":\"langegen-sha\","
            "\"titledbCommit\":\"titledb-sha\",\"index\":{"
            "\"url\":\"https://raw.githubusercontent.com/i3sey/"
            "pipensx-metadata/data/game_metadata_index.json\","
            "\"bytes\":2,\"sha256\":\"" + sha +
            "\",\"entries\":1}}");
        std::string error;
        assert(metadata.load(error));
        const GameMetadata* found = metadata.findByInfoHash(
            "E21269D03D34B557F63CE915DEA14F765C9C9798");
        assert(found && found->name == "Bundled");
    }
    unlink((root + "/catalog/metadata/manifest.json").c_str());
    unlink((root + "/catalog/metadata/" + sha + ".json").c_str());
    unlink(bundled.c_str());
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

void testCatalogAndMetadataRefreshAdoptIndependently() {
    const std::string root = "/tmp/pipensx-refresh-adopt-" +
        std::to_string(static_cast<long long>(getpid()));
    mkdir(root.c_str(), 0755);
    {
        CatalogService catalog(root, "");
        CatalogEntry oldEntry;
        oldEntry.title = "Old";
        catalog.adopt({oldEntry});
        GameMetadataService metadata(root, root + "/missing.json");

        CatalogEntry newEntry;
        newEntry.title = "New";
        CatalogRefreshBatch catalogOnly;
        catalogOnly.catalogOk = true;
        catalogOnly.catalogEntries = {newEntry};
        const auto first = adoptCatalogRefresh(
            catalog, metadata, std::move(catalogOnly));
        assert(first.catalogChanged && !first.metadataChanged);
        assert(catalog.entries()[0].title == "New");
        assert(metadata.size() == 0);

        const std::string index =
            "[{\"infoHash\":\"E21269D03D34B557F63CE915DEA14F765C9C9798\","
            "\"titleId\":\"0100230005A52000\",\"name\":\"Runtime\"}]";
        const std::string manifest =
            "{\"schemaVersion\":1,\"generatedAt\":\"2026-07-09T00:00:00Z\","
            "\"langegenCommit\":\"langegen-sha\","
            "\"titledbCommit\":\"titledb-sha\",\"index\":{"
            "\"url\":\"https://raw.githubusercontent.com/i3sey/"
            "pipensx-metadata/data/game_metadata_index.json\","
            "\"bytes\":103,\"sha256\":\"75b92238836d44279c24060d060e49f5"
            "b76e8049ca7fade0b736078433c4de80\",\"entries\":1}}";
        CatalogRefreshBatch metadataOnly;
        metadataOnly.metadataOk = true;
        std::string error;
        assert(GameMetadataService::prepareSnapshot(
            manifest, index, metadataOnly.metadata, error));
        const auto second = adoptCatalogRefresh(
            catalog, metadata, std::move(metadataOnly));
        assert(!second.catalogChanged && second.metadataChanged);
        assert(catalog.entries()[0].title == "New");
        assert(metadata.size() == 1);
    }
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

void testOptionalCatalogDataMayBeAbsent() {
    const std::string root = "/tmp/pipensx-optional-catalog-" +
                             std::to_string(static_cast<long long>(getpid()));
    mkdir(root.c_str(), 0755);

    std::string error;
    {
        CatalogService catalog(root, "");
        assert(catalog.load(error));
        assert(catalog.entries().empty());
        assert(error.empty());

        GameMetadataService metadata(root, root + "/missing-index.json");
        assert(metadata.load(error));
        assert(metadata.size() == 0);
        assert(error.empty());
    }

    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

void testCatalogPresentationUsesGameMetadata() {
    GameMetadata metadata;
    metadata.screenshots = {
        "https://example/meta-1.jpg",
        "https://example/shared.jpg",
    };
    CatalogEntry entry;
    entry.screenshots = {
        "https://example/shared.jpg",
        "https://example/catalog-1.jpg",
        "https://example/catalog-2.jpg",
        "https://example/catalog-3.jpg",
        "https://example/catalog-4.jpg",
        "https://example/catalog-5.jpg",
    };
    std::vector<std::string> screenshots =
        mergeScreenshotUrls(&metadata, entry, 6);
    assert(screenshots.size() == 6);
    assert(screenshots[0] == "https://example/meta-1.jpg");
    assert(screenshots[1] == "https://example/shared.jpg");
    assert(screenshots[2] == "https://example/catalog-1.jpg");
}

void testCatalogPresentationFallsBackFieldByField() {
    GameMetadata metadata;
    metadata.titleId = "0100230005A52000";
    metadata.name = "eShop name";
    metadata.description = "eShop description";
    metadata.iconUrl = "https://example/icon.jpg";
    metadata.categories = {"Action", "Party"};
    metadata.screenshots = {"https://example/meta.jpg"};

    CatalogEntry entry;
    entry.title = "Langegen title";
    entry.posterUrl = "https://example/cover.jpg";
    entry.description = "Langegen description";
    entry.developer = "Langegen developer";
    entry.publisher = "Langegen publisher";
    entry.year = "2026";
    entry.genre = "Langegen genre";
    entry.screenshots = {"https://example/catalog.jpg"};

    CatalogPresentation resolved =
        resolveCatalogPresentation(entry, &metadata);
    assert(resolved.title == "eShop name");
    assert(resolved.titleId == "0100230005A52000");
    assert(resolved.iconUrl == "https://example/icon.jpg");
    assert(!resolved.iconPreserveAspect);
    assert(resolved.coverUrl == "https://example/icon.jpg");
    assert(resolved.description == "eShop description");
    assert(resolved.developer == "Langegen developer");
    assert(resolved.publisher == "Langegen publisher");
    assert(resolved.releaseDate == "2026");
    assert(resolved.genre == "Action, Party");
    assert(resolved.screenshots.size() == 2);

    CatalogPresentation fallback =
        resolveCatalogPresentation(entry, nullptr);
    assert(fallback.title == "Langegen title");
    assert(fallback.iconUrl == "https://example/cover.jpg");
    assert(fallback.iconPreserveAspect);
    assert(fallback.coverUrl == "https://example/cover.jpg");
    assert(fallback.description == "Langegen description");
    assert(fallback.genre == "Langegen genre");
    assert(fallback.screenshots.size() == 1);
}

void testCatalogGameFilterKeepsUnmatchedSwitchReleases() {
    CatalogEntry release;
    release.title = "New game [NSZ][ENG]";
    assert(catalogEntryIsGame(release, nullptr));

    CatalogEntry nspRelease;
    nspRelease.title = "Base game [NSP][RUS]";
    assert(catalogEntryIsGame(nspRelease, nullptr));

    CatalogEntry cartridgeRelease;
    cartridgeRelease.title = "Cart dump [XCI][ENG]";
    assert(catalogEntryIsGame(cartridgeRelease, nullptr));

    CatalogEntry homebrew;
    homebrew.title = "Source port [NRO][ENG]";
    assert(!catalogEntryIsGame(homebrew, nullptr));

    GameMetadata emptyMetadata;
    assert(!catalogEntryIsGame(homebrew, &emptyMetadata));

    GameMetadata metadata;
    metadata.titleId = "0100230005A52000";
    assert(catalogEntryIsGame(homebrew, &metadata));

    GameMetadata invalidMetadata;
    invalidMetadata.titleId = "not-a-title-id";
    assert(!catalogEntryIsGame(homebrew, &invalidMetadata));
}

void testCatalogBrowseMatchFilterRequiresTitleIdMetadata() {
    assert(!catalogEntryHasMatchedTitle(nullptr));

    GameMetadata emptyMetadata;
    assert(!catalogEntryHasMatchedTitle(&emptyMetadata));

    GameMetadata metadata;
    metadata.titleId = "0100230005A52000";
    assert(catalogEntryHasMatchedTitle(&metadata));

    GameMetadata invalidMetadata;
    invalidMetadata.titleId = "not-a-title-id";
    assert(!catalogEntryHasMatchedTitle(&invalidMetadata));
}

void testAsyncImageDiskCache() {
    std::string root = "/tmp/pipensx-image-test-" +
                       std::to_string(static_cast<long long>(getpid()));
    std::string catalog = root + "/catalog";
    std::string images = catalog + "/images";
    mkdir(root.c_str(), 0755);
    mkdir(catalog.c_str(), 0755);
    mkdir(images.c_str(), 0755);

    const std::string url = "https://example.invalid/cached-cover.jpg";
    uint8_t digest[20];
    sha1(url.data(), url.size(), digest);
    static const char digits[] = "0123456789abcdef";
    std::string cacheName(40, '0');
    for (size_t i = 0; i < 20; ++i) {
        cacheName[i * 2] = digits[digest[i] >> 4];
        cacheName[i * 2 + 1] = digits[digest[i] & 15];
    }
    const std::vector<uint8_t> expected {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99,
        0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    {
        std::ofstream output(images + "/" + cacheName + ".img",
                             std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(expected.data()),
                     static_cast<std::streamsize>(expected.size()));
        assert(output.good());
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<GameMetadataService::ImageData> results;
    {
        GameMetadataService service(root, root + "/missing-index.json");
        service.setImageNetworkPaused(true);
        auto callback = [&](GameMetadataService::ImageData data) {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(data));
            ready.notify_all();
        };
        service.requestImage(url, callback);
        service.requestImage(url, callback);
        std::unique_lock<std::mutex> lock(mutex);
        assert(ready.wait_for(lock, std::chrono::seconds(5), [&] {
            return results.size() == 2;
        }));
        assert(results[0] && results[1]);
        assert(results[0]->width == 1 && results[0]->height == 1);
        assert(results[0]->pixels.size() == 4);
        assert(results[1]->pixels == results[0]->pixels);
        assert(results[0].get() == results[1].get());
    }
    std::remove((images + "/" + cacheName + ".img").c_str());
    rmdir(images.c_str());
    rmdir(catalog.c_str());
    rmdir(root.c_str());
}

// UI_PLAN F6: memory cache — synchronous hit after a load, prefetch warms
// it without a callback, dropMemoryImageCache() invalidates it.
void testImageMemoryCache() {
    std::string root = "/tmp/pipensx-image-mem-test-" +
                       std::to_string(static_cast<long long>(getpid()));
    std::string catalog = root + "/catalog";
    std::string images = catalog + "/images";
    mkdir(root.c_str(), 0755);
    mkdir(catalog.c_str(), 0755);
    mkdir(images.c_str(), 0755);

    const std::string url = "https://example.invalid/memory-cover.jpg";
    uint8_t digest[20];
    sha1(url.data(), url.size(), digest);
    static const char digits[] = "0123456789abcdef";
    std::string cacheName(40, '0');
    for (size_t i = 0; i < 20; ++i) {
        cacheName[i * 2] = digits[digest[i] >> 4];
        cacheName[i * 2 + 1] = digits[digest[i] & 15];
    }
    const std::vector<uint8_t> png {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99,
        0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    {
        std::ofstream output(images + "/" + cacheName + ".img",
                             std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
        assert(output.good());
    }

    {
        GameMetadataService service(root, root + "/missing-index.json");
        service.setImageNetworkPaused(true);

        // Cold: nothing decoded yet.
        assert(!service.cachedImage(url));

        // Prefetch decodes without a callback; poll until the cache warms.
        service.prefetchImage(url);
        GameMetadataService::ImageData cached;
        for (int i = 0; i < 500 && !cached; ++i) {
            cached = service.cachedImage(url);
            if (!cached)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(cached);
        assert(cached->width == 1 && cached->height == 1);
        assert(cached->pixels.size() == 4);

        // A regular request coalesces onto the cached copy.
        std::mutex mutex;
        std::condition_variable ready;
        GameMetadataService::ImageData result;
        bool done = false;
        service.requestImage(url, [&](GameMetadataService::ImageData data) {
            std::lock_guard<std::mutex> lock(mutex);
            result = std::move(data);
            done = true;
            ready.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            assert(ready.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return done; }));
        }
        assert(result.get() == cached.get());

        // Invalidation empties the memory cache; disk copy survives.
        service.dropMemoryImageCache();
        assert(!service.cachedImage(url));
    }
    std::remove((images + "/" + cacheName + ".img").c_str());
    rmdir(images.c_str());
    rmdir(catalog.c_str());
    rmdir(root.c_str());
}

void testImageNetworkWaitsForActiveTransfer() {
    std::string root = "/tmp/pipensx-image-defer-test-" +
                       std::to_string(static_cast<long long>(getpid()));
    std::mutex mutex;
    std::condition_variable ready;
    int callbacks = 0;
    GameMetadataService::ImageData result;
    {
        GameMetadataService service(root, root + "/missing-index.json");
        service.setImageNetworkPaused(true);
        service.requestImage("http://127.0.0.1:1/deferred-cover.jpg",
            [&](GameMetadataService::ImageData data) {
                std::lock_guard<std::mutex> lock(mutex);
                result = std::move(data);
                callbacks++;
                ready.notify_all();
            });
        {
            std::unique_lock<std::mutex> lock(mutex);
            assert(!ready.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return callbacks > 0;
            }));
        }
        service.setImageNetworkPaused(false);
        std::unique_lock<std::mutex> lock(mutex);
        assert(ready.wait_for(lock, std::chrono::seconds(5), [&] {
            return callbacks == 1;
        }));
        assert(!result);
    }
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

int cancelTracker(void* user) {
    return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
}

void testTrackerCancellation() {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t addressSize = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                       &addressSize) == 0);

    std::atomic<bool> requestDone {false};
    std::thread server([&] {
        int client = accept(listener, nullptr, nullptr);
        if (client >= 0) {
            while (!requestDone)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            close(client);
        }
    });

    std::atomic<bool> cancelled {false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancelled = true;
    });
    char url[128];
    std::snprintf(url, sizeof(url), "http://127.0.0.1:%u/announce",
                  ntohs(address.sin_port));
    uint8_t hash[20] {};
    uint8_t peerId[20] {};
    uint8_t peers[6] {};
    tracker_announce_result_t result {};
    auto started = std::chrono::steady_clock::now();
    tracker_announce_url_ex_cancel(
        url, hash, peerId, 6881, 0, 0, peers, 1, &result,
        cancelTracker, &cancelled);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    requestDone = true;
    canceller.join();
    server.join();
    close(listener);
    assert(cancelled);
    assert(elapsed < std::chrono::seconds(2));
}

void runLiveResolutionIfRequested() {
    const char* magnet = std::getenv("PIPENSX_LIVE_MAGNET");
    if (!magnet || !*magnet)
        return;
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    std::atomic<bool> cancelled{false};
    std::string error;
    MagnetResolver resolver;
    bool ok = resolver.resolveToFile(
        magnet, "/tmp/pipensx-live.torrent", cancelled,
        [](const pipensx::MagnetProgress& progress) {
            std::printf("live stage=%d pieces=%u/%u peer=%u/%u\n",
                        static_cast<int>(progress.stage),
                        progress.completedPieces, progress.totalPieces,
                        progress.peerIndex, progress.peerCount);
        },
        error);
    if (!ok)
        std::fprintf(stderr, "live resolution failed: %s\n", error.c_str());
    assert(ok);
    std::remove("/tmp/pipensx-live.torrent");
    curl_global_cleanup();
}

} // namespace

// Trusted-source allowlist gating every network catalog fetch: only the
// Langegen switch-games repo on GitHub's raw host is accepted; look-alike
// hosts, wrong repos, plain HTTP and path-prefix tricks are rejected.
void testTrustedSourceAllowlist() {
    assert(CatalogService::isTrustedSource(
        "https://raw.githubusercontent.com/Langegen/switch-games/"
        "refs/heads/main/switch_games.json"));

    assert(!CatalogService::isTrustedSource(
        "http://raw.githubusercontent.com/Langegen/switch-games/main/x.json"));
    assert(!CatalogService::isTrustedSource(
        "https://raw.githubusercontent.com/evil/switch-games/main/x.json"));
    assert(!CatalogService::isTrustedSource(
        "https://github.com/bqio/switch-dumps/releases/download/v1/catalog.json"));
    assert(!CatalogService::isTrustedSource(
        "https://raw.githubusercontent.com.evil.example/Langegen/switch-games/x"));
    assert(!CatalogService::isTrustedSource(""));
    assert(GameMetadataService::isTrustedSource(
        "https://raw.githubusercontent.com/i3sey/pipensx-metadata/"
        "data/manifest.json"));
    assert(GameMetadataService::isTrustedSource(
        "https://github.com/i3sey/pipensx-metadata/releases/latest/"
        "download/manifest.json"));
    assert(!GameMetadataService::isTrustedSource(
        "https://raw.githubusercontent.com/i3sey/pipensx-metadata.evil/"
        "data/manifest.json"));
    assert(GameMetadataService::isTrustedRedirect(
        "https://release-assets.githubusercontent.com/github-production-"
        "release-asset/file"));
    assert(!GameMetadataService::isTrustedRedirect(
        "https://release-assets.githubusercontent.com.evil.example/file"));
}

// The Langegen switch_games.json shape: magnet under "magnet", cover under
// "cover", a human "size" string, string "topic_id", and inline metadata the
// detail card renders. parseJson must read all of it.
void testLangegenSchemaParsing() {
    const char* json =
        "[{"
        "\"title\":\"Sonic 06\","
        "\"size\":\"5.19 GB\","
        "\"magnet\":\"magnet:?xt=urn:btih:"
        "8B8016FD97F08E2CC46E3B104B72EC758173C3C9&tr="
        "http%3A%2F%2Fbt.t-ru.org%2Fann%3Fmagnet\","
        "\"topic_id\":\"6878751\","
        "\"year\":\"2006\","
        "\"genre\":\"3D Platformer\","
        "\"developer\":\"Sonic Team\","
        "\"publisher\":\"SEGA\","
        "\"cover\":\"https://example.invalid/cover.png\","
        "\"screenshots\":[\"https://example.invalid/s1.jpg\"],"
        "\"description\":\"A port.\""
        "}]";
    std::vector<CatalogEntry> entries;
    std::string error;
    assert(CatalogService::parseJson(json, entries, error));
    assert(entries.size() == 1);
    const CatalogEntry& e = entries[0];
    assert(e.title == "Sonic 06");
    assert(e.infoHash == "8B8016FD97F08E2CC46E3B104B72EC758173C3C9");
    // 5.19 * 1024^3, rounded.
    assert(e.size == static_cast<uint64_t>(5.19 * 1024.0 * 1024 * 1024 + 0.5));
    assert(e.topicId == 6878751);
    assert(e.year == "2006");
    assert(e.genre == "3D Platformer");
    assert(e.developer == "Sonic Team");
    assert(e.publisher == "SEGA");
    assert(e.posterUrl == "https://example.invalid/cover.png");
    assert(e.screenshots.size() == 1);
    assert(e.description == "A port.");
}

void testBundledLangegenSnapshotLoadsWithoutNetwork() {
    const std::string root = "/tmp/pipensx-bundled-catalog-" +
        std::to_string(static_cast<long long>(getpid()));
    mkdir(root.c_str(), 0755);
    {
        CatalogService catalog(root, "resources/catalog/switch_games.json");
        std::string error;
        assert(catalog.load(error));
        assert(catalog.entries().size() > 1000);

        bool hasInlineArtwork = false;
        bool hasInlineDescription = false;
        for (const CatalogEntry& entry : catalog.entries()) {
            if (!entry.posterUrl.empty() || !entry.screenshots.empty())
                hasInlineArtwork = true;
            if (!entry.description.empty())
                hasInlineDescription = true;
        }
        assert(hasInlineArtwork);
        assert(hasInlineDescription);
    }
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

void testBundledMetadataSnapshotLoadsWithoutNetwork() {
    const std::string root = "/tmp/pipensx-bundled-metadata-" +
        std::to_string(static_cast<long long>(getpid()));
    mkdir(root.c_str(), 0755);
    {
        GameMetadataService metadata(
            root, "resources/catalog/game_metadata_index.json");
        std::string error;
        assert(metadata.load(error));
        assert(metadata.size() > 1000);
        const GameMetadata* known = metadata.findByInfoHash(
            "000762AF952A9ECE472A5C713E3B5D87EAA9DF45");
        assert(known);
        assert(!known->name.empty());
        assert(!known->iconUrl.empty());
        assert(!known->bannerUrl.empty());
    }
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

int main() {
    testMagnetParsing();
    testTrustedSourceAllowlist();
    testCatalogParsing();
    testLangegenSchemaParsing();
    testBundledLangegenSnapshotLoadsWithoutNetwork();
    testBundledMetadataSnapshotLoadsWithoutNetwork();
    testCatalogV2HealthParsing();
    testInfoDictParsing();
    testResolveFromPresetInfoDict();
    testTorrentConstruction();
    testMetadataIndexParsing();
    testMetadataSnapshotAcceptsVerifiedIndex();
    testMetadataSnapshotRejectsTamperedIndex();
    testMetadataLoadPrefersVerifiedRuntimeCache();
    testMetadataFetchVerifiesBeforeAdoptAndFallsBackToCache();
    testMetadataLoadFallsBackWhenRuntimeCacheIsCorrupt();
    testCatalogAndMetadataRefreshAdoptIndependently();
    testOptionalCatalogDataMayBeAbsent();
    testCatalogPresentationUsesGameMetadata();
    testCatalogPresentationFallsBackFieldByField();
    testCatalogGameFilterKeepsUnmatchedSwitchReleases();
    testCatalogBrowseMatchFilterRequiresTitleIdMetadata();
    testAsyncImageDiskCache();
    testImageMemoryCache();
    testImageNetworkWaitsForActiveTransfer();
    testTrackerCancellation();
    runLiveResolutionIfRequested();
    std::puts("catalog tests passed");
    return 0;
}
