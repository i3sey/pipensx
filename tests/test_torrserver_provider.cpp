#include "app/torrserver_provider.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace pipensx;

namespace {

// One entry per expected call, answered in order. Every request is recorded so
// the assertions can check what actually went over the wire.
struct Recorder {
    std::vector<TsHttpRequest> seen;
    std::vector<std::pair<long, std::string>> replies;
    size_t next = 0;
};

TsTransport scripted(Recorder* rec) {
    return [rec](const TsHttpRequest& request, TsHttpResponse& response,
                 std::string&) {
        rec->seen.push_back(request);
        assert(rec->next < rec->replies.size());
        response.status = rec->replies[rec->next].first;
        response.body = rec->replies[rec->next].second;
        ++rec->next;
        return true;
    };
}

const char* kGettingInfo =
    "{\"name\":\"infohash:abc\",\"hash\":\"abc\",\"stat\":1,"
    "\"stat_string\":\"Torrent getting info\"}";

const char* kWorking =
    "{\"name\":\"Big Buck Bunny\",\"hash\":\"abc\",\"stat\":3,"
    "\"stat_string\":\"Torrent working\",\"torrent_size\":276445467,"
    "\"file_stats\":[{\"id\":1,\"path\":\"BBB/sub.srt\",\"length\":140},"
    "{\"id\":2,\"path\":\"BBB/movie.mp4\",\"length\":276134947}]}";

void testNormalizeBaseUrl() {
    assert(TorrserverProvider::normalizeBaseUrl("192.168.1.10:8090") ==
           "http://192.168.1.10:8090");
    assert(TorrserverProvider::normalizeBaseUrl(" http://box:8090/ ") ==
           "http://box:8090");
    assert(TorrserverProvider::normalizeBaseUrl("https://box/") ==
           "https://box");
    assert(TorrserverProvider::normalizeBaseUrl("   ").empty());
    assert(TorrserverProvider::normalizeBaseUrl("").empty());
}

void testValidate() {
    Recorder rec;
    rec.replies = {{200, "MatriX.141\n"}, {200, "  "}, {401, ""}};
    TorrserverProvider provider("box:8090", scripted(&rec));
    std::string error;
    assert(provider.validate(error));
    assert(rec.seen[0].url == "http://box:8090/echo");
    // Something answered on that port, but it is not a TorrServer.
    assert(!provider.validate(error) && !error.empty());
    assert(!provider.validate(error) &&
           error.find("credentials") != std::string::npos);
    // An address nobody typed cannot be reached, and says so without a call.
    Recorder empty;
    TorrserverProvider unset("", scripted(&empty));
    assert(!unset.validate(error) && empty.seen.empty());
}

void testCreatePollResolveRemove() {
    Recorder rec;
    rec.replies = {{200, kGettingInfo}, {200, kGettingInfo}, {200, kWorking},
                   {200, "{}"}};
    TorrserverProvider provider("http://box:8090", scripted(&rec));
    std::string id, error;
    assert(provider.createFromMagnet("magnet:?xt=urn:btih:abc&dn=x", id,
                                     error));
    assert(id == "abc");
    assert(rec.seen[0].url == "http://box:8090/torrents");
    assert(rec.seen[0].body.find("\"action\":\"add\"") != std::string::npos);
    // The magnet must survive JSON encoding intact — '&' and ':' and all.
    assert(rec.seen[0].body.find("magnet:?xt=urn:btih:abc&dn=x") !=
           std::string::npos);

    // Metadata not in yet: no files, so the transfer keeps polling.
    DebridInfo info;
    assert(provider.fetchInfo(id, info, error));
    assert(info.phase == DebridInfo::Phase::Creating && info.files.empty());
    // Polling re-adds by infohash, which also reloads a torrent the server
    // has dropped from memory.
    assert(rec.seen[1].body.find("magnet:?xt=urn:btih:abc") !=
           std::string::npos);

    assert(provider.fetchInfo(id, info, error));
    assert(info.phase == DebridInfo::Phase::Ready);
    assert(info.name == "Big Buck Bunny" && info.bytes == 276445467ull);
    assert(info.files.size() == 2);
    assert(info.files[1].id == "2" && info.files[1].path == "BBB/movie.mp4" &&
           info.files[1].bytes == 276134947ull);
    // Nothing to select, and no request for it.
    assert(provider.selectFiles(id, {"2"}, error));

    std::string url;
    assert(provider.resolveDownloadUrl(id, info, 1, info.files[1], url,
                                       error));
    assert(url == "http://box:8090/play/abc/2");
    assert(provider.allowsPlaintextLinks());

    assert(provider.remove(id, error));
    assert(rec.seen.back().body.find("\"action\":\"rem\"") !=
           std::string::npos);
    assert(rec.seen.back().body.find("\"hash\":\"abc\"") != std::string::npos);
    assert(rec.seen.size() == 4);
}

void testServerErrorsSurface() {
    Recorder rec;
    rec.replies = {{500, "boom"}, {200, "not json"}};
    TorrserverProvider provider("box", scripted(&rec));
    std::string id, error;
    assert(!provider.createFromMagnet("magnet:?xt=urn:btih:abc", id, error));
    assert(error.find("500") != std::string::npos);
    error.clear();
    assert(!provider.createFromMagnet("magnet:?xt=urn:btih:abc", id, error));
    assert(!error.empty());
}

} // namespace

int main() {
    testNormalizeBaseUrl();
    testValidate();
    testCreatePollResolveRemove();
    testServerErrorsSurface();
    std::puts("torrserver provider ok");
    return 0;
}
