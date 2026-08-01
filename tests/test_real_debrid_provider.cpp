#include "app/real_debrid_provider.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pipensx;

int main() {
    auto s = std::make_shared<std::vector<std::pair<std::string, std::string>>>(
        std::vector<std::pair<std::string, std::string>>{
            {"addMagnet", "{\"id\":\"T1\",\"uri\":\"u\"}"},
            {"info/T1",
             "{\"id\":\"T1\",\"filename\":\"G\","
             "\"status\":\"waiting_files_selection\","
             "\"bytes\":10,\"progress\":0,"
             "\"files\":[{\"id\":1,\"path\":\"a.nsp\",\"bytes\":10,"
             "\"selected\":0}],"
             "\"links\":[]}"},
            {"selectFiles/T1", ""},
            {"info/T1",
             "{\"id\":\"T1\",\"filename\":\"G\",\"status\":\"downloaded\","
             "\"bytes\":10,\"progress\":100,"
             "\"files\":[{\"id\":1,\"path\":\"a.nsp\",\"bytes\":10,"
             "\"selected\":1}],"
             "\"links\":[\"https://rd/l1\"]}"},
            {"unrestrict/link", "{\"download\":\"https://direct/a\"}"},
        });
    RdTransport t = [s](const RdHttpRequest& r, RdHttpResponse& res,
                        std::string&) {
        for (auto it = s->begin(); it != s->end(); ++it)
            if (r.url.find(it->first) != std::string::npos) {
                res.status = 200;
                res.body = it->second;
                s->erase(it);
                return true;
            }
        res.status = 200;
        res.body = "{}";
        return true;
    };
    RealDebridProvider p("tok", t);
    std::string id, err;
    assert(p.createFromMagnet("magnet:?xt=urn:btih:h", id, err) &&
           id == "T1");
    DebridInfo a;
    assert(p.fetchInfo(id, a, err));
    assert(a.phase == DebridInfo::Phase::AwaitingSelection);
    assert(a.files.size() == 1 && a.files[0].id == "1");
    assert(p.selectFiles(id, {"1"}, err));
    DebridInfo b;
    assert(p.fetchInfo(id, b, err));
    assert(b.phase == DebridInfo::Phase::Ready && b.links.size() == 1);
    std::string url;
    assert(p.resolveDownloadUrl(id, b, 0, b.files[0], url, err) &&
           url == "https://direct/a");
    std::puts("realdebrid provider ok");
    return 0;
}
