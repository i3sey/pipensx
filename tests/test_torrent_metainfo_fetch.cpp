#include "app/torrent_metainfo_fetch.hpp"
#include "app/magnet_resolver.hpp"

extern "C" {
#include "core/sha1.h"
}

#include <atomic>
#include <cassert>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace pipensx;

// Same info dict as tests/test_catalog.cpp (SHA-1 = kHash).
constexpr const char* kInfoDictB64 =
    "ZDY6bGVuZ3RoaTFlNDpuYW1lODp0ZXN0Lm5zcDEyOnBpZWNlIGxlbmd0aGkxNjM4NGU2On"
    "BpZWNlczIwOjAxMjM0NTY3ODkwMTIzNDU2Nzg5ZQ==";
static const char kHash[] = "9e2b6f8acd7b3da966e5ff4ba4c0e00750ac2595";

static int b64Value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static std::vector<uint8_t> decodeB64(const char* text) {
    std::vector<uint8_t> out;
    uint32_t val = 0;
    int valb = -8;
    for (const char* p = text; *p; ++p) {
        if (*p == '=' || *p == '\n')
            break;
        const int c = b64Value(*p);
        if (c < 0)
            continue;
        val = (val << 6) + static_cast<uint32_t>(c);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

static void testItorrentsUrl() {
    assert(itorrentsUrlForHash(kHash) ==
           "https://itorrents.net/torrent/"
           "9E2B6F8ACD7B3DA966E5FF4BA4C0E00750AC2595.torrent");
}

static void testWriteFromInfoDict() {
    char dir[] = "/tmp/pipensx-metaXXXXXX";
    assert(mkdtemp(dir));
    const std::string path = std::string(dir) + "/out.torrent";
    const std::string magnet =
        std::string("magnet:?xt=urn:btih:") + kHash +
        "&tr=http://bt.t-ru.org/ann?magnet";
    std::vector<uint8_t> infoDict = decodeB64(kInfoDictB64);
    assert(infoDict.size() == 82);
    std::string error;
    assert(writeTorrentFromInfoDict(magnet, infoDict, path, error));
    const std::string body = readFile(path);
    assert(torrentBodyMatchesInfoHash(
        std::vector<uint8_t>(body.begin(), body.end()), kHash, error));
    unlink(path.c_str());
    rmdir(dir);
}

static void testRejectBadBody() {
    std::string error;
    const std::vector<uint8_t> html = {'<', 'h', 't', 'm', 'l'};
    assert(!torrentBodyMatchesInfoHash(html, kHash, error));
    assert(!error.empty());

    std::vector<uint8_t> infoDict = decodeB64(kInfoDictB64);
    MagnetSpec spec;
    assert(MagnetResolver::parse(
        std::string("magnet:?xt=urn:btih:") + kHash +
            "&tr=http://bt.t-ru.org/ann?magnet",
        spec, error));
    std::vector<uint8_t> torrent;
    assert(MagnetResolver::buildTorrent(spec, infoDict, torrent, error));
    const char* wrong = "e21269d03d34b557f63ce915dea14f765c9c9798";
    assert(!torrentBodyMatchesInfoHash(torrent, wrong, error));
}

static void testFetchViaTransport() {
    char dir[] = "/tmp/pipensx-metaXXXXXX";
    assert(mkdtemp(dir));
    const std::string path = std::string(dir) + "/fetched.torrent";
    const std::string magnet =
        std::string("magnet:?xt=urn:btih:") + kHash +
        "&tr=http://bt.t-ru.org/ann?magnet";
    std::vector<uint8_t> infoDict = decodeB64(kInfoDictB64);
    std::string error;
    assert(writeTorrentFromInfoDict(magnet, infoDict, path + ".src", error));
    const std::string body = readFile(path + ".src");

    TorrentHttpGet transport = [&](const std::string& url,
                                   std::vector<uint8_t>& out, long& status,
                                   std::string&) {
        assert(url == itorrentsUrlForHash(kHash));
        out.assign(body.begin(), body.end());
        status = 200;
        return true;
    };
    std::atomic<bool> cancelled{false};
    assert(fetchTorrentByInfoHash(kHash, path, cancelled, error, &transport));
    const std::string fetched = readFile(path);
    assert(torrentBodyMatchesInfoHash(
        std::vector<uint8_t>(fetched.begin(), fetched.end()), kHash, error));

    TorrentHttpGet badStatus = [](const std::string&, std::vector<uint8_t>&,
                                  long& status, std::string&) {
        status = 404;
        return true;
    };
    assert(!fetchTorrentByInfoHash(kHash, path + ".404", cancelled, error,
                                   &badStatus));

    TorrentHttpGet garbage = [](const std::string&, std::vector<uint8_t>& out,
                                long& status, std::string&) {
        out = {'n', 'o', 'p', 'e'};
        status = 200;
        return true;
    };
    assert(!fetchTorrentByInfoHash(kHash, path + ".bad", cancelled, error,
                                   &garbage));

    unlink((path + ".src").c_str());
    unlink(path.c_str());
    rmdir(dir);
}

static void testEnsurePrefersInfoDict() {
    char dir[] = "/tmp/pipensx-metaXXXXXX";
    assert(mkdtemp(dir));
    const std::string path = std::string(dir) + "/ensured.torrent";
    const std::string magnet =
        std::string("magnet:?xt=urn:btih:") + kHash +
        "&tr=http://bt.t-ru.org/ann?magnet";
    std::vector<uint8_t> infoDict = decodeB64(kInfoDictB64);
    bool httpCalled = false;
    TorrentHttpGet transport = [&](const std::string&, std::vector<uint8_t>&,
                                   long&, std::string&) {
        httpCalled = true;
        return false;
    };
    std::atomic<bool> cancelled{false};
    std::string error;
    assert(ensureTorrentFileForDebrid(magnet, kHash, infoDict, path, cancelled,
                                      error, &transport));
    assert(!httpCalled);
    unlink(path.c_str());
    rmdir(dir);
}

static void testHttpsCacheRetryable() {
    // curl timeout wording from the failing Switch log.
    assert(httpsCacheErrorRetryable(
        "Torrent cache request failed: Timeout was reached."));
    assert(httpsCacheErrorRetryable("SSL connection timeout"));
    // Definitive failures must not be retried.
    assert(!httpsCacheErrorRetryable("Torrent cache HTTP 404."));
    assert(!httpsCacheErrorRetryable(
        "Torrent cache body does not match the info hash."));
    assert(!httpsCacheErrorRetryable("Cancelled."));
    assert(!httpsCacheErrorRetryable(""));
}

static void runLiveCacheFetchIfRequested() {
    const char* hash = std::getenv("PIPENSX_LIVE_TORRENT_HASH");
    if (!hash || !*hash)
        return;
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    std::atomic<bool> cancelled{false};
    std::string error;
    assert(fetchTorrentByInfoHash(hash, "/tmp/pipensx-live-cache.torrent",
                                  cancelled, error));
    unlink("/tmp/pipensx-live-cache.torrent");
    curl_global_cleanup();
}

class PendingProvider : public DebridProvider {
public:
    int magnets = 0, uploads = 0, removals = 0;
    bool ready = false, failed = false, cancelOnPoll = false;
    std::atomic<bool>* cancelled = nullptr;
    bool validate(std::string&) override { return true; }
    bool createFromMagnet(const std::string&, std::string& id,
                          std::string&) override {
        ++magnets;
        id = "pending";
        return true;
    }
    bool createFromFile(const std::string&, std::string&, std::string&) override {
        ++uploads;
        return false;
    }
    bool fetchInfo(const std::string&, DebridInfo& info, std::string&) override {
        info = {};
        info.phase = failed ? DebridInfo::Phase::Failed
                            : DebridInfo::Phase::Downloading;
        if (ready)
            info.files.push_back({"1", "test.nsp", 1});
        if (cancelOnPoll)
            cancelled->store(true);
        return true;
    }
    bool selectFiles(const std::string&, const std::vector<std::string>&,
                     std::string&) override { return true; }
    bool resolveDownloadUrl(const std::string&, const DebridInfo&, size_t,
                            const DebridFile&, std::string&,
                            std::string&) override { return false; }
    bool remove(const std::string&, std::string&) override {
        ++removals;
        return true;
    }
    const char* name() const override { return "pending"; }
};

static void testKeepUncached() {
    char dir[] = "/tmp/pipensx-keepXXXXXX";
    assert(mkdtemp(dir));
    const std::string path = std::string(dir) + "/out.torrent";
    const std::string magnet = std::string("magnet:?xt=urn:btih:") + kHash +
        "&tr=http://bt.t-ru.org/ann?magnet";
    for (int scenario = 0; scenario < 6; ++scenario) {
        std::atomic<bool> cancelled{false};
        PendingProvider provider;
        provider.cancelled = &cancelled;
        provider.cancelOnPoll = scenario == 2 || scenario == 5;
        provider.failed = scenario == 3;
        provider.ready = scenario == 4;
        const bool keep = scenario != 0 && scenario != 5;
        std::string id, error;
        DebridInfo info;
        std::ofstream(path) << "temporary data";
        const bool ok = createDebridWithMetainfoFallback(
            provider, magnet, kHash, decodeB64(kInfoDictB64), path,
            cancelled, std::chrono::steady_clock::now(), id, info, error,
            {}, keep);
        assert(ok == provider.ready);
        assert(provider.magnets == 1);
        assert(access(path.c_str(), F_OK) != 0);
        if (scenario == 1 || scenario == 2) {
            assert(id == "pending");
            assert(provider.removals == 0 && provider.uploads == 0);
            assert(error.find("kept") != std::string::npos);
        } else if (provider.ready) {
            assert(id == "pending");
            assert(provider.removals == 0 && provider.uploads == 0);
        } else {
            assert(id.empty());
            assert(provider.removals == 1);
            assert(provider.uploads == (provider.cancelOnPoll ? 0 : 1));
        }
    }
    rmdir(dir);
}

static void testReuseSavedAndAccountHash() {
    class ReuseProvider final : public DebridProvider {
    public:
        bool validate(std::string&) override { return true; }
        bool createFromMagnet(const std::string&, std::string& id,
                              std::string&) override {
            ++creates;
            id = "fresh-created";
            return true;
        }
        bool createFromFile(const std::string&, std::string& id,
                            std::string&) override {
            ++uploads;
            id = "fresh-upload";
            return true;
        }
        bool fetchInfo(const std::string& id, DebridInfo& info,
                       std::string& error) override {
            if (id == "gone-11") {
                error = "TorBox torrent not found.";
                return false;
            }
            if (id == "caching-13") {
                info = DebridInfo{};
                info.phase = DebridInfo::Phase::Downloading;
                info.progress = 0.45;
                if (cancelOnFetch)
                    *cancelOnFetch = true;
                return true;
            }
            info = DebridInfo{};
            info.phase = DebridInfo::Phase::Ready;
            info.name = "Existing";
            info.files.push_back({"1", "game.nsp", 4096});
            return true;
        }
        bool findByHash(const std::string&, std::string& id,
                        DebridInfo& info, std::string&) override {
            if (!hasHashMatch)
                return false;
            id = "hash-12";
            info = DebridInfo{};
            info.phase = DebridInfo::Phase::Ready;
            info.name = "Hash Matched";
            info.files.push_back({"1", "game.nsp", 4096});
            return true;
        }
        bool selectFiles(const std::string&, const std::vector<std::string>&,
                         std::string&) override {
            return true;
        }
        bool resolveDownloadUrl(const std::string&, const DebridInfo&, size_t,
                                const DebridFile&, std::string&,
                                std::string&) override {
            return false;
        }
        bool remove(const std::string&, std::string&) override {
            ++removals;
            return true;
        }
        const char* name() const override { return "reuse"; }

        int creates = 0;
        int uploads = 0;
        int removals = 0;
        bool hasHashMatch = false;
        std::atomic<bool>* cancelOnFetch = nullptr;
    };

    const std::string magnet = std::string("magnet:?xt=urn:btih:") + kHash;
    const std::string tmp = "/tmp/pipensx-reuse-unused.torrent";

    // 1. Saved existingDebridId ready: reuses without creating a new transfer.
    {
        ReuseProvider provider;
        std::atomic<bool> cancelled{false};
        std::string id, error;
        DebridInfo info;
        bool reused = false;
        assert(createDebridWithMetainfoFallback(
            provider, magnet, kHash, {}, tmp, cancelled,
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            id, info, error, {}, true, "saved-10", &reused));
        assert(id == "saved-10");
        assert(reused);
        assert(provider.creates == 0 && provider.uploads == 0);
    }

    // 2. Stale saved ID ("not found") falls back to provider.findByHash match.
    {
        ReuseProvider provider;
        provider.hasHashMatch = true;
        std::atomic<bool> cancelled{false};
        std::string id, error;
        DebridInfo info;
        bool reused = false;
        assert(createDebridWithMetainfoFallback(
            provider, magnet, kHash, {}, tmp, cancelled,
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            id, info, error, {}, true, "gone-11", &reused));
        assert(id == "hash-12");
        assert(reused);
        assert(provider.creates == 0 && provider.uploads == 0);
    }

    // 3. Unfinished saved transfer retained without removal on cancel.
    {
        ReuseProvider provider;
        std::atomic<bool> cancelled{false};
        provider.cancelOnFetch = &cancelled;
        std::string id, error;
        DebridInfo info;
        bool reused = false;
        assert(!createDebridWithMetainfoFallback(
            provider, magnet, kHash, {}, tmp, cancelled,
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            id, info, error, {}, true, "caching-13", &reused));
        assert(cancelled.load());
        assert(id == "caching-13");
        assert(reused);
        assert(provider.removals == 0);
        assert(provider.creates == 0);
    }
}

int main() {
    testKeepUncached();
    testReuseSavedAndAccountHash();
    testItorrentsUrl();
    testWriteFromInfoDict();
    testRejectBadBody();
    testFetchViaTransport();
    testEnsurePrefersInfoDict();
    testHttpsCacheRetryable();
    runLiveCacheFetchIfRequested();
    std::printf("test_torrent_metainfo_fetch: all assertions passed\n");
    return 0;
}
