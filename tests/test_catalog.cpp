#include "app/catalog_service.hpp"
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
using pipensx::CatalogService;
using pipensx::catalogContentBadge;
using pipensx::GameMetadata;
using pipensx::GameMetadataService;
using pipensx::MagnetResolver;
using pipensx::MagnetSpec;
using pipensx::mergeScreenshotUrls;

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
    assert(catalogContentBadge(&metadata) == "Contains NSP/NSZ");
    assert(catalogContentBadge(nullptr) == "Does not contain NSP/NSZ");
    std::vector<std::string> screenshots =
        mergeScreenshotUrls(&metadata, entry, 6);
    assert(screenshots.size() == 6);
    assert(screenshots[0] == "https://example/meta-1.jpg");
    assert(screenshots[1] == "https://example/shared.jpg");
    assert(screenshots[2] == "https://example/catalog-1.jpg");
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

int main() {
    testMagnetParsing();
    testCatalogParsing();
    testCatalogV2HealthParsing();
    testTorrentConstruction();
    testMetadataIndexParsing();
    testCatalogPresentationUsesGameMetadata();
    testAsyncImageDiskCache();
    testTrackerCancellation();
    runLiveResolutionIfRequested();
    std::puts("catalog tests passed");
    return 0;
}
