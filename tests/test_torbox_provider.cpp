#include "app/torbox_provider.hpp"
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
                {"createtorrent",
                 "{\"success\":true,\"data\":{\"torrent_id\":42}}"},
                {"mylist",
                 "{\"success\":true,\"data\":{\"id\":42,\"name\":\"G\","
                 "\"size\":10,\"progress\":1.0,"
                 "\"download_state\":\"completed\","
                 "\"download_finished\":true,\"download_present\":true,"
                 "\"files\":[{\"id\":7,\"name\":\"a.nsp\",\"size\":10}]}}"},
            });
    TorboxTransport t = [script](const TorboxHttpRequest& r,
                                 TorboxHttpResponse& res, std::string&) {
        for (auto it = script->begin(); it != script->end(); ++it)
            if (r.url.find(it->first) != std::string::npos) {
                res.status = 200;
                res.body = it->second;
                if (it->first == std::string("createtorrent"))
                    script->erase(it);
                return true;
            }
        res.status = 200;
        res.body = "{\"success\":false}";
        return true;
    };
    TorboxProvider p("key", t);
    std::string id, err;
    assert(p.createFromMagnet("magnet:?xt=urn:btih:h", id, err));
    assert(id == "42");
    DebridInfo info;
    assert(p.fetchInfo(id, info, err));
    assert(info.phase == DebridInfo::Phase::Ready);
    assert(info.files.size() == 1 && info.files[0].id == "7" &&
           info.files[0].path == "a.nsp");
    assert(p.selectFiles(id, {"7"}, err));
    std::puts("torbox provider ok");
    return 0;
}
