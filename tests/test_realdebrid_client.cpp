#include "app/realdebrid_client.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using pipensx::RdClient;
using pipensx::RdTorrentInfo;

namespace {

const char* kUserOk = R"({"id":12345,"username":"example",
  "email":"user@example.com","points":1000,"locale":"en",
  "avatar":"https://cdn.real-debrid.com/u/12345.jpg","type":"premium",
  "premium":1234567890,"expiration":"2025-01-01T00:00:00.000Z"})";

const char* kAddMagnetOk =
    R"({"id":"abc123def456","uri":"magnet:?xt=urn:btih:abcdef123456"})";

const char* kInfoMagnetConversion = R"({"id":"abc123def456",
  "filename":"Example Game.nsp","hash":"abcdef1234567890","bytes":0,
  "progress":0,"status":"magnet_conversion","added":"2024-01-01",
  "files":[],"links":[],"ended":"","speed":0,"seeders":0})";

const char* kInfoWaitingSelect = R"({"id":"abc123def456",
  "filename":"Example Game","hash":"abcdef1234567890","bytes":1000000000,
  "progress":0,"status":"waiting_files_selection","added":"2024-01-01",
  "files":[{"id":1,"path":"game.nsp","bytes":900000000,"selected":0},
           {"id":2,"path":"readme.txt","bytes":100000000,"selected":0}],
  "links":[],"ended":"","speed":0,"seeders":0})";

const char* kInfoDownloaded = R"({"id":"abc123def456",
  "filename":"Example Game","hash":"abcdef1234567890","bytes":1000000000,
  "progress":100,"status":"downloaded","added":"2024-01-01",
  "files":[{"id":1,"path":"game.nsp","bytes":900000000,"selected":1},
           {"id":2,"path":"readme.txt","bytes":100000000,"selected":1}],
  "links":["https://rd.to/dl/xyz/file1","https://rd.to/dl/xyz/file2"],
  "ended":"2024-01-01","speed":0,"seeders":0})";

const char* kInfoFailed = R"({"id":"abc123def456",
  "filename":"bad.torrent","hash":"bad","bytes":0,
  "progress":0,"status":"error","added":"2024-01-01",
  "files":[],"links":[],"ended":"","speed":0,"seeders":0})";

const char* kInfoDead = R"({"id":"abc123def456",
  "filename":"dead.torrent","hash":"dead","bytes":0,
  "progress":0,"status":"dead","added":"2024-01-01",
  "files":[],"links":[],"ended":"","speed":0,"seeders":0})";

const char* kUnrestrictOk = R"({"id":"unrestrict123",
  "filename":"game.nsp","mimeType":"application/octet-stream",
  "filesize":900000000,
  "link":"https://rd.dl1.real-debrid.com/dl/abcdef/game.nsp",
  "host":"real-debrid.com","host_icon":"https://...",
  "chunks":16,
  "download":"https://rd.dl1.real-debrid.com/dl/abcdef/game.nsp",
  "streamable":0,"generated":"2024-01-01","type":"torrent"})";

const char* kAuthFail = R"({"error":"Bad token","error_code":8})";

void testParseUserResponse() {
    std::string error;
    assert(RdClient::parseUserResponse(kUserOk, error));
    assert(error.empty());
    assert(!RdClient::parseUserResponse("{not json", error));
    assert(!error.empty());
    assert(!RdClient::parseUserResponse("{\"no_id\":1}", error));
    assert(!error.empty());
}

void testParseAddMagnetResponse() {
    std::string id, error;
    assert(RdClient::parseAddMagnetResponse(kAddMagnetOk, id, error));
    assert(id == "abc123def456");
    assert(!RdClient::parseAddMagnetResponse(kAuthFail, id, error));
    assert(!error.empty());
    assert(!RdClient::parseAddMagnetResponse("{not json", id, error));
    assert(!error.empty());
}

void testParseAddTorrentResponse() {
    std::string id, error;
    assert(RdClient::parseAddTorrentResponse(kAddMagnetOk, id, error));
    assert(id == "abc123def456");
}

void testParseInfo() {
    RdTorrentInfo info;
    std::string error;

    assert(RdClient::parseInfo(kInfoMagnetConversion, info, error));
    assert(info.id == "abc123def456");
    assert(info.status == "magnet_conversion");
    assert(info.files.empty());

    assert(RdClient::parseInfo(kInfoWaitingSelect, info, error));
    assert(info.id == "abc123def456");
    assert(info.status == "waiting_files_selection");
    assert(info.files.size() == 2);
    assert(info.files[0].id == "1");
    assert(info.files[0].path == "game.nsp");
    assert(info.files[0].bytes == 900000000);
    assert(info.files[1].id == "2");
    assert(info.files[1].path == "readme.txt");
    assert(info.files[1].bytes == 100000000);
    assert(info.links.empty());

    assert(RdClient::parseInfo(kInfoDownloaded, info, error));
    assert(info.id == "abc123def456");
    assert(info.status == "downloaded");
    assert(info.files.size() == 2);
    assert(info.links.size() == 2);
    assert(info.links[0] == "https://rd.to/dl/xyz/file1");
    assert(info.links[1] == "https://rd.to/dl/xyz/file2");
    assert(info.progress >= 0.99);

    assert(RdClient::parseInfo(kInfoFailed, info, error));
    assert(info.status == "error");

    assert(RdClient::parseInfo(kInfoDead, info, error));
    assert(info.status == "dead");

    assert(!RdClient::parseInfo("{not json", info, error));
    assert(!error.empty());
}

void testParseUnrestrict() {
    std::string url, error;
    assert(RdClient::parseUnrestrict(kUnrestrictOk, url, error));
    assert(url == "https://rd.dl1.real-debrid.com/dl/abcdef/game.nsp");
    assert(!RdClient::parseUnrestrict("{not json", url, error));
    assert(!error.empty());
    assert(!RdClient::parseUnrestrict("{}", url, error));
    assert(!error.empty());
}

} // namespace

int main() {
    testParseUserResponse();
    testParseAddMagnetResponse();
    testParseAddTorrentResponse();
    testParseInfo();
    testParseUnrestrict();
    std::printf("test_realdebrid_client: all assertions passed\n");
    return 0;
}
