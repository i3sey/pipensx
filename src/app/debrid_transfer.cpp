#include "debrid_transfer.hpp"
#include "curl_https.hpp"
#include "download_manager.hpp"
#include "debrid_provider.hpp"
#include "install_pacer.hpp"
#include "nx_file_types.hpp"
#include "stream_ram_budget.hpp"
#include "../install/install_backend.hpp"
#include "../install/package_stream.hpp"

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
}

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace pipensx {

DebridProgress::DebridProgress() : status(DownloadStatus::Fetching) {}

DebridTaskSpec::DebridTaskSpec()
    : installTarget(install::InstallStorageTarget::SdCard),
      mode(TransferMode::DownloadOnly) {}

std::string buildRichMagnet(const std::string& infoHashHex,
                            const std::string& name,
                            const std::vector<std::string>& trackers) {
    auto encode = [](const std::string& in) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(in.size() * 3);
        for (unsigned char c : in) {
            bool unreserved = (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') ||
                              c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved) {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            }
        }
        return out;
    };
    std::string magnet = "magnet:?xt=urn:btih:" + infoHashHex;
    if (!name.empty())
        magnet += "&dn=" + encode(name);
    for (const std::string& tracker : trackers)
        if (!tracker.empty())
            magnet += "&tr=" + encode(tracker);
    return magnet;
}

namespace {

using Clock = std::chrono::steady_clock;

enum class Step { Ok, Stopped, Failed };

// B4: a bare stream error ("not a PFS0 NSP/NSZ") tells the user nothing
// when the transfer holds several files — name the offending package and
// its size so the broken download is identifiable without guessing.
std::string describeStreamError(const DebridFile& file,
                                const std::string& displayName,
                                const std::string& streamError) {
    const std::string& name = displayName.empty() ? file.path : displayName;
    return "Package '" + name + "' (" +
           std::to_string(
               static_cast<unsigned long long>(file.bytes)) +
           " bytes): " + streamError;
}

class DebridStreamQueue {
public:
    DebridStreamQueue(size_t maximumBytes, InstallPacer& pacer,
                      const std::function<bool()>& cancelled)
        : maximumBytes_(maximumBytes), pacer_(pacer), cancelled_(cancelled) {}

    bool push(const uint8_t* data, size_t size) {
        pacer_.observeSource(size, now_ms());
        std::unique_lock<std::mutex> lock(mutex_);
        bool measurementPaused = false;
        while (!stopped_ && !cancelled_() && !queue_.empty() &&
               bufferedBytes_ + size > maximumBytes_) {
            if (!measurementPaused) {
                pacer_.setSourceMeasurementEnabled(false, now_ms());
                measurementPaused = true;
            }
            ready_.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (measurementPaused)
            pacer_.setSourceMeasurementEnabled(true, now_ms());
        if (stopped_ || cancelled_())
            return false;
        queue_.emplace_back(data, data + size);
        bufferedBytes_ += size;
        pacer_.setBufferedBytes(bufferedBytes_);
        ready_.notify_all();
        return true;
    }

    bool pop(std::vector<uint8_t>& chunk) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !finished_ && !stopped_ && !cancelled_())
            ready_.wait_for(lock, std::chrono::milliseconds(50));
        if (queue_.empty())
            return false;
        chunk = std::move(queue_.front());
        queue_.pop_front();
        bufferedBytes_ -= chunk.size();
        pacer_.setBufferedBytes(bufferedBytes_);
        ready_.notify_all();
        return true;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        // Success or failure, no more bytes can arrive from this attempt.
        // Let the consumer drain what was already delivered and then report
        // the fetch result instead of waiting forever on an empty queue.
        pacer_.setSourceComplete(true);
        ready_.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        ready_.notify_all();
    }

private:
    size_t maximumBytes_;
    InstallPacer& pacer_;
    const std::function<bool()>& cancelled_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t bufferedBytes_ = 0;
    bool finished_ = false;
    bool stopped_ = false;
};

bool makeDirectories(const std::string& path) {
    if (path.empty())
        return false;
    char buffer[1024];
    if (path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* p = buffer + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

std::string parentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return path.substr(0, slash);
}

bool statSize(const std::string& path, uint64_t& size) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return false;
    size = static_cast<uint64_t>(st.st_size);
    return true;
}

std::string sanitizeRelative(const std::string& name,
                             const std::string& torrentName, bool& ok) {
    ok = true;
    std::string rel = name;
    if (!torrentName.empty()) {
        std::string prefix = torrentName + "/";
        if (rel.size() >= prefix.size() &&
            rel.compare(0, prefix.size(), prefix) == 0)
            rel = rel.substr(prefix.size());
    }
    while (!rel.empty() && rel.front() == '/')
        rel.erase(rel.begin());
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        std::string comp = slash == std::string::npos
                               ? rel.substr(start)
                               : rel.substr(start, slash - start);
        if (comp == "..") {
            ok = false;
            return {};
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return rel;
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool selected(const DebridTaskSpec& spec, size_t index,
              const DebridFile& file) {
    if (!spec.selectionPaths.empty()) {
        std::string base = baseName(file.path);
        for (const auto& entry : spec.selectionPaths)
            if (entry.first == base && entry.second == file.bytes)
                return true;
        return false;
    }
    return spec.fileSelection.empty() ||
           (index < spec.fileSelection.size() && spec.fileSelection[index]);
}

bool installs(const DebridTaskSpec& spec, size_t index,
              const DebridFile& file) {
    if (spec.mode != TransferMode::StreamInstall || !isPackageName(file.path))
        return false;
    if (spec.fileSelection.empty() || !spec.selectionPaths.empty())
        return true;
    for (uint8_t action : spec.fileSelection)
        if (action == static_cast<uint8_t>(FileAction::Install))
            return index < spec.fileSelection.size() &&
                   spec.fileSelection[index] ==
                       static_cast<uint8_t>(FileAction::Install);
    // Older persisted debrid tasks used 1 as their selected-package marker.
    return true;
}

bool sleepSlices(const std::function<bool()>& shouldStop, int totalMs) {
    int slices = totalMs / 250;
    for (int i = 0; i < slices; ++i) {
        if (shouldStop())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;
}

constexpr size_t kInstallBatchBytes = 1u << 20;
constexpr unsigned kRangeWorkers = 4;
constexpr uint64_t kRangeSegmentBytes = 4ull << 20;

struct CurlSink {
    const std::function<bool(const uint8_t*, size_t)>* sink;
    const std::function<bool()>* cancelled;
    CURL* curl = nullptr;
    bool ranged = false;
    bool aborted = false;
    // Which side gave up matters: the sink refusing bytes is an install-side
    // failure, and blaming the network for it sends everyone hunting in the
    // wrong place.
    bool sinkRefused = false;
    bool rangeIgnored = false;
    uint64_t received = 0;
};

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    CurlSink* c = static_cast<CurlSink*>(userdata);
    size_t total = size * nmemb;
    try {
        if (c->ranged && c->curl) {
            long status = 0;
            curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
            if (status == 200) {
                c->rangeIgnored = true;
                c->aborted = true;
                return 0;
            }
        }
        if ((*c->cancelled)()) {
            c->aborted = true;
            return 0;
        }
        if (!(*c->sink)(reinterpret_cast<const uint8_t*>(ptr), total)) {
            c->aborted = true;
            c->sinkRefused = true;
            return 0;
        }
        c->received += total;
        return total;
    } catch (...) {
        // libcurl is C: an exception through the write callback is
        // std::terminate with no live exception, which is how a TorrServer
        // stream-install presented after GET /echo already succeeded.
        c->aborted = true;
        c->sinkRefused = true;
        return 0;
    }
}

RangeFetcher curlRangeFetcher() {
    return [](const std::string& url, uint64_t offset, uint64_t endExclusive,
              const std::function<bool(const uint8_t*, size_t)>& sink,
              const std::function<bool()>& cancelled,
              std::string& error) -> bool {
        const bool plain = url.compare(0, 7, "http://") == 0;
        if (!plain && url.compare(0, 8, "https://") != 0) {
            error = "Unsupported download URL scheme.";
            return false;
        }
        // Whether plaintext is acceptable at all is the provider's call and
        // was settled before we got here; this only has to speak both.
        const size_t hostStart = plain ? 7 : 8;
        size_t hostEnd = url.find('/', hostStart);
        const std::string host = url.substr(hostStart, hostEnd - hostStart);
        log_msg("[debrid] downloading from %s over %s\n", host.c_str(),
                plain ? "HTTP" : "HTTPS");
        CURL* curl = curl_easy_init();
        if (!curl) {
            error = "Unable to initialize network transfer.";
            return false;
        }
        CurlSink state;
        state.sink = &sink;
        state.cancelled = &cancelled;
        state.curl = curl;
        state.ranged = endExclusive != 0;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (state.ranged) {
            char range[80];
            std::snprintf(range, sizeof(range), "%llu-%llu",
                          (unsigned long long)offset,
                          (unsigned long long)(endExclusive - 1));
            curl_easy_setopt(curl, CURLOPT_RANGE, range);
        } else {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(offset));
        }
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curlPinScheme(curl, url);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curlApplyTrustedSsl(curl);
        curlTuneDownloadSocket(curl);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        CURLcode rc = curl_easy_perform(curl);
        long httpStatus = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        curl_easy_cleanup(curl);
        if (state.rangeIgnored || (httpStatus == 200 && state.ranged)) {
            error = kDebridRangeNotSupported;
            return false;
        }
        if (rc != CURLE_OK || state.aborted)
            log_msg("[debrid] fetch ended rc=%d (%s) http=%ld got=%llu%s\n",
                    static_cast<int>(rc), curl_easy_strerror(rc), httpStatus,
                    static_cast<unsigned long long>(state.received),
                    state.sinkRefused ? " sink-refused" : "");
        if (state.aborted) {
            if (cancelled())
                return false;
            error = state.sinkRefused
                ? "Unable to write the downloaded data."
                : "The download stream was interrupted.";
            return false;
        }
        if (state.ranged && httpStatus != 206) {
            error = kDebridRangeNotSupported;
            return false;
        }
        if (rc != CURLE_OK) {
            // The provider hands out a specific CDN node, and that node can
            // be unreachable while its API is fine. "Timeout was reached" on
            // its own sends people to check their account; naming the host
            // and the fact that nothing arrived points at the network.
            if (state.received == 0 && httpStatus == 0)
                error = "Could not reach " + host +
                        " — the debrid service is up but its download server "
                        "is not reachable from this console (" +
                        curl_easy_strerror(rc) + ").";
            else
                error = std::string(curl_easy_strerror(rc)) + " from " + host +
                        " (HTTP " + std::to_string(httpStatus) + ", " +
                        std::to_string(state.received) + " bytes)";
            return false;
        }
        return true;
    };
}

// A hosted service hands out links that carry the account's credentials, so a
// plaintext one is a leak and a bug. A LAN server the user pointed us at is
// plain HTTP by nature — the provider says which it is.
bool linkAllowed(const DebridProvider& provider, const std::string& url) {
    return url.compare(0, 8, "https://") == 0 ||
           (provider.allowsPlaintextLinks() &&
            url.compare(0, 7, "http://") == 0);
}

struct RunContext {
    RunContext(DebridProvider& p, const RangeFetcher& f,
               const DebridTaskSpec& s, const std::function<bool()>* stop,
               const std::function<void(const DebridProgress&)>* prog)
        : provider(p), fetcher(f), spec(s), shouldStop(stop), progress(prog) {
    }

    DebridProvider& provider;
    const RangeFetcher& fetcher;
    const DebridTaskSpec& spec;
    const std::function<bool()>* shouldStop;
    const std::function<void(const DebridProgress&)>* progress;

    std::string debridId;
    uint64_t totalBytes = 0;
    uint64_t completedSoFar = 0;
    uint32_t packagesInstalled = 0;
    bool magnetCreated = false;
    bool fallbackUsed = false;
    uint32_t resolveWindowMs = 60000;
    std::string torrentName;
    std::vector<DebridFile> files;
    std::vector<std::string> selectedLinks;
    bool filesSelected = false;
    std::unique_ptr<install::InstallBackend> backend;
    std::string error;

    // Compressed bytes pulled from the debrid link for the package currently
    // streaming. Added to completedSoFar so the download bar advances mid-
    // package instead of only jumping when a package finishes.
    std::atomic<uint64_t> packageDownloadedBytes {0};

    void emit(const DebridProgress& p) const {
        if (*progress)
            (*progress)(p);
    }
    bool stop() const { return (*shouldStop)(); }
};

Step ensureCreated(RunContext& ctx) {
    if (!ctx.spec.debridId.empty()) {
        ctx.debridId = ctx.spec.debridId;
        log_msg("[debrid] task %s using pre-created id=%s\n",
                ctx.spec.taskId.c_str(), ctx.debridId.c_str());
        return Step::Ok;
    }
    bool ok;
    if (!ctx.spec.magnet.empty()) {
        ctx.magnetCreated = true;
        log_msg("[debrid] task %s creating via magnet\n",
                ctx.spec.taskId.c_str());
        ok = ctx.provider.createFromMagnet(ctx.spec.magnet, ctx.debridId,
                                           ctx.error);
    } else {
        ctx.magnetCreated = false;
        log_msg("[debrid] task %s creating via file upload\n",
                ctx.spec.taskId.c_str());
        ok = ctx.provider.createFromFile(ctx.spec.torrentPath, ctx.debridId,
                                         ctx.error);
    }
    return ok ? Step::Ok : Step::Failed;
}

Step fallbackToFile(RunContext& ctx) {
    log_msg("[debrid] task %s magnet stuck after %ums, falling back to file upload\n",
            ctx.spec.taskId.c_str(), ctx.resolveWindowMs);
    std::string ignored;
    ctx.provider.remove(ctx.debridId, ignored);
    ctx.debridId.clear();
    ctx.fallbackUsed = true;
    ctx.magnetCreated = false;
    if (!ctx.provider.createFromFile(ctx.spec.torrentPath, ctx.debridId,
                                     ctx.error))
        return Step::Failed;
    return Step::Ok;
}

Step pollUntilReady(RunContext& ctx) {
    int failures = 0;
    bool canFallback =
        ctx.magnetCreated && !ctx.fallbackUsed && !ctx.spec.torrentPath.empty();
    Clock::time_point windowStart = Clock::now();
    while (true) {
        if (ctx.stop())
            return Step::Stopped;
        DebridInfo info;
        std::string err;
        if (!ctx.provider.fetchInfo(ctx.debridId, info, err)) {
            if (++failures >= 3) {
                ctx.error = err.empty() ? "Unable to reach debrid service."
                                        : err;
                return Step::Failed;
            }
            if (!sleepSlices(*ctx.shouldStop, 5000))
                return Step::Stopped;
            continue;
        }
        failures = 0;
        ctx.torrentName = info.name;
        ctx.files = info.files;

        log_msg("[debrid] poll id=%s state=%s progress=%.0f%% files=%zu "
                "links=%zu phase=%d\n",
                ctx.debridId.c_str(), info.rawState.c_str(),
                info.progress * 100.0, info.files.size(), info.links.size(),
                static_cast<int>(info.phase));

        DebridProgress p;
        p.status = DownloadStatus::Fetching;
        p.fetchProgress = info.progress;
        p.totalBytes = info.bytes;
        p.packagesInstalled = ctx.packagesInstalled;
        ctx.emit(p);

        if (info.phase == DebridInfo::Phase::Failed) {
            ctx.error = "Debrid transfer failed on the server ("
                        + info.rawState + ").";
            return Step::Failed;
        }

        if (!ctx.filesSelected && !info.links.empty())
            ctx.selectedLinks = info.links;
        // Select files only while the service is actually waiting for a
        // selection. Resuming a torrent that is already downloading or
        // downloaded must not re-select: Real-Debrid restarts the server-side
        // torrent on POST /torrents/selectFiles, wasting a full re-download.
        if (!ctx.filesSelected &&
            info.phase == DebridInfo::Phase::AwaitingSelection &&
            !info.files.empty()) {
            std::vector<std::string> selectedIds;
            for (size_t i = 0; i < info.files.size(); ++i) {
                if (selected(ctx.spec, i, info.files[i]))
                    selectedIds.push_back(info.files[i].id);
            }
            if (selectedIds.empty()) {
                ctx.error = "No debrid files matched the selected files.";
                return Step::Failed;
            }
            if (!ctx.provider.selectFiles(ctx.debridId, selectedIds,
                                          ctx.error))
                return Step::Failed;
            ctx.filesSelected = true;
            ctx.selectedLinks = info.links;
        }

        if (info.phase == DebridInfo::Phase::Ready && !info.files.empty()) {
            if (ctx.filesSelected)
                ctx.selectedLinks = info.links;
            if (ctx.spec.filesResolved) {
                std::vector<DebridTaskSpec::ResolvedFile> resolved;
                resolved.reserve(info.files.size());
                for (size_t i = 0; i < info.files.size(); ++i) {
                    bool pathOk = true;
                    DebridTaskSpec::ResolvedFile file;
                    file.path = sanitizeRelative(info.files[i].path, info.name,
                                                 pathOk);
                    if (!pathOk || file.path.empty())
                        file.path = baseName(info.files[i].path);
                    while (!file.path.empty() && file.path.front() == '/')
                        file.path.erase(file.path.begin());
                    file.localPath = file.path;
                    file.bytes = info.files[i].bytes;
                    file.action = !selected(ctx.spec, i, info.files[i])
                        ? static_cast<uint8_t>(FileAction::Skip)
                        : installs(ctx.spec, i, info.files[i])
                            ? static_cast<uint8_t>(FileAction::Install)
                            : static_cast<uint8_t>(FileAction::Download);
                    resolved.push_back(std::move(file));
                }
                ctx.spec.filesResolved(resolved);
            }
            return Step::Ok;
        }

        if (canFallback) {
            uint64_t elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - windowStart).count());
            if (elapsedMs >= ctx.resolveWindowMs) {
                Step step = fallbackToFile(ctx);
                if (step != Step::Ok)
                    return step;
                canFallback = false;
                windowStart = Clock::now();
                continue;
            }
        }
        if (!sleepSlices(*ctx.shouldStop, 5000))
            return Step::Stopped;
    }
}

bool shouldEmitProgress(Clock::time_point& lastEmit) {
    Clock::time_point now = Clock::now();
    if (lastEmit == Clock::time_point::min() ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit)
                .count() >= 250) {
        lastEmit = now;
        return true;
    }
    return false;
}

// One TCP stream from offset to EOF. Stream-install always uses this, and
// Real-Debrid download-only does too: parallel Range workers on RD CDN URLs
// corrupt the byte stream (RD returns 206 but pipensx sees non-PFS0 payloads).
bool fetchSequential(const RangeFetcher& fetcher, const std::string& url,
                     uint64_t offset,
                     const std::function<bool(const uint8_t*, size_t)>& sink,
                     const std::function<bool()>& cancelled,
                     std::string& error) {
    return fetcher(url, offset, 0, sink, cancelled, error);
}

// Pull [offset, totalBytes) through the injected fetcher. Several Range
// workers fill a reorder map; only the next sequential offset is pushed to
// sink. A 206-less CDN falls back to one stream from the committed offset.
bool fetchOrdered(const RangeFetcher& fetcher, const std::string& url,
                  uint64_t offset, uint64_t totalBytes,
                  size_t maxInFlightBytes,
                  const std::function<bool(const uint8_t*, size_t)>& sink,
                  const std::function<bool()>& cancelled, std::string& error) {
    if (offset > totalBytes) {
        error = "range past end";
        return false;
    }
    const uint64_t remaining = totalBytes - offset;
    size_t maxInFlight = kRangeSegmentBytes
        ? std::max<size_t>(1, maxInFlightBytes / kRangeSegmentBytes)
        : 1;
    if (maxInFlight > kRangeWorkers)
        maxInFlight = kRangeWorkers;
    if (remaining < 2 * kRangeSegmentBytes || maxInFlight < 2)
        return fetcher(url, offset, 0, sink, cancelled, error);

    std::mutex mu;
    std::condition_variable cv;
    std::map<uint64_t, std::vector<uint8_t>> ready;
    uint64_t nextToAssign = offset;
    uint64_t nextToEmit = offset;
    unsigned inFlight = 0;
    bool failed = false;
    bool rangeIgnored = false;
    bool stopWorkers = false;
    std::string workerError;

    auto takeJob = [&](uint64_t& start, uint64_t& end) -> bool {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] {
            if (stopWorkers || cancelled())
                return true;
            if (nextToAssign >= totalBytes)
                return true;
            return (inFlight + ready.size()) < maxInFlight;
        });
        if (stopWorkers || cancelled() || nextToAssign >= totalBytes)
            return false;
        start = nextToAssign;
        end = std::min(totalBytes, start + kRangeSegmentBytes);
        nextToAssign = end;
        ++inFlight;
        return true;
    };

    std::vector<std::thread> workers;
    workers.reserve(maxInFlight);
    for (size_t i = 0; i < maxInFlight; ++i) {
        workers.emplace_back([&] {
            try {
                uint64_t start = 0;
                uint64_t end = 0;
                while (takeJob(start, end)) {
                    std::vector<uint8_t> buf;
                    buf.reserve(static_cast<size_t>(end - start));
                    auto collect = [&](const uint8_t* data, size_t n) -> bool {
                        buf.insert(buf.end(), data, data + n);
                        return !cancelled();
                    };
                    std::string err;
                    auto workerCancel = [&] {
                        std::lock_guard<std::mutex> lock(mu);
                        return stopWorkers || cancelled();
                    };
                    bool ok = fetcher(url, start, end, collect, workerCancel,
                                      err);
                    if (ok && buf.size() != static_cast<size_t>(end - start)) {
                        ok = false;
                        err = "short range";
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        --inFlight;
                        if (!ok) {
                            if (err == kDebridRangeNotSupported)
                                rangeIgnored = true;
                            else {
                                failed = true;
                                if (workerError.empty())
                                    workerError = err;
                            }
                            stopWorkers = true;
                        } else {
                            ready.emplace(start, std::move(buf));
                        }
                    }
                    cv.notify_all();
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mu);
                failed = true;
                stopWorkers = true;
                if (workerError.empty())
                    workerError = e.what();
                cv.notify_all();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mu);
                failed = true;
                stopWorkers = true;
                if (workerError.empty())
                    workerError = "Download worker failed.";
                cv.notify_all();
            }
        });
    }

    bool sinkOk = true;
    while (nextToEmit < totalBytes) {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&] {
                return cancelled() || failed || rangeIgnored || stopWorkers ||
                       ready.count(nextToEmit);
            });
            if (cancelled() || failed || rangeIgnored ||
                (stopWorkers && !ready.count(nextToEmit)))
                break;
            auto it = ready.find(nextToEmit);
            chunk = std::move(it->second);
            ready.erase(it);
        }
        if (!sink(chunk.data(), chunk.size())) {
            sinkOk = false;
            std::lock_guard<std::mutex> lock(mu);
            failed = true;
            stopWorkers = true;
            if (workerError.empty())
                workerError = "Unable to write the downloaded data.";
            cv.notify_all();
            break;
        }
        nextToEmit += chunk.size();
        cv.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(mu);
        stopWorkers = true;
    }
    cv.notify_all();
    for (std::thread& t : workers)
        if (t.joinable())
            t.join();

    if (cancelled())
        return false;
    if (rangeIgnored)
        return fetcher(url, nextToEmit, 0, sink, cancelled, error);
    if (!sinkOk || failed) {
        error = workerError.empty() ? "Download failed." : workerError;
        return false;
    }
    return nextToEmit == totalBytes;
}

Step fetchAppend(RunContext& ctx, const std::string& url,
                 const std::string& localPath, uint64_t offset,
                 uint64_t fileBytes, uint64_t baseCompleted) {
    FILE* out = std::fopen(localPath.c_str(), "ab");
    if (!out) {
        ctx.error = "Unable to open the download file for writing.";
        return Step::Failed;
    }
    std::vector<char> writeBuffer(kInstallBatchBytes);
    std::setvbuf(out, writeBuffer.data(), _IOFBF, writeBuffer.size());
    Clock::time_point windowStart = Clock::now();
    Clock::time_point lastEmit = Clock::time_point::min();
    uint64_t windowBytes = 0;
    uint64_t written = 0;
    uint64_t speed = 0;
    auto sink = [&](const uint8_t* data, size_t n) -> bool {
        if (std::fwrite(data, 1, n, out) != n)
            return false;
        written += n;
        windowBytes += n;
        Clock::time_point now = Clock::now();
        double elapsed =
            std::chrono::duration<double>(now - windowStart).count();
        if (elapsed >= 1.0) {
            speed = static_cast<uint64_t>(windowBytes / elapsed);
            windowStart = now;
            windowBytes = 0;
        }
        if (!shouldEmitProgress(lastEmit))
            return true;
        DebridProgress p;
        p.status = DownloadStatus::Downloading;
        p.completedBytes = baseCompleted + offset + written;
        p.totalBytes = ctx.totalBytes;
        p.speedBytesPerSecond = speed;
        p.packagesInstalled = ctx.packagesInstalled;
        ctx.emit(p);
        return true;
    };
    std::string err;
    StreamRamBudget budget = detectStreamRamBudget(1 * 1024 * 1024);
    const size_t maximumBuffered = budget.valid
        ? budget.maxBufferedBytes : 64 * 1024 * 1024;
    // TorBox/TorrServer still pipeline Range workers; RD's CDN does not.
    bool ok = std::strcmp(ctx.provider.name(), "realdebrid") == 0
        ? fetchSequential(ctx.fetcher, url, offset, sink, *ctx.shouldStop, err)
        : fetchOrdered(ctx.fetcher, url, offset, fileBytes,
                       maximumBuffered / 2, sink, *ctx.shouldStop, err);
    std::fflush(out);
    std::fclose(out);
    if (!ok) {
        if (ctx.stop())
            return Step::Stopped;
        ctx.error = err.empty() ? "Download failed." : err;
        return Step::Failed;
    }
    return Step::Ok;
}

Step downloadPlainFile(RunContext& ctx, size_t kthSelected,
                       const DebridFile& file) {
    bool ok = true;
    std::string rel = sanitizeRelative(file.path, ctx.torrentName, ok);
    if (!ok) {
        ctx.error = "Refusing to write outside the download folder.";
        return Step::Failed;
    }
    std::string localPath = ctx.spec.dataPath + "/" + rel;
    std::string parent = parentDir(localPath);
    if (!parent.empty() && !makeDirectories(parent)) {
        ctx.error = "Unable to create the download directory.";
        return Step::Failed;
    }

    uint64_t existing = 0;
    bool exists = statSize(localPath, existing);
    if (exists && existing == file.bytes)
        return Step::Ok;
    if (exists && existing > file.bytes) {
        std::ofstream truncate(localPath,
                               std::ios::binary | std::ios::trunc);
    }

    DebridInfo info;
    info.links = ctx.selectedLinks;
    std::string url;
    if (!ctx.provider.resolveDownloadUrl(ctx.debridId, info, kthSelected,
                                         file, url, ctx.error))
        return Step::Failed;
    if (!linkAllowed(ctx.provider, url)) {
        ctx.error = "Debrid service returned a plaintext download link.";
        return Step::Failed;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        uint64_t offset = 0;
        statSize(localPath, offset);
        Step s = fetchAppend(ctx, url, localPath, offset, file.bytes,
                             ctx.completedSoFar);
        if (s == Step::Ok)
            break;
        if (s == Step::Stopped)
            return Step::Stopped;
        if (attempt == 0) {
            std::string fresh;
            if (ctx.provider.resolveDownloadUrl(ctx.debridId, info,
                                                kthSelected, file, fresh,
                                                ctx.error)) {
                if (!linkAllowed(ctx.provider, fresh)) {
                    ctx.error =
                        "Debrid service returned a plaintext download link.";
                    return Step::Failed;
                }
                url = fresh;
            }
            continue;
        }
        return Step::Failed;
    }

    uint64_t finalSize = 0;
    statSize(localPath, finalSize);
    if (finalSize != file.bytes) {
        ctx.error = "Downloaded file size mismatch.";
        return Step::Failed;
    }
    return Step::Ok;
}

install::PackageCallbacks makeCallbacks(RunContext& ctx,
                                        install::InstallBackend* backend,
                                        const std::string& displayName,
                                        InstallPacer& pacer) {
    install::PackageCallbacks callbacks;
    callbacks.skipFile = [backend](const std::string& name) {
        return backend->shouldSkipFile(name);
    };
    callbacks.beginFile = [backend, &ctx](const std::string& name,
                                          uint64_t size) {
        bool ok = backend->beginFile(name, size);
        if (!ok)
            ctx.error = backend->error();
        return ok;
    };
    callbacks.setFileSize = [backend, &ctx](uint64_t size) {
        bool ok = backend->setFileSize(size);
        if (!ok)
            ctx.error = backend->error();
        return ok;
    };
    callbacks.writeFile = [backend, &ctx, displayName, &pacer,
                           lastEmit = std::make_shared<Clock::time_point>(
                               Clock::time_point::min())](
                              const uint8_t* data, size_t size) {
        if (!pacer.waitForWrite(size, [&ctx] { return ctx.stop(); }))
            return false;
        bool ok = backend->writeFile(data, size);
        if (ok) {
            if (shouldEmitProgress(*lastEmit)) {
                DebridProgress p;
                p.status = DownloadStatus::Installing;
                p.totalBytes = ctx.totalBytes;
                p.completedBytes = ctx.completedSoFar +
                                   ctx.packageDownloadedBytes.load();
                p.packagesInstalled = ctx.packagesInstalled;
                p.currentPackage = displayName;
                p.installedBytes = backend->installedBytes();
                p.installTotalBytes = backend->expectedBytes();
                ctx.emit(p);
            }
        } else {
            ctx.error = backend->error();
        }
        return ok;
    };
    callbacks.endFile = [backend, &ctx] {
        bool ok = backend->endFile();
        if (!ok)
            ctx.error = backend->error();
        return ok;
    };
    return callbacks;
}

Step attemptStreamInstall(RunContext& ctx, const DebridFile& file,
                          const std::string& url,
                          const std::string& displayName) {
    ctx.packageDownloadedBytes = 0;
    if (!ctx.backend)
        ctx.backend = install::createInstallBackend(ctx.spec.workingRoot,
                                                    ctx.spec.installTarget);
    install::InstallBackend* backend = ctx.backend.get();
    if (!backend->beginPackage(ctx.spec.taskId, displayName)) {
        ctx.error = backend->error();
        return Step::Failed;
    }
    StreamRamBudget budget = detectStreamRamBudget(1 * 1024 * 1024);
    const size_t maximumBuffered = budget.valid
        ? budget.maxBufferedBytes : 64 * 1024 * 1024;
    InstallPacer pacer(maximumBuffered);
    pacer.beginPackage(isCompressedName(file.path));
    install::PackageStream stream(
        isCompressedName(file.path),
        makeCallbacks(ctx, backend, displayName, pacer), ctx.spec.taskId);
    DebridStreamQueue queue(maximumBuffered, pacer, *ctx.shouldStop);
    std::string fetchError;
    bool fetchOk = false;
    std::thread producer([&] {
        auto sink = [&queue, &ctx](const uint8_t* data, size_t n) -> bool {
            ctx.packageDownloadedBytes.fetch_add(n);
            return queue.push(data, n);
        };
        fetchOk = fetchSequential(ctx.fetcher, url, 0, sink, *ctx.shouldStop,
                                  fetchError);
        queue.finish();
    });

    bool streamOk = true;
    std::vector<uint8_t> chunk;
    std::vector<uint8_t> batch;
    batch.reserve(kInstallBatchBytes);
    auto flushBatch = [&]() -> bool {
        if (batch.empty())
            return true;
        if (!stream.write(batch.data(), batch.size())) {
            streamOk = false;
            queue.stop();
            batch.clear();
            return false;
        }
        pacer.observeConsumed(stream.consumed(), backend->installedBytes(),
                              now_ms());
        batch.clear();
        return true;
    };
    while (queue.pop(chunk)) {
        if (batch.empty() && chunk.size() >= kInstallBatchBytes) {
            if (!stream.write(chunk.data(), chunk.size())) {
                streamOk = false;
                queue.stop();
                break;
            }
            pacer.observeConsumed(stream.consumed(), backend->installedBytes(),
                                  now_ms());
        } else {
            batch.insert(batch.end(), chunk.begin(), chunk.end());
            if (batch.size() >= kInstallBatchBytes && !flushBatch())
                break;
        }
    }
    if (streamOk)
        flushBatch();
    queue.stop();
    producer.join();
    if (!fetchOk || !streamOk) {
        backend->rollbackPackage();
        if (ctx.stop())
            return Step::Stopped;
        if (ctx.error.empty())
            ctx.error = !stream.error().empty()
                ? describeStreamError(file, displayName, stream.error())
                : (fetchError.empty() ? "Package download failed."
                                      : fetchError);
        return Step::Failed;
    }

    DebridProgress committing;
    committing.status = DownloadStatus::Committing;
    committing.totalBytes = ctx.totalBytes;
    // The download for this package is complete at commit time.
    committing.completedBytes = ctx.completedSoFar + file.bytes;
    committing.packagesInstalled = ctx.packagesInstalled;
    committing.currentPackage = displayName;
    committing.installedBytes = backend->installedBytes();
    committing.installTotalBytes = backend->expectedBytes();
    ctx.emit(committing);

    if (!stream.finish()) {
        ctx.error = stream.error().empty()
            ? "Package finalize failed."
            : describeStreamError(file, displayName, stream.error());
        backend->rollbackPackage();
        return Step::Failed;
    }
    bool alreadyInstalled = false;
    if (!backend->commitPackage(alreadyInstalled)) {
        ctx.error = backend->error();
        backend->rollbackPackage();
        return Step::Failed;
    }
    ctx.packagesInstalled += 1;
    DebridProgress done;
    done.status = DownloadStatus::Installing;
    done.totalBytes = ctx.totalBytes;
    done.completedBytes = ctx.completedSoFar + file.bytes;
    done.packagesInstalled = ctx.packagesInstalled;
    done.currentPackage = displayName;
    ctx.emit(done);
    return Step::Ok;
}

Step streamInstallPackage(RunContext& ctx, size_t kthSelected,
                          const DebridFile& file) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        DebridInfo info;
        info.links = ctx.selectedLinks;
        std::string url;
        if (!ctx.provider.resolveDownloadUrl(ctx.debridId, info,
                                             kthSelected, file, url,
                                             ctx.error))
            return Step::Failed;
        if (!linkAllowed(ctx.provider, url)) {
            ctx.error = "Debrid service returned a plaintext download link.";
            return Step::Failed;
        }
        Step s = attemptStreamInstall(ctx, file, url, file.path);
        if (s == Step::Ok)
            return Step::Ok;
        if (s == Step::Stopped)
            return Step::Stopped;
        if (attempt == 0) {
            ctx.error.clear();
            continue;
        }
        return Step::Failed;
    }
    return Step::Failed;
}

} // namespace

DebridTransfer::DebridTransfer(DebridProvider& provider, RangeFetcher fetcher)
    : provider_(provider),
      fetcher_(fetcher ? std::move(fetcher) : curlRangeFetcher()) {}

DebridRunResult DebridTransfer::run(
    const DebridTaskSpec& spec, const std::function<bool()>& shouldStop,
    const std::function<void(const DebridProgress&)>& progress,
    std::string& debridIdOut, std::string& error,
    uint32_t resolveWindowMsOverride) {
    RunContext ctx{provider_, fetcher_, spec, &shouldStop, &progress};
    ctx.packagesInstalled = spec.packagesInstalled;
    ctx.resolveWindowMs =
        (resolveWindowMsOverride == UINT32_MAX) ? 60000 : resolveWindowMsOverride;

    // Paint Fetching before the create/poll HTTPS round-trips so the queue
    // does not sit on a stale status while the magnet is submitted.
    {
        DebridProgress starting;
        starting.status = DownloadStatus::Fetching;
        starting.fetchProgress = 0.0;
        ctx.emit(starting);
    }

    Step s = ensureCreated(ctx);
    debridIdOut = ctx.debridId;
    if (s == Step::Failed) {
        error = ctx.error;
        return DebridRunResult::Failed;
    }

    s = pollUntilReady(ctx);
    debridIdOut = ctx.debridId;
    if (s == Step::Stopped)
        return DebridRunResult::Stopped;
    if (s == Step::Failed) {
        error = ctx.error;
        return DebridRunResult::Failed;
    }

    bool hadNonEmptySelection = !spec.selectionPaths.empty();
    if (!hadNonEmptySelection && !spec.fileSelection.empty()) {
        for (uint8_t v : spec.fileSelection)
            if (v) { hadNonEmptySelection = true; break; }
    }

    uint64_t selectedCount = 0;
    for (size_t i = 0; i < ctx.files.size(); ++i) {
        if (selected(spec, i, ctx.files[i])) {
            ctx.totalBytes += ctx.files[i].bytes;
            ++selectedCount;
        }
    }

    if (hadNonEmptySelection && selectedCount == 0) {
        error = "No debrid files matched the selected files.";
        return DebridRunResult::Failed;
    }

    size_t kthSelected = 0;
    uint32_t packageOrdinal = 0;
    for (size_t i = 0; i < ctx.files.size(); ++i) {
        if (!selected(spec, i, ctx.files[i]))
            continue;
        if (ctx.stop())
            return DebridRunResult::Stopped;
        const DebridFile& file = ctx.files[i];
        Step fs = Step::Ok;
        if (installs(ctx.spec, i, file)) {
            if (packageOrdinal >= spec.packagesInstalled)
                fs = streamInstallPackage(ctx, kthSelected, file);
            ++packageOrdinal;
        } else {
            fs = downloadPlainFile(ctx, kthSelected, file);
        }
        ++kthSelected;
        if (fs == Step::Stopped)
            return DebridRunResult::Stopped;
        if (fs == Step::Failed) {
            error = ctx.error;
            return DebridRunResult::Failed;
        }
        ctx.completedSoFar += file.bytes;
    }

    DebridProgress fin;
    fin.status = spec.mode == TransferMode::StreamInstall
                     ? DownloadStatus::Installed
                     : DownloadStatus::Completed;
    fin.completedBytes = ctx.totalBytes;
    fin.totalBytes = ctx.totalBytes;
    fin.packagesInstalled = ctx.packagesInstalled;
    ctx.emit(fin);
    return DebridRunResult::Finished;
}

} // namespace pipensx
