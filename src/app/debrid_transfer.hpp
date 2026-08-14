#pragma once

#include "download_manager.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pipensx {

std::string buildRichMagnet(const std::string& infoHashHex,
                            const std::string& name,
                            const std::vector<std::string>& trackers);

class DebridProvider;

enum class DebridRunResult { Finished, Stopped, Failed };

struct DebridProgress {
    DebridProgress();
    DownloadStatus status;
    uint64_t completedBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t speedBytesPerSecond = 0;
    double fetchProgress = 0.0;
    uint32_t packagesInstalled = 0;
    std::string currentPackage;
    uint64_t installedBytes = 0;
    uint64_t installTotalBytes = 0;
};

struct DebridTaskSpec {
    DebridTaskSpec();
    std::string taskId;
    std::string debridId;
    std::string torrentPath;
    std::string magnet;
    std::string dataPath;
    std::string workingRoot;
    install::InstallStorageTarget installTarget;
    TransferMode mode;
    std::vector<uint8_t> fileSelection;
    std::vector<std::pair<std::string, uint64_t>> selectionPaths;
    uint32_t packagesInstalled = 0;
    struct ResolvedFile {
        std::string path;
        std::string localPath;
        uint64_t bytes = 0;
        uint8_t action = 0;
    };
    std::function<void(const std::vector<ResolvedFile>&)> filesResolved;
};

// Fetcher reported HTTP 200 for a bounded Range request: the server ignored
// Range. The transfer falls back to one sequential stream from the committed
// offset. Never treat a 200 body as a mid-file slice.
inline constexpr const char* kDebridRangeNotSupported = "range-not-supported";

// endExclusive == 0 means "to EOF" (CURLOPT_RESUME_FROM). A non-zero end is
// exclusive and must be served as HTTP 206.
using RangeFetcher = std::function<bool(
    const std::string& url, uint64_t offset, uint64_t endExclusive,
    const std::function<bool(const uint8_t*, size_t)>& sink,
    const std::function<bool()>& cancelled,
    std::string& error)>;

class DebridTransfer {
public:
    explicit DebridTransfer(DebridProvider& provider,
                            RangeFetcher fetcher = {});

    DebridRunResult run(const DebridTaskSpec& spec,
                        const std::function<bool()>& shouldStop,
                        const std::function<void(const DebridProgress&)>&
                            progress,
                        std::string& debridIdOut,
                        std::string& error,
                        uint32_t resolveWindowMsOverride = UINT32_MAX);

private:
    DebridProvider& provider_;
    RangeFetcher fetcher_;
};

} // namespace pipensx
