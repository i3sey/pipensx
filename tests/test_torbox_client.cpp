#include "app/torbox_client.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using pipensx::TorboxClient;
using pipensx::TorboxTorrentInfo;

namespace {

const char* kCreateOk = R"({"success":true,"error":null,
  "detail":"Torrent queued.","data":{"torrent_id":297464,"name":"x",
  "hash":"aa11bb22cc33dd44ee55ff667788990011223344"}})";

const char* kCreateAuthFail = R"({"success":false,"error":"BAD_TOKEN",
  "detail":"Invalid token."})";

const char* kInfoFetching = R"({"success":true,"data":{
  "id":297464,"hash":"aa11bb22cc33dd44ee55ff667788990011223344",
  "name":"Example Game","size":1000000,"progress":0.42,
  "download_state":"downloading","download_finished":false,
  "download_present":false,
  "files":[]}})";

const char* kInfoReady = R"({"success":true,"data":{
  "id":297464,"hash":"aa11bb22cc33dd44ee55ff667788990011223344",
  "name":"Example Game","size":1000000,"progress":1.0,
  "download_state":"completed","download_finished":true,
  "download_present":true,
  "files":[{"id":0,"name":"Example Game/game.nsp","size":900000},
           {"id":1,"name":"Example Game/readme.txt","size":100000}]}})";

const char* kInfoArray = R"({"success":true,"data":[{
  "id":11,"name":"other","size":1,"progress":1.0,
  "download_finished":true,"download_present":true,"files":[]},{
  "id":297464,"hash":"aa11bb22cc33dd44ee55ff667788990011223344",
  "name":"Example Game","size":1000000,"progress":1.0,
  "download_state":"completed","download_finished":true,
  "download_present":true,"files":[]}]})";

const char* kLinkOk =
    R"({"success":true,"data":"https://store.torbox.app/dl/abc?sig=1"})";

void testParseCreate() {
    uint64_t id = 0;
    std::string error;
    assert(TorboxClient::parseCreate(kCreateOk, id, error));
    assert(id == 297464);
    assert(!TorboxClient::parseCreate(kCreateAuthFail, id, error));
    assert(error.rfind("TorBox key rejected", 0) == 0);
    assert(!TorboxClient::parseCreate("{not json", id, error));
    assert(!error.empty());
}

void testParseInfo() {
    TorboxTorrentInfo info;
    std::string error;
    assert(TorboxClient::parseInfo(kInfoFetching, 297464, info, error));
    assert(!info.ready);
    assert(info.progress > 0.41 && info.progress < 0.43);
    assert(info.state == "downloading");
    assert(info.files.empty());

    assert(TorboxClient::parseInfo(kInfoReady, 297464, info, error));
    assert(info.ready);
    assert(info.files.size() == 2);
    assert(info.files[0].name == "Example Game/game.nsp");
    assert(info.files[0].size == 900000);
    assert(info.files[1].id == 1);

    assert(TorboxClient::parseInfo(kInfoArray, 297464, info, error));
    assert(info.id == 297464);
    assert(!TorboxClient::parseInfo(kInfoArray, 999, info, error));
    assert(error == "TorBox torrent not found.");
}

void testParseDownloadLink() {
    std::string url, error;
    assert(TorboxClient::parseDownloadLink(kLinkOk, url, error));
    assert(url == "https://store.torbox.app/dl/abc?sig=1");
    assert(!TorboxClient::parseDownloadLink(R"({"success":false,
        "detail":"Link limit reached."})", url, error));
    assert(error == "Link limit reached.");
}

void testRequestDownloadLinkSendsToken() {
    // Regression: TorBox's /torrents/requestdl authenticates via a `token`
    // query parameter, NOT the Authorization header. Omitting it yields an
    // HTTP 422 validation error ({"detail":[{"loc":["query","token"],...}]}),
    // which the client surfaces as the generic "TorBox request failed
    // (HTTP 422)". The request URL must carry token/torrent_id/file_id.
    pipensx::TorboxHttpRequest seen;
    pipensx::TorboxClient client("test-key",
        [&seen](const pipensx::TorboxHttpRequest& request,
                pipensx::TorboxHttpResponse& response, std::string&) {
            seen = request;
            response.status = 200;
            response.body = kLinkOk;
            return true;
        });
    std::string url, error;
    assert(client.requestDownloadLink(297464, 3, url, error));
    assert(url == "https://store.torbox.app/dl/abc?sig=1");
    assert(seen.method == "GET");
    assert(seen.url.find("/torrents/requestdl") != std::string::npos);
    assert(seen.url.find("token=test-key") != std::string::npos);
    assert(seen.url.find("torrent_id=297464") != std::string::npos);
    assert(seen.url.find("file_id=3") != std::string::npos);
}

void testTransportInjection() {
    pipensx::TorboxHttpRequest seen;
    pipensx::TorboxClient client("test-key",
        [&seen](const pipensx::TorboxHttpRequest& request,
                pipensx::TorboxHttpResponse& response, std::string&) {
            seen = request;
            response.status = 200;
            response.body = kCreateOk;
            return true;
        });
    uint64_t id = 0;
    std::string error;
    assert(client.createFromMagnet("magnet:?xt=urn:btih:aa11", id, error));
    assert(id == 297464);
    assert(seen.method == "POST");
    assert(seen.url.find("/torrents/createtorrent") != std::string::npos);
    assert(seen.magnet == "magnet:?xt=urn:btih:aa11");
    assert(seen.apiKey == "test-key");

    // Test HTTP 401 auth failure
    pipensx::TorboxClient authFailClient("bad-key",
        [](const pipensx::TorboxHttpRequest&,
           pipensx::TorboxHttpResponse& response, std::string&) {
            response.status = 401;
            response.body = "";
            return true;
        });
    assert(!authFailClient.createFromMagnet("magnet:?xt=urn:btih:aa11", id,
                                            error));
    assert(error == "TorBox key rejected - relink in Settings.");

    // Test HTTP 500 with generic failure message gets status appended
    pipensx::TorboxClient serverErrClient("test-key",
        [](const pipensx::TorboxHttpRequest&,
           pipensx::TorboxHttpResponse& response, std::string&) {
            response.status = 500;
            response.body = R"({"success":false})";
            return true;
        });
    assert(!serverErrClient.createFromMagnet("magnet:?xt=urn:btih:aa11", id,
                                             error));
    assert(error == "TorBox request failed (HTTP 500).");
}

// Verify that parsers handle wrong-typed / null JSON fields without throwing.
// A null or mistyped field must yield a safe default (or false), never crash.
void testMalformedFieldsNoThrow() {
    // parseInfo: key fields present but typed as null / wrong type.
    {
        const char* malformed =
            "{\"success\":true,\"data\":{"
            "\"id\":42,\"hash\":null,\"size\":null,\"progress\":null,"
            "\"download_finished\":null,\"download_present\":null,"
            "\"files\":[{\"id\":0,\"name\":null,\"size\":\"big\"}]}}";
        TorboxTorrentInfo info;
        std::string error;
        bool threw = false;
        bool ok = false;
        try {
            ok = TorboxClient::parseInfo(malformed, 42, info, error);
        } catch (...) {
            threw = true;
        }
        assert(!threw);
        // Result is either true-with-safe-defaults or false; never a crash.
        if (ok) {
            assert(info.id == 42);
            assert(info.hash.empty());
            assert(info.size == 0);
            assert(info.progress == 0.0);
            assert(!info.ready);
        }
    }

    // checkSuccess / parseSuccess: "success" field present as a string.
    {
        std::string error;
        bool threw = false;
        try {
            TorboxClient::parseSuccess(
                "{\"success\":\"true\"}", error);
        } catch (...) {
            threw = true;
        }
        assert(!threw);
    }

    // parseCreate: "success" typed as non-bool should not throw.
    {
        uint64_t id = 0;
        std::string error;
        bool threw = false;
        try {
            TorboxClient::parseCreate(
                "{\"success\":\"true\",\"data\":{\"torrent_id\":\"bad\"}}",
                id, error);
        } catch (...) {
            threw = true;
        }
        assert(!threw);
    }

    // parseDownloadLink: "data" typed as integer instead of string.
    {
        std::string url;
        std::string error;
        bool threw = false;
        bool ok = false;
        try {
            ok = TorboxClient::parseDownloadLink(
                "{\"success\":true,\"data\":12345}", url, error);
        } catch (...) {
            threw = true;
        }
        assert(!threw);
        assert(!ok); // wrong type → no link returned
        assert(!error.empty());
    }
}

} // namespace

int main() {
    testParseCreate();
    testParseInfo();
    testParseDownloadLink();
    testRequestDownloadLinkSendsToken();
    testTransportInjection();
    testMalformedFieldsNoThrow();
    std::printf("test_torbox_client ok\n");
    return 0;
}
