#include "app/real_debrid_client.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>

using namespace pipensx;

static RdTransport scripted(
    std::vector<std::pair<std::string, std::string>>* s,
    std::vector<RdHttpRequest>* seen = nullptr) {
    return [s, seen](const RdHttpRequest& req, RdHttpResponse& res,
                     std::string&) {
        if (seen)
            seen->push_back(req);
        for (auto it = s->begin(); it != s->end(); ++it)
            if (req.url.find(it->first) != std::string::npos) {
                res.status = 200;
                res.body = it->second;
                s->erase(it);
                return true;
            }
        res.status = 200;
        res.body = "{}";
        return true;
    };
}

static void testParsers() {
    std::string id, err;
    assert(RealDebridClient::parseAdd("{\"id\":\"ABC\",\"uri\":\"x\"}", id,
                                      err));
    assert(id == "ABC");

    RdInfo info;
    assert(RealDebridClient::parseInfo(
        "{\"id\":\"ABC\",\"filename\":\"Game\",\"hash\":\"h\","
        "\"bytes\":1000,\"progress\":100,\"status\":\"downloaded\","
        "\"files\":[{\"id\":1,\"path\":\"/game.nsp\",\"bytes\":1000,"
        "\"selected\":1}],"
        "\"links\":[\"https://rd/dl1\"]}",
        info, err));
    assert(info.status == "downloaded");
    assert(info.progress == 1.0);
    assert(info.files.size() == 1 && info.files[0].id == 1 &&
           info.files[0].selected);
    assert(info.links.size() == 1 && info.links[0] == "https://rd/dl1");

    std::string url;
    assert(RealDebridClient::parseUnrestrict(
        "{\"download\":\"https://direct/file\",\"filename\":\"game.nsp\"}",
        url, err));
    assert(url == "https://direct/file");

    RdInfo bad;
    assert(!RealDebridClient::parseInfo("{\"files\":\"nope\"}", bad, err) ||
           bad.files.empty());
    std::puts("rd parsers ok");
}

static void testAddSelectPollUnrestrict() {
    std::vector<std::pair<std::string, std::string>> s = {
        {"addMagnet", "{\"id\":\"T1\",\"uri\":\"u\"}"},
        {"info/T1",
         "{\"id\":\"T1\",\"filename\":\"G\",\"status\":\"downloaded\","
         "\"bytes\":10,\"progress\":100,"
         "\"files\":[{\"id\":1,\"path\":\"/a.nsp\",\"bytes\":10,"
         "\"selected\":1}],"
         "\"links\":[\"https://rd/l1\"]}"},
        {"selectFiles/T1", ""},
        {"unrestrict/link", "{\"download\":\"https://direct/a\"}"},
    };
    std::vector<RdHttpRequest> seen;
    RealDebridClient c("tok", scripted(&s, &seen));
    std::string id, err;
    assert(c.addMagnet("magnet:?xt=urn:btih:h", id, err) && id == "T1");
    RdInfo info;
    assert(c.info("T1", info, err) && info.status == "downloaded");
    assert(c.selectFiles("T1", "1", err));
    std::string url;
    assert(c.unrestrict("https://rd/l1", url, err) &&
           url == "https://direct/a");
    bool sawMagnetBody = false, sawFilesBody = false;
    for (const auto& r : seen) {
        assert(r.token == "tok");
        if (r.url.find("addMagnet") != std::string::npos)
            sawMagnetBody = r.body.find("magnet=") != std::string::npos;
        if (r.url.find("selectFiles") != std::string::npos)
            sawFilesBody = r.body.find("files=1") != std::string::npos;
    }
    assert(sawMagnetBody && sawFilesBody);
    std::puts("rd add/select/poll/unrestrict ok");
}

int main() {
    testParsers();
    testAddSelectPollUnrestrict();
    std::printf("test_real_debrid_client ok\n");
    return 0;
}
