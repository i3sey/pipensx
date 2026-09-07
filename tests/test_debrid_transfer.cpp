#include "app/debrid_transfer.hpp"
#include "app/realdebrid_provider.hpp"
#include "app/torbox_provider.hpp"
#include "app/torrserver_provider.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

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

void testStreamInstallLargeFileUsesSequentialFetch() {
    const std::string root = "/tmp/pipensx-torbox-stream-large-test";
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
    assert(rangedCalls == 0);
    assert(sequentialCalls >= 1);
    assert(last.status == DownloadStatus::Installed);
    assert(last.packagesInstalled == 1);
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

void testRealdebridRejectsLinkCountMismatch() {
    const std::string root = "/tmp/pipensx-rd-link-mismatch-test";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);

    RdTransport transport = [](const RdHttpRequest& request,
                               RdHttpResponse& response, std::string&) {
        response.status = 200;
        if (request.url.find("/torrents/info/") != std::string::npos)
            response.body =
                "{\"id\":\"abc123\",\"filename\":\"Minecraft [NSP]\","
                "\"bytes\":200,\"progress\":100,\"status\":\"downloaded\","
                "\"files\":["
                "{\"id\":\"1\",\"path\":\"/Minecraft [0100D71004694800][v10420224].nsp\","
                "\"bytes\":100,\"selected\":1},"
                "{\"id\":\"2\",\"path\":\"/Minecraft [0100D71004694000][v0].nsp\","
                "\"bytes\":100,\"selected\":1}],"
                "\"links\":[\"https://rd.to/dl/only-one\"]}";
        else
            response.body = "{}";
        return true;
    };
    RealdebridProvider provider("k", transport);
    DebridTransfer transfer(provider, memoryFetcher(std::string(16, 'x')));

    DebridTaskSpec spec;
    spec.taskId = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
    spec.debridId = "abc123";
    spec.dataPath = root;
    spec.workingRoot = root;
    spec.mode = TransferMode::StreamInstall;

    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Failed);
    assert(error.find("1 download") != std::string::npos);
    assert(error.find("2 selected") != std::string::npos);
}

void testRealdebridInstallsBaseBeforeUpdate() {
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

    std::vector<std::string> fetched;
    RangeFetcher fetcher = [&](const std::string& url, uint64_t offset,
                               uint64_t,
                               const std::function<bool(const uint8_t*, size_t)>&
                                   sink,
                               const std::function<bool()>&, std::string&) {
        fetched.push_back(url);
        const std::string& body =
            url.find("base") != std::string::npos ? baseContent : updateContent;
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
                "{\"id\":\"1\",\"path\":\"/Minecraft [0100D71004694800][v10420224].nsp\","
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
                     : "Minecraft [0100D71004694800][v10420224].nsp";
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
    DebridRunResult result = transfer.run(
        spec, [] { return false; }, [](const DebridProgress&) {}, debridId,
        error);
    assert(result == DebridRunResult::Finished);
    assert(fetched.size() >= 2);
    assert(fetched[0].find("base") != std::string::npos);
    assert(fetched[1].find("update") != std::string::npos);
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
    testDownloadOnlyFullRun();
    testResumeUsesOnDiskOffset();
    testStopRequestedReturnsStopped();
    testFetchProgressEmittedWhilePolling();
    testStreamInstallCommitsPackage();
    testPartialStreamFailureRetriesWithoutPacerDeadlock();
    testSelectionPathsPicksOneFile();
    testSelectionPathsNoMatchReturnsFailed();
    testMagnetFileFallback();
    testOutOfOrderRangesAssembleInOrder();
    testRangeIgnoredFallsBackToSequential();
    testStreamInstallCoalescesTinyChunks();
    testStreamInstallLargeFileUsesSequentialFetch();
    testRealdebridDownloadOnlyUsesSequentialFetch();
    testTorrserverDownloadOnlySequentialAndRemoves();
    testTorrserverStopDoesNotRemove();
    testRealdebridRejectsLinkCountMismatch();
    testRealdebridInstallsBaseBeforeUpdate();
    testFilesResolvedStripsLeadingSlash();
    testBuildRichMagnet();
    std::printf("test_debrid_transfer ok\n");
    return 0;
}
