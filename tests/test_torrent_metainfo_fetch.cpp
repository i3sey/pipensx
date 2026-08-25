#include "app/torrent_metainfo_fetch.hpp"
#include "app/magnet_resolver.hpp"

extern "C" {
#include "core/sha1.h"
}

#include <atomic>
#include <cassert>
#include <cstdio>
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
    int val = 0;
    int valb = -8;
    for (const char* p = text; *p; ++p) {
        if (*p == '=' || *p == '\n')
            break;
        const int c = b64Value(*p);
        if (c < 0)
            continue;
        val = (val << 6) + c;
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
           "https://itorrents.org/torrent/"
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

int main() {
    testItorrentsUrl();
    testWriteFromInfoDict();
    testRejectBadBody();
    testFetchViaTransport();
    testEnsurePrefersInfoDict();
    std::printf("test_torrent_metainfo_fetch: all assertions passed\n");
    return 0;
}
