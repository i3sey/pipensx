#include "app/alldebrid_client.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using pipensx::AdClient;
using pipensx::AdFile;
using pipensx::AdHttpRequest;
using pipensx::AdHttpResponse;
using pipensx::AdTorrentInfo;
using pipensx::AdUnrestrict;

namespace {

const char* kUserOk = R"({"status":"success","data":{"user":{
  "username":"example","email":"user@example.com","isPremium":true,
  "isSubscribed":true,"isTrial":false,"premiumUntil":1893456000,
  "lang":"en","preferedDomain":"com","fidelityPoints":10,
  "limitedHostersQuotas":{},"notifications":[]}}})";

const char* kUserFree = R"({"status":"success","data":{"user":{
  "username":"free","email":"free@example.com","isPremium":false,
  "isSubscribed":false,"isTrial":false,"premiumUntil":0,
  "lang":"en","preferedDomain":"com","fidelityPoints":0,
  "limitedHostersQuotas":{},"notifications":[]}}})";

const char* kAddMagnetOk =
    R"({"status":"success","data":{"magnets":[{
      "magnet":"magnet:?xt=urn:btih:abcdef123456",
      "hash":"abcdef123456","name":"Example Game","size":1000000000,
      "ready":false,"id":123456}]}})";

const char* kAddTorrentOk =
    R"({"status":"success","data":{"files":[{
      "file":"game.torrent","name":"Example Game","size":1000000000,
      "hash":"abcdef123456","ready":false,"id":123456}]}})";

const char* kStatusDownloading =
    R"({"status":"success","data":{"magnets":[{
      "id":123456,"filename":"Example Game","size":1000000000,
      "status":"Downloading","statusCode":1,"downloaded":420000000,
      "uploaded":0,"seeders":7,"downloadSpeed":1000,"uploadSpeed":0,
      "uploadDate":1700000000,"completionDate":0}]}})";

const char* kStatusReady =
    R"({"status":"success","data":{"magnets":[{
      "id":123456,"filename":"Example Game","size":1000000000,
      "status":"Ready","statusCode":4,"downloaded":1000000000,
      "uploadDate":1700000000,"completionDate":1700000100}]}})";

const char* kStatusFailed =
    R"({"status":"success","data":{"magnets":[{
      "id":123456,"filename":"bad.torrent","size":0,
      "status":"Error","statusCode":9,"downloaded":0}]}})";

const char* kFilesFlat =
    R"({"status":"success","data":{"magnets":[{
      "id":"123456","files":[
        {"n":"game.nsp","s":900000000,"l":"https://alldebrid.com/f/aaa"},
        {"n":"readme.txt","s":100000000,"l":"https://alldebrid.com/f/bbb"}
      ]}]}})";

const char* kFilesNested =
    R"({"status":"success","data":{"magnets":[{
      "id":"123456","files":[
        {"n":"Subfolder","e":[
          {"n":"game.nsp","s":900000000,"l":"https://alldebrid.com/f/aaa"},
          {"n":"Deep","e":[
            {"n":"extra.nfo","s":12,"l":"https://alldebrid.com/f/ccc"}
          ]}
        ]},
        {"n":"readme.txt","s":100000000,"l":"https://alldebrid.com/f/bbb"}
      ]}]}})";

const char* kUnlockOk =
    R"({"status":"success","data":{
      "link":"http://ombfyx.debrid.it/dl/abcdef/game.nsp",
      "host":"alldebrid","filename":"game.nsp","filesize":900000000,
      "id":"abcdef","hostDomain":"alldebrid.com"}})";

const char* kAuthFail =
    R"({"status":"error","error":{"code":"AUTH_BAD_APIKEY",
      "message":"The auth apikey is invalid"}})";

const char* kMagnetInvalid =
    R"({"status":"success","data":{"magnets":[{
      "magnet":"bad","error":{"code":"MAGNET_INVALID_URI",
        "message":"Magnet is not valid"}}]}})";

void testParseUserResponse() {
    std::string error;
    assert(AdClient::parseUserResponse(kUserOk, error));
    assert(error.empty());
    assert(!AdClient::parseUserResponse(kUserFree, error));
    assert(error.find("premium") != std::string::npos);
    assert(!AdClient::parseUserResponse("{not json", error));
    assert(!error.empty());
    assert(!AdClient::parseUserResponse(
        "{\"status\":\"success\",\"data\":{}}", error));
    assert(!error.empty());
}

void testParseAddMagnetResponse() {
    std::string id, error;
    assert(AdClient::parseAddMagnetResponse(kAddMagnetOk, id, error));
    assert(id == "123456");
    assert(!AdClient::parseAddMagnetResponse(kAuthFail, id, error));
    assert(error == "AllDebrid key rejected - relink in Settings.");
    assert(!AdClient::parseAddMagnetResponse(kMagnetInvalid, id, error));
    assert(error == "Magnet is not valid");
    assert(!AdClient::parseAddMagnetResponse("{not json", id, error));
    assert(!error.empty());
}

void testParseAddTorrentResponse() {
    std::string id, error;
    assert(AdClient::parseAddTorrentResponse(kAddTorrentOk, id, error));
    assert(id == "123456");
}

void testParseStatus() {
    AdTorrentInfo info;
    std::string error;

    assert(AdClient::parseStatus(kStatusDownloading, "123456", info, error));
    assert(info.id == "123456");
    assert(info.status == "Downloading");
    assert(info.statusCode == 1);
    assert(info.progress > 0.41 && info.progress < 0.43);
    assert(info.files.empty());

    assert(AdClient::parseStatus(kStatusReady, "123456", info, error));
    assert(info.statusCode == 4);
    assert(info.progress >= 0.99);

    assert(AdClient::parseStatus(kStatusFailed, "123456", info, error));
    assert(info.statusCode == 9);
    assert(info.status == "Error");

    assert(!AdClient::parseStatus("{not json", "123456", info, error));
    assert(!error.empty());
}

void testParseFiles() {
    std::vector<AdFile> files;
    std::string error;

    assert(AdClient::parseFiles(kFilesFlat, "123456", files, error));
    assert(files.size() == 2);
    assert(files[0].path == "game.nsp");
    assert(files[0].bytes == 900000000);
    assert(files[0].link == "https://alldebrid.com/f/aaa");
    assert(files[1].path == "readme.txt");

    assert(AdClient::parseFiles(kFilesNested, "123456", files, error));
    assert(files.size() == 3);
    assert(files[0].path == "Subfolder/game.nsp");
    assert(files[0].bytes == 900000000);
    assert(files[1].path == "Subfolder/Deep/extra.nfo");
    assert(files[2].path == "readme.txt");
}

void testParseUnrestrict() {
    AdUnrestrict got;
    std::string error;
    assert(AdClient::parseUnrestrict(kUnlockOk, got, error));
    assert(got.url == "https://ombfyx.debrid.it/dl/abcdef/game.nsp");
    assert(got.filename == "game.nsp");
    assert(got.filesize == 900000000);
    assert(!AdClient::parseUnrestrict("{not json", got, error));
    assert(!error.empty());
    assert(!AdClient::parseUnrestrict(
        "{\"status\":\"success\",\"data\":{}}", got, error));
    assert(!error.empty());
}

void testCheckAuthErrorPaths() {
    std::string id, error;

    AdClient clientAuth(
        "key", [](const AdHttpRequest&, AdHttpResponse& resp, std::string&) {
            resp.status = 200;
            resp.body = kAuthFail;
            return true;
        });
    assert(!clientAuth.createFromMagnet("magnet:?xt=urn:btih:abc", id, error));
    assert(error == "AllDebrid key rejected - relink in Settings.");

    AdClient client401(
        "key", [](const AdHttpRequest&, AdHttpResponse& resp, std::string&) {
            resp.status = 401;
            resp.body = kAuthFail;
            return true;
        });
    assert(!client401.createFromMagnet("magnet:?xt=urn:btih:abc", id, error));
    assert(error == "AllDebrid key rejected - relink in Settings.");

    AdClient client400(
        "key", [](const AdHttpRequest&, AdHttpResponse& resp, std::string&) {
            resp.status = 200;
            resp.body = kMagnetInvalid;
            return true;
        });
    assert(!client400.createFromMagnet("magnet:?xt=urn:btih:abc", id, error));
    assert(error == "Magnet is not valid");

    AdClient client503(
        "key", [](const AdHttpRequest&, AdHttpResponse& resp, std::string&) {
            resp.status = 503;
            return true;
        });
    assert(!client503.createFromMagnet("magnet:?xt=urn:btih:abc", id, error));
    assert(error.find("HTTP 503") != std::string::npos);
}

} // namespace

int main() {
    testParseUserResponse();
    testParseAddMagnetResponse();
    testParseAddTorrentResponse();
    testParseStatus();
    testParseFiles();
    testParseUnrestrict();
    testCheckAuthErrorPaths();
    std::printf("test_alldebrid_client: all assertions passed\n");
    return 0;
}
