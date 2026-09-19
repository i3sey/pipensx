#include "app/alldebrid_provider.hpp"

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
                 "{\"status\":\"success\",\"data\":{\"user\":"
                 "{\"username\":\"test\",\"isPremium\":true}}}"},
                {"/magnet/upload",
                 "{\"status\":\"success\",\"data\":{\"magnets\":["
                 "{\"id\":123456}]}}"},
                {"/magnet/status",
                 "{\"status\":\"success\",\"data\":{\"magnets\":[{"
                 "\"id\":123456,\"filename\":\"Game.nsp\",\"size\":1000000,"
                 "\"status\":\"Ready\",\"statusCode\":4,"
                 "\"downloaded\":1000000}]}}"},
                {"/magnet/files",
                 "{\"status\":\"success\",\"data\":{\"magnets\":[{"
                 "\"id\":\"123456\",\"files\":[{\"n\":\"game.nsp\","
                 "\"s\":900000,\"l\":\"https://alldebrid.com/f/x\"}]}]}}"},
                {"/link/unlock",
                 "{\"status\":\"success\",\"data\":{\"filename\":\"game.nsp\","
                 "\"filesize\":900000,\"link\":"
                 "\"https://xx.debrid.it/dl/x/game.nsp\"}}"},
                {"/magnet/delete",
                 "{\"status\":\"success\",\"data\":{\"message\":\"deleted\"}}"},
            });
    AdTransport t = [script](const AdHttpRequest& r,
                             AdHttpResponse& res, std::string&) {
        for (auto it = script->begin(); it != script->end(); ++it)
            if (r.url.find(it->first) != std::string::npos) {
                res.status = 200;
                res.body = it->second;
                return true;
            }
        res.status = 200;
        res.body = "{\"status\":\"success\",\"data\":{}}";
        return true;
    };
    AlldebridProvider p("key", t);
    std::string id, err;
    assert(p.validate(err));
    assert(p.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    assert(id == "123456");
    DebridInfo info;
    assert(p.fetchInfo(id, info, err));
    assert(info.phase == DebridInfo::Phase::Ready);
    assert(info.name == "Game.nsp");
    assert(info.files.size() == 1);
    assert(info.files[0].path == "game.nsp");
    assert(info.links.size() == 1);

    assert(p.selectFiles(id, {"1"}, err));

    std::string url;
    assert(p.resolveDownloadUrl(id, info, 0, info.files[0], url, err));
    assert(url.find("game.nsp") != std::string::npos);
    assert(url.compare(0, 8, "https://") == 0);

    info.files[0].bytes = 900000;
    AdTransport zipT =
        [](const AdHttpRequest& r, AdHttpResponse& res, std::string&) {
            res.status = 200;
            if (r.url.find("/link/unlock") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":"
                    "{\"filename\":\"game.zip\",\"filesize\":900000,"
                    "\"link\":\"https://xx.debrid.it/x.zip\"}}";
            else
                res.body = "{\"status\":\"success\",\"data\":{}}";
            return true;
        };
    AlldebridProvider zipP("key", zipT);
    assert(!zipP.resolveDownloadUrl(id, info, 0, info.files[0], url, err));
    assert(err.find("archive") != std::string::npos);

    AdTransport sizeT =
        [](const AdHttpRequest& r, AdHttpResponse& res, std::string&) {
            res.status = 200;
            if (r.url.find("/link/unlock") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":"
                    "{\"filename\":\"game.nsp\",\"filesize\":1,"
                    "\"link\":\"https://xx.debrid.it/game.nsp\"}}";
            else
                res.body = "{\"status\":\"success\",\"data\":{}}";
            return true;
        };
    AlldebridProvider sizeP("key", sizeT);
    assert(!sizeP.resolveDownloadUrl(id, info, 0, info.files[0], url, err));
    assert(err.find("file size") != std::string::npos);

    assert(p.remove(id, err));

    AdTransport failedT =
        [](const AdHttpRequest& r, AdHttpResponse& res, std::string&) {
            res.status = 200;
            if (r.url.find("/magnet/status") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"magnets\":[{"
                    "\"id\":1,\"filename\":\"bad\",\"size\":0,"
                    "\"status\":\"Error\",\"statusCode\":9}]}}";
            else if (r.url.find("/magnet/upload") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"magnets\":["
                    "{\"id\":1}]}}";
            else if (r.url.find("/user") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"user\":"
                    "{\"username\":\"t\",\"isPremium\":true}}}";
            else
                res.body = "{\"status\":\"success\",\"data\":{}}";
            return true;
        };
    AlldebridProvider failP("key", failedT);
    assert(failP.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    DebridInfo failInfo;
    assert(failP.fetchInfo(id, failInfo, err));
    assert(failInfo.phase == DebridInfo::Phase::Failed);

    AdTransport dlT =
        [](const AdHttpRequest& r, AdHttpResponse& res, std::string&) {
            res.status = 200;
            if (r.url.find("/magnet/status") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"magnets\":[{"
                    "\"id\":2,\"filename\":\"Game\",\"size\":100,"
                    "\"status\":\"Downloading\",\"statusCode\":1,"
                    "\"downloaded\":42}]}}";
            else if (r.url.find("/magnet/upload") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"magnets\":["
                    "{\"id\":2}]}}";
            else if (r.url.find("/user") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"user\":"
                    "{\"username\":\"t\",\"isPremium\":true}}}";
            else
                res.body = "{\"status\":\"success\",\"data\":{}}";
            return true;
        };
    AlldebridProvider dlP("key", dlT);
    assert(dlP.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    DebridInfo dlInfo;
    assert(dlP.fetchInfo(id, dlInfo, err));
    assert(dlInfo.phase == DebridInfo::Phase::Downloading);
    assert(dlInfo.progress > 0.41 && dlInfo.progress < 0.43);

    AdTransport queueT =
        [](const AdHttpRequest& r, AdHttpResponse& res, std::string&) {
            res.status = 200;
            if (r.url.find("/magnet/status") != std::string::npos)
                res.body =
                    "{\"status\":\"success\",\"data\":{\"magnets\":[{"
                    "\"id\":3,\"filename\":\"Game\",\"size\":100,"
                    "\"status\":\"In Queue\",\"statusCode\":0,"
                    "\"downloaded\":0}]}}";
            else
                res.body = "{\"status\":\"success\",\"data\":{}}";
            return true;
        };
    AlldebridProvider queueP("key", queueT);
    DebridInfo queueInfo;
    assert(queueP.fetchInfo("3", queueInfo, err));
    assert(queueInfo.phase == DebridInfo::Phase::Creating);

    std::printf("test_alldebrid_provider: all assertions passed\n");
    return 0;
}
