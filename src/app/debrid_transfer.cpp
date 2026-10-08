#include "debrid_transfer.hpp"
#include "curl_https.hpp"
#include "download_manager.hpp"
#include "debrid_provider.hpp"
#include "remote_archive.hpp"
#include "install_pacer.hpp"
#include "nx_file_types.hpp"
#include "stream_ram_budget.hpp"
#include "../install/install_backend.hpp"
#include "../install/install_journal.hpp"
#include "../install/package_stream.hpp"

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
#include "../platform/storage.h"
}

#include <cerrno>
#include <algorithm>
#include <array>
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
#include <unistd.h>

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
                      const std::function<bool()>& cancelled, std::string taskId)
        : maximumBytes_(maximumBytes), taskId_(std::move(taskId)),
          pacer_(pacer), cancelled_(cancelled) {}

    bool push(const uint8_t* data, size_t size) {
        pacer_.observeSource(size, now_ms());
        std::unique_lock<std::mutex> lock(mutex_);
        const uint64_t waitStarted = telemetry_enabled() ? now_us() : 0;
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
        if (waitStarted && measurementPaused)
            producerWaitUs_ += now_us() - waitStarted;
        if (stopped_ || cancelled_())
            return false;
        queue_.emplace_back(data, data + size);
        bufferedBytes_ += size;
        peakBytes_ = std::max(peakBytes_, bufferedBytes_);
        sourceBytes_ += size;
        reportTelemetry();
        pacer_.setBufferedBytes(bufferedBytes_);
        ready_.notify_all();
        return true;
    }

    bool pop(std::vector<uint8_t>& chunk) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !finished_ && !stopped_ && !cancelled_()) {
            const uint64_t started = telemetry_enabled() ? now_us() : 0;
            ready_.wait_for(lock, std::chrono::milliseconds(50));
            if (started) consumerWaitUs_ += now_us() - started;
            reportTelemetry();
        }
        if (queue_.empty())
            return false;
        chunk = std::move(queue_.front());
        queue_.pop_front();
        bufferedBytes_ -= chunk.size();
        consumedBytes_ += chunk.size();
        reportTelemetry();
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
        reportTelemetry(true);
        stopped_ = true;
        ready_.notify_all();
    }

private:
    // Called under mutex_ on both threads. Five-second aggregates explain
    // whether HTTP is waiting for installation or installation for HTTP.
    void reportTelemetry(bool summary = false) {
        const uint64_t now = now_ms();
        if (generation_ != telemetry_generation()) {
            generation_ = telemetry_generation();
            lastReportMs_ = now;
            sourceBytes_ = consumedBytes_ = producerWaitUs_ = consumerWaitUs_ = 0;
            peakBytes_ = bufferedBytes_;
        }
        if (!telemetry_enabled() || (!summary && now - lastReportMs_ < 5000))
            return;
        const uint64_t elapsed = now - lastReportMs_;
        telemetry_log("debrid_queue", taskId_.c_str(),
            "event=%s interval_ms=%llu source_bytes=%llu consumed_bytes=%llu buffered_bytes=%zu capacity_bytes=%zu peak_bytes=%zu producer_wait_us=%llu consumer_wait_us=%llu download_bps=%llu consume_bps=%llu",
            summary ? "end" : "window", (unsigned long long)elapsed,
            (unsigned long long)sourceBytes_, (unsigned long long)consumedBytes_,
            bufferedBytes_, maximumBytes_, peakBytes_,
            (unsigned long long)producerWaitUs_, (unsigned long long)consumerWaitUs_,
            (unsigned long long)(elapsed ? sourceBytes_ * 1000 / elapsed : 0),
            (unsigned long long)(elapsed ? consumedBytes_ * 1000 / elapsed : 0));
        lastReportMs_ = now;
        sourceBytes_ = consumedBytes_ = producerWaitUs_ = consumerWaitUs_ = 0;
        peakBytes_ = bufferedBytes_;
    }
    uint32_t generation_ = 0;
    uint64_t lastReportMs_ = 0;
    uint64_t sourceBytes_ = 0, consumedBytes_ = 0;
    uint64_t producerWaitUs_ = 0, consumerWaitUs_ = 0;
    size_t peakBytes_ = 0;
    size_t maximumBytes_;
    std::string taskId_;
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
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    size = static_cast<uint64_t>(st.st_size);
    return true;
}

constexpr uint64_t kFat32MaximumFileBytes = 0xFFFFFFFFull;
constexpr uint64_t kSplitResumeCheckpointBytes = 4ull * 1024 * 1024;

bool needsSplitDownload(uint64_t fileBytes, bool forceSplit = false) {
    if (forceSplit) return true;
#ifdef __SWITCH__
    // Use the same DBI/HOS concatenation-folder format as torrent storage.
    // Always splitting on Switch also works on exFAT and avoids a destructive
    // filesystem probe after a multi-gigabyte partial download already exists.
    return fileBytes > kFat32MaximumFileBytes;
#else
    (void)fileBytes;
    return false;
#endif
}

std::string splitResumePath(const std::string& localPath) {
    return localPath + ".pipensx-partial";
}

bool splitResumeExists(const std::string& localPath) {
    return access(splitResumePath(localPath).c_str(), F_OK) == 0;
}

bool loadSplitResume(const std::string& localPath, uint64_t expected,
                     uint64_t& offset) {
    FILE* file = std::fopen(splitResumePath(localPath).c_str(), "rb");
    if (!file)
        return false;
    unsigned long long parsed = 0;
    const bool ok = std::fscanf(file, "%llu", &parsed) == 1 &&
                    parsed <= expected;
    std::fclose(file);
    if (ok)
        offset = static_cast<uint64_t>(parsed);
    return ok;
}

bool saveSplitResume(const std::string& localPath, uint64_t offset,
                     std::string& error) {
    const std::string path = splitResumePath(localPath);
    // fsdev rename does not replace an existing destination on Switch. Keep
    // the marker inode in place and rewrite its tiny payload instead. If the
    // console loses power during this write, an empty/partial marker remains
    // and loadSplitResume rejects it rather than mistaking the split folder
    // for a completed download.
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        error = std::string("Unable to create download resume state: ") +
                std::strerror(errno);
        return false;
    }
    bool ok = std::fprintf(file, "%llu\n",
                           static_cast<unsigned long long>(offset)) > 0;
    int savedErrno = ok ? 0 : errno;
    if (ok && std::fflush(file) != 0) {
        ok = false;
        savedErrno = errno;
    }
    if (ok && fsync(fileno(file)) != 0) {
        ok = false;
        savedErrno = errno;
    }
    if (std::fclose(file) != 0 && ok) {
        ok = false;
        savedErrno = errno;
    }
    if (!ok) {
        error = std::string("Unable to save download resume state: ") +
                std::strerror(savedErrno ? savedErrno : EIO);
    }
    return ok;
}

// A pre-fix build appended a >4 GiB debrid file as one FAT32 file and stopped
// at the filesystem ceiling. It cannot be converted safely in place to the
// fixed-size split format, so discard that one legacy partial and restart it.
bool splitDownloadOffset(const std::string& localPath, uint64_t expected,
                         uint64_t& offset, std::string& error,
                         bool* restarted = nullptr) {
    offset = 0;
    if (restarted)
        *restarted = false;
    const bool markerPresent = splitResumeExists(localPath);
    if (markerPresent && !loadSplitResume(localPath, expected, offset)) {
        error = "The large-file download resume state is invalid. Delete its "
                "downloaded data and retry.";
        return false;
    }

    struct stat st {};
    const bool outputExists = stat(localPath.c_str(), &st) == 0;
    if (markerPresent) {
        if (offset > 0 && !outputExists) {
            offset = 0;
            return saveSplitResume(localPath, offset, error);
        }
        if (outputExists && S_ISREG(st.st_mode)) {
            if (static_cast<uint64_t>(st.st_size) == expected) {
                std::remove(splitResumePath(localPath).c_str());
                offset = expected;
                return true;
            }
            if (std::remove(localPath.c_str()) != 0) {
                error = std::string("Unable to replace the legacy large file: ") +
                        std::strerror(errno);
                return false;
            }
            offset = 0;
            log_msg("[storage] restarting legacy unsplit debrid file '%s'\n",
                    localPath.c_str());
            if (restarted)
                *restarted = true;
            return saveSplitResume(localPath, offset, error);
        }
        return true;
    }

    if (!outputExists)
        return true;
    if (S_ISDIR(st.st_mode)) {
        // New split downloads keep the marker until every byte is committed
        // and the concatenation attribute is set. No marker means complete.
        offset = expected;
        return true;
    }
    if (S_ISREG(st.st_mode) &&
        static_cast<uint64_t>(st.st_size) == expected) {
        offset = expected; // A complete file on exFAT from an older build.
        return true;
    }
    if (S_ISREG(st.st_mode) && std::remove(localPath.c_str()) == 0) {
        log_msg("[storage] restarting legacy unsplit debrid file '%s'\n",
                localPath.c_str());
        if (restarted)
            *restarted = true;
        return true;
    }
    error = std::string("Unable to replace the legacy large file: ") +
            std::strerror(errno);
    return false;
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
              const DebridFile& file, const std::string& torrentName) {
    bool wanted = false;
    if (!spec.selectionRelativePaths.empty()) {
        bool safe = false;
        const std::string relative = sanitizeRelative(file.path, torrentName, safe);
        if (!safe) return false;
        for (const auto& entry : spec.selectionRelativePaths)
            if (entry.first == relative && entry.second == file.bytes) {
                wanted = true;
                break;
            }
    } else if (!spec.selectionPaths.empty()) {
        std::string base = baseName(file.path);
        for (const auto& entry : spec.selectionPaths)
            if (entry.first == base && entry.second == file.bytes) {
                wanted = true;
                break;
            }
    } else {
        wanted = spec.fileSelection.empty() ||
                 (index < spec.fileSelection.size() && spec.fileSelection[index]);
    }
    if (!wanted)
        return false;
    // Stream-install packages go to NCM. Explicit Download extras (zip /
    // LayeredFS) still fetch so they can deploy after the packages commit.
    // Unselected junk stays skipped so a leftover rusifikator cannot fail
    // the task at 100%.
    //
    // Live debrid tasks send the mask as selectionPaths (basename+size)
    // because the provider's file order is not the torrent's. Do not index
    // fileSelection with the debrid list position — a zip listed first
    // would otherwise see the NSP's Install slot and be dropped.
    if (spec.mode == TransferMode::StreamInstall && !isPackageName(file.path)) {
        if (!spec.selectionPaths.empty() || !spec.selectionRelativePaths.empty())
            return true;
        if (index >= spec.fileSelection.size())
            return false;
        const uint8_t action = spec.fileSelection[index];
        return action == static_cast<uint8_t>(FileAction::Download) ||
               action == static_cast<uint8_t>(FileAction::Install);
    }
    return true;
}

size_t kthSelectedIndex(const DebridTaskSpec& spec,
                        const std::vector<DebridFile>& files, size_t index,
                        const std::string& torrentName) {
    size_t k = 0;
    for (size_t i = 0; i < index; ++i)
        if (selected(spec, i, files[i], torrentName))
            ++k;
    return k;
}

std::vector<size_t> selectedFileOrder(const DebridTaskSpec& spec,
                                      const std::vector<DebridFile>& files,
                                      const std::string& torrentName) {
    std::vector<size_t> order;
    for (size_t i = 0; i < files.size(); ++i)
        if (selected(spec, i, files[i], torrentName))
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) {
                         const int ra = packageInstallRank(files[a].path);
                         const int rb = packageInstallRank(files[b].path);
                         if (ra != rb)
                             return ra < rb;
                         return a < b;
                     });
    return order;
}

bool installs(const DebridTaskSpec& spec, size_t index,
              const DebridFile& file) {
    if (spec.mode != TransferMode::StreamInstall || !isPackageName(file.path))
        return false;
    if (spec.fileSelection.empty() || !spec.selectionPaths.empty() || !spec.selectionRelativePaths.empty())
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

bool sequentialHttp(const DebridProvider& provider) {
    const char* name = provider.name();
    return std::strcmp(name, "realdebrid") == 0 ||
           std::strcmp(name, "torrserver") == 0;
}

int pollWaitMs(const DebridProvider& provider) {
    const char* name = provider.name();
    if (std::strcmp(name, "torrserver") == 0)
        return 250;
    // TorBox/RD: the cloud copy can take minutes; 2s is enough to notice
    // Ready without burning the 300/min (TorBox) / 250/min (RD) budgets.
    return 2000;
}

// Piece-buffer RAM is for the torrent picker. Debrid HTTP has no pieces;
// fold that reservation into the reorder queue so the TorrServer/CDN reader
// stays ahead of ncm instead of sitting idle in unused piece slots.
size_t debridQueueBytes() {
    StreamRamBudget budget = detectStreamRamBudget(1 * 1024 * 1024);
    if (!budget.valid)
        return 64 * 1024 * 1024;
    uint64_t bytes = static_cast<uint64_t>(budget.maxBufferedBytes) +
                     budget.maxPieceBufferBytes;
    constexpr uint64_t kMax = 256ull * 1024 * 1024;
    if (bytes > kMax)
        bytes = kMax;
    if (bytes < budget.maxBufferedBytes)
        bytes = budget.maxBufferedBytes;
    return static_cast<size_t>(bytes);
}

void remTorrserverIfFinished(DebridProvider& provider, const std::string& id,
                             DebridRunResult result) {
    if (result != DebridRunResult::Finished || id.empty() ||
        std::strcmp(provider.name(), "torrserver") != 0)
        return;
    std::string error;
    if (!provider.remove(id, error))
        log_msg("[debrid] torrserver rem failed id=%s: %s\n", id.c_str(),
                error.c_str());
    else
        log_msg("[debrid] torrserver rem id=%s\n", id.c_str());
}

constexpr size_t kInstallBatchBytes = 1u << 20;
constexpr unsigned kRangeWorkers = 4;
constexpr uint64_t kRangeSegmentBytes = 4ull << 20;

class ThroughputMeter {
public:
    ThroughputMeter(const char* stage, std::string tag)
        : stage_(stage), tag_(std::move(tag)) { reset(); }
    ~ThroughputMeter() { report(true); }
    void record(size_t bytes, uint64_t workUs) {
        if (generation_ != telemetry_generation()) reset();
        if (!telemetry_enabled()) return;
        bytes_ += bytes;
        workUs_ += workUs;
        maxUs_ = std::max(maxUs_, workUs);
        ++calls_;
        report(false);
    }
private:
    void reset() {
        generation_ = telemetry_generation();
        lastMs_ = now_ms();
        bytes_ = workUs_ = maxUs_ = calls_ = 0;
    }
    void report(bool final) {
        if (generation_ != telemetry_generation()) reset();
        const uint64_t interval = now_ms() - lastMs_;
        if (!telemetry_enabled() || !calls_ || (!final && interval < 5000)) return;
        telemetry_log(stage_, tag_.c_str(),
            "event=%s interval_ms=%llu bytes=%llu wall_bps=%llu work_us=%llu work_bps=%llu max_call_us=%llu calls=%llu",
            final ? "end" : "window", (unsigned long long)interval,
            (unsigned long long)bytes_,
            (unsigned long long)(interval ? bytes_ * 1000 / interval : 0),
            (unsigned long long)workUs_,
            (unsigned long long)(workUs_ ? bytes_ * 1000000 / workUs_ : 0),
            (unsigned long long)maxUs_, (unsigned long long)calls_);
        reset();
    }
    const char* stage_;
    std::string tag_;
    uint32_t generation_ = 0;
    uint64_t lastMs_ = 0, bytes_ = 0, workUs_ = 0, maxUs_ = 0, calls_ = 0;
};

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
    uint64_t sinkUs = 0;
    ThroughputMeter* meter = nullptr;
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
        const uint64_t started = telemetry_enabled() ? now_us() : 0;
        const bool accepted = (*c->sink)(reinterpret_cast<const uint8_t*>(ptr), total);
        const uint64_t elapsed = started ? now_us() - started : 0;
        c->sinkUs += elapsed;
        if (c->meter) c->meter->record(accepted ? total : 0, elapsed);
        if (!accepted) {
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

// Keep connection/DNS/TLS caches across files and retries. Each worker owns
// its easy handle exclusively; only idle handles may move between threads.
class CurlHandlePool {
public:
    ~CurlHandlePool() {
        for (CURL* curl : idle_)
            if (curl) curl_easy_cleanup(curl);
    }

    CURL* acquire() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (CURL*& curl : idle_) {
                if (!curl) continue;
                CURL* result = curl;
                curl = nullptr;
                return result;
            }
        }
        return curl_easy_init();
    }

    void release(CURL* curl) {
        // Clear request-specific ranges and pointers to stack callbacks, but
        // retain libcurl's reusable connections. The next request sets every
        // option again, including its URL and trusted transport policy.
        curl_easy_reset(curl);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (CURL*& slot : idle_) {
                if (slot) continue;
                slot = curl;
                return;
            }
        }
        curl_easy_cleanup(curl);
    }

private:
    std::mutex mutex_;
    std::array<CURL*, kRangeWorkers> idle_{};
};

RangeFetcher curlRangeFetcher() {
    auto pool = std::make_shared<CurlHandlePool>();
    return [pool](const std::string& url, uint64_t offset, uint64_t endExclusive,
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
        const std::string host = urlHostForDisplay(url);
        log_msg("[debrid] downloading from %s over %s\n", host.c_str(),
                plain ? "HTTP" : "HTTPS");
        auto release = [pool](CURL* curl) { pool->release(curl); };
        std::unique_ptr<CURL, decltype(release)> lease(pool->acquire(), release);
        CURL* curl = lease.get();
        if (!curl) {
            error = "Unable to initialize network transfer.";
            return false;
        }
        CurlSink state;
        state.sink = &sink;
        state.cancelled = &cancelled;
        state.curl = curl;
        state.ranged = endExclusive != 0;
        // One cached connection per worker bounds scarce Switch sockets,
        // even when consecutive file URLs point to different CDN hosts.
        curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
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
        CurlSocketTuning socketTuning;
        curlTuneDownloadSocket(curl, &socketTuning);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        ThroughputMeter httpMeter("debrid_http", host);
        state.meter = &httpMeter;
        CURLcode rc = curl_easy_perform(curl);
        long httpStatus = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        double dns = 0, connect = 0, tls = 0, firstByte = 0, total = 0;
        long newConnections = 0;
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &firstByte);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &newConnections);
        if (newConnections > 0)
            log_msg("[net] download socket host=%s requested_rx=%d actual_rx=%d "
                    "fallback_errno=%d\n", host.c_str(), socketTuning.requestedBytes,
                    socketTuning.actualBytes, socketTuning.lastError);
        telemetry_log("debrid_http", host.c_str(),
            "event=request_end offset=%llu end=%llu http=%ld curl=%d bytes=%llu dns_ms=%.1f connect_ms=%.1f tls_ms=%.1f ttfb_ms=%.1f total_ms=%.1f sink_us=%llu new_connections=%ld average_bps=%.0f sink_refused=%d",
            (unsigned long long)offset, (unsigned long long)endExclusive,
            httpStatus, (int)rc, (unsigned long long)state.received,
            dns * 1000, connect * 1000, tls * 1000, firstByte * 1000,
            total * 1000, (unsigned long long)state.sinkUs, newConnections,
            total > 0 ? state.received / total : 0, state.sinkRefused ? 1 : 0);
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
        if (state.ranged && httpStatus != 206 && rc == CURLE_OK) {
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
    bool selectionReplacementCreated = false;
    bool groupedDownload = false;
    std::string resolvedSourceUrl;
    bool retryableFetchFailure = false;
    bool localSource = false;
    std::unique_ptr<install::InstallBackend> backend;
    std::string error;
    bool recoveryNoted = false;
    bool recoveryBytePoint = false;
    uint64_t recoveryUnitBytes = 0;
    uint64_t recoveryUnitTotal = 0;

    // Compressed bytes pulled from the debrid link for the package currently
    // streaming. Added to completedSoFar so the download bar advances mid-
    // package instead of only jumping when a package finishes.
    std::atomic<uint64_t> packageDownloadedBytes {0};
    std::atomic<uint64_t> packageDownloadSpeed {0};
    std::atomic<uint64_t> packageDownloadSpeedAtMs {0};

    void emit(const DebridProgress& p) const {
        if (*progress)
            (*progress)(p);
    }
    bool stop() const { return (*shouldStop)(); }
};

void noteFileRecovery(RunContext& ctx, bool bytePoint, uint64_t unitBytes,
                      uint64_t unitTotal) {
    ctx.recoveryNoted = true;
    ctx.recoveryBytePoint = bytePoint;
    ctx.recoveryUnitBytes = unitBytes;
    ctx.recoveryUnitTotal = unitTotal;
}

void applyFileRecovery(DebridProgress& progress, const RunContext& ctx) {
    if (!ctx.recoveryNoted)
        return;
    progress.hasRecovery = true;
    progress.recoveryBytePoint = ctx.recoveryBytePoint;
    progress.recoveryUnitBytes = ctx.recoveryUnitBytes;
    progress.recoveryUnitTotal = ctx.recoveryUnitTotal;
}

Step ensureCreated(RunContext& ctx) {
    if (ctx.stop()) return Step::Stopped;
    log_msg("[debrid-intent] source=queue phase=claimed task=%s provider=%s selected_actions=%zu resumed=%d\n",
            ctx.spec.taskId.c_str(), ctx.provider.name(),
            ctx.spec.fileSelection.size(), ctx.spec.debridId.empty() ? 0 : 1);
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
    int staleSelectionPolls = 0;
    bool canFallback =
        ctx.magnetCreated && !ctx.fallbackUsed && !ctx.spec.torrentPath.empty();
    Clock::time_point windowStart = Clock::now();
    auto replaceFinalizedRealDebridSelection = [&]() -> Step {
        if (ctx.selectionReplacementCreated) {
            ctx.error = "Real-Debrid could not create a complete file "
                        "selection for this torrent.";
            return Step::Failed;
        }
        const std::string previousId = ctx.debridId;
        std::string replacementId;
        if (!ctx.provider.createSelectionReplacement(previousId, replacementId,
                                                     ctx.error))
            return Step::Failed;
        ctx.debridId = std::move(replacementId);
        ctx.selectionReplacementCreated = true;
        ctx.filesSelected = false;
        ctx.selectedLinks.clear();
        ctx.files.clear();
        staleSelectionPolls = 0;
        log_msg("[debrid] Real-Debrid selection finalized on id=%s; "
                "created one replacement id=%s for the complete selection\n",
                previousId.c_str(), ctx.debridId.c_str());
        return Step::Ok;
    };
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
            if (!sleepSlices(*ctx.shouldStop, pollWaitMs(ctx.provider)))
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

        // Real-Debrid keeps the selected-file set on the torrent itself. A
        // resumed task may therefore point at an already-downloaded torrent
        // whose old selection contains only the base game. Re-select the
        // complete wanted set on that SAME torrent instead of cloning the
        // magnet once per update/DLC. This also makes nested paths a single
        // server-side selection/seed operation.
        const bool realdebrid =
            std::strcmp(ctx.provider.name(), "realdebrid") == 0;
        std::vector<std::string> selectedIds;
        size_t wantedCount = 0;
        size_t wantedSelectedOnServer = 0;
        size_t serverSelectedCount = 0;
        for (size_t i = 0; i < info.files.size(); ++i) {
            const bool wanted = selected(ctx.spec, i, info.files[i], info.name);
            if (wanted) {
                ++wantedCount;
                if (!info.files[i].id.empty())
                    selectedIds.push_back(info.files[i].id);
                if (info.files[i].serverSelected == 1)
                    ++wantedSelectedOnServer;
            }
            if (info.files[i].serverSelected == 1)
                ++serverSelectedCount;
        }
        if (info.phase == DebridInfo::Phase::Ready &&
            !ctx.spec.selectionRelativePaths.empty() &&
            wantedCount != ctx.spec.selectionRelativePaths.size()) {
            // Do this before filesResolved overwrites the saved manifest or
            // any package commits: a partial match is not a complete install.
            ctx.error = "Debrid file list does not match all saved selected files.";
            return Step::Failed;
        }
        const bool rdSelectionMismatch =
            realdebrid && wantedCount > 0 &&
            wantedSelectedOnServer != wantedCount;
        const bool rdLinkMismatch =
            realdebrid && wantedCount > 0 &&
            info.phase == DebridInfo::Phase::Ready &&
            info.links.size() != serverSelectedCount;

        if (!ctx.filesSelected && !info.files.empty() &&
            (info.phase == DebridInfo::Phase::AwaitingSelection ||
             rdSelectionMismatch)) {
            if (wantedCount == 0) {
                ctx.error = "No debrid files matched the selected files.";
                return Step::Failed;
            }
            if (selectedIds.size() != wantedCount) {
                ctx.error = "Real-Debrid did not return file ids for all "
                            "selected files.";
                return Step::Failed;
            }
            if (realdebrid &&
                (rdSelectionMismatch || rdLinkMismatch)) {
                log_msg("[debrid] refreshing Real-Debrid selection in-place "
                        "id=%s wanted=%zu server=%zu links=%zu\n",
                        ctx.debridId.c_str(), wantedCount,
                        serverSelectedCount, info.links.size());
            }
            if (!ctx.provider.selectFiles(ctx.debridId, selectedIds,
                                          ctx.error))
                return Step::Failed;
            if (realdebrid && ctx.provider.selectionAlreadyFinalized() &&
                (rdSelectionMismatch || rdLinkMismatch)) {
                const Step replacement =
                    replaceFinalizedRealDebridSelection();
                if (replacement != Step::Ok)
                    return replacement;
                continue;
            }
            ctx.filesSelected = true;
            ctx.selectedLinks.clear();
            // A downloaded RD torrent can briefly keep reporting the old
            // selected set/links after POST. Refetch before resolving URLs.
            if (realdebrid && info.phase == DebridInfo::Phase::Ready)
                continue;
        }

        if (info.phase == DebridInfo::Phase::Ready && !info.files.empty()) {
            if (ctx.filesSelected && realdebrid &&
                rdSelectionMismatch) {
                // Selection changes on a cached torrent are asynchronous.
                // Wait for the wanted file set before replacing an immutable
                // old selection. A grouped link alone is not a stale selection.
                if (++staleSelectionPolls < 5) {
                    if (!sleepSlices(*ctx.shouldStop,
                                     pollWaitMs(ctx.provider)))
                        return Step::Stopped;
                    continue;
                }
                if (!ctx.selectionReplacementCreated) {
                    const Step replacement =
                        replaceFinalizedRealDebridSelection();
                    if (replacement != Step::Ok)
                        return replacement;
                    continue;
                }
            }
            if (realdebrid && rdLinkMismatch) {
                // RD can publish one archive for an already complete set.
                // Cloning that same complete selection cannot make its links
                // separate: preserve the cached parent and resolve only the
                // individual files needed for this task.
                ctx.groupedDownload = true;
                log_msg("[debrid] Real-Debrid kept grouped links after complete "
                        "selection id=%s wanted=%zu server=%zu links=%zu; "
                        "reading cached archive members without new selections\n",
                        ctx.debridId.c_str(), wantedCount, serverSelectedCount,
                        info.links.size());
            }
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
                    file.action = !selected(ctx.spec, i, info.files[i], info.name)
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
        if (!sleepSlices(*ctx.shouldStop, pollWaitMs(ctx.provider)))
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

bool transientFetchError(const std::string& error) {
    std::string folded = error;
    for (char& ch : folded)
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    for (const char* phrase : {"timed out", "timeout", "could not reach",
                              "couldn't connect", "could not connect",
                              "couldn't resolve", "could not resolve",
                              "connection lost", "connection reset",
                              "recv failure", "send failure", "partial file",
                              "transferred a partial", "server returned nothing",
                              "stream was interrupted", "http 429", "http 500",
                              "http 502", "http 503", "http 504"})
        if (folded.find(phrase) != std::string::npos) return true;
    return false;
}

// One TCP stream from offset to EOF. Real-Debrid's CDN returns 206 but the
// body is not the requested slice (PFS0 / installer rejects). TorrServer's
// RAM cache is a sequential reader — Range workers thrash it. TorBox's CDN
// is the remaining WAN path that actually pipelines 206s, which is what
// keeps Switch's 256 KiB TCP window off the ~5 MB/s floor.
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

// Let HTTP receive while the SD is busy. Drain delivered bytes on source
// failure, and join before file handles or callbacks leave the caller's scope.
bool fetchToDiskBuffered(RunContext& ctx, const std::string& url,
                         uint64_t offset, uint64_t totalBytes,
                         const std::function<bool(const uint8_t*, size_t)>& sink,
                         std::string& error) {
    const size_t budget = debridQueueBytes();
    const size_t capacity = std::min<size_t>(budget, 8u << 20);
    InstallPacer pacer(capacity);
    pacer.beginPackage(false);
    std::atomic<bool> consumerStopped{false};
    const std::function<bool()> cancelled = [&] {
        return consumerStopped.load() || ctx.stop();
    };
    DebridStreamQueue queue(capacity, pacer, cancelled, ctx.spec.taskId);
    auto fetch = [&](const std::function<bool(const uint8_t*, size_t)>& target) {
        return sequentialHttp(ctx.provider)
            ? fetchSequential(ctx.fetcher, url, offset, target, cancelled, error)
            : fetchOrdered(ctx.fetcher, url, offset, totalBytes,
                           budget / 2, target, cancelled, error);
    };
    bool sourceOk = false;
    std::thread producer;
    try {
        producer = std::thread([&] {
            try {
                sourceOk = fetch([&](const uint8_t* data, size_t size) {
                    while (size) {
                        const size_t part = std::min<size_t>(size, capacity);
                        if (!queue.push(data, part)) return false;
                        data += part;
                        size -= part;
                    }
                    return true;
                });
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Download worker failed.";
            }
            queue.finish();
        });
    } catch (const std::system_error&) {
        return fetch(sink); // Constrained applet: retain synchronous fallback.
    }
    bool sinkOk = true;
    try {
        std::vector<uint8_t> chunk;
        // A pause stops new input, but the accepted queue prefix must land
        // on disk so the next HTTP request resumes from that exact offset.
        while (queue.pop(chunk)) {
            if (!sink(chunk.data(), chunk.size())) {
                sinkOk = false;
                break;
            }
        }
    } catch (...) {
        consumerStopped = true;
        queue.stop();
        producer.join();
        throw;
    }
    const bool stopped = ctx.stop();
    consumerStopped = true;
    queue.stop();
    producer.join();
    return sourceOk && sinkOk && !stopped;
}

Step fetchSplitDownload(RunContext& ctx, const std::string& url,
                        const std::string& localPath, uint64_t offset,
                        uint64_t fileBytes, uint64_t baseCompleted) {
    std::string stateError;
    if (!saveSplitResume(localPath, offset, stateError)) {
        ctx.error = stateError;
        diagnostic_error("storage", "debrid_resume", "file=%s error=%s",
                         baseName(localPath).c_str(), stateError.c_str());
        return Step::Failed;
    }

    const std::string directory = parentDir(localPath);
    const std::string leaf = baseName(localPath);
    if (directory.empty() || leaf.empty() || leaf.size() >= MAX_NAME_LEN) {
        ctx.error = "The large download file path is too long.";
        return Step::Failed;
    }

    mi_file_t file {};
    std::snprintf(file.path, sizeof(file.path), "%s", leaf.c_str());
    file.length = static_cast<int64_t>(fileBytes);
    file.offset = 0;
    metainfo_t metainfo {};
    std::snprintf(metainfo.name, sizeof(metainfo.name), "%s", leaf.c_str());
    metainfo.total_length = file.length;
    metainfo.num_files = 1;
    metainfo.files = &file;

    storage_file_config_t config {};
    config.mode = STORAGE_FILE_DISK;
    config.force_split = 1;
    storage_t* output = storage_open_ex(&metainfo, directory.c_str(), &config);
    if (!output) {
        const char* detail = storage_open_error();
        ctx.error = detail && *detail
            ? std::string("Unable to prepare the large download: ") + detail
            : "Unable to prepare the large download file.";
        diagnostic_error("storage", "debrid_open", "file=%s error=%s",
                         leaf.c_str(), ctx.error.c_str());
        return Step::Failed;
    }

    if (offset == fileBytes) {
        storage_finalize(output);
        storage_close(output);
        std::remove(splitResumePath(localPath).c_str());
        return Step::Ok;
    }

    Clock::time_point windowStart = Clock::now();
    Clock::time_point lastEmit = Clock::time_point::min();
    uint64_t windowBytes = 0;
    uint64_t written = 0;
    uint64_t speed = 0;
    uint64_t checkpoint = offset;
    std::string sinkError;
    ThroughputMeter diskMeter("debrid_sd", ctx.spec.taskId);
    auto writeBatch = [&](const uint8_t* data, size_t n) -> bool {
        const uint64_t started = telemetry_enabled() ? now_us() : 0;
        const bool writtenOk = storage_write(output,
            static_cast<int64_t>(offset + written), data, n);
        diskMeter.record(writtenOk ? n : 0, started ? now_us() - started : 0);
        if (!writtenOk) {
            const char* detail = storage_error(output);
            sinkError = detail && *detail
                ? std::string("Unable to write the large download: ") + detail
                : "Unable to write the large download file.";
            diagnostic_error("storage", "debrid_write", "file=%s error=%s",
                             leaf.c_str(), sinkError.c_str());
            return false;
        }
        written += n;
        windowBytes += n;
        const uint64_t committed = offset + written;
        if (committed - checkpoint >= kSplitResumeCheckpointBytes) {
            if (!saveSplitResume(localPath, committed, sinkError)) {
                diagnostic_error("storage", "debrid_resume",
                                 "file=%s error=%s", leaf.c_str(),
                                 sinkError.c_str());
                return false;
            }
            checkpoint = committed;
        }
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
        p.completedBytes = baseCompleted + committed;
        p.totalBytes = ctx.totalBytes;
        p.speedBytesPerSecond = speed;
        p.packagesInstalled = ctx.packagesInstalled;
        applyFileRecovery(p, ctx);
        ctx.emit(p);
        return true;
    };

    // storage_write closes/flushed disk handles on every call. Coalesce the
    // small HTTP callbacks so large FAT32 downloads pay that cost per MiB.
    std::vector<uint8_t> pending;
    pending.reserve(kInstallBatchBytes);
    uint64_t inputCallbacks = 0;
    uint64_t diskBatches = 0;
    auto flushPending = [&] {
        if (pending.empty()) return true;
        if (!writeBatch(pending.data(), pending.size())) return false;
        ++diskBatches;
        pending.clear();
        return true;
    };
    auto sink = [&](const uint8_t* data, size_t n) {
        ++inputCallbacks;
        while (n) {
            const size_t count = std::min(n, kInstallBatchBytes - pending.size());
            pending.insert(pending.end(), data, data + count);
            data += count;
            n -= count;
            if (pending.size() == kInstallBatchBytes && !flushPending()) return false;
        }
        return true;
    };

    std::string fetchError;
    bool ok = fetchToDiskBuffered(ctx, url, offset, fileBytes, sink, fetchError);
    // Even a cancelled/failed fetch may have a valid tail worth preserving.
    if (sinkError.empty() && !flushPending()) ok = false;
    log_msg("[debrid] cached disk batches=%llu HTTP callbacks=%llu saved=%llu\n",
            static_cast<unsigned long long>(diskBatches),
            static_cast<unsigned long long>(inputCallbacks),
            static_cast<unsigned long long>(offset + written));
    if (!storage_flush(output)) {
        const int savedErrno = errno;
        sinkError = std::string("Unable to flush the large download: ") +
                    std::strerror(savedErrno ? savedErrno : EIO);
        diagnostic_error("storage", "debrid_flush", "file=%s error=%s",
                         leaf.c_str(), sinkError.c_str());
        ok = false;
    }
    const uint64_t committed = offset + written;
    if (ok && committed != fileBytes) {
        fetchError = committed < fileBytes
            ? "Download transferred a partial file."
            : "Downloaded file size mismatch.";
        ok = false;
    }
    if (ok)
        storage_finalize(output);
    storage_close(output);

    if (!ok) {
        std::string resumeError;
        if (!saveSplitResume(localPath, committed, resumeError)) {
            diagnostic_error("storage", "debrid_resume", "file=%s error=%s",
                             leaf.c_str(), resumeError.c_str());
            if (sinkError.empty()) sinkError = resumeError;
        }
        if (ctx.stop())
            return Step::Stopped;
        ctx.error = !sinkError.empty() ? sinkError
                  : !fetchError.empty() ? fetchError
                                        : "Download failed.";
        ctx.retryableFetchFailure = sinkError.empty() &&
                                    transientFetchError(ctx.error);
        return Step::Failed;
    }

    std::remove(splitResumePath(localPath).c_str());
    return Step::Ok;
}

Step fetchAppend(RunContext& ctx, const std::string& url,
                 const std::string& localPath, uint64_t offset,
                 uint64_t fileBytes, uint64_t baseCompleted,
                 bool refetchWhole = false) {
    ctx.retryableFetchFailure = false;
    const bool bytePoint = !refetchWhole && offset > 0 && offset < fileBytes;
    noteFileRecovery(ctx, bytePoint, refetchWhole ? fileBytes : offset,
                     fileBytes);
    if (needsSplitDownload(fileBytes, ctx.spec.forceSplitDownloads))
        return fetchSplitDownload(ctx, url, localPath, offset, fileBytes,
                                  baseCompleted);

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
    std::string sinkError;
    ThroughputMeter diskMeter("debrid_sd", ctx.spec.taskId);
    auto sink = [&](const uint8_t* data, size_t n) -> bool {
        const uint64_t started = telemetry_enabled() ? now_us() : 0;
        const size_t stored = std::fwrite(data, 1, n, out);
        diskMeter.record(stored, started ? now_us() - started : 0);
        if (stored != n) {
            const int savedErrno = errno;
            sinkError = std::string("Unable to write the download file: ") +
                        std::strerror(savedErrno ? savedErrno : EIO);
            diagnostic_error("storage", "debrid_write", "file=%s error=%s",
                             baseName(localPath).c_str(), sinkError.c_str());
            return false;
        }
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
        applyFileRecovery(p, ctx);
        ctx.emit(p);
        return true;
    };
    std::string err;
    bool ok = fetchToDiskBuffered(ctx, url, offset, fileBytes, sink, err);
    if (ok && std::fflush(out) != 0) {
        const int savedErrno = errno;
        sinkError = std::string("Unable to flush the download file: ") +
                    std::strerror(savedErrno ? savedErrno : EIO);
        diagnostic_error("storage", "debrid_flush", "file=%s error=%s",
                         baseName(localPath).c_str(), sinkError.c_str());
        ok = false;
    }
    if (std::fclose(out) != 0) {
        const int savedErrno = errno;
        sinkError = std::string("Unable to close the download file: ") +
                    std::strerror(savedErrno ? savedErrno : EIO);
        diagnostic_error("storage", "debrid_close", "file=%s error=%s",
                         baseName(localPath).c_str(), sinkError.c_str());
        ok = false;
    }
    if (!ok) {
        if (ctx.stop())
            return Step::Stopped;
        ctx.error = !sinkError.empty() ? sinkError
                                      : err.empty() ? "Download failed." : err;
        ctx.retryableFetchFailure = sinkError.empty() &&
                                    transientFetchError(ctx.error);
        return Step::Failed;
    }
    return Step::Ok;
}

Step downloadPlainFile(RunContext& ctx, size_t kthSelected,
                       const DebridFile& file, bool reconnect = false) {
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
    bool restarted = false;
    if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
        if (!splitDownloadOffset(localPath, file.bytes, existing, ctx.error,
                                 &restarted))
            return Step::Failed;
        if (existing == file.bytes && !splitResumeExists(localPath))
            return Step::Ok;
    } else {
        bool exists = statSize(localPath, existing);
        if (exists && existing == file.bytes)
            return Step::Ok;
        if (exists && existing > file.bytes) {
            std::ofstream truncate(localPath,
                                   std::ios::binary | std::ios::trunc);
            restarted = true;
            existing = 0;
        }
    }

    DebridInfo info;
    info.links = ctx.selectedLinks;
    std::string url;
    url = ctx.resolvedSourceUrl;
    if (url.empty() && !ctx.provider.resolveDownloadUrl(ctx.debridId, info, kthSelected,
                                         file, url, ctx.error))
        return Step::Failed;
    if (!linkAllowed(ctx.provider, url)) {
        ctx.error = "Debrid service returned a plaintext download link.";
        return Step::Failed;
    }

    auto outageStarted = Clock::time_point::min();
    for (int attempt = 0;; ++attempt) {
        uint64_t offset = 0;
        if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
            if (!splitDownloadOffset(localPath, file.bytes, offset, ctx.error,
                                     &restarted))
                return Step::Failed;
        } else {
            statSize(localPath, offset);
        }
        Step s = fetchAppend(ctx, url, localPath, offset, file.bytes,
                             ctx.completedSoFar, restarted);
        if (s == Step::Ok) {
            uint64_t received = 0;
            if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
                if (!splitDownloadOffset(localPath, file.bytes, received, ctx.error))
                    return Step::Failed;
            } else statSize(localPath, received);
            if (received == file.bytes) break;
            ctx.error = "Download transferred a partial file.";
            ctx.retryableFetchFailure = received < file.bytes;
            s = Step::Failed;
        }
        if (s == Step::Stopped)
            return Step::Stopped;
        if (!ctx.resolvedSourceUrl.empty() &&
            ctx.error.find("archive member failed") != std::string::npos) {
            // Never mistake a complete-sized corrupt cache for a successful
            // member on retry. Split storage overwrites from this marker.
            if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
                std::string ignored;
                saveSplitResume(localPath, 0, ignored);
            } else {
                std::ofstream discard(localPath, std::ios::binary | std::ios::trunc);
            }
            return Step::Failed;
        }
        if (attempt == 0 || (reconnect && ctx.retryableFetchFailure)) {
            if (attempt > 0) {
                const auto now = Clock::now();
                if (outageStarted == Clock::time_point::min()) outageStarted = now;
                if (now - outageStarted >= std::chrono::milliseconds(ctx.spec.reconnectTimeoutMs))
                    return Step::Failed;
                uint64_t savedBytes = 0;
                if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
                    if (!splitDownloadOffset(localPath, file.bytes, savedBytes, ctx.error))
                        return Step::Failed;
                } else statSize(localPath, savedBytes);
                log_msg("[debrid] reconnecting cached download at %llu bytes: %s\n",
                        static_cast<unsigned long long>(savedBytes), ctx.error.c_str());
                noteFileRecovery(ctx, savedBytes > 0, savedBytes, file.bytes);
                DebridProgress waiting;
                waiting.status = DownloadStatus::Downloading;
                waiting.completedBytes = ctx.completedSoFar + savedBytes;
                waiting.totalBytes = ctx.totalBytes;
                waiting.packagesInstalled = ctx.packagesInstalled;
                waiting.currentPackage = file.path;
                applyFileRecovery(waiting, ctx);
                ctx.emit(waiting);
                const auto until = now + std::chrono::milliseconds(ctx.spec.reconnectDelayMs);
                while (Clock::now() < until) {
                    if (ctx.stop()) return Step::Stopped;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (ctx.stop()) return Step::Stopped;
            }
            std::string fresh;
            fresh = ctx.resolvedSourceUrl;
            if (!fresh.empty() || ctx.provider.resolveDownloadUrl(ctx.debridId, info,
                                                kthSelected, file, fresh,
                                                ctx.error)) {
                if (!linkAllowed(ctx.provider, fresh)) {
                    ctx.error =
                        "Debrid service returned a plaintext download link.";
                    return Step::Failed;
                }
                url = fresh;
            }
            ctx.error.clear();
            continue;
        }
        return Step::Failed;
    }

    uint64_t finalSize = 0;
    if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
        if (!splitDownloadOffset(localPath, file.bytes, finalSize, ctx.error))
            return Step::Failed;
    } else {
        statSize(localPath, finalSize);
    }
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
                const uint64_t rateAtMs = ctx.packageDownloadSpeedAtMs.load();
                const uint64_t nowMs = now_ms();
                if (rateAtMs && nowMs >= rateAtMs && nowMs - rateAtMs <= 2000)
                    p.speedBytesPerSecond = ctx.packageDownloadSpeed.load();
                p.packagesInstalled = ctx.packagesInstalled;
                p.currentPackage = displayName;
                p.installedBytes = backend->installedBytes();
                p.installTotalBytes = backend->expectedBytes();
                if (!ctx.recoveryBytePoint && p.installedBytes > 0)
                    noteFileRecovery(ctx, false, p.installedBytes,
                                     p.installTotalBytes);
                applyFileRecovery(p, ctx);
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
    ctx.packageDownloadSpeed = 0;
    ctx.packageDownloadSpeedAtMs = 0;
    if (!ctx.backend)
        ctx.backend = install::createInstallBackend(ctx.spec.workingRoot,
                                                    ctx.spec.installTarget);
    install::InstallBackend* backend = ctx.backend.get();
    const std::string journalPath =
        install::installJournalPath(ctx.spec.workingRoot, ctx.spec.taskId);
    const std::string markerPath =
        install::installMarkerPath(ctx.spec.workingRoot, ctx.spec.taskId);
    constexpr uint64_t kJournalIntervalBytes = 32ull * 1024 * 1024;
    // F5: first safe point lands much earlier than the steady-state interval
    // (see PackageCoordinator::maybeCheckpoint).
    constexpr uint64_t kJournalFirstIntervalBytes = 4ull * 1024 * 1024;
    const bool compressed = isCompressedName(file.path);
    uint64_t fetchOffset = 0;
    uint64_t journalConsumed = 0;

    auto clearJournal = [&] {
        install::removeInstallJournal(journalPath);
        install::removeInstallMarker(markerPath);
        journalConsumed = 0;
    };

    const size_t maximumBuffered = debridQueueBytes();
    // The benchmark measures a complete local cache. A live HTTP producer
    // must begin installing its first MiB instead of waiting for a larger
    // batch (or EOF) while the source is slow or temporarily stalled.
    const size_t installBatch = std::clamp<size_t>(ctx.spec.installBatchBytes,
        256u << 10, ctx.localSource ? 4u << 20 : 1u << 20);
    InstallPacer pacer(maximumBuffered);
    pacer.beginPackage(compressed);
    // The bounded queue controls direct HTTP streaming, including NSZ, without
    // waiting for a WAN prebuffer or limiting writes to a measured source rate.
    // Complete local caches also need no network pacing.
    if (ctx.localSource || !compressed || !ctx.spec.cacheCompressedPackages)
        pacer.setSourceComplete(true);

    auto makeStream = [&] {
        return std::make_unique<install::PackageStream>(
            compressed, makeCallbacks(ctx, backend, displayName, pacer),
            ctx.spec.taskId);
    };
    std::unique_ptr<install::PackageStream> stream;

    // F5: refresh the install liveness marker from the current safe point.
    // A marker left on disk means the next session must report an unclean
    // shutdown of this install; its memory / target headroom is the
    // post-mortem the dying session could not write itself.
    auto writeMarker = [&](uint64_t consumed) {
        install::InstallMarker marker;
        marker.taskId = ctx.spec.taskId;
        marker.packageId = file.path;
        marker.packageSize = file.bytes;
        marker.consumed = consumed;
        const StreamRamMemorySnapshot memory = detectStreamRamMemorySnapshot();
        marker.heapAvailableBytes = memory.heapDetected
            ? memory.heapAvailableBytes : 0;
        marker.kernelHeadroomBytes = memory.kernelHeadroomDetected
            ? memory.kernelHeadroomBytes : 0;
        marker.storageFreeBytes = backend ? backend->freeSpaceBytes() : 0;
        std::string markerError;
        if (!install::saveInstallMarker(markerPath, marker, &markerError)) {
            log_msg("[debrid] marker write failed '%s': %s\n",
                    markerPath.c_str(),
                    markerError.empty() ? "unknown error"
                                        : markerError.c_str());
        }
    };

    install::InstallJournal saved;
    bool resumed = false;
    if (install::loadInstallJournal(journalPath, saved)) {
        if (saved.packageId == file.path && saved.packageSize == file.bytes &&
            saved.compressed == compressed && saved.state.consumed > 0 &&
            saved.state.consumed < file.bytes && !saved.backendState.empty() &&
            backend->resumePackage(ctx.spec.taskId, displayName,
                                   saved.backendState)) {
            stream = makeStream();
            if (stream->restore(saved.state)) {
                resumed = true;
                fetchOffset = saved.state.consumed;
                journalConsumed = saved.state.consumed;
                ctx.packageDownloadedBytes = fetchOffset;
                log_msg("[debrid] resuming package='%s' at %llu of %llu bytes\n",
                        file.path.c_str(),
                        (unsigned long long)saved.state.consumed,
                        (unsigned long long)file.bytes);
            } else {
                backend->rollbackPackage();
                clearJournal();
                stream.reset();
            }
        } else {
            clearJournal();
        }
    }
    if (resumed)
        noteFileRecovery(ctx, true, journalConsumed, file.bytes);
    else
        noteFileRecovery(ctx, false, 0, file.bytes);
    if (resumed)
        writeMarker(journalConsumed);

    // Start the HTTP reader before beginPackage so TorrServer's sequential
    // cache and a WAN CDN handshake run during ncm/FS setup instead of after.
    DebridStreamQueue queue(maximumBuffered, pacer, *ctx.shouldStop, ctx.spec.taskId);
    telemetry_log("debrid_package", ctx.spec.taskId.c_str(),
        "event=start compressed=%d local_source=%d cache_compressed=%d package_bytes=%llu resume_offset=%llu queue_bytes=%zu batch_bytes=%zu workers=%u",
        compressed ? 1 : 0, ctx.localSource ? 1 : 0,
        ctx.spec.cacheCompressedPackages ? 1 : 0,
        (unsigned long long)file.bytes, (unsigned long long)fetchOffset,
        maximumBuffered, installBatch,
        sequentialHttp(ctx.provider) ? 1u : kRangeWorkers);
    std::string fetchError;
    bool fetchOk = false;
    const uint64_t producerOffset = fetchOffset;
    std::thread producer([&] {
        uint64_t rateStartedMs = now_ms();
        uint64_t rateBytes = 0;
        auto sink = [&](const uint8_t* data, size_t n) -> bool {
            ctx.packageDownloadedBytes.fetch_add(n);
            rateBytes += n;
            const uint64_t nowMs = now_ms();
            if (nowMs >= rateStartedMs && nowMs - rateStartedMs >= 500) {
                ctx.packageDownloadSpeed = rateBytes * 1000 / (nowMs - rateStartedMs);
                ctx.packageDownloadSpeedAtMs = nowMs;
                rateBytes = 0;
                rateStartedMs = nowMs;
            }
            return queue.push(data, n);
        };
        fetchOk = sequentialHttp(ctx.provider)
            ? fetchSequential(ctx.fetcher, url, producerOffset, sink,
                              *ctx.shouldStop, fetchError)
            : fetchOrdered(ctx.fetcher, url, producerOffset, file.bytes,
                           maximumBuffered / 2, sink, *ctx.shouldStop,
                           fetchError);
        // The installer can keep draining the buffer after HTTP has finished.
        ctx.packageDownloadSpeed = 0;
        ctx.packageDownloadSpeedAtMs = 0;
        queue.finish();
    });

    if (!resumed) {
        if (!backend->beginPackage(ctx.spec.taskId, displayName)) {
            queue.stop();
            producer.join();
            ctx.error = backend->error();
            return Step::Failed;
        }
        stream = makeStream();
        // F5: the stream is live from here; record the session before the
        // first byte so a reboot at any point is attributable.
        writeMarker(0);
    }

    auto maybeCheckpoint = [&](bool force) {
        if (!stream)
            return;
        const uint64_t consumed = stream->consumed();
        const uint64_t interval = journalConsumed == 0
            ? kJournalFirstIntervalBytes : kJournalIntervalBytes;
        if (!force && consumed < journalConsumed + interval)
            return;
        if (force && consumed <= journalConsumed)
            return;
        install::InstallJournal journal;
        if (!stream->checkpoint(journal.state) || journal.state.consumed == 0)
            return;
        journal.backendState = backend->checkpointPackage();
        if (journal.backendState.empty())
            return;
        journal.packageId = file.path;
        journal.packageSize = file.bytes;
        journal.compressed = compressed;
        std::string journalError;
        if (!install::saveInstallJournal(journalPath, journal, &journalError)) {
            log_msg("[debrid] journal write failed '%s': %s\n",
                    journalPath.c_str(),
                    journalError.empty() ? "unknown error"
                                         : journalError.c_str());
            return;
        }
        journalConsumed = consumed;
        noteFileRecovery(ctx, true, journalConsumed, file.bytes);
        // Same safe point as the journal, so the startup report names the
        // exact byte the resumed install continues from.
        writeMarker(journal.state.consumed);
    };

    bool streamOk = true;
    std::vector<uint8_t> chunk;
    std::vector<uint8_t> batch;
    batch.reserve(installBatch);
    ThroughputMeter streamMeter("debrid_stream", ctx.spec.taskId);
    auto writeStream = [&](const uint8_t* data, size_t size) {
        const uint64_t started = telemetry_enabled() ? now_us() : 0;
        const bool ok = stream->write(data, size);
        streamMeter.record(ok ? size : 0, started ? now_us() - started : 0);
        return ok;
    };
    auto flushBatch = [&]() -> bool {
        if (batch.empty())
            return true;
        if (!writeStream(batch.data(), batch.size())) {
            streamOk = false;
            queue.stop();
            batch.clear();
            return false;
        }
        pacer.observeConsumed(stream->consumed(), backend->installedBytes(),
                              now_ms());
        maybeCheckpoint(false);
        batch.clear();
        return true;
    };
    while (queue.pop(chunk)) {
        if (batch.empty() && chunk.size() >= installBatch) {
            if (!writeStream(chunk.data(), chunk.size())) {
                streamOk = false;
                queue.stop();
                break;
            }
            pacer.observeConsumed(stream->consumed(), backend->installedBytes(),
                                  now_ms());
            maybeCheckpoint(false);
        } else {
            batch.insert(batch.end(), chunk.begin(), chunk.end());
            if (batch.size() >= installBatch && !flushBatch())
                break;
        }
    }
    if (streamOk)
        flushBatch();
    queue.stop();
    producer.join();
    if (fetchOk && streamOk && ctx.packageDownloadedBytes.load() < file.bytes) {
        fetchOk = false;
        fetchError = "Package download ended before its expected size.";
    }
    if (!fetchOk || !streamOk) {
        if (ctx.stop()) {
            maybeCheckpoint(true);
            backend->suspendPackage();
            install::removeInstallMarker(markerPath);
            return Step::Stopped;
        }
        // A disconnected HTTP stream is not a corrupt package. Preserve its
        // last durable parser/backend checkpoint for URL retry or wake/resume.
        // Parser and storage errors must still discard untrusted partial data.
        if (!fetchOk && streamOk) {
            maybeCheckpoint(true);
            if (journalConsumed > 0) {
                backend->suspendPackage();
                install::removeInstallMarker(markerPath);
                log_msg("[debrid] keeping package checkpoint at %llu bytes "
                        "after interrupted download\n",
                        static_cast<unsigned long long>(journalConsumed));
            } else {
                backend->rollbackPackage();
                clearJournal();
            }
        } else {
            backend->rollbackPackage();
            clearJournal();
        }
        if (ctx.error.empty())
            ctx.error = !stream->error().empty()
                ? describeStreamError(file, displayName, stream->error())
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
    applyFileRecovery(committing, ctx);
    ctx.emit(committing);

    if (!stream->finish()) {
        ctx.error = stream->error().empty()
            ? "Package finalize failed."
            : describeStreamError(file, displayName, stream->error());
        backend->rollbackPackage();
        clearJournal();
        return Step::Failed;
    }
    bool alreadyInstalled = false;
    if (!backend->commitPackage(alreadyInstalled)) {
        ctx.error = backend->error();
        backend->rollbackPackage();
        clearJournal();
        return Step::Failed;
    }
    clearJournal();
    ctx.packagesInstalled += 1;
    noteFileRecovery(ctx, false, 0, 0);
    DebridProgress done;
    done.status = DownloadStatus::Installing;
    done.totalBytes = ctx.totalBytes;
    done.completedBytes = ctx.completedSoFar + file.bytes;
    done.packagesInstalled = ctx.packagesInstalled;
    done.currentPackage = displayName;
    applyFileRecovery(done, ctx);
    ctx.emit(done);
    return Step::Ok;
}

Step streamInstallPackage(RunContext& ctx, size_t kthSelected,
                          const DebridFile& file) {
    bool useCache = ctx.spec.cacheCompressedPackages;
    if (isPackageName(file.path) && !useCache) {
        // Do not discard partial/complete NSZ downloads made by the earlier
        // cached build. Only fresh packages return to direct HTTP installation.
        bool safe = false;
        const std::string relative = sanitizeRelative(file.path, ctx.torrentName, safe);
        const std::string path = ctx.spec.dataPath + "/" + relative;
        struct stat st{};
        useCache = safe && stat(path.c_str(), &st) == 0 &&
                   (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
    }
    if (isPackageName(file.path) && useCache) {
        // Solid NCZ has no mid-frame decompressor checkpoint. Cache the HTTP
        // input first, so restarting decompression never restarts its download.
        const Step downloaded = downloadPlainFile(ctx, kthSelected, file, true);
        if (downloaded != Step::Ok) return downloaded;
        bool safe = false;
        const std::string relative = sanitizeRelative(file.path, ctx.torrentName, safe);
        if (!safe) { ctx.error = "Unsafe cached package path."; return Step::Failed; }
        const std::string path = ctx.spec.dataPath + "/" + relative;
        const std::string leaf = baseName(path);
        if (leaf.size() >= MAX_NAME_LEN) {
            ctx.error = "Cached package file name is too long.";
            return Step::Failed;
        }
        RangeFetcher local = [path, leaf, bytes = file.bytes](
            const std::string&, uint64_t offset, uint64_t end,
            const std::function<bool(const uint8_t*, size_t)>& sink,
            const std::function<bool()>& stopped, std::string& error) {
            if (offset > bytes || (end && (end < offset || end > bytes))) {
                error = "Invalid cached package range."; return false;
            }
            mi_file_t inputFile {};
            std::snprintf(inputFile.path, sizeof(inputFile.path), "%s", leaf.c_str());
            inputFile.length = static_cast<int64_t>(bytes);
            metainfo_t meta {};
            std::snprintf(meta.name, sizeof(meta.name), "%s", leaf.c_str());
            meta.total_length = inputFile.length;
            meta.piece_length = 1024 * 1024;
            meta.num_files = 1;
            meta.files = &inputFile;
            const std::string directory = parentDir(path);
            storage_t* input = storage_open(&meta, directory.c_str());
            if (!input) { error = "Unable to open cached package."; return false; }
            const uint64_t limit = end ? end : bytes;
            std::vector<uint8_t> buffer(1024 * 1024);
            bool ok = true;
            while (offset < limit && !stopped()) {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), limit - offset));
                const int got = storage_read(input, static_cast<int64_t>(offset), buffer.data(), count);
                if (got != static_cast<int>(count) || !sink(buffer.data(), count)) {
                    error = "Unable to read cached package."; ok = false; break;
                }
                offset += count;
            }
            storage_close(input);
            return ok && !stopped() && offset == limit;
        };
        std::function<void(const DebridProgress&)> progress = [&ctx, &file](const DebridProgress& sample) {
            DebridProgress p = sample;
            p.completedBytes = ctx.completedSoFar + file.bytes;
            p.speedBytesPerSecond = 0; // Local reads are not network traffic.
            p.hasRecovery = true;
            p.recoveryBytePoint = true;
            p.recoveryUnitBytes = p.recoveryUnitTotal = file.bytes;
            ctx.emit(p);
        };
        RunContext cached{ctx.provider, local, ctx.spec, ctx.shouldStop, &progress};
        cached.totalBytes = ctx.totalBytes;
        cached.completedSoFar = ctx.completedSoFar;
        cached.packagesInstalled = ctx.packagesInstalled;
        cached.localSource = true;
        cached.backend = std::move(ctx.backend);
        log_msg("[debrid] installing cached package '%s' (%llu bytes); no network fetch\n",
                file.path.c_str(), static_cast<unsigned long long>(file.bytes));
        const Step installed = attemptStreamInstall(cached, file, "cached-package", file.path);
        ctx.backend = std::move(cached.backend);
        ctx.packagesInstalled = cached.packagesInstalled;
        ctx.error = cached.error;
        return installed;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        DebridInfo info;
        info.links = ctx.selectedLinks;
        std::string url;
        url = ctx.resolvedSourceUrl;
        if (url.empty() && !ctx.provider.resolveDownloadUrl(ctx.debridId, info,
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


bool completeCachedFile(const RunContext& ctx, const DebridFile& file) {
    bool safe = false;
    const std::string relative = sanitizeRelative(file.path, ctx.torrentName, safe);
    if (!safe) return false;
    const std::string path = ctx.spec.dataPath + "/" + relative;
    uint64_t bytes = 0;
    if (statSize(path, bytes)) return bytes == file.bytes;
    if (needsSplitDownload(file.bytes, ctx.spec.forceSplitDownloads)) {
        std::string ignored;
        return splitDownloadOffset(path, file.bytes, bytes, ignored) &&
               bytes == file.bytes && !splitResumeExists(path);
    }
    return false;
}

struct GroupedSource {
    std::shared_ptr<RemoteArchive> archive;
    size_t entry = 0;
};

Step prepareGroupedSources(RunContext& ctx, const std::vector<size_t>& order,
                           std::map<size_t, GroupedSource>& sources) {
    auto archivePath = [&](std::string path, bool& safe) {
        // RD paths start with '/', ZIP member paths normally do not. Compare
        // both with the same root normalization without changing cache paths
        // recorded by older manifests.
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
        return sanitizeRelative(path, ctx.torrentName, safe);
    };
    std::vector<size_t> needed;
    uint32_t ordinal = 0;
    for (size_t index : order) {
        const bool package = installs(ctx.spec, index, ctx.files[index]);
        const bool installed = package && ordinal++ < ctx.spec.packagesInstalled;
        if (!installed && !completeCachedFile(ctx, ctx.files[index])) needed.push_back(index);
    }
    if (needed.empty()) return Step::Ok;
    DebridInfo info;
    info.links = ctx.selectedLinks;
    if (info.links.empty()) {
        ctx.error = "Real-Debrid completed the torrent without publishing a download link.";
        return Step::Failed;
    }
    // More than one standalone ZIP link is allowed. A split-volume archive
    // fails explicitly instead of starting another cloud download.
    for (size_t k = 0; k < info.links.size(); ++k) {
        if (ctx.stop()) return Step::Stopped;
        std::string url;
        uint64_t bytes = 0;
        if (!ctx.provider.resolveGroupedDownloadUrl(info, k, url, bytes, ctx.error))
            return Step::Failed;
        auto archive = std::make_shared<RemoteArchive>(url, bytes, ctx.fetcher);
        if (!archive->open(*ctx.shouldStop, ctx.error))
            return ctx.stop() ? Step::Stopped : Step::Failed;
        log_msg("[debrid] cached grouped archive id=%s link=%zu bytes=%llu members=%zu format=%s; no new torrent\n",
                ctx.debridId.c_str(), k, (unsigned long long)bytes, archive->entries().size(), archive->format());
        for (size_t index : needed) {
            bool safe = false;
            const auto& file = ctx.files[index];
            const std::string wanted = archivePath(file.path, safe);
            if (!safe) { ctx.error = "Unsafe selected grouped file path."; return Step::Failed; }
            for (size_t e = 0; e < archive->entries().size(); ++e) {
                const auto& member = archive->entries()[e];
                if (member.bytes != file.bytes) continue;
                bool memberSafe = false;
                const std::string relative = archivePath(member.path, memberSafe);
                // Some cloud ZIPs add one enclosing folder. Keep the entire
                // wanted path; never identify nested files by basename alone.
                const std::string suffix = "/" + wanted;
                if (!memberSafe || (relative != wanted &&
                    (relative.size() <= suffix.size() ||
                     relative.compare(relative.size() - suffix.size(), suffix.size(), suffix) != 0))) continue;
                if (sources.count(index)) {
                    ctx.error = "Grouped archive contains ambiguous paths for '" + wanted + "'.";
                    return Step::Failed;
                }
                if (!archive->prepare(e, *ctx.shouldStop, ctx.error))
                    return ctx.stop() ? Step::Stopped : Step::Failed;
                log_msg("[debrid] grouped member selected path='%s' bytes=%llu method=%u link=%zu\n",
                        wanted.c_str(), (unsigned long long)member.bytes, member.method, k);
                sources.emplace(index, GroupedSource{archive, e});
            }
        }
    }
    if (sources.size() != needed.size()) {
        ctx.error = "Real-Debrid grouped download does not contain every selected file with its expected path and size.";
        return Step::Failed;
    }
    return Step::Ok;
}

Step transferGroupedFile(RunContext& ctx, const GroupedSource& source,
                         const DebridFile& file, bool package) {
    RangeFetcher member = [source](const std::string&, uint64_t offset, uint64_t end,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        const std::function<bool()>& stopped, std::string& error) {
        return source.archive->read(source.entry, offset, end, sink, stopped, error);
    };
    RunContext nested{ctx.provider, member, ctx.spec, ctx.shouldStop, ctx.progress};
    nested.debridId = ctx.debridId;
    nested.torrentName = ctx.torrentName;
    nested.totalBytes = ctx.totalBytes;
    nested.completedSoFar = ctx.completedSoFar;
    nested.packagesInstalled = ctx.packagesInstalled;
    // Used only to bypass per-file unrestriction. The member fetcher owns the
    // actual credential-bearing URL, which is never written to logs/state.
    nested.resolvedSourceUrl = "https://grouped-member.invalid/";
    nested.backend = std::move(ctx.backend);
    const Step result = package ? streamInstallPackage(nested, 0, file)
                                : downloadPlainFile(nested, 0, file);
    ctx.backend = std::move(nested.backend);
    ctx.packagesInstalled = nested.packagesInstalled;
    ctx.error = nested.error;
    return result;
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
        starting.packagesInstalled = ctx.packagesInstalled;
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

    bool hadNonEmptySelection = !spec.selectionPaths.empty() || !spec.selectionRelativePaths.empty();
    if (!hadNonEmptySelection && !spec.fileSelection.empty()) {
        for (uint8_t v : spec.fileSelection)
            if (v) { hadNonEmptySelection = true; break; }
    }

    uint64_t selectedCount = 0;
    for (size_t i = 0; i < ctx.files.size(); ++i) {
        if (selected(spec, i, ctx.files[i], ctx.torrentName)) {
            ctx.totalBytes += ctx.files[i].bytes;
            ++selectedCount;
        }
    }

    if (hadNonEmptySelection && selectedCount == 0) {
        error = "No debrid files matched the selected files.";
        return DebridRunResult::Failed;
    }

    uint32_t packageOrdinal = 0;
    const std::vector<size_t> order = selectedFileOrder(spec, ctx.files, ctx.torrentName);
    std::map<size_t, GroupedSource> groupedSources;
    if (ctx.groupedDownload) {
        const Step prepared = prepareGroupedSources(ctx, order, groupedSources);
        if (prepared == Step::Stopped) return DebridRunResult::Stopped;
        if (prepared == Step::Failed) { error = ctx.error; return DebridRunResult::Failed; }
    }
    for (size_t position = 0; position < order.size(); ++position) {
        const size_t i = order[position];
        if (ctx.stop())
            return DebridRunResult::Stopped;
        const DebridFile& file = ctx.files[i];
        size_t kthSelected = kthSelectedIndex(spec, ctx.files, i, ctx.torrentName);
        if (std::strcmp(ctx.provider.name(), "realdebrid") == 0) {
            // Ready account torrents can include files this task does not
            // want. Individual links follow SERVER selection order.
            kthSelected = 0;
            for (size_t j = 0; j < i; ++j)
                if (ctx.files[j].serverSelected == 1) ++kthSelected;
        }
        Step fs = Step::Ok;
        if (installs(ctx.spec, i, file)) {
            if (packageOrdinal >= spec.packagesInstalled) {
                if (groupedSources.count(i)) {
                    fs = transferGroupedFile(ctx, groupedSources.at(i), file, true);
                } else {
                    fs = streamInstallPackage(ctx, kthSelected, file);
                }
            } else {
                log_msg("[debrid] resume skip committed package task=%s ordinal=%u path='%s' bytes=%llu\n",
                        spec.taskId.c_str(), packageOrdinal, file.path.c_str(),
                        static_cast<unsigned long long>(file.bytes));
            }
            ++packageOrdinal;
        } else {
            if (groupedSources.count(i)) {
                fs = transferGroupedFile(ctx, groupedSources.at(i), file, false);
            } else {
                fs = downloadPlainFile(ctx, kthSelected, file);
            }
        }
        if (fs == Step::Stopped)
            return DebridRunResult::Stopped;
        if (fs == Step::Failed) {
            if (std::strcmp(ctx.provider.name(), "realdebrid") != 0 &&
                spec.mode == TransferMode::StreamInstall &&
                ctx.packagesInstalled > 0 && !installs(spec, i, file)) {
                bool morePackages = false;
                for (size_t next = position + 1; next < order.size(); ++next) {
                    const size_t j = order[next];
                    if (installs(spec, j, ctx.files[j])) {
                        morePackages = true;
                        break;
                    }
                }
                if (!morePackages)
                    break;
            }
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
    remTorrserverIfFinished(ctx.provider, ctx.debridId,
                            DebridRunResult::Finished);
    return DebridRunResult::Finished;
}

} // namespace pipensx
