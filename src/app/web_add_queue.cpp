#include "web_add_queue.hpp"

#include "install_space.hpp"

extern "C" {
#include "../core/util.h"
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <unistd.h>

namespace pipensx {

namespace {

constexpr uint64_t kTerminalJobTtlMs = 120 * 1000;

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool isTerminal(WebAddJobState s) {
    return s == WebAddJobState::Done || s == WebAddJobState::Error ||
           s == WebAddJobState::Cancelled;
}

}  // namespace

const char* webAddJobStateName(WebAddJobState state) {
    switch (state) {
        case WebAddJobState::Queued: return "queued";
        case WebAddJobState::Resolving: return "resolving";
        case WebAddJobState::Importing: return "importing";
        case WebAddJobState::Done: return "done";
        case WebAddJobState::Error: return "error";
        case WebAddJobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

WebAddQueue::WebAddQueue(DownloadManager& manager, Resolver resolver)
    : manager_(manager), resolver_(std::move(resolver)) {
    if (!resolver_) {
        resolver_ = [](const WebAddJob& job, const std::string& path,
                       std::atomic<bool>& cancelled,
                       const MagnetResolver::ProgressCallback& progress,
                       std::vector<uint8_t>& initialPeers,
                       std::string& error) {
            MagnetResolver resolver;
            return resolver.resolveToFile(
                job.magnetUri, path, cancelled, progress, error, &initialPeers,
                job.infoDict.empty() ? nullptr : &job.infoDict);
        };
    }
}

WebAddQueue::~WebAddQueue() { shutdown(); }

WebAddQueue::Job* WebAddQueue::findLocked(const std::string& jobId) {
    for (Job& job : jobs_)
        if (job.data.jobId == jobId) return &job;
    return nullptr;
}

std::string WebAddQueue::enqueue(std::string title, std::string magnetUri,
                                 std::string infoHashHex,
                                 std::vector<uint8_t> infoDict,
                                 TransferMode mode, StreamSelection selection,
                                 std::string& error) {
    std::string hash = lowerAscii(std::move(infoHashHex));
    if (!hash.empty() && manager_.hasTask(hash)) {
        error = "This torrent is already in the download manager.";
        return "";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hash.empty()) {
        for (const Job& job : jobs_) {
            if (!isTerminal(job.data.state) && job.data.infoHashHex == hash) {
                error = "This torrent is already being added.";
                return "";
            }
        }
    }
    Job job;
    job.data.jobId = "job-" + std::to_string(++serial_);
    job.data.title = title.empty() ? (hash.empty() ? "magnet" : hash)
                                   : std::move(title);
    job.data.infoHashHex = std::move(hash);
    job.data.magnetUri = std::move(magnetUri);
    job.data.infoDict = std::move(infoDict);
    job.data.requestedMode = mode;
    job.data.selection = selection;
    job.cancelled = std::make_shared<std::atomic<bool>>(false);
    std::string jobId = job.data.jobId;
    jobs_.push_back(std::move(job));
    if (!workerStarted_) {
        workerStarted_ = true;
        worker_ = std::thread(&WebAddQueue::workerMain, this);
    }
    cv_.notify_all();
    return jobId;
}

bool WebAddQueue::cancel(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = findLocked(jobId);
    if (!job || isTerminal(job->data.state)) return false;
    job->cancelled->store(true);
    if (job->data.state == WebAddJobState::Queued) {
        job->data.state = WebAddJobState::Cancelled;
        job->data.finishedAtMs = nowMs();
    }
    return true;
}

std::vector<WebAddJob> WebAddQueue::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = nowMs();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [now](const Job& job) {
                                   return isTerminal(job.data.state) &&
                                          now - job.data.finishedAtMs >
                                              kTerminalJobTtlMs;
                               }),
                jobs_.end());
    std::vector<WebAddJob> out;
    out.reserve(jobs_.size());
    for (const Job& job : jobs_) out.push_back(job.data);
    return out;
}

void WebAddQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true);
        for (Job& job : jobs_) job.cancelled->store(true);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void WebAddQueue::workerMain() {
    for (;;) {
        std::string jobId;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                if (stopping_.load()) return true;
                for (const Job& job : jobs_)
                    if (job.data.state == WebAddJobState::Queued) return true;
                return false;
            });
            if (stopping_.load()) return;
            for (Job& job : jobs_) {
                if (job.data.state == WebAddJobState::Queued) {
                    job.data.state = WebAddJobState::Resolving;
                    jobId = job.data.jobId;
                    break;
                }
            }
        }
        if (!jobId.empty()) runJob(jobId);
    }
}

void WebAddQueue::runJob(const std::string& jobId) {
    WebAddJob data;
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = findLocked(jobId);
        if (!job) return;
        data = job->data;
        cancelled = job->cancelled;
    }

    auto fail = [&](const std::string& message, WebAddJobState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = findLocked(jobId);
        if (!job) return;
        job->data.state = state;
        job->data.error = message;
        job->data.finishedAtMs = nowMs();
    };

    const std::string path =
        manager_.rootPath() + "/_web_add_" +
        (data.infoHashHex.empty() ? "unknown" : data.infoHashHex) + "_" +
        jobId.substr(4) + ".torrent";

    auto progressCb = [this, &jobId](const MagnetProgress& progress) {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = findLocked(jobId);
        if (job) job->data.progress = progress;
    };

    // Resolving a magnet talks to the DHT and to peers, so it has to sit
    // behind the same gate as the transfer itself — otherwise a phone could
    // put the console on the torrent network while the user has torrenting
    // switched off. The companion has no debrid path of its own yet.
    if (!manager_.torrentingEnabled()) {
        fail("Torrenting is disabled. Enable it in Settings, or add this "
             "release from the console in debrid mode.",
             WebAddJobState::Error);
        return;
    }

    std::string error;
    std::vector<uint8_t> initialPeers;
    if (!resolver_(data, path, *cancelled, progressCb, initialPeers, error)) {
        ::unlink(path.c_str());
        fail(cancelled->load()
                 ? ""
                 : (error.empty() ? "Unable to resolve torrent metadata."
                                  : error),
             cancelled->load() ? WebAddJobState::Cancelled
                               : WebAddJobState::Error);
        return;
    }
    if (cancelled->load()) {
        ::unlink(path.c_str());
        fail("", WebAddJobState::Cancelled);
        return;
    }

    TorrentPreview preview;
    if (!DownloadManager::previewTorrent(path, preview, error)) {
        ::unlink(path.c_str());
        fail(error, WebAddJobState::Error);
        return;
    }
    if (!data.infoHashHex.empty() &&
        data.infoHashHex != lowerAscii(preview.infoHash)) {
        ::unlink(path.c_str());
        fail("Resolved torrent does not match the requested hash.",
             WebAddJobState::Error);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = findLocked(jobId);
        if (job) {
            job->data.state = WebAddJobState::Importing;
            job->data.title = preview.name.empty() ? job->data.title
                                                   : preview.name;
        }
    }

    // Same default-actions logic as the catalog batch installer
    // (catalog_batch_installer.cpp): stream install with the settings-driven
    // selection, falling back to a plain download when nothing is installable.
    TransferMode mode = data.requestedMode;
    std::vector<uint8_t> mask;
    if (mode == TransferMode::StreamInstall) {
        mask = defaultInstallSelection(preview, mode, data.selection);
        InstallSpaceEstimate space = estimateInstallSpace(preview, mask, mode);
        if (space.packageFiles == 0) mode = TransferMode::DownloadOnly;
    }

    std::string taskId;
    bool ok = manager_.importTorrent(path, mode, mask, taskId, error,
                                     initialPeers);
    ::unlink(path.c_str());
    if (!ok) {
        fail(error.empty() ? "Import failed." : error, WebAddJobState::Error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = findLocked(jobId);
        if (job) {
            job->data.state = WebAddJobState::Done;
            job->data.taskId = taskId;
            job->data.finishedAtMs = nowMs();
        }
    }
    log_msg("[web] job %s imported as %s\n", jobId.c_str(), taskId.c_str());
}

}  // namespace pipensx
