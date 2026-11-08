#include "app/download_manager.hpp"
#include "app/http_server.hpp"
#include "app/stream_install_flag.hpp"
extern "C" {
#include "core/sha1.h"
}
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace pipensx;

static std::string bstr(const std::string& s) {
    return std::to_string(s.size()) + ":" + s;
}

static DownloadTask waitFor(DownloadManager& manager, const std::string& id,
                            const std::function<bool(const DownloadTask&)>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        auto task = manager.snapshot(id);
        if (task && ready(*task)) return *task;
        assert(!task || task->status != DownloadStatus::Error);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "seeding lifecycle timed out");
    return {};
}

int main() {
    char root[] = "/tmp/pipensx-live-seeding-XXXXXX";
    assert(mkdtemp(root));
    const std::string payload(65536, 'x');
    std::string nsp = "PFS0";
    auto le = [&](uint64_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) nsp.push_back(static_cast<char>(value >> (8 * i)));
    };
    const std::string ncaName = "00112233445566778899aabbccddeeff.nca";
    le(1, 4); le(ncaName.size() + 1, 4); le(0, 4);
    le(0, 8); le(payload.size(), 8); le(0, 4); le(0, 4);
    nsp += ncaName; nsp.push_back(0); nsp += payload;
    HttpServer server;
    std::string error;
    assert(server.start(0, [&](const HttpRequest& req) {
        const auto& body = req.path == "/game.nsp" ? nsp : payload;
        unsigned long long begin = 0, end = 0;
        if (sscanf(req.header("range").c_str(), "bytes=%llu-%llu", &begin, &end) != 2 ||
            end >= body.size() || begin > end) return HttpResponse::empty(416);
        auto response = HttpResponse::text(206, body.substr(begin, end - begin + 1),
                                            "application/octet-stream");
        response.extraHeaders.emplace_back("Content-Range", "bytes " +
            std::to_string(begin) + "-" + std::to_string(end) + "/" + std::to_string(body.size()));
        return response;
    }, error));
    uint8_t hash[20];
    sha1(reinterpret_cast<const uint8_t*>(payload.data()), 16384, hash);
    std::string hashes;
    for (int i = 0; i < 4; ++i) hashes.append(reinterpret_cast<char*>(hash), 20);
    std::string torrent = "d4:infod6:lengthi65536e4:name8:seed.bin12:piece lengthi16384e6:pieces" +
        bstr(hashes) + "7:privatei1ee8:url-list" +
        bstr("http://127.0.0.1:" + std::to_string(server.boundPort()) + "/seed.bin") + "e";
    std::string path = std::string(root) + "/source.torrent";
    { std::ofstream out(path, std::ios::binary); out << torrent; }
    hashes.clear();
    for (size_t off = 0; off < nsp.size(); off += 16384) {
        sha1(reinterpret_cast<const uint8_t*>(nsp.data() + off),
              std::min<size_t>(16384, nsp.size() - off), hash);
        hashes.append(reinterpret_cast<char*>(hash), 20);
    }
    const std::string installTorrent = "d4:infod6:lengthi" + std::to_string(nsp.size()) +
        "e4:name8:game.nsp12:piece lengthi16384e6:pieces" + bstr(hashes) +
        "7:privatei1ee8:url-list" + bstr("http://127.0.0.1:" +
        std::to_string(server.boundPort()) + "/game.nsp") + "e";
    const std::string installPath = std::string(root) + "/install.torrent";
    { std::ofstream out(installPath, std::ios::binary); out << installTorrent; }
    std::string id;
    {
        DownloadManager manager(std::string(root) + "/app");
        manager.setTorrentingEnabled(true);
        assert(manager.importTorrent(path, TransferMode::DownloadOnly, id, error));
        auto seeded = waitFor(manager, id, [](const auto& task) {
            return task.status == DownloadStatus::Completed && task.seeding;
        });
        assert(!manager.hasActiveTransfer());
        assert(seeded.completedBytes == payload.size());
        std::string installedId;
        assert(manager.importTorrent(installPath, TransferMode::StreamInstall, installedId, error));
        const auto installed = waitFor(manager, installedId, [](const auto& task) {
            return task.status == DownloadStatus::Installed && task.seeding && !streamInstallActive();
        });
        assert(installed.seedCacheBytes >= nsp.size());
        assert(!manager.snapshot(id)->seeding && !manager.hasActiveTransfer());
        {
            auto lease = manager.beginExternalDeploy(installedId, error);
            assert(lease);
            assert(!manager.snapshot(installedId)->seeding);
        }
        assert(manager.verify(id, error));
        waitFor(manager, id, [](const auto& task) {
            return task.status == DownloadStatus::Completed && task.seeding;
        });
        ClearCompletedResult result;
        assert(manager.clearCompleted(true, error, &result));
        assert(result.cleared == 2);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (manager.snapshot(id) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(!manager.snapshot(id));
    }
    server.stop();
    std::filesystem::remove_all(root);
    puts("live seeding lifecycle tests passed");
}
