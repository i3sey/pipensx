#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_settings.hpp"
#include "download_manager.hpp"
#include "magnet_resolver.hpp"

namespace pipensx {

// Background add-jobs for the web companion: a POST returns immediately and
// the magnet resolve (tracker → peers → ut_metadata, potentially minutes)
// runs here on one worker thread, surfaced to browsers as pseudo-tasks in
// the SSE state frame. On success the resolved .torrent is imported into the
// DownloadManager with the same default file actions the catalog batch
// installer uses, then deleted. Terminal jobs linger for a couple minutes so
// a reconnecting browser still sees the outcome.

enum class WebAddJobState {
    Queued,
    Resolving,
    Importing,
    Done,
    Error,
    Cancelled,
};

struct WebAddJob {
    std::string jobId;
    std::string title;
    std::string infoHashHex;  // lowercase; may be empty for odd magnets
    std::string magnetUri;
    std::vector<uint8_t> infoDict;  // catalog fast path; empty = network resolve
    TransferMode requestedMode = TransferMode::StreamInstall;
    StreamSelection selection = StreamSelection::AllFiles;
    WebAddJobState state = WebAddJobState::Queued;
    MagnetProgress progress;
    std::string error;
    std::string taskId;  // filled once imported (== info hash)
    uint64_t finishedAtMs = 0;
};

const char* webAddJobStateName(WebAddJobState state);

class WebAddQueue {
public:
    // Writes the job's .torrent to path. Injected by tests; the default runs
    // MagnetResolver::resolveToFile (with the infoDict preset when present).
    using Resolver = std::function<bool(
        const WebAddJob& job, const std::string& path,
        std::atomic<bool>& cancelled,
        const MagnetResolver::ProgressCallback& progress,
        std::vector<uint8_t>& initialPeers, std::string& error)>;

    explicit WebAddQueue(DownloadManager& manager, Resolver resolver = {});
    ~WebAddQueue();

    WebAddQueue(const WebAddQueue&) = delete;
    WebAddQueue& operator=(const WebAddQueue&) = delete;

    // Returns the job id, or "" with error set (duplicate / already queued).
    std::string enqueue(std::string title, std::string magnetUri,
                        std::string infoHashHex, std::vector<uint8_t> infoDict,
                        TransferMode mode, StreamSelection selection,
                        std::string& error);
    bool cancel(const std::string& jobId);
    // Thread-safe copy; prunes terminal jobs older than ~2 minutes.
    std::vector<WebAddJob> snapshot();
    void shutdown();

private:
    struct Job {
        WebAddJob data;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    void workerMain();
    void runJob(const std::string& jobId);
    Job* findLocked(const std::string& jobId);

    DownloadManager& manager_;
    Resolver resolver_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> jobs_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    bool workerStarted_ = false;
    uint64_t serial_ = 0;
};

}  // namespace pipensx
