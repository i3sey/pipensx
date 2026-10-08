#include "app/debrid_transfer.hpp"
#include "zip_fixture.hpp"
#include <filesystem>
#include "app/realdebrid_provider.hpp"
#include "app/torbox_provider.hpp"
#include "app/torrserver_provider.hpp"
#include "install/install_journal.hpp"
extern "C" {
#include "core/util.h"
}
#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>
#include <zstd.h>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace pipensx;

namespace {

TorboxTransport scriptedTransport(
    std::vector<std::pair<std::string, std::string>>* script) {
    return [script](const TorboxHttpRequest& request,
                    TorboxHttpResponse& response, std::string&) {
        for (auto it = script->begin(); it != script->end(); ++it) {
            if (request.url.find(it->first) != std::string::npos) {
                response.status = 200;
                response.body = it->second;
                script->erase(it);
                return true;
            }
        }
        response.status = 200;
        response.body = "{\"success\":false,\"detail\":\"unexpected\"}";
        return true;
    };
}

RangeFetcher memoryFetcher(const std::string& content) {
    return [content](const std::string&, uint64_t offset, uint64_t endExclusive,
                     const std::function<bool(const uint8_t*, size_t)>& sink,
                     const std::function<bool()>&, std::string& error) {
        if (offset > content.size()) {
            error = "range past end";
            return false;
        }
        uint64_t end = endExclusive == 0
            ? content.size()
            : std::min<uint64_t>(endExclusive, content.size());
        if (offset > end) {
            error = "range past end";
            return false;
        }
        std::string slice = content.substr(static_cast<size_t>(offset),
                                           static_cast<size_t>(end - offset));
        size_t half = slice.size() / 2;
        if (half && !sink(reinterpret_cast<const uint8_t*>(slice.data()),
                          half))
            return false;
        if (!sink(reinterpret_cast<const uint8_t*>(slice.data()) + half,
                  slice.size() - half))
            return false;
        return true;
    };
}

RangeFetcher chunkedFetcher(const std::string& content, size_t piece) {
    return [content, piece](const std::string&, uint64_t offset,
                            uint64_t endExclusive,
                            const std::function<bool(const uint8_t*, size_t)>&
                                sink,
                            const std::function<bool()>&, std::string& error) {
        if (offset > content.size()) {
            error = "range past end";
            return false;
        }
        uint64_t end = endExclusive == 0
            ? content.size()
            : std::min<uint64_t>(endExclusive, content.size());
        size_t pos = static_cast<size_t>(offset);
        size_t stop = static_cast<size_t>(end);
        while (pos < stop) {
            size_t n = std::min(piece, stop - pos);
            if (!sink(reinterpret_cast<const uint8_t*>(content.data()) + pos,
                      n))
                return false;
            pos += n;
        }
        return true;
    };
}

std::string infoReadyJson(const std::string& fileName, size_t size) {
    return "{\"success\":true,\"data\":{\"id\":42,\"name\":\"Example\","
           "\"size\":" + std::to_string(size) + ",\"progress\":1.0,"
           "\"download_state\":\"completed\",\"download_finished\":true,"
           "\"download_present\":true,\"files\":[{\"id\":7,\"name\":\"" +
           fileName + "\",\"size\":" + std::to_string(size) + "}]}}";
}

std::string infoReadyJsonTwo(const std::string& nameA, size_t sizeA,
                             const std::string& nameB, size_t sizeB) {
    return "{\"success\":true,\"data\":{\"id\":42,\"name\":\"Example\","
           "\"size\":" + std::to_string(sizeA + sizeB) + ",\"progress\":1.0,"
           "\"download_state\":\"completed\",\"download_finished\":true,"
           "\"download_present\":true,\"files\":["
           "{\"id\":7,\"name\":\"" + nameA + "\",\"size\":" +
           std::to_string(sizeA) + "},"
           "{\"id\":8,\"name\":\"" + nameB + "\",\"size\":" +
           std::to_string(sizeB) + "}]}}";
}

void append32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void append64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

std::vector<uint8_t> makePfs0(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> strings;
    std::vector<uint32_t> nameOffsets;
    for (const auto& file : files) {
        nameOffsets.push_back(static_cast<uint32_t>(strings.size()));
        strings.insert(strings.end(), file.first.begin(), file.first.end());
        strings.push_back(0);
    }
    std::vector<uint8_t> out{'P', 'F', 'S', '0'};
    append32(out, static_cast<uint32_t>(files.size()));
    append32(out, static_cast<uint32_t>(strings.size()));
    append32(out, 0);
    uint64_t offset = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        append64(out, offset);
        append64(out, files[i].second.size());
        append32(out, nameOffsets[i]);
        append32(out, 0);
        offset += files[i].second.size();
    }
    out.insert(out.end(), strings.begin(), strings.end());
    for (const auto& file : files)
        out.insert(out.end(), file.second.begin(), file.second.end());
    return out;
}

// Real HTTP exercises libcurl's connection cache and per-request option reset,
// which injected RangeFetchers cannot check. A closed keepalive must reconnect.
void testHttpConnectionReuse(bool closeFirst) {
    const std::string root = "/tmp/pipensx-http-reuse-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::vector<std::string> bodies{
        std::string(65536, 'A'), std::string(32768, 'B')};
    log_init((root + "/metrics.log").c_str());
    telemetry_set_enabled(1);
    const size_t resumeOffset = 123;
    { std::ofstream partial(root + "/first.bin", std::ios::binary);
      partial << bodies[0].substr(0, resumeOffset); }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                       &addressSize) == 0);
    assert(listen(listener, 4) == 0);
    unsigned connections = 0;
    std::thread server([&] {
        unsigned served = 0;
        while (served < bodies.size()) {
            pollfd ready{listener, POLLIN, 0};
            assert(poll(&ready, 1, 10000) == 1);
            int client = accept(listener, nullptr, nullptr);
            assert(client >= 0);
            ++connections;
            timeval timeout{10, 0};
            assert(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                              sizeof(timeout)) == 0);
            assert(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                              sizeof(timeout)) == 0);
            while (served < bodies.size()) {
                std::string request;
                char bytes[2048];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    ssize_t count = recv(client, bytes, sizeof(bytes), 0);
                    assert(count >= 0);
                    if (count == 0) break;
                    request.append(bytes, static_cast<size_t>(count));
                    assert(request.size() < 8192);
                }
                if (request.empty()) break;
                assert(request.find("GET /play/abc/" +
                                    std::to_string(served + 1) + " HTTP/1.1") == 0);
                size_t offset = served == 0 ? resumeOffset : 0;
                if (offset)
                    assert(request.find("Range: bytes=123-") != std::string::npos);
                else
                    assert(request.find("Range:") == std::string::npos);
                std::string response = offset ? "HTTP/1.1 206 Partial Content\r\n"
                                              : "HTTP/1.1 200 OK\r\n";
                if (offset)
                    response += "Content-Range: bytes " + std::to_string(offset) +
                                "-" + std::to_string(bodies[served].size() - 1) +
                                "/" + std::to_string(bodies[served].size()) + "\r\n";
                const bool disconnect = closeFirst && served == 0;
                response += "Content-Length: " +
                            std::to_string(bodies[served].size() - offset) +
                            "\r\nConnection: " + (disconnect ? "close" : "keep-alive") +
                            "\r\n\r\n" + bodies[served].substr(offset);
                size_t sent = 0;
                while (sent < response.size()) {
                    ssize_t count = send(client, response.data() + sent,
                                         response.size() - sent, MSG_NOSIGNAL);
                    assert(count > 0);
                    sent += static_cast<size_t>(count);
                }
                ++served;
                if (disconnect) break;
            }
            close(client);
        }
    });

    using Json = nlohmann::json;
    Json info{{"name", "Example"}, {"hash", "abc"}, {"stat", 3},
              {"torrent_size", bodies[0].size() + bodies[1].size()},
              {"file_stats", Json::array({
                  {{"id", 1}, {"path", "Example/first.bin"}, {"length", bodies[0].size()}},
                  {{"id", 2}, {"path", "Example/second.bin"}, {"length", bodies[1].size()}}})}};
    TorrserverProvider provider("http://127.0.0.1:" +
                                    std::to_string(ntohs(address.sin_port)),
        [&](const TsHttpRequest&, TsHttpResponse& response, std::string&) {
            response.status = 200;
            response.body = info.dump();
            return true;
        });
    DebridTransfer transfer(provider);
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc";
    spec.dataPath = root;
    spec.workingRoot = root;
    std::string id, error;
    assert(transfer.run(spec, [] { return false; }, [](const DebridProgress&) {},
                        id, error) == DebridRunResult::Finished);
    server.join();
    close(listener);
    assert(connections == (closeFirst ? 2u : 1u));
    std::string metrics(65536, '\0');
    metrics.resize(log_read_tail(metrics.data(), metrics.size()));
    assert(metrics.find("stage=debrid_http") != std::string::npos);
    assert(metrics.find("event=request_end offset=123") != std::string::npos);
    assert(metrics.find("ttfb_ms=") != std::string::npos);
    assert(metrics.find("new_connections=1") != std::string::npos);
    if (!closeFirst) assert(metrics.find("new_connections=0") != std::string::npos);
    assert(metrics.find("stage=debrid_sd") != std::string::npos);
    assert(metrics.find("/play/abc/") == std::string::npos);
    telemetry_set_enabled(0);
    log_close();
    for (size_t i = 0; i < bodies.size(); ++i) {
        std::ifstream file(root + (i == 0 ? "/first.bin" : "/second.bin"),
                           std::ios::binary);
        std::string written((std::istreambuf_iterator<char>(file)), {});
        assert(written == bodies[i]);
    }
    std::printf("HTTP reuse resume/reset/reconnect ok connections=%u\n", connections);
}

void testNspWritesBeforeNetworkPrebuffer() {
    const std::string root = "/tmp/pipensx-nsp-no-prebuffer-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const auto nsp = makePfs0({{"00112233445566778899aabbccddeeff.nca",
                               std::vector<uint8_t>(3 * 1024 * 1024, 0x5a)}});
    TorrserverProvider provider("http://box:8090",
        [&](const TsHttpRequest&, TsHttpResponse& response, std::string&) {
            response.status = 200;
            response.body = "{\"name\":\"Example\",\"hash\":\"abc\",\"stat\":3,"
                "\"torrent_size\":" + std::to_string(nsp.size()) +
                ",\"file_stats\":[{\"id\":1,\"path\":\"Example/game.nsp\","
                "\"length\":" + std::to_string(nsp.size()) + "}]}";
            return true;
        });
    std::mutex mutex;
    std::condition_variable wrote;
    bool installedFirstBatch = false;
    bool fetchingRest = false;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset, uint64_t end,
                              const auto& sink, const auto&, std::string&) {
        assert(offset == 0 && end == 0);
        const size_t batch = 1024 * 1024;
        if (!sink(nsp.data(), batch)) return false;
        std::unique_lock<std::mutex> lock(mutex);
        // No more bytes arrive until the installer consumes the first batch.
        // The former WAN prebuffer would deadlock this until its 15s timeout.
        assert(wrote.wait_for(lock, std::chrono::seconds(10),
                              [&] { return installedFirstBatch; }));
        fetchingRest = true;
        lock.unlock();
        return sink(nsp.data() + batch, nsp.size() - batch);
    };
    DebridTransfer transfer(provider, fetcher);
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    std::string id, error;
    DebridProgress last;
    assert(transfer.run(spec, [] { return false; }, [&](const DebridProgress& p) {
        last = p;
        if (p.installedBytes > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!fetchingRest) {
                installedFirstBatch = true;
                wrote.notify_one();
            }
        }
    }, id, error) == DebridRunResult::Finished);
    assert(installedFirstBatch && last.packagesInstalled == 1);
    assert(last.status == DownloadStatus::Installed);
    std::puts("NSP installs before network prebuffer ok");
}

void testDownloadOnlyFullRun() {
    const std::string root = "/tmp/pipensx-torbox-transfer-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::string content(100000, 'x');
    std::vector<std::pair<std::string, std::string>> script = {
        {"createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":42}}"},
        {"mylist", infoReadyJson("Example/file.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(content));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.magnet = "magnet:?xt=urn:btih:" + spec.taskId;
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;
    std::vector<DebridTaskSpec::ResolvedFile> resolvedFiles;
    spec.filesResolved = [&resolvedFiles](
                             const std::vector<DebridTaskSpec::ResolvedFile>&
                                 files) { resolvedFiles = files; };

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(debridId == "42");
    assert(last.status == DownloadStatus::Completed);
    assert(last.completedBytes == content.size());
    assert(resolvedFiles.size() == 1);
    assert(resolvedFiles[0].path == "file.bin");
    assert(resolvedFiles[0].localPath == "file.bin");
    assert(resolvedFiles[0].action ==
           static_cast<uint8_t>(FileAction::Download));

    std::ifstream check(data + "/file.bin", std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(check)),
                        std::istreambuf_iterator<char>());
    assert(written == content);
}

void testDownloadReceivesWhileWriterIsBusy() {
    const std::string root = "/tmp/pipensx-buffered-disk-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string content(384 * 1024, 'q');
    std::vector<std::pair<std::string, std::string>> script = {
        {"createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":42}}"},
        {"mylist", infoReadyJson("Example/file.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    std::atomic<bool> writerEntered{false}, sourceFinished{false};
    auto waitFor = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (!flag.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        assert(flag.load());
    };
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset, uint64_t,
            const std::function<bool(const uint8_t*, size_t)>& sink,
            const std::function<bool()>&, std::string&) {
        assert(offset == 0);
        const auto* bytes = reinterpret_cast<const uint8_t*>(content.data());
        if (!sink(bytes, 64 * 1024)) return false;
        waitFor(writerEntered);
        if (!sink(bytes + 64 * 1024, content.size() - 64 * 1024)) return false;
        sourceFinished = true;
        return true;
    };
    DebridTransfer transfer(provider, fetcher);
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.magnet = "magnet:?xt=urn:btih:" + spec.taskId;
    spec.dataPath = spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;
    std::string id, error;
    const auto result = transfer.run(spec, [] { return false; },
        [&](const DebridProgress& p) {
            if (p.status == DownloadStatus::Downloading && p.completedBytes > 0) {
                writerEntered = true;
                waitFor(sourceFinished); // Slow consumer cannot stall HTTP.
            }
        }, id, error);
    assert(result == DebridRunResult::Finished && error.empty());
    std::ifstream file(root + "/file.bin", std::ios::binary);
    assert(std::string((std::istreambuf_iterator<char>(file)), {}) == content);
}

void testResumeUsesOnDiskOffset() {
    const std::string root = "/tmp/pipensx-torbox-resume-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::string content(50000, 'y');
    {
        std::ofstream partial(data + "/file.bin", std::ios::binary);
        partial << content.substr(0, 20000);
    }
    uint64_t seenOffset = UINT64_MAX;
    RangeFetcher fetcher = [&content, &seenOffset](
        const std::string&, uint64_t offset, uint64_t endExclusive,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>&, std::string&) {
        seenOffset = offset;
        uint64_t end = endExclusive == 0
            ? content.size()
            : std::min<uint64_t>(endExclusive, content.size());
        std::string slice = content.substr(static_cast<size_t>(offset),
                                           static_cast<size_t>(end - offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/file.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(seenOffset == 20000);
    struct stat st {};
    assert(stat((data + "/file.bin").c_str(), &st) == 0);
    assert(static_cast<uint64_t>(st.st_size) == content.size());
}

void testStopRequestedReturnsStopped() {
    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", "{\"success\":true,\"data\":{\"id\":42,\"name\":\"E\","
                   "\"size\":10,\"progress\":0.5,"
                   "\"download_finished\":false,\"download_present\":false,"
                   "\"files\":[]}}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(""));
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = "/tmp";
    spec.workingRoot = "/tmp";
    bool stop = false;
    double maxFetch = -1.0;
    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [&stop] { return stop; },
        [&stop, &maxFetch](const DebridProgress& p) {
            if (p.status == DownloadStatus::Fetching) {
                if (p.fetchProgress > maxFetch)
                    maxFetch = p.fetchProgress;
                // Initial emit is 0; wait for a polled server-side fraction.
                if (p.fetchProgress > 0.0)
                    stop = true;
            }
        },
        debridId, error);
    assert(result == DebridRunResult::Stopped);
    assert(maxFetch > 0.49 && maxFetch < 0.51);
}

void testFetchProgressEmittedWhilePolling() {
    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", "{\"success\":true,\"data\":{\"id\":42,\"name\":\"E\","
                   "\"size\":100,\"progress\":0.25,"
                   "\"download_state\":\"downloading\","
                   "\"download_finished\":false,\"download_present\":false,"
                   "\"files\":[]}}"},
        {"mylist", infoReadyJson("file.bin", 4)},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher("data"));
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = "/tmp/pipensx-debrid-fetch-progress";
    spec.workingRoot = "/tmp/pipensx-debrid-fetch-progress";
    system(("rm -rf " + spec.dataPath).c_str());
    mkdir(spec.dataPath.c_str(), 0755);

    bool sawPartial = false;
    bool sawStart = false;
    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&](const DebridProgress& p) {
            if (p.status == DownloadStatus::Fetching) {
                if (p.fetchProgress == 0.0)
                    sawStart = true;
                if (p.fetchProgress > 0.24 && p.fetchProgress < 0.26)
                    sawPartial = true;
            }
        },
        debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(sawStart);
    assert(sawPartial);
}

void testStreamInstallCommitsPackage() {
    const std::string root = "/tmp/pipensx-torbox-stream-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(4096);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 7) ^ (i >> 3));
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/game.nsp", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(content));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);

    std::string committed = root + "/install-sim/" + spec.taskId +
                            "-Example_game.nsp";
    struct stat st {};
    assert(stat(committed.c_str(), &st) == 0);
}

void testStreamInstallStopResumesFromJournal() {
    const std::string root = "/tmp/pipensx-torbox-stream-pause-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(2 * 1024 * 1024, 0x5a);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 7) ^ (i >> 3));
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());

    std::vector<uint64_t> seenOffsets;
    RangeFetcher fetcher = [&content, &seenOffsets](
        const std::string&, uint64_t offset, uint64_t,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>& cancelled, std::string&) {
        seenOffsets.push_back(offset);
        uint64_t pos = offset;
        const uint64_t haltAt = content.size() / 2;
        while (pos < content.size()) {
            if (cancelled())
                return false;
            size_t n = std::min<size_t>(64 * 1024, content.size() - pos);
            if (!sink(reinterpret_cast<const uint8_t*>(content.data() +
                                                       static_cast<size_t>(pos)),
                      n))
                return false;
            pos += n;
            if (offset == 0 && pos >= haltAt) {
                while (!cancelled())
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return false;
            }
        }
        return true;
    };

    TorboxTransport transport = [&content](const TorboxHttpRequest& request,
                                           TorboxHttpResponse& response,
                                           std::string&) {
        response.status = 200;
        if (request.url.find("mylist") != std::string::npos)
            response.body = infoReadyJson("Example/game.nsp", content.size());
        else if (request.url.find("requestdl") != std::string::npos)
            response.body = "{\"success\":true,\"data\":\"https://x/dl\"}";
        else
            response.body = "{}";
        return true;
    };
    TorboxProvider provider("k", transport);
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::atomic<bool> stop{false};
    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [&stop] { return stop.load(); },
        [&stop](const DebridProgress& p) {
            if (p.status == DownloadStatus::Installing && p.installedBytes > 0)
                stop.store(true);
        },
        debridId, error);
    assert(result == DebridRunResult::Stopped);

    const std::string journalPath =
        pipensx::install::installJournalPath(root, spec.taskId);
    pipensx::install::InstallJournal journal;
    assert(pipensx::install::loadInstallJournal(journalPath, journal));
    assert(journal.state.consumed > 0);
    assert(journal.packageId == "Example/game.nsp");

    const std::string ncaPath =
        root + "/install-sim/" + spec.taskId +
        "-Example_game.nsp/00112233445566778899aabbccddeeff.nca";
    struct stat st {};
    assert(stat(ncaPath.c_str(), &st) == 0);

    stop.store(false);
    result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    bool sawResumeOffset = false;
    for (uint64_t offset : seenOffsets) {
        if (offset > 0)
            sawResumeOffset = true;
    }
    assert(sawResumeOffset);
    assert(!pipensx::install::loadInstallJournal(journalPath, journal));
}

void testPartialStreamFailureRetriesWithoutPacerDeadlock() {
    const std::string root = "/tmp/pipensx-torbox-stream-failure-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);

    std::vector<uint8_t> nca(2 * 1024 * 1024, 0x5a);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());
    int attempts = 0;
    RangeFetcher fetcher = [&content, &attempts](
        const std::string&, uint64_t, uint64_t,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>&, std::string& error) {
        ++attempts;
        const size_t partial = content.size() / 2;
        if (!sink(reinterpret_cast<const uint8_t*>(content.data()), partial))
            return false;
        error = "intentional partial failure";
        return false;
    };
    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/game.nsp", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/first\"}"},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/second\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Failed);
    assert(attempts == 2);
    assert(error == "intentional partial failure");
}

void testRealdebridInterruptedStreamResumes(bool failBothAttempts,
                                          bool truncatedSuccess = false,
                                          bool corrupt = false) {
    const std::string root = "/tmp/pipensx-rd-disconnect-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::vector<uint8_t> nca(2 * 1024 * 1024, 0x5a);
    const auto nsp = makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    const std::string content(nsp.begin(), nsp.end());
    std::vector<uint64_t> offsets;
    bool recovered = false;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset, uint64_t end,
                               const std::function<bool(const uint8_t*, size_t)>& sink,
                               const std::function<bool()>&, std::string& error) {
        assert(end == 0 && offset < content.size());
        offsets.push_back(offset);
        const bool interrupt = !recovered &&
                               (failBothAttempts || offsets.size() == 1);
        if (interrupt) {
            const size_t bytes = std::min<size_t>(512 * 1024, content.size() - offset);
            std::string partial = content.substr(static_cast<size_t>(offset), bytes);
            if (corrupt)
                partial.replace(0, 4, "BAD!");
            if (!sink(reinterpret_cast<const uint8_t*>(partial.data()), partial.size()))
                return false;
            error = "connection lost while screen was off";
            return truncatedSuccess;
        }
        return sink(reinterpret_cast<const uint8_t*>(content.data() + offset),
                    content.size() - static_cast<size_t>(offset));
    };
    RdTransport transport = [&](const RdHttpRequest& request,
                                RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos)
            response.body =
                "{\"id\":\"original\",\"filename\":\"Example\","
                "\"status\":\"downloaded\",\"progress\":100,\"files\":[{"
                "\"id\":1,\"path\":\"game.nsp\",\"selected\":1,\"bytes\":" +
                std::to_string(content.size()) +
                "}],\"links\":[\"https://rd.to/file\"]}";
        else {
            assert(request.url.find("/unrestrict/link") != std::string::npos);
            response.body = "{\"download\":\"https://cdn.example/game.nsp\"}";
        }
        return true;
    };
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "original";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    std::string id, error;
    const std::string journalPath = install::installJournalPath(root, spec.taskId);
    install::InstallJournal journal;
    // New provider/transfer instances model coming back after app interruption.
    {
        RealdebridProvider provider("key", transport);
        DebridTransfer transfer(provider, fetcher);
        const auto result = transfer.run(spec, [] { return false; },
                                        [](const DebridProgress&) {}, id, error);
        assert(result == (failBothAttempts ? DebridRunResult::Failed
                                          : DebridRunResult::Finished));
        assert(offsets.size() == 2 && offsets[0] == 0);
        assert((offsets[1] > 0) == !corrupt);
        if (failBothAttempts) {
            assert(install::loadInstallJournal(journalPath, journal) == !corrupt);
            if (!corrupt)
                assert(journal.state.consumed > offsets[1]);
        }
    }
    if (failBothAttempts) {
        recovered = true;
        error.clear();
        RealdebridProvider provider("key", transport);
        DebridTransfer transfer(provider, fetcher);
        const auto result = transfer.run(spec, [] { return false; },
                                        [](const DebridProgress&) {}, id, error);
        assert(result == DebridRunResult::Finished);
        assert((offsets.back() > 0) == !corrupt);
    }
    assert(!install::loadInstallJournal(journalPath, journal));
    const std::string output = root + "/install-sim/" + spec.taskId +
                              "-game.nsp/00112233445566778899aabbccddeeff.nca";
    std::ifstream in(output, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    assert(bytes == std::string(nca.begin(), nca.end()));
}

enum class CachedNszCase { Reconnect, PauseDownload, PauseInstall, PermanentError, CancelReconnect };

std::vector<uint8_t> makeSolidNsz(const std::vector<uint8_t>& nca) {
    assert(nca.size() > 0x4000);
    std::vector<uint8_t> compressed(ZSTD_compressBound(nca.size() - 0x4000));
    const size_t size = ZSTD_compress(compressed.data(), compressed.size(),
                                     nca.data() + 0x4000, nca.size() - 0x4000, 3);
    assert(!ZSTD_isError(size));
    compressed.resize(size);
    std::vector<uint8_t> ncz(nca.begin(), nca.begin() + 0x4000);
    ncz.insert(ncz.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
    append64(ncz, 1);
    append64(ncz, 0x4000);
    append64(ncz, nca.size() - 0x4000);
    append64(ncz, 1); // Unencrypted NCZ section.
    append64(ncz, 0);
    ncz.insert(ncz.end(), 32, 0);
    ncz.insert(ncz.end(), compressed.begin(), compressed.end());
    return makePfs0({{"00112233445566778899aabbccddeeff.ncz", ncz}});
}

void testSolidNszCacheRecovery(CachedNszCase scenario, bool splitCache = false,
                               bool resumeWithDefaultPolicy = false,
                               size_t batchBytes = 1u << 20) {
    const std::string root = "/tmp/pipensx-rd-nsz-cache-test";
    assert(system(("rm -rf " + root).c_str()) == 0);
    mkdir(root.c_str(), 0755);
    // Keep at least two input batches so pause can interrupt before EOF.
    std::vector<uint8_t> nca(2 * batchBytes);
    uint32_t seed = 12345;
    for (uint8_t& byte : nca) {
        seed = seed * 1664525U + 1013904223U;
        byte = static_cast<uint8_t>(seed >> 24);
    }
    const auto package = makeSolidNsz(nca);
    const std::string content(package.begin(), package.end());
    if (splitCache) {
        // Model a freshly prepared DBI folder without allocating >4 GiB.
        // storage's force_split contract intentionally keeps small files plain.
        assert(mkdir((root + "/game.nsz").c_str(), 0755) == 0);
        std::ofstream resume(root + "/game.nsz.pipensx-partial");
        resume << "0\n";
    }
    std::atomic<bool> stopped{false};
    bool secondRun = false;
    std::vector<uint64_t> offsets;
    RdTransport transport = [&](const RdHttpRequest& request,
                                RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos)
            response.body =
                "{\"id\":\"original\",\"filename\":\"Example\","
                "\"status\":\"downloaded\",\"progress\":100,\"files\":[{"
                "\"id\":1,\"path\":\"game.nsz\",\"selected\":1,\"bytes\":" +
                std::to_string(content.size()) +
                "}],\"links\":[\"https://rd.to/file\"]}";
        else {
            assert(request.url.find("/unrestrict/link") != std::string::npos);
            response.body = "{\"download\":\"https://cdn.example/game.nsz\"}";
        }
        return true;
    };
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset, uint64_t end,
                               const std::function<bool(const uint8_t*, size_t)>& sink,
                               const std::function<bool()>&, std::string& error) {
        assert(end == 0 && offset < content.size());
        offsets.push_back(offset);
        if (scenario == CachedNszCase::CancelReconnect) {
            error = "connection lost";
            return false;
        }
        const bool partial = !secondRun &&
            ((scenario == CachedNszCase::Reconnect && offsets.size() <= 3) ||
             scenario == CachedNszCase::PauseDownload ||
             scenario == CachedNszCase::PermanentError);
        const size_t count = partial ? 256 * 1024 : content.size() - offset;
        for (size_t sent = 0; sent < count;) {
            const size_t chunk = std::min<size_t>(16 * 1024, count - sent);
            if (!sink(reinterpret_cast<const uint8_t*>(content.data() + offset + sent), chunk))
                return false;
            sent += chunk;
        }
        if (scenario == CachedNszCase::PauseDownload && !secondRun) {
            stopped = true;
            error = "connection lost";
            return false;
        }
        if (partial) {
            error = scenario == CachedNszCase::PermanentError ? "HTTP 403" :
                    offsets.size() == 1 ? "connection lost" : "HTTP 503";
            return scenario == CachedNszCase::Reconnect && offsets.size() == 3;
        }
        return true;
    };
    DebridTaskSpec spec;
    spec.taskId = "ffeeddccbbaa99887766554433221100ffeeddcc";
    spec.debridId = "original";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    spec.cacheCompressedPackages = true;
    spec.installBatchBytes = batchBytes;
    spec.reconnectDelayMs = 1;
    spec.reconnectTimeoutMs = 1000;
    spec.forceSplitDownloads = splitCache;
    auto progress = [&](const DebridProgress& p) {
        if (p.status == DownloadStatus::Installing) {
            assert(p.completedBytes == content.size());
            assert(p.speedBytesPerSecond == 0);
            if (scenario == CachedNszCase::PauseInstall && !secondRun) {
                if (p.installedBytes > 64 * 1024)
                    stopped = true;
                else if (p.installedBytes > 0)
                    // Allow the next throttled progress sample inside zstd.
                    std::this_thread::sleep_for(std::chrono::milliseconds(275));
            }
        }
        if (scenario == CachedNszCase::CancelReconnect &&
            p.status == DownloadStatus::Downloading && p.currentPackage == "game.nsz")
            stopped = true;
    };
    std::string id, error;
    auto run = [&] {
        RealdebridProvider provider("key", transport);
        DebridTransfer transfer(provider, fetcher);
        return transfer.run(spec, [&] { return stopped.load(); }, progress, id, error);
    };
    const auto first = run();
    if (scenario == CachedNszCase::PermanentError) {
        assert(first == DebridRunResult::Failed);
        assert(offsets.size() == 2);
        assert(error == "HTTP 403");
        return;
    }
    if (scenario == CachedNszCase::CancelReconnect) {
        assert(first == DebridRunResult::Stopped);
        assert(offsets.size() == 2);
        return;
    }
    if (scenario == CachedNszCase::Reconnect) {
        assert(first == DebridRunResult::Finished);
        assert(offsets.size() == 4);
        for (size_t i = 1; i < offsets.size(); ++i)
            assert(offsets[i] == i * 256 * 1024);
    } else {
        assert(first == DebridRunResult::Stopped);
        assert(offsets.size() == 1);
        stopped = false;
        secondRun = true;
        if (resumeWithDefaultPolicy) spec.cacheCompressedPackages = false;
        assert(run() == DebridRunResult::Finished);
        if (scenario == CachedNszCase::PauseDownload) {
            assert(offsets.size() == 2 && offsets.back() == 256 * 1024);
        } else {
            assert(offsets.size() == 1); // Complete cache: no second HTTP request.
        }
    }
    const std::string output = root + "/install-sim/" + spec.taskId +
                              "-game.nsz/00112233445566778899aabbccddeeff.nca";
    std::ifstream in(output, std::ios::binary);
    const std::string installed((std::istreambuf_iterator<char>(in)), {});
    assert(installed == std::string(nca.begin(), nca.end()));
    std::ifstream cache(root + "/game.nsz" + (splitCache ? "/00" : ""), std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(cache)), {});
    assert(saved == content);
}

void testFreshNszStreamsWithoutDiskCache() {
    const std::string root = "/tmp/pipensx-rd-direct-nsz-test";
    assert(system(("rm -rf " + root).c_str()) == 0);
    assert(mkdir(root.c_str(), 0755) == 0);
    std::vector<uint8_t> nca(3 * 1024 * 1024);
    uint32_t seed = 12345;
    for (uint8_t& byte : nca) {
        seed = seed * 1664525U + 1013904223U;
        byte = static_cast<uint8_t>(seed >> 24);
    }
    const auto package = makeSolidNsz(nca);
    RdTransport transport = [&](const RdHttpRequest& request,
                                RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos)
            response.body = "{\"id\":\"original\",\"filename\":\"Example\","
                "\"status\":\"downloaded\",\"progress\":100,\"files\":[{"
                "\"id\":1,\"path\":\"game.nsz\",\"selected\":1,\"bytes\":" +
                std::to_string(package.size()) +
                "}],\"links\":[\"https://rd.to/file\"]}";
        else {
            assert(request.url.find("/unrestrict/link") != std::string::npos);
            response.body = "{\"download\":\"https://cdn.example/game.nsz\"}";
        }
        return true;
    };
    std::mutex mutex;
    std::condition_variable wrote;
    bool installedBeforeEof = false;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset, uint64_t end,
                              const auto& sink, const auto&, std::string&) {
        assert(offset == 0 && end == 0);
        const size_t batch = 1024 * 1024;
        if (!sink(package.data(), batch)) return false;
        std::unique_lock<std::mutex> lock(mutex);
        // With the old download-first policy no installer callback can run
        // until EOF. Direct NSZ must begin writing while HTTP is still active.
        assert(wrote.wait_for(lock, std::chrono::seconds(5),
                              [&] { return installedBeforeEof; }));
        lock.unlock();
        return sink(package.data() + batch, package.size() - batch);
    };
    RealdebridProvider provider("key", transport);
    DebridTransfer transfer(provider, fetcher);
    DebridTaskSpec spec;
    spec.taskId = "ffeeddccbbaa99887766554433221100ffeeddcc";
    spec.debridId = "original";
    spec.dataPath = spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    assert(!spec.cacheCompressedPackages);
    std::string id, error;
    uint32_t packages = 0;
    assert(transfer.run(spec, [] { return false; }, [&](const DebridProgress& p) {
        packages = p.packagesInstalled;
        if (p.installedBytes > 0 && p.completedBytes < package.size()) {
            std::lock_guard<std::mutex> lock(mutex);
            installedBeforeEof = true;
            wrote.notify_one();
        }
    }, id, error) == DebridRunResult::Finished);
    assert(installedBeforeEof && packages == 1);
    struct stat st{};
    assert(stat((root + "/game.nsz").c_str(), &st) != 0);
    const std::string output = root + "/install-sim/" + spec.taskId +
                              "-game.nsz/00112233445566778899aabbccddeeff.nca";
    std::ifstream file(output, std::ios::binary);
    const std::string installed((std::istreambuf_iterator<char>(file)), {});
    assert(installed == std::string(nca.begin(), nca.end()));
    std::puts("fresh NSZ direct HTTP install before EOF, no disk cache ok");
}

void testSelectionPathsPicksOneFile() {
    const std::string root = "/tmp/pipensx-torbox-selection-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::string contentB(60000, 'z');
    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/skip.bin", 40000,
                                    "Example/keep.bin", contentB.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(contentB));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;
    spec.selectionPaths = {{"keep.bin", contentB.size()}};

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.completedBytes == contentB.size());

    struct stat st {};
    assert(stat((data + "/skip.bin").c_str(), &st) != 0);
    std::ifstream check(data + "/keep.bin", std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(check)),
                        std::istreambuf_iterator<char>());
    assert(written == contentB);
}

void testManifestSelectionDistinguishesNestedBasenames(bool missingFile = false) {
    char temp[] = "/tmp/pipensx-manifest-selection-XXXXXX";
    const char* created = mkdtemp(temp);
    assert(created);
    const std::string root = created;
    const std::string content(123, 'm');
    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/unselected/same.bin", content.size(),
                                    "Example/DLC/nested/same.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    unsigned fetched = 0;
    const auto fetch = memoryFetcher(content);
    DebridTransfer transfer(provider, [&](const std::string& url, uint64_t offset, uint64_t end,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>& cancelled, std::string& error) {
        ++fetched;
        return fetch(url, offset, end, sink, cancelled, error);
    });
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;
    spec.selectionRelativePaths = {{"DLC/nested/same.bin", content.size()}};
    if (missingFile) spec.selectionRelativePaths.emplace_back("Updates/missing.nsp", 456);
    bool manifestReplaced = false;
    spec.filesResolved = [&](const auto&) { manifestReplaced = true; };
    // A stale index mask cannot override the recorded full path.
    spec.fileSelection = {1, 0};
    std::string id, error;
    const auto result = transfer.run(spec, [] { return false; }, [](const DebridProgress&) {}, id, error);
    if (missingFile) {
        assert(result == DebridRunResult::Failed);
        assert(fetched == 0 && !manifestReplaced);
        assert(error.find("all saved selected files") != std::string::npos);
        return;
    }
    assert(result == DebridRunResult::Finished);
    assert(fetched == 1);
    assert(access((root + "/unselected/same.bin").c_str(), F_OK) != 0);
    std::ifstream input(root + "/DLC/nested/same.bin", std::ios::binary);
    assert(std::string((std::istreambuf_iterator<char>(input)), {}) == content);
}

void testSelectionPathsNoMatchReturnsFailed() {
    const std::string root = "/tmp/pipensx-torbox-nomatch-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/alpha.bin", 10000,
                                    "Example/beta.bin", 20000)},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(""));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;
    spec.selectionPaths = {{"gamma.bin", 10000}};

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Failed);
    assert(!error.empty());

    struct stat st {};
    assert(stat((data + "/alpha.bin").c_str(), &st) != 0);
    assert(stat((data + "/beta.bin").c_str(), &st) != 0);
    assert(stat((data + "/gamma.bin").c_str(), &st) != 0);
}

void testMagnetFileFallback() {
    const std::string root = "/tmp/pipensx-torbox-fallback-test";
    system(("rm -rf " + root).c_str());
    ::mkdir(root.c_str(), 0777);

    std::vector<std::pair<std::string, std::string>> script = {
        {"createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":42}}"},
        {"mylist", "{\"success\":true,\"data\":{\"id\":42,"
                   "\"name\":\"Example\",\"size\":4,\"progress\":0.1,"
                   "\"download_state\":\"downloading\","
                   "\"download_finished\":false,"
                   "\"download_present\":false,\"files\":[]}}"},
        {"controltorrent", "{\"success\":true}"},
        {"createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":42}}"},
        {"mylist", infoReadyJson("data.bin", 4)},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("key", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher("data"));

    std::string torrentPath = root + "/fallback.torrent";
    std::ofstream(torrentPath, std::ios::binary)
        << "d4:infod4:name8:data.binee";

    DebridTaskSpec spec;
    spec.taskId = "hash";
    spec.magnet = "magnet:?xt=urn:btih:hash";
    spec.torrentPath = torrentPath;
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;

    auto never = [] { return false; };
    auto noprog = [](const DebridProgress&) {};
    std::string createdId;
    std::string error;
    DebridRunResult result = transfer.run(spec, never, noprog, createdId,
                                          error, 0);
    assert(result == DebridRunResult::Finished);
    assert(script.empty());
    std::puts("magnet->file fallback ok");
}

void testOutOfOrderRangesAssembleInOrder() {
    const std::string root = "/tmp/pipensx-torbox-ooo-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    const size_t size = 10 * 1024 * 1024;
    std::string content(size, '\0');
    for (size_t i = 0; i < size; ++i)
        content[i] = static_cast<char>(i * 131u);

    std::mutex mu;
    std::vector<uint64_t> starts;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        {
            std::lock_guard<std::mutex> lock(mu);
            starts.push_back(offset);
        }
        if (endExclusive != 0 && offset == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        uint64_t end = endExclusive == 0
            ? content.size()
            : std::min<uint64_t>(endExclusive, content.size());
        if (offset > end) {
            error = "range past end";
            return false;
        }
        return sink(reinterpret_cast<const uint8_t*>(content.data()) +
                        static_cast<size_t>(offset),
                    static_cast<size_t>(end - offset));
    };

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/file.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(starts.size() >= 2);

    std::ifstream check(data + "/file.bin", std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(check)),
                        std::istreambuf_iterator<char>());
    assert(written == content);
}

void testRangeIgnoredFallsBackToSequential() {
    const std::string root = "/tmp/pipensx-torbox-range200-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    const size_t size = 10 * 1024 * 1024;
    std::string content(size, 'R');
    int rangedCalls = 0;
    int sequentialCalls = 0;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (endExclusive != 0) {
            ++rangedCalls;
            error = kDebridRangeNotSupported;
            return false;
        }
        ++sequentialCalls;
        std::string slice = content.substr(static_cast<size_t>(offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/file.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(rangedCalls >= 1);
    assert(sequentialCalls == 1);

    std::ifstream check(data + "/file.bin", std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(check)),
                        std::istreambuf_iterator<char>());
    assert(written == content);
}

void testStreamInstallCoalescesTinyChunks() {
    const std::string root = "/tmp/pipensx-torbox-coalesce-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(2 * 1024 * 1024, 0x3c);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/game.nsp", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, chunkedFetcher(content, 4096));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
}

void testTorboxStreamInstallUsesRangeFetch() {
    const std::string root = "/tmp/pipensx-torbox-stream-range-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    const size_t size = 10 * 1024 * 1024;
    std::vector<uint8_t> nca(size, 0x2a);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());

    int rangedCalls = 0;
    int sequentialCalls = 0;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (offset > content.size()) {
            error = "range past end";
            return false;
        }
        uint64_t end = endExclusive == 0
            ? content.size()
            : std::min<uint64_t>(endExclusive, content.size());
        if (offset > end) {
            error = "range past end";
            return false;
        }
        if (endExclusive != 0)
            ++rangedCalls;
        else
            ++sequentialCalls;
        std::string slice = content.substr(static_cast<size_t>(offset),
                                           static_cast<size_t>(end - offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJson("Example/game.nsp", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(error.empty());
    assert(rangedCalls >= 1);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
}

void testRealdebridStreamInstallUsesSequentialFetch(bool measureSpeed = false) {
    const std::string root = "/tmp/pipensx-rd-stream-seq-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    const size_t size = (measureSpeed ? 32 : 10) * 1024 * 1024;
    std::vector<uint8_t> nca(size, 0x2b);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());

    int rangedCalls = 0;
    int sequentialCalls = 0;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (endExclusive != 0) {
            ++rangedCalls;
            error = kDebridRangeNotSupported;
            return false;
        }
        ++sequentialCalls;
        if (measureSpeed) {
            for (size_t pos = static_cast<size_t>(offset); pos < content.size();) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const size_t n = std::min<size_t>(512 * 1024, content.size() - pos);
                if (!sink(reinterpret_cast<const uint8_t*>(content.data() + pos), n))
                    return false;
                pos += n;
            }
            return true;
        }
        std::string slice = content.substr(static_cast<size_t>(offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<std::pair<std::string, std::string>> script = {
        {"/torrents/info/",
         "{\"id\":\"abc123\",\"filename\":\"Example\",\"bytes\":" +
             std::to_string(content.size()) +
             ",\"progress\":100,\"status\":\"downloaded\","
             "\"files\":[{\"id\":\"1\",\"path\":\"/Example/game.nsp\","
             "\"bytes\":" +
             std::to_string(content.size()) +
             ",\"selected\":1}],"
             "\"links\":[\"https://rd.to/dl/x\"]}"},
        {"/unrestrict/link",
         "{\"download\":\"https://cdn.example/game.nsp\","
         "\"filename\":\"game.nsp\",\"filesize\":" +
             std::to_string(content.size()) + "}"},
    };
    RdTransport transport = [&script](const RdHttpRequest& request,
                                      RdHttpResponse& response, std::string&) {
        for (const auto& entry : script) {
            if (request.url.find(entry.first) != std::string::npos) {
                response.status = 200;
                response.body = entry.second;
                return true;
            }
        }
        response.status = 200;
        response.body = "{}";
        return true;
    };
    RealdebridProvider provider("k", transport);
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc123";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridProgress last;
    bool measuredWhileInstalling = false;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&](const DebridProgress& p) {
            last = p;
            if (p.status == DownloadStatus::Installing && p.installedBytes > 0 &&
                p.speedBytesPerSecond > 0)
                measuredWhileInstalling = true;
        }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(error.empty());
    assert(rangedCalls == 0);
    assert(sequentialCalls >= 1);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
    if (measureSpeed) {
        assert(measuredWhileInstalling);
        assert(last.speedBytesPerSecond == 0);
    }
}

void testRealdebridDownloadOnlyUsesSequentialFetch() {
    const std::string root = "/tmp/pipensx-rd-download-seq-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    const size_t size = 10 * 1024 * 1024;
    std::string content(size, 'D');
    int rangedCalls = 0;
    int sequentialCalls = 0;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (endExclusive != 0) {
            ++rangedCalls;
            error = kDebridRangeNotSupported;
            return false;
        }
        ++sequentialCalls;
        std::string slice = content.substr(static_cast<size_t>(offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<std::pair<std::string, std::string>> script = {
        {"/torrents/info/",
         "{\"id\":\"abc123\",\"filename\":\"Example\",\"bytes\":" +
             std::to_string(size) +
             ",\"progress\":100,\"status\":\"downloaded\","
             "\"files\":[{\"id\":\"1\",\"path\":\"/file.bin\","
             "\"bytes\":" +
             std::to_string(size) +
             ",\"selected\":1}],"
             "\"links\":[\"https://rd.to/dl/x\"]}"},
        {"/unrestrict/link",
         "{\"download\":\"https://cdn.example/file.bin\"}"},
    };
    RdTransport transport = [&script](const RdHttpRequest& request,
                                      RdHttpResponse& response, std::string&) {
        for (const auto& entry : script) {
            if (request.url.find(entry.first) != std::string::npos) {
                response.status = 200;
                response.body = entry.second;
                return true;
            }
        }
        response.status = 200;
        response.body = "{}";
        return true;
    };
    RealdebridProvider provider("k", transport);
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc123";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(rangedCalls == 0);
    assert(sequentialCalls >= 1);

    std::ifstream check(data + "/file.bin", std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(check)),
                        std::istreambuf_iterator<char>());
    assert(written == content);
}

void testTorrserverDownloadOnlySequentialAndRemoves() {
    const std::string root = "/tmp/pipensx-ts-download-seq-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    const size_t size = 10 * 1024 * 1024;
    std::string content(size, 'T');
    int rangedCalls = 0;
    int sequentialCalls = 0;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (endExclusive != 0) {
            ++rangedCalls;
            error = kDebridRangeNotSupported;
            return false;
        }
        ++sequentialCalls;
        std::string slice = content.substr(static_cast<size_t>(offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<TsHttpRequest> seen;
    std::vector<std::pair<long, std::string>> replies = {
        {200, "{\"name\":\"Example\",\"hash\":\"abc\",\"stat\":3,"
              "\"stat_string\":\"Torrent working\",\"torrent_size\":" +
                  std::to_string(size) +
                  ",\"file_stats\":[{\"id\":1,\"path\":\"Example/file.bin\","
                  "\"length\":" +
                  std::to_string(size) + "}]}"},
        {200, ""},
    };
    size_t next = 0;
    TsTransport transport = [&](const TsHttpRequest& request,
                                TsHttpResponse& response, std::string&) {
        seen.push_back(request);
        assert(next < replies.size());
        response.status = replies[next].first;
        response.body = replies[next].second;
        ++next;
        return true;
    };
    TorrserverProvider provider("http://box:8090", transport);
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(rangedCalls == 0);
    assert(sequentialCalls >= 1);
    assert(seen.size() == 2);
    assert(seen[0].body.find("\"action\":\"add\"") != std::string::npos);
    assert(seen[1].body.find("\"action\":\"rem\"") != std::string::npos);
    assert(seen[1].body.find("\"hash\":\"abc\"") != std::string::npos);

    std::ifstream check(data + "/file.bin", std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(check)),
                        std::istreambuf_iterator<char>());
    assert(written == content);
}

void testTorrserverStreamInstallSequentialAndRemoves() {
    const std::string root = "/tmp/pipensx-ts-stream-seq-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(4096);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 7) ^ (i >> 3));
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::string content(nsp.begin(), nsp.end());

    int rangedCalls = 0;
    int sequentialCalls = 0;
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (endExclusive != 0) {
            ++rangedCalls;
            error = kDebridRangeNotSupported;
            return false;
        }
        ++sequentialCalls;
        std::string slice = content.substr(static_cast<size_t>(offset));
        return sink(reinterpret_cast<const uint8_t*>(slice.data()),
                    slice.size());
    };

    std::vector<TsHttpRequest> seen;
    std::vector<std::pair<long, std::string>> replies = {
        {200, "{\"name\":\"Example\",\"hash\":\"abc\",\"stat\":3,"
              "\"stat_string\":\"Torrent working\",\"torrent_size\":" +
                  std::to_string(content.size()) +
                  ",\"file_stats\":[{\"id\":1,\"path\":\"Example/game.nsp\","
                  "\"length\":" +
                  std::to_string(content.size()) + "}]}"},
        {200, ""},
    };
    size_t next = 0;
    TsTransport transport = [&](const TsHttpRequest& request,
                                TsHttpResponse& response, std::string&) {
        seen.push_back(request);
        assert(next < replies.size());
        response.status = replies[next].first;
        response.body = replies[next].second;
        ++next;
        return true;
    };
    TorrserverProvider provider("http://box:8090", transport);
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(error.empty());
    assert(rangedCalls == 0);
    assert(sequentialCalls >= 1);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
    assert(seen.size() == 2);
    assert(seen[1].body.find("\"action\":\"rem\"") != std::string::npos);
}

void testTorrserverStopDoesNotRemove() {
    std::vector<TsHttpRequest> seen;
    std::vector<std::pair<long, std::string>> replies = {
        {200, "{\"name\":\"E\",\"hash\":\"abc\",\"stat\":1,"
              "\"stat_string\":\"Torrent getting info\"}"},
    };
    size_t next = 0;
    TsTransport transport = [&](const TsHttpRequest& request,
                                TsHttpResponse& response, std::string&) {
        seen.push_back(request);
        assert(next < replies.size());
        response.status = replies[next].first;
        response.body = replies[next].second;
        ++next;
        return true;
    };
    TorrserverProvider provider("http://box:8090", transport);
    DebridTransfer transfer(provider, memoryFetcher(""));
    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc";
    spec.dataPath = "/tmp";
    spec.workingRoot = "/tmp";
    bool stop = false;
    int fetches = 0;
    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [&stop] { return stop; },
        [&stop, &fetches](const DebridProgress& p) {
            if (p.status == DownloadStatus::Fetching) {
                ++fetches;
                if (fetches >= 2)
                    stop = true;
            }
        },
        debridId, error);
    assert(result == DebridRunResult::Stopped);
    for (const TsHttpRequest& request : seen)
        assert(request.body.find("\"action\":\"rem\"") == std::string::npos);
}

enum class GroupedCase { Success, Missing, Cancelled, Corrupt, Resume, Zip64, Cached, Nsz, RarNsz };

void testFourPackagesResumeAfterRepeatedInterruptions() {
    const std::string root = "/tmp/pipensx-four-package-resume";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto package = makePfs0({{"00112233445566778899aabbccddeeff.nca",
                                   std::vector<uint8_t>(4096, 0x35)}});
    const std::string body(package.begin(), package.end());
    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i)
        paths.push_back("Example/subfolder/part" + std::to_string(i) + ".nsp");
    RdTransport transport = [&](const RdHttpRequest& request,
                                RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos) {
            nlohmann::json files = nlohmann::json::array();
            nlohmann::json links = nlohmann::json::array();
            for (size_t i = 0; i < paths.size(); ++i) {
                files.push_back({{"id", i + 1}, {"path", "/" + paths[i]},
                                 {"bytes", body.size()}, {"selected", 1}});
                links.push_back("https://rd.to/part" + std::to_string(i));
            }
            response.body = nlohmann::json({{"id", "existing"}, {"filename", "Example"},
                {"status", "downloaded"}, {"progress", 100},
                {"files", files}, {"links", links}}).dump();
        } else if (request.url.find("/unrestrict/link") != std::string::npos) {
            const auto pos = request.body.find("part");
            assert(pos != std::string::npos);
            const auto part = request.body.substr(pos, 5);
            response.body = nlohmann::json({{"filename", part + ".nsp"},
                {"filesize", body.size()}, {"download", "https://cdn.example/" + part}}).dump();
        } else {
            assert(false && "resume must reuse the existing account torrent");
        }
        return true;
    };
    std::vector<std::string> fetched;
    const auto source = memoryFetcher(body);
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset, uint64_t end,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>& cancelled, std::string& error) {
        fetched.push_back(url);
        return source(url, offset, end, sink, cancelled, error);
    };
    DebridTaskSpec spec;
    spec.taskId = std::string(40, 'e'); spec.debridId = "existing";
    spec.dataPath = root; spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    RealdebridProvider provider("test-key", transport);
    uint32_t savedPackages = 0;
    bool stop = false;
    std::string id, error;
    {
        DebridTransfer first(provider, fetcher);
        const auto result = first.run(spec, [&] { return stop; },
            [&](const DebridProgress& p) {
                assert(p.packagesInstalled >= savedPackages);
                savedPackages = p.packagesInstalled;
                if (savedPackages == 3) stop = true;
            }, id, error);
        assert(result == DebridRunResult::Stopped && savedPackages == 3);
        assert(fetched.size() == 3);
    }
    spec.packagesInstalled = savedPackages;
    fetched.clear(); stop = false;
    {
        // Another interruption while starting the provider must never replace
        // the durable 3/4 checkpoint with the default progress value of zero.
        DebridTransfer interrupted(provider, fetcher);
        const auto result = interrupted.run(spec, [&] { return stop; },
            [&](const DebridProgress& p) {
                savedPackages = p.packagesInstalled;
                stop = true;
            }, id, error);
        assert(result == DebridRunResult::Stopped && savedPackages == 3);
        assert(fetched.empty());
    }
    spec.packagesInstalled = savedPackages; stop = false;
    {
        DebridTransfer resumed(provider, fetcher);
        const auto result = resumed.run(spec, [&] { return stop; },
            [&](const DebridProgress& p) {
                assert(p.packagesInstalled >= 3);
                savedPackages = p.packagesInstalled;
            }, id, error);
        assert(result == DebridRunResult::Finished && savedPackages == 4);
        assert(fetched.size() == 1 && fetched.front().find("part3") != std::string::npos);
    }
    std::filesystem::remove_all(root);
}

void testRealdebridGroupedLinks(TransferMode mode, GroupedCase scenario) {
    using Json = nlohmann::json;
    const std::string root = "/tmp/pipensx-rd-grouped-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto package = scenario == GroupedCase::Nsz || scenario == GroupedCase::RarNsz
        ? makeSolidNsz(std::vector<uint8_t>(0x10000, 0x11))
        : makePfs0({{"00112233445566778899aabbccddeeff.nca",
                     std::vector<uint8_t>(64, 0x11)}});
    const std::string body(package.begin(), package.end());
    // Cloud selection is a superset. Same basenames in distinct subfolders
    // must not redirect a requested update to an unrelated member.
    std::vector<std::string> paths = {
        "Updates/Example [0100D71004694800][v1].nsp",
        "Unused/Example [0100D71004694800][v1].nsp",
        "DLC/Example [0100D71004695001][v0].nsp",
        "Base/Example [0100D71004694000][v0].nsp",
        "Homebrew/switch/smwr_nx/smwr.nro"};
    if (scenario == GroupedCase::Nsz || scenario == GroupedCase::RarNsz)
        for (size_t i = 0; i < 4; ++i) paths[i].replace(paths[i].size() - 3, 3, "nsz");
    std::vector<zip_fixture::File> members;
    for (size_t i = 0; i < paths.size(); ++i)
        if (!(scenario == GroupedCase::Missing && i == 2))
            members.push_back({"Example/" + paths[i], i == 4 ? "port-data" : body, i == 2});
    std::string archive = scenario == GroupedCase::RarNsz ? zip_fixture::rar4(members, true)
        : zip_fixture::build(members, scenario == GroupedCase::Zip64);
    if (scenario == GroupedCase::Corrupt) archive[30 + members[0].path.size() + body.size() - 1] ^= 1;
    if (scenario == GroupedCase::Cached) {
        for (size_t i = 0; i < paths.size(); ++i) if (i != 1) {
            std::filesystem::create_directories(std::filesystem::path(root + "/Example/" + paths[i]).parent_path());
            std::ofstream output(root + "/Example/" + paths[i], std::ios::binary);
            output << (i == 4 ? "port-data" : body);
        }
    }
    int unrestrictCalls = 0, cloudMutations = 0, ranges = 0;
    bool stop = false;
    RdTransport transport = [&](const RdHttpRequest& request, RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos) {
            Json files = Json::array();
            for (size_t i = 0; i < paths.size(); ++i)
                files.push_back({{"id", i + 1}, {"path", "/Example/" + paths[i]},
                    {"bytes", i == 4 ? 9 : body.size()}, {"selected", 1}});
            response.body = Json({{"id", "original"}, {"filename", "Example"},
                {"status", "downloaded"}, {"progress", 100}, {"files", files},
                {"links", Json::array({"https://rd.to/grouped.zip"})}}).dump();
        } else if (request.url.find("/unrestrict/link") != std::string::npos) {
            ++unrestrictCalls;
            response.body = Json({{"filename", "bundle.zip"}, {"filesize", archive.size()},
                                  {"download", "https://cdn.example/bundle"}}).dump();
        } else {
            ++cloudMutations;
            assert(false && "completed grouped torrent must not be recreated/reselected/deleted");
        }
        return true;
    };
    RangeFetcher fetcher = [&](const std::string&, uint64_t offset, uint64_t end,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>&, std::string&) {
        ++ranges;
        assert(end && offset <= end && end <= archive.size());
        if (scenario == GroupedCase::Cancelled) { stop = true; return false; }
        return sink(reinterpret_cast<const uint8_t*>(archive.data() + offset), end - offset);
    };
    RealdebridProvider provider("k", transport);
    DebridTransfer transfer(provider, fetcher);
    DebridTaskSpec spec;
    spec.taskId = std::string(40, 'a'); spec.debridId = "original";
    spec.dataPath = root; spec.workingRoot = root; spec.mode = mode;
    for (size_t i = 0; i < paths.size(); ++i)
        if (i != 1) spec.selectionRelativePaths.emplace_back("Example/" + paths[i], i == 4 ? 9 : body.size());
    if (scenario == GroupedCase::Resume) spec.packagesInstalled = 1;
    std::string id, error;
    DebridProgress last;
    const auto result = transfer.run(spec, [&] { return stop; },
        [&](const DebridProgress& p) {
            assert(p.packagesInstalled >= spec.packagesInstalled);
            last = p;
        }, id, error);
    if (result == DebridRunResult::Failed)
        std::fprintf(stderr, "grouped case=%d mode=%d error=%s\n", int(scenario), int(mode), error.c_str());
    assert(id == "original" && cloudMutations == 0);
    if (scenario == GroupedCase::Missing || scenario == GroupedCase::Corrupt) {
        assert(result == DebridRunResult::Failed);
        assert(last.status != DownloadStatus::Installed && last.status != DownloadStatus::Completed);
        if (scenario == GroupedCase::Missing) assert(last.packagesInstalled == 0);
    } else if (scenario == GroupedCase::Cancelled) {
        assert(result == DebridRunResult::Stopped);
    } else {
        assert(result == DebridRunResult::Finished && error.empty());
        assert(last.packagesInstalled == (mode == TransferMode::StreamInstall ? 3 : 0));
        assert(std::filesystem::exists(root + "/Example/" + paths[4]));
        assert(!std::filesystem::exists(root + "/Example/" + paths[1]));
        if (scenario == GroupedCase::Cached) assert(unrestrictCalls == 0 && ranges == 0);
        else assert(unrestrictCalls == 1 && ranges > 0);
    }
}

void testRealdebridRefreshesNestedPackagesOnExistingTorrent(bool completedSuperset = false) {
    using Json = nlohmann::json;
    const std::string root = "/tmp/pipensx-rd-existing-selection-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);

    const std::vector<std::string> paths = {
        "/Crash Team Racing Nitro-Fueled/Base/Crash Team Racing "
        "[0100F9F00C696000][v0].nsp",
        "/Crash Team Racing Nitro-Fueled/Updates/Crash Team Racing "
        "[0100F9F00C696800][v917504].nsp",
        "/Crash Team Racing Nitro-Fueled/DLC/Cove Cruiser "
        "[0100F9F00C697001][v0].nsp",
        "/Crash Team Racing Nitro-Fueled/Extras/ReadMe.txt"};
    const auto base = makePfs0({{"00112233445566778899aabbccddeeff.nca",
                                 std::vector<uint8_t>(64, 0x11)}});
    const auto update = makePfs0({{"ffeeddccbbaa99887766554433221100.nca",
                                   std::vector<uint8_t>(64, 0x22)}});
    const auto dlc = makePfs0({{"11223344556677889900aabbccddeeff.nca",
                                std::vector<uint8_t>(64, 0x33)}});
    const std::vector<std::string> bodies = {
        std::string(base.begin(), base.end()),
        std::string(update.begin(), update.end()),
        std::string(dlc.begin(), dlc.end())};

    bool replacementSelected = false;
    int selectCalls = 0;
    int addMagnetCalls = 0;
    int deleteCalls = 0;
    std::vector<std::string> fetched;
    RdTransport transport = [&](const RdHttpRequest& request,
                                RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/original") != std::string::npos) {
            Json files = Json::array();
            for (size_t position = 0; position < paths.size(); ++position) {
                const size_t i = completedSuperset ? (position + 3) % paths.size() : position;
                const bool wantedPackage = i < 3;
                files.push_back({{"id", std::to_string(10 + i)},
                                 {"path", paths[i]},
                                 {"bytes", wantedPackage ? bodies[i].size() : 9},
                                 {"selected", completedSuperset || i == 0 ? 1 : 0}});
            }
            response.body = Json({{"id", "original"},
                                  {"filename", "Crash Team Racing Nitro-Fueled"},
                                  {"hash", std::string(40, 'c')},
                                  {"status", "downloaded"},
                                  {"progress", 100}, {"files", files},
                                  {"links", completedSuperset
                    ? Json::array({"https://rd.to/readme", "https://rd.to/base",
                                   "https://rd.to/update", "https://rd.to/dlc"})
                    : Json::array({"https://rd.to/base"})}}).dump();
        } else if (request.url.find("/torrents/info/replacement") !=
                   std::string::npos) {
            Json files = Json::array();
            for (size_t i = 0; i < paths.size(); ++i) {
                const bool wantedPackage = i < 3;
                files.push_back({{"id", std::to_string(10 + i)},
                                 {"path", paths[i]},
                                 {"bytes", wantedPackage ? bodies[i].size() : 9},
                                 {"selected", replacementSelected && wantedPackage
                                                  ? 1 : 0}});
            }
            response.body = Json({{"id", "replacement"},
                                  {"filename", "Crash Team Racing Nitro-Fueled"},
                                  {"hash", std::string(40, 'c')},
                                  {"status", replacementSelected
                                                 ? "downloaded"
                                                 : "waiting_files_selection"},
                                  {"progress", replacementSelected ? 100 : 0},
                                  {"files", files},
                                  {"links", replacementSelected
                                                ? Json::array({"https://rd.to/base",
                                                               "https://rd.to/update",
                                                               "https://rd.to/dlc"})
                                                : Json::array()}}).dump();
        } else if (request.url.find("/torrents/selectFiles/original") !=
                   std::string::npos) {
            ++selectCalls;
            assert(request.body == "files=10,11,12");
            // RD documents 202 as "Action already done": this selection is
            // finalized and cannot be expanded in-place.
            response.status = 202;
            response.body.clear();
        } else if (request.url.find("/torrents/selectFiles/replacement") !=
                   std::string::npos) {
            ++selectCalls;
            assert(request.body == "files=10,11,12");
            replacementSelected = true;
            response.status = 204;
            response.body.clear();
        } else if (request.url.find("/torrents/addMagnet") !=
                   std::string::npos) {
            ++addMagnetCalls;
            assert(request.body.find(std::string(40, 'c')) !=
                   std::string::npos);
            response.status = 201;
            response.body = Json({{"id", "replacement"}}).dump();
        } else if (request.url.find("/torrents/delete/") !=
                   std::string::npos) {
            ++deleteCalls;
            assert(false && "existing RD torrent must not be deleted");
        } else if (request.url.find("/unrestrict/link") !=
                   std::string::npos) {
            size_t i = request.body.find("base") != std::string::npos ? 0
                     : request.body.find("update") != std::string::npos ? 1 : 2;
            response.body = Json({{"filename", paths[i]},
                                  {"filesize", bodies[i].size()},
                                  {"download", "https://cdn.example/file" +
                                                   std::to_string(i)}}).dump();
        } else {
            assert(false && "unexpected Real-Debrid request");
        }
        return true;
    };
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset,
                               uint64_t,
                               const std::function<bool(const uint8_t*, size_t)>& sink,
                               const std::function<bool()>&, std::string&) {
        const size_t i = url.find("file0") != std::string::npos ? 0
                       : url.find("file1") != std::string::npos ? 1 : 2;
        fetched.push_back(url);
        return sink(reinterpret_cast<const uint8_t*>(bodies[i].data() + offset),
                    bodies[i].size() - static_cast<size_t>(offset));
    };

    RealdebridProvider provider("k", transport);
    DebridTransfer transfer(provider, fetcher);
    DebridTaskSpec spec;
    spec.taskId = std::string(40, 'c');
    spec.debridId = "original";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    spec.fileSelection = {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip)};

    if (completedSuperset)
        for (size_t i = 0; i < 3; ++i)
            spec.selectionRelativePaths.emplace_back(paths[i].substr(1), bodies[i].size());
    std::string id, error;
    DebridProgress last;
    const DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&](const DebridProgress& p) { last = p; }, id, error);
    assert(result == DebridRunResult::Finished);
    assert(error.empty());
    assert(id == (completedSuperset ? "original" : "replacement"));
    assert(selectCalls == (completedSuperset ? 0 : 2));
    assert(addMagnetCalls == (completedSuperset ? 0 : 1));
    assert(deleteCalls == 0);
    assert(fetched.size() == 3);
    assert(last.packagesInstalled == 3);
}

void testRealdebridInstallsBaseBeforeUpdate(bool failLast = false,
                                           bool dlcLast = false,
                                           bool corruptLast = false) {
    const std::string root = "/tmp/pipensx-rd-base-first-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);

    std::vector<uint8_t> nca(64, 0x11);
    std::vector<uint8_t> baseNsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    std::vector<uint8_t> updateNca(64, 0x22);
    std::vector<uint8_t> updateNsp =
        makePfs0({{"ffeeddccbbaa99887766554433221100.nca", updateNca}});
    const std::string baseContent(baseNsp.begin(), baseNsp.end());
    const std::string updateContent(updateNsp.begin(), updateNsp.end());
    const std::string corruptContent(updateContent.size(), '\0');
    const std::string updateName = dlcLast
        ? "Minecraft DLC [0100D71004695001][v196608].nsp"
        : "Minecraft [0100D71004694800][v10420224].nsp";

    std::vector<std::string> fetched;
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset,
                               uint64_t,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        fetched.push_back(url);
        if (failLast && url.find("update") != std::string::npos) {
            error = "package fetch failed";
            return false;
        }
        const std::string& body = url.find("base") != std::string::npos
            ? baseContent : corruptLast ? corruptContent : updateContent;
        if (offset >= body.size())
            return false;
        return sink(reinterpret_cast<const uint8_t*>(body.data() + offset),
                    body.size() - static_cast<size_t>(offset));
    };

    RdTransport transport = [&](const RdHttpRequest& request,
                                RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos) {
            response.body =
                "{\"id\":\"abc123\",\"filename\":\"Minecraft [NSP]\","
                "\"bytes\":" +
                std::to_string(updateContent.size() + baseContent.size()) +
                ",\"progress\":100,\"status\":\"downloaded\","
                "\"files\":["
                "{\"id\":\"1\",\"path\":\"/" + updateName + "\","
                "\"bytes\":" +
                std::to_string(updateContent.size()) +
                ",\"selected\":1},"
                "{\"id\":\"2\",\"path\":\"/Minecraft [0100D71004694000][v0].nsp\","
                "\"bytes\":" +
                std::to_string(baseContent.size()) +
                ",\"selected\":1}],"
                "\"links\":[\"https://rd.to/dl/update\",\"https://rd.to/dl/base\"]}";
        } else if (request.url.find("/unrestrict/link") != std::string::npos) {
            const bool base = request.body.find("base") != std::string::npos;
            const std::string name =
                base ? "Minecraft [0100D71004694000][v0].nsp"
                     : updateName;
            const size_t size =
                base ? baseContent.size() : updateContent.size();
            const std::string host = base ? "https://cdn.example/base.nsp"
                                          : "https://cdn.example/update.nsp";
            response.body = "{\"filename\":\"" + name +
                            "\",\"filesize\":" + std::to_string(size) +
                            ",\"download\":\"" + host + "\"}";
        } else {
            response.body = "{}";
        }
        return true;
    };
    RealdebridProvider provider("k", transport);
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc123";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridProgress last;
    bool reportedInstalled = false;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [&](const DebridProgress& p) {
            last = p;
            reportedInstalled |= p.status == DownloadStatus::Installed;
        }, debridId,
        error);
    assert(result == ((failLast || corruptLast) ? DebridRunResult::Failed
                                              : DebridRunResult::Finished));
    assert(fetched.size() >= 2);
    assert(fetched[0].find("base") != std::string::npos);
    assert(fetched[1].find("update") != std::string::npos);
    if (failLast || corruptLast) {
        assert(!reportedInstalled);
        assert(last.packagesInstalled == 1);
        assert(!error.empty());
        // Retrying the same task preserves the committed base; only the
        // failed patch/DLC is transferred and committed.
        failLast = false;
        corruptLast = false;
        fetched.clear();
        spec.packagesInstalled = 1;
        result = transfer.run(spec, [] { return false; },
            [&](const DebridProgress& p) { last = p; }, debridId, error);
        assert(result == DebridRunResult::Finished);
        assert(last.packagesInstalled == 2);
        assert(fetched.size() == 1);
        assert(fetched[0].find("update") != std::string::npos);
    }
}

void testStreamInstallDownloadsSelectedExtras() {
    const std::string root = "/tmp/pipensx-torbox-stream-extra-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(4096);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 7) ^ (i >> 3));
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    const std::string package(nsp.begin(), nsp.end());
    const std::string extra(512, 'Z');

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/game.nsp", package.size(),
                                    "Example/rus.zip", extra.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/nsp\"}"},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/zip\"}"},
    };
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string&) {
        const std::string& body =
            url.find("zip") != std::string::npos ? extra : package;
        uint64_t end = endExclusive == 0
            ? body.size()
            : std::min<uint64_t>(endExclusive, body.size());
        if (offset >= end)
            return false;
        return sink(reinterpret_cast<const uint8_t*>(body.data() + offset),
                    static_cast<size_t>(end - offset));
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    spec.fileSelection = {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Download),
    };

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
    struct stat st {};
    assert(stat((data + "/rus.zip").c_str(), &st) == 0);
    assert(static_cast<uint64_t>(st.st_size) == extra.size());
}

void testStreamInstallSkipsUnselectedExtras() {
    const std::string root = "/tmp/pipensx-torbox-stream-skip-extra-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(1024, 0x33);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    const std::string package(nsp.begin(), nsp.end());
    const std::string extra(64, 'Y');

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/game.nsp", package.size(),
                                    "Example/readme.txt", extra.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/nsp\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(package));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    spec.fileSelection = {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    };

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.status == DownloadStatus::Installed);
    struct stat st {};
    assert(stat((data + "/readme.txt").c_str(), &st) != 0);
}

void testStreamInstallSelectionPathsDownloadsExtraOutOfOrder() {
    const std::string root = "/tmp/pipensx-torbox-stream-extra-order-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(2048, 0x11);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    const std::string package(nsp.begin(), nsp.end());
    const std::string extra(256, 'R');

    // Provider lists the zip first; the torrent mask is still [Install, Download].
    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/rus.zip", extra.size(),
                                    "Example/game.nsp", package.size())},
        {"file_id=7", "{\"success\":true,\"data\":\"https://x/zip\"}"},
        {"file_id=8", "{\"success\":true,\"data\":\"https://x/nsp\"}"},
    };
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string&) {
        const std::string& body =
            url.find("zip") != std::string::npos ? extra : package;
        uint64_t end = endExclusive == 0
            ? body.size()
            : std::min<uint64_t>(endExclusive, body.size());
        if (offset >= end)
            return false;
        return sink(reinterpret_cast<const uint8_t*>(body.data() + offset),
                    static_cast<size_t>(end - offset));
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    spec.fileSelection = {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Download),
    };
    spec.selectionPaths = {
        {"game.nsp", package.size()},
        {"rus.zip", extra.size()},
    };

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
    struct stat st {};
    assert(stat((data + "/rus.zip").c_str(), &st) == 0);
    assert(static_cast<uint64_t>(st.st_size) == extra.size());
}

void testStreamInstallExtraFailureAfterPackagesStaysInstalled() {
    const std::string root = "/tmp/pipensx-torbox-stream-extra-fail-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::vector<uint8_t> nca(1024, 0x22);
    std::vector<uint8_t> nsp =
        makePfs0({{"00112233445566778899aabbccddeeff.nca", nca}});
    const std::string package(nsp.begin(), nsp.end());
    const std::string extra(64, 'F');

    std::vector<std::pair<std::string, std::string>> script = {
        {"mylist", infoReadyJsonTwo("Example/game.nsp", package.size(),
                                    "Example/rus.zip", extra.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/nsp\"}"},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/zip\"}"},
    };
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset,
                               uint64_t endExclusive,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string& error) {
        if (url.find("zip") != std::string::npos) {
            error = "extra fetch failed";
            return false;
        }
        uint64_t end = endExclusive == 0
            ? package.size()
            : std::min<uint64_t>(endExclusive, package.size());
        if (offset >= end)
            return false;
        return sink(reinterpret_cast<const uint8_t*>(package.data() + offset),
                    static_cast<size_t>(end - offset));
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, fetcher);

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "42";
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;
    spec.fileSelection = {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Download),
    };

    std::string debridId;
    std::string error;
    DebridProgress last;
    DebridRunResult result = transfer.run(
        spec, [] { return false; },
        [&last](const DebridProgress& p) { last = p; }, debridId, error);
    assert(result == DebridRunResult::Finished);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
}

void testFilesResolvedStripsLeadingSlash() {
    const std::string root = "/tmp/pipensx-torbox-slash-path-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    const std::string data = root + "/data";
    mkdir(data.c_str(), 0755);

    std::string content(1000, 'p');
    std::vector<std::pair<std::string, std::string>> script = {
        {"createtorrent", "{\"success\":true,\"data\":{\"torrent_id\":42}}"},
        {"mylist", infoReadyJson("/Example/file.bin", content.size())},
        {"requestdl", "{\"success\":true,\"data\":\"https://x/dl\"}"},
    };
    TorboxProvider provider("k", scriptedTransport(&script));
    DebridTransfer transfer(provider, memoryFetcher(content));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.magnet = "magnet:?xt=urn:btih:" + spec.taskId;
    spec.dataPath = data;
    spec.workingRoot = root;
    spec.mode = TransferMode::DownloadOnly;
    std::vector<DebridTaskSpec::ResolvedFile> resolvedFiles;
    spec.filesResolved = [&resolvedFiles](
                             const std::vector<DebridTaskSpec::ResolvedFile>&
                                 files) { resolvedFiles = files; };

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(resolvedFiles.size() == 1);
    assert(resolvedFiles[0].path == "Example/file.bin");
    assert(resolvedFiles[0].localPath == "Example/file.bin");
    assert(resolvedFiles[0].path.front() != '/');
}

void testBuildRichMagnet() {
    std::string m = buildRichMagnet(
        "0123456789abcdef0123456789abcdef01234567",
        "Cool Game",
        {"http://tracker.example/announce", "udp://t2.example:80"});
    assert(m.find("magnet:?xt=urn:btih:"
                  "0123456789abcdef0123456789abcdef01234567") == 0);
    assert(m.find("&dn=Cool%20Game") != std::string::npos);
    assert(m.find("&tr=http%3A%2F%2Ftracker.example%2Fannounce") !=
           std::string::npos);
    assert(m.find("&tr=udp%3A%2F%2Ft2.example%3A80") != std::string::npos);
    std::string bare = buildRichMagnet("abcd", "", {});
    assert(bare == "magnet:?xt=urn:btih:abcd");
    std::puts("buildRichMagnet ok");
}

} // namespace

int main() {
    testFourPackagesResumeAfterRepeatedInterruptions();
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    testHttpConnectionReuse(false);
    testHttpConnectionReuse(true);
    testNspWritesBeforeNetworkPrebuffer();
    testFreshNszStreamsWithoutDiskCache();
    testDownloadOnlyFullRun();
    testDownloadReceivesWhileWriterIsBusy();
    testResumeUsesOnDiskOffset();
    testStopRequestedReturnsStopped();
    testFetchProgressEmittedWhilePolling();
    testStreamInstallCommitsPackage();
    testStreamInstallDownloadsSelectedExtras();
    testStreamInstallSkipsUnselectedExtras();
    testStreamInstallSelectionPathsDownloadsExtraOutOfOrder();
    testStreamInstallExtraFailureAfterPackagesStaysInstalled();
    testStreamInstallStopResumesFromJournal();
    testPartialStreamFailureRetriesWithoutPacerDeadlock();
    testRealdebridInterruptedStreamResumes(false);
    testRealdebridInterruptedStreamResumes(true);
    testRealdebridInterruptedStreamResumes(false, true);
    testRealdebridInterruptedStreamResumes(true, false, true);
    testSolidNszCacheRecovery(CachedNszCase::Reconnect);
    testSolidNszCacheRecovery(CachedNszCase::PauseDownload);
    testSolidNszCacheRecovery(CachedNszCase::PauseInstall);
    testSolidNszCacheRecovery(CachedNszCase::PermanentError);
    testSolidNszCacheRecovery(CachedNszCase::CancelReconnect);
    testSolidNszCacheRecovery(CachedNszCase::Reconnect, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseDownload, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseInstall, true);
    testSolidNszCacheRecovery(CachedNszCase::PermanentError, true);
    testSolidNszCacheRecovery(CachedNszCase::CancelReconnect, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseDownload, false, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseInstall, false, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseDownload, true, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseInstall, true, true);
    testSolidNszCacheRecovery(CachedNszCase::PauseInstall, false, true, 4u << 20);
    testSelectionPathsPicksOneFile();
    testManifestSelectionDistinguishesNestedBasenames();
    testManifestSelectionDistinguishesNestedBasenames(true);
    testSelectionPathsNoMatchReturnsFailed();
    testMagnetFileFallback();
    testOutOfOrderRangesAssembleInOrder();
    testRangeIgnoredFallsBackToSequential();
    testStreamInstallCoalescesTinyChunks();
    testTorboxStreamInstallUsesRangeFetch();
    testRealdebridStreamInstallUsesSequentialFetch();
    testRealdebridStreamInstallUsesSequentialFetch(true);
    testRealdebridDownloadOnlyUsesSequentialFetch();
    testTorrserverDownloadOnlySequentialAndRemoves();
    testTorrserverStreamInstallSequentialAndRemoves();
    testTorrserverStopDoesNotRemove();
    for (auto mode : {TransferMode::StreamInstall, TransferMode::DownloadOnly}) {
        testRealdebridGroupedLinks(mode, GroupedCase::Success);
        testRealdebridGroupedLinks(mode, GroupedCase::Zip64);
        testRealdebridGroupedLinks(mode, GroupedCase::Nsz);
        testRealdebridGroupedLinks(mode, GroupedCase::RarNsz);
        testRealdebridGroupedLinks(mode, GroupedCase::Cached);
        testRealdebridGroupedLinks(mode, GroupedCase::Missing);
        testRealdebridGroupedLinks(mode, GroupedCase::Corrupt);
        testRealdebridGroupedLinks(mode, GroupedCase::Cancelled);
    }
    testRealdebridGroupedLinks(TransferMode::StreamInstall, GroupedCase::Resume);
    testRealdebridRefreshesNestedPackagesOnExistingTorrent();
    testRealdebridRefreshesNestedPackagesOnExistingTorrent(true);
    testRealdebridInstallsBaseBeforeUpdate();
    testRealdebridInstallsBaseBeforeUpdate(true);
    testRealdebridInstallsBaseBeforeUpdate(true, true);
    testRealdebridInstallsBaseBeforeUpdate(false, false, true);
    testFilesResolvedStripsLeadingSlash();
    testBuildRichMagnet();
    std::printf("test_debrid_transfer ok\n");
    curl_global_cleanup();
    return 0;
}
