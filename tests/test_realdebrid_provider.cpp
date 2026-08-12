#include "app/realdebrid_provider.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pipensx;

int main() {
    auto script =
        std::make_shared<std::vector<std::pair<std::string, std::string>>>(
            std::vector<std::pair<std::string, std::string>>{
                {"/user",
                 "{\"id\":12345,\"username\":\"test\"}"},
                {"/torrents/addMagnet",
                 "{\"id\":\"abc123\"}"},
                {"/torrents/info/abc123",
                 "{\"id\":\"abc123\",\"filename\":\"Game.nsp\","
                 "\"bytes\":1000000,\"progress\":100,"
                 "\"status\":\"downloaded\","
                 "\"files\":[{\"id\":\"1\",\"path\":\"game.nsp\","
                 "\"bytes\":900000,\"selected\":1}],"
                 "\"links\":[\"https://rd.to/dl/x\"]}"},
                {"/torrents/selectFiles/abc123",
                 ""},
                {"/unrestrict/link",
                 "{\"id\":\"unrestrict123\",\"download\":"
                 "\"https://rd.dl1.real-debrid.com/dl/x/game.nsp\"}"},
                {"/torrents/delete/abc123",
                 ""},
            });
    RdTransport t = [script](const RdHttpRequest& r,
                              RdHttpResponse& res, std::string&) {
        for (auto it = script->begin(); it != script->end(); ++it)
            if (r.url.find(it->first) != std::string::npos) {
                res.status = 200;
                res.body = it->second;
                return true;
            }
        res.status = 200;
        res.body = "{}";
        return true;
    };
    RealdebridProvider p("key", t);
    std::string id, err;
    assert(p.validate(err));
    assert(p.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    assert(id == "abc123");
    DebridInfo info;
    assert(p.fetchInfo(id, info, err));
    assert(info.phase == DebridInfo::Phase::Ready);
    assert(info.name == "Game.nsp");
    assert(info.files.size() == 1 && info.files[0].id == "1");
    assert(info.links.size() == 1);

    assert(p.selectFiles(id, {"1"}, err));

    std::string url;
    assert(p.resolveDownloadUrl(id, info, 0, info.files[0], url, err));

    assert(p.remove(id, err));

    // Test failed status detection
    RdTransport failedT =
        [](const RdHttpRequest& r, RdHttpResponse& res, std::string&) {
            if (r.url.find("/torrents/info/") != std::string::npos) {
                res.status = 200;
                res.body =
                    "{\"id\":\"err\",\"filename\":\"bad\",\"bytes\":0,"
                    "\"progress\":0,\"status\":\"error\","
                    "\"files\":[],\"links\":[]}";
            } else if (r.url.find("/torrents/addMagnet") != std::string::npos) {
                res.status = 200;
                res.body = "{\"id\":\"err\"}";
            } else if (r.url.find("/user") != std::string::npos) {
                res.status = 200;
                res.body = "{\"id\":12345}";
            } else {
                res.status = 200;
                res.body = "{}";
            }
            return true;
        };
    RealdebridProvider failP("key", failedT);
    assert(failP.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    DebridInfo failInfo;
    assert(failP.fetchInfo(id, failInfo, err));
    assert(failInfo.phase == DebridInfo::Phase::Failed);

    // Test waiting_files_selection phase
    RdTransport selectT =
        [](const RdHttpRequest& r, RdHttpResponse& res, std::string&) {
            if (r.url.find("/torrents/info/") != std::string::npos) {
                res.status = 200;
                res.body =
                    "{\"id\":\"sel\",\"filename\":\"Game\",\"bytes\":100,"
                    "\"progress\":0,\"status\":\"waiting_files_selection\","
                    "\"files\":[{\"id\":\"1\",\"path\":\"f\",\"bytes\":100}],"
                    "\"links\":[]}";
            } else if (r.url.find("/torrents/addMagnet") != std::string::npos) {
                res.status = 200;
                res.body = "{\"id\":\"sel\"}";
            } else if (r.url.find("/user") != std::string::npos) {
                res.status = 200;
                res.body = "{\"id\":12345}";
            } else {
                res.status = 200;
                res.body = "{}";
            }
            return true;
        };
    RealdebridProvider selP("key", selectT);
    assert(selP.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    DebridInfo selInfo;
    assert(selP.fetchInfo(id, selInfo, err));
    assert(selInfo.phase == DebridInfo::Phase::AwaitingSelection);

    // Test downloading phase
    RdTransport dlT =
        [](const RdHttpRequest& r, RdHttpResponse& res, std::string&) {
            if (r.url.find("/torrents/info/") != std::string::npos) {
                res.status = 200;
                res.body =
                    "{\"id\":\"dl\",\"filename\":\"Game\",\"bytes\":100,"
                    "\"progress\":42,\"status\":\"downloading\","
                    "\"files\":[],\"links\":[]}";
            } else if (r.url.find("/torrents/addMagnet") != std::string::npos) {
                res.status = 200;
                res.body = "{\"id\":\"dl\"}";
            } else if (r.url.find("/user") != std::string::npos) {
                res.status = 200;
                res.body = "{\"id\":12345}";
            } else {
                res.status = 200;
                res.body = "{}";
            }
            return true;
        };
    RealdebridProvider dlP("key", dlT);
    assert(dlP.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    DebridInfo dlInfo;
    assert(dlP.fetchInfo(id, dlInfo, err));
    assert(dlInfo.phase == DebridInfo::Phase::Downloading);

    std::printf("test_realdebrid_provider: all assertions passed\n");
    return 0;
}
