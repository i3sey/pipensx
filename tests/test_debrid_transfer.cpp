#include "app/debrid_transfer.hpp"
#include "app/torbox_provider.hpp"

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
    bool sawFetch = false;
    std::string debridId;
    std::string error;
    DebridRunResult result = transfer.run(
        spec, [&sawFetch] { return sawFetch; },
        [&sawFetch](const DebridProgress& p) {
            if (p.status == DownloadStatus::Fetching)
                sawFetch = true;
        },
        debridId, error);
    assert(result == DebridRunResult::Stopped);
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
    testStreamInstallCommitsPackage();
    testPartialStreamFailureRetriesWithoutPacerDeadlock();
    testSelectionPathsPicksOneFile();
    testSelectionPathsNoMatchReturnsFailed();
    testMagnetFileFallback();
    testOutOfOrderRangesAssembleInOrder();
    testRangeIgnoredFallsBackToSequential();
    testStreamInstallCoalescesTinyChunks();
    testBuildRichMagnet();
    std::printf("test_debrid_transfer ok\n");
    return 0;
}
