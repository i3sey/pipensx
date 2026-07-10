#include "download_manager.hpp"
#include "request_gate.hpp"
#include "stream_ram_budget.hpp"
#include "../install/install_backend.hpp"
#include "../install/install_journal.hpp"
#include "../install/package_stream.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/metainfo.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace pipensx {
namespace {

bool hasPackageExtension(const std::string& path) {
    std::string lower = path;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.size() >= 4 &&
           (lower.substr(lower.size() - 4) == ".nsp" ||
            lower.substr(lower.size() - 4) == ".nsz");
}

bool isCompressedPackage(const std::string& path) {
    std::string lower = path;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.size() >= 4 &&
           lower.substr(lower.size() - 4) == ".nsz";
}

bool isCartridgeDump(const std::string& path) {
    std::string lower = path;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.size() >= 4 &&
           (lower.substr(lower.size() - 4) == ".xci" ||
            lower.substr(lower.size() - 4) == ".xcz");
}

uint8_t actionValue(FileAction action) {
    return static_cast<uint8_t>(action);
}

bool isValidFileAction(uint8_t value) {
    return value == actionValue(FileAction::Skip) ||
           value == actionValue(FileAction::Download) ||
           value == actionValue(FileAction::Install);
}

FileAction defaultActionFor(const TorrentPreview::File& file,
                            TransferMode mode) {
    return mode == TransferMode::StreamInstall && file.package
        ? FileAction::Install
        : FileAction::Download;
}

std::vector<uint8_t> actionsFromLegacySelection(
    const TorrentPreview& preview,
    TransferMode mode,
    const std::vector<uint8_t>& selectedFiles) {
    std::vector<uint8_t> actions;
    actions.reserve(preview.files.size());
    const bool useSelection = !selectedFiles.empty();
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const bool selected = !useSelection || selectedFiles[i] != 0;
        if (!selected) {
            actions.push_back(actionValue(FileAction::Skip));
        } else {
            actions.push_back(actionValue(defaultActionFor(preview.files[i],
                                                          mode)));
        }
    }
    return actions;
}

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

bool copyFile(const std::string& source, const std::string& destination) {
    std::ifstream input(source, std::ios::binary);
    if (!input)
        return false;
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << input.rdbuf();
    output.flush();
    return input.good() || input.eof() ? output.good() : false;
}

bool removeTree(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path.c_str()) == 0;

    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        std::string child = path + "/" + entry->d_name;
        if (!removeTree(child))
            ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
}

std::string safeComponent(const std::string& name) {
    std::string result;
    result.reserve(std::min<size_t>(name.size(), 64));
    for (unsigned char c : name) {
        if (result.size() >= 64)
            break;
        if (std::isalnum(c) || c == '-' || c == '_')
            result.push_back(static_cast<char>(c));
        else if (c == ' ' || c == '.')
            result.push_back('_');
    }
    if (result.empty())
        result = "download";
    return result;
}

bool isManagedChild(const std::string& root, const std::string& path) {
    std::string prefix = root + "/";
    if (path.rfind(prefix, 0) != 0)
        return false;
    std::string child = path.substr(prefix.size());
    return !child.empty() && child.find('/') == std::string::npos &&
           child != "." && child != "..";
}

std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

std::string bint(uint64_t value) {
    return "i" + std::to_string(value) + "e";
}

bool dictionaryString(const be_node_t& dict, const char* key,
                      std::string& value) {
    be_node_t node;
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &node) ||
        node.type != BE_STR)
        return false;
    value.assign(node.sval, node.slen);
    return true;
}

bool dictionaryInteger(const be_node_t& dict, const char* key,
                       uint64_t& value) {
    be_node_t node;
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &node) ||
        node.type != BE_INT || node.ival < 0)
        return false;
    value = static_cast<uint64_t>(node.ival);
    return true;
}

// IMPROVEMENT_PLAN F-B: one journal file per task next to the queue state.
std::string installJournalPath(const std::string& root,
                               const std::string& taskId) {
    return root + "/install-journal-" + taskId + ".bencode";
}

void upgradeLegacySelection(DownloadTask& task) {
    if (task.fileSelection.empty())
        return;
    metainfo_t metainfo;
    if (!metainfo_load(task.metainfoPath.c_str(), &metainfo))
        return;
    if (task.fileSelection.size() == metainfo.num_files) {
        std::vector<uint8_t> actions;
        actions.reserve(task.fileSelection.size());
        for (uint32_t i = 0; i < metainfo.num_files; ++i) {
            if (task.fileSelection[i] == 0) {
                actions.push_back(actionValue(FileAction::Skip));
            } else if (task.mode == TransferMode::StreamInstall &&
                       hasPackageExtension(metainfo.files[i].path)) {
                actions.push_back(actionValue(FileAction::Install));
            } else {
                actions.push_back(actionValue(FileAction::Download));
            }
        }
        task.fileSelection = std::move(actions);
    }
    metainfo_free(&metainfo);
}

class PackageCoordinator {
public:
    using Progress = std::function<void(
        uint32_t, const std::string&, uint64_t, uint64_t, DownloadStatus)>;

    PackageCoordinator(const metainfo_t& metainfo, std::string taskId,
                       const std::string& workingRoot, bool streamInstall,
                       const std::vector<uint8_t>& fileSelection,
                       uint32_t completedPackages,
                       install::InstallStorageTarget installTarget,
                       Progress progress)
        : metainfo_(metainfo), taskId_(std::move(taskId)),
          backend_(streamInstall
                       ? install::createInstallBackend(workingRoot, installTarget)
                       : nullptr),
          streamInstall_(streamInstall),
          completedPackages_(completedPackages),
          initialCompletedPackages_(completedPackages),
          producerOrdinal_(completedPackages),
          progress_(std::move(progress)) {
        bool useSelection = !fileSelection.empty();
        if (useSelection && fileSelection.size() != metainfo_.num_files) {
            error_ = "Selected file actions do not match the torrent.";
            return;
        }
        configs_.resize(metainfo_.num_files);
        uint32_t ordinal = 0;
        for (uint32_t i = 0; i < metainfo_.num_files; ++i) {
            FileAction action = streamInstall_ &&
                                      hasPackageExtension(metainfo_.files[i].path)
                                  ? FileAction::Install
                                  : FileAction::Download;
            if (useSelection) {
                if (!isValidFileAction(fileSelection[i])) {
                    error_ = "Selected file action is invalid.";
                    return;
                }
                action = static_cast<FileAction>(fileSelection[i]);
            }
            if (action == FileAction::Skip) {
                configs_[i].mode = STORAGE_FILE_SKIP;
                continue;
            }
            if (action == FileAction::Download) {
                configs_[i].mode = STORAGE_FILE_DISK;
                continue;
            }
            if (!streamInstall_ ||
                !hasPackageExtension(metainfo_.files[i].path)) {
                error_ = "Only NSP/NSZ package files can be installed.";
                return;
            }
            packageOrdinals_[i] = ordinal;
            configs_[i].mode = ordinal < completedPackages_
                             ? STORAGE_FILE_SKIP : STORAGE_FILE_SINK;
            configs_[i].sink = &PackageCoordinator::sinkThunk;
            configs_[i].user = this;
            ++ordinal;
        }
        packageCount_ = ordinal;
        pieceLengthBytes_ = metainfo_.piece_length > 0
            ? static_cast<uint64_t>(metainfo_.piece_length)
            : 4 * 1024 * 1024;
        buildPieceOrder();
        if (streamInstall_ && error_.empty() && packageCount_ > completedPackages_) {
            journalPath_ = installJournalPath(workingRoot, taskId_);
            tryResume();
            StreamRamBudget budget = detectStreamRamBudget(pieceLengthBytes_);
            if (!budget.valid) {
                log_msg("[install] RAM budget rejected source=%s available=%llu "
                        "reserve=%llu piece=%llu kernel_headroom=%llu "
                        "kernel_detected=%d\n",
                        budget.memoryDetected ? "heap" : "fallback",
                        static_cast<unsigned long long>(budget.availableBytes),
                        static_cast<unsigned long long>(budget.reserveBytes),
                        static_cast<unsigned long long>(pieceLengthBytes_),
                        static_cast<unsigned long long>(
                            budget.kernelHeadroomBytes),
                        budget.kernelHeadroomDetected ? 1 : 0);
                telemetry_log("ram_budget", taskId_.c_str(),
                    "valid=0 source=%s available_bytes=%llu reserve_bytes=%llu "
                    "piece_bytes=%llu kernel_headroom_bytes=%llu "
                    "kernel_headroom_detected=%d",
                    budget.memoryDetected ? "heap" : "fallback",
                    static_cast<unsigned long long>(budget.availableBytes),
                    static_cast<unsigned long long>(budget.reserveBytes),
                    static_cast<unsigned long long>(pieceLengthBytes_),
                    static_cast<unsigned long long>(
                        budget.kernelHeadroomBytes),
                    budget.kernelHeadroomDetected ? 1 : 0);
                error_ = "Not enough free memory for stream installation.";
                return;
            }
            maxQueuedBytes_ = budget.maxQueuedBytes;
            maxBufferedBytes_ = budget.maxBufferedBytes;
            requestAheadBytes_ = budget.requestAheadBytes;
            lookaheadMin_ = budget.lookaheadMin;
            lookaheadMax_ = budget.lookaheadMax;
            lookaheadWindow_ = budget.lookaheadStart;
            lookaheadHealthy_ = budget.lookaheadStart;
            requestGate_.configure(maxBufferedBytes_, requestAheadBytes_,
                                   pieceLengthBytes_, producerOrdinal_);
            log_msg("[install] RAM budget source=%s available=%llu reserve=%llu "
                    "piece=%llu kernel_headroom=%llu peak=%llu reorder=%zu "
                    "queue=%zu lookahead=%u/%u/%u\n",
                    budget.memoryDetected ? "heap" : "fallback",
                    static_cast<unsigned long long>(budget.availableBytes),
                    static_cast<unsigned long long>(budget.reserveBytes),
                    static_cast<unsigned long long>(pieceLengthBytes_),
                    static_cast<unsigned long long>(
                        budget.kernelHeadroomBytes),
                    static_cast<unsigned long long>(budget.peakBytes),
                    maxBufferedBytes_, maxQueuedBytes_, lookaheadMin_,
                    lookaheadWindow_, lookaheadMax_);
            telemetry_log("ram_budget", taskId_.c_str(),
                "valid=1 source=%s available_bytes=%llu reserve_bytes=%llu "
                "piece_bytes=%llu kernel_headroom_bytes=%llu "
                "kernel_headroom_detected=%d peak_bytes=%llu "
                "reorder_bytes=%zu queue_bytes=%zu lookahead_min=%u "
                "lookahead_start=%u lookahead_max=%u",
                budget.memoryDetected ? "heap" : "fallback",
                static_cast<unsigned long long>(budget.availableBytes),
                static_cast<unsigned long long>(budget.reserveBytes),
                static_cast<unsigned long long>(pieceLengthBytes_),
                static_cast<unsigned long long>(budget.kernelHeadroomBytes),
                budget.kernelHeadroomDetected ? 1 : 0,
                static_cast<unsigned long long>(budget.peakBytes),
                maxBufferedBytes_, maxQueuedBytes_, lookaheadMin_,
                lookaheadWindow_, lookaheadMax_);
            installWorker_ = std::thread(&PackageCoordinator::installMain, this);
        }
    }

    ~PackageCoordinator() {
        cancel();
        if (!backend_)
            return;
        // F-B: an interruption with a journaled safe point keeps the partial
        // install on disk for a later resume; anything else rolls back and
        // drops the journal.
        if (journalValid_ && !abandonResume_ && error().empty()) {
            backend_->suspendPackage();
            return;
        }
        backend_->rollbackPackage();
        if (!journalPath_.empty())
            install::removeInstallJournal(journalPath_);
    }

    // The task is going away: never keep partial data or the journal.
    void abandonResume() { abandonResume_ = true; }

    const std::vector<storage_file_config_t>& configs() const {
        return configs_;
    }
    const std::vector<uint32_t>& pieceOrder() const { return pieceOrder_; }
    uint32_t packageCount() const { return packageCount_; }
    uint32_t initialLookahead() const { return lookaheadWindow_; }
    static int requestAllowedThunk(void* user, uint32_t piece) {
        return static_cast<PackageCoordinator*>(user)->canRequestPiece(piece)
            ? 1 : 0;
    }

    // Adaptive strict-order lookahead (PERF_PLAN 5.1). AIMD driven by the
    // install sink: a request-gate pause (PERF_PLAN 5.3) or a nearly full
    // buffer halves the window (the sink is the bottleneck, a wide window
    // only builds an avalanche), a comfortably empty buffer grows it
    // additively so more of the swarm is usable. The band between the two
    // thresholds holds the window steady, which keeps the loop from
    // oscillating. The lower clamp scales with the live swarm (PERF_PLAN
    // 7.3): each active peer needs a couple of pieces in the window to be
    // schedulable at all, so sustained pressure narrows the window to
    // 2*active instead of starving inflight to single blocks. Rate-limited
    // internally; callers may invoke it every tick.
    uint32_t adaptiveLookahead(uint32_t activePeers) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        uint64_t now = now_ms();
        // Heartbeat for the rate-matched gate (PERF_PLAN 7.1): the manager
        // loop calls this every tick, so the token bucket keeps refilling
        // even when no sink or install events fire (rx lull).
        updateRequestGateLocked(now);
        // While the gate is hard-paused no requests flow at all, so buffer
        // state carries no signal about the swarm — hold the window instead
        // of grinding it to the minimum (PERF_PLAN 7.2). The pause
        // transition already recorded one stall event, which still shrinks
        // the window once after resume.
        if (requestGate_.paused())
            return lookaheadWindow_;
        if (!lookaheadLastAdaptMs_) {
            lookaheadLastAdaptMs_ = now;
            return lookaheadWindow_;
        }
        if (now - lookaheadLastAdaptMs_ < kLookaheadAdaptIntervalMs)
            return lookaheadWindow_;
        lookaheadLastAdaptMs_ = now;
        uint32_t floorWindow = std::max(
            lookaheadMin_, std::min(lookaheadMax_, activePeers * 2));
        bool stalled = lookaheadStallEvents_ > 0;
        lookaheadStallEvents_ = 0;
        size_t buffered = bufferedBytesLocked();
        if (stalled || buffered > maxBufferedBytes_ / 4 * 3)
            lookaheadWindow_ = std::max(floorWindow, lookaheadWindow_ / 2);
        else if (buffered < maxBufferedBytes_ / 2)
            lookaheadWindow_ = std::min(lookaheadMax_,
                                        lookaheadWindow_ + kLookaheadStep);
        if (lookaheadWindow_ < floorWindow)
            lookaheadWindow_ = floorWindow;
        // Remember the window that worked under healthy conditions; the
        // resume path restores it instead of regrowing from the minimum
        // (PERF_PLAN 7.3).
        if (!stalled &&
            requestGate_.state() == pipensx::RequestGate::State::Free)
            lookaheadHealthy_ = lookaheadWindow_;
        return lookaheadWindow_;
    }
    // While the request gate curtails new requests (throttle or pause) a
    // peer's measured throughput reflects the gate, not the peer — the
    // engine freezes its rate EMAs for the duration (PERF_PLAN 7.2).
    bool requestsCurtailed() const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return requestGate_.state() != pipensx::RequestGate::State::Free;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return error_;
    }

    bool finish() {
        if (!installWorker_.joinable())
            return error().empty();
        std::unique_lock<std::mutex> lock(queueMutex_);
        accepting_ = false;
        queueReady_.notify_all();
        drained_.wait(lock, [this] {
            return !error_.empty() ||
                   (pending_.empty() && queue_.empty() && !processing_);
        });
        maybeEmitTelemetryLocked(now_ms(), true);
        drainComplete_ = error_.empty();
        return drainComplete_;
    }

private:
    struct InstallChunk {
        uint32_t fileIndex = UINT32_MAX;
        uint64_t fileOffset = 0;
        std::vector<uint8_t> data;
        bool final = false;
    };

    struct PendingKey {
        uint32_t ordinal = 0;
        uint64_t offset = 0;

        bool operator<(const PendingKey& other) const {
            if (ordinal != other.ordinal)
                return ordinal < other.ordinal;
            return offset < other.offset;
        }
    };

    struct PieceGate {
        bool package = false;
        uint32_t ordinal = UINT32_MAX;
        uint64_t offset = 0;
    };

    static int sinkThunk(void* user, uint32_t fileIndex,
                         int64_t fileOffset, const uint8_t* data, size_t size) {
        return static_cast<PackageCoordinator*>(user)->sink(
            fileIndex, fileOffset, data, size) ? 1 : 0;
    }

    bool sink(uint32_t fileIndex, int64_t fileOffset,
              const uint8_t* data, size_t size) {
        auto ordinalIt = packageOrdinals_.find(fileIndex);
        if (fileIndex >= metainfo_.num_files ||
            ordinalIt == packageOrdinals_.end()) {
            return setError("Invalid package stream routing.");
        }
        if (ordinalIt->second < initialCompletedPackages_)
            return true;
        const mi_file_t& file = metainfo_.files[fileIndex];
        if (fileOffset < 0 || !data || size == 0)
            return setError("Invalid package stream chunk.");

        std::unique_lock<std::mutex> lock(queueMutex_);
        if (!error_.empty() || !accepting_)
            return false;

        uint32_t ordinal = ordinalIt->second;
        if (ordinal < producerOrdinal_)
            return true;
        uint64_t offset = static_cast<uint64_t>(fileOffset);
        if (offset >= static_cast<uint64_t>(file.length))
            return setErrorLocked("Invalid package stream offset.");

        PendingKey key {ordinal, offset};
        if (pending_.find(key) != pending_.end())
            return setErrorLocked("Duplicate package stream chunk.");
        // Never wait for buffer space here (PERF_PLAN 5.3): this runs inside
        // the torrent thread's piece callback, so blocking would stall the
        // whole event loop. Chunks already in flight are always accepted —
        // the request gate below stops new requests once the buffer is full,
        // and the strict-order window bounds the overshoot.
        InstallChunk chunk;
        chunk.fileIndex = fileIndex;
        chunk.fileOffset = offset;
        chunk.data.assign(data, data + size);
        chunk.final = chunk.fileOffset + size == static_cast<uint64_t>(file.length);
        pendingBytes_ += chunk.data.size();
        pending_.emplace(key, std::move(chunk));
        requestGate_.onArrived(ordinal, offset + size);
        telemetrySinkBytes_ += size;
        telemetrySinkChunks_++;
        telemetryHighBufferedBytes_ = std::max(
            telemetryHighBufferedBytes_, bufferedBytesLocked());
        enqueueReadyLocked();
        uint64_t now = now_ms();
        updateRequestGateLocked(now);
        maybeEmitTelemetryLocked(now, false);
        return true;
    }

    size_t bufferedBytesLocked() const {
        return pendingBytes_ + queuedBytes_ + processingBytes_;
    }

    // Backpressure without blocking the event loop (PERF_PLAN 5.3 + 7.1):
    // the reorder buffer state drives a rate-matched request gate instead of
    // making the torrent thread wait inside the piece callback. Above the
    // throttle threshold new requests are admitted at the measured drain
    // rate; the hard pause at the buffer limit remains as an emergency
    // ceiling. Call whenever bufferedBytesLocked() changes and as a
    // periodic heartbeat so the token bucket refills during rx lulls.
    void updateRequestGateLocked(uint64_t now) {
        using GateState = pipensx::RequestGate::State;
        requestGate_.update(bufferedBytesLocked(), producerOrdinal_,
                            producerOffset_, now);
        GateState state = requestGate_.state();
        GateState previous = requestGateState_;
        if (state == previous)
            return;
        uint64_t stateMs = requestGateStateSinceMs_ &&
                           now >= requestGateStateSinceMs_
            ? now - requestGateStateSinceMs_ : 0;
        requestGateState_ = state;
        requestGateStateSinceMs_ = now;
        size_t buffered = bufferedBytesLocked();
        if (previous == GateState::Paused) {
            telemetryPausedMs_ += stateMs;
            telemetryPauseMaxMs_ = std::max(telemetryPauseMaxMs_, stateMs);
            // Resume at the last healthy window instead of regrowing +4/s
            // from the minimum (PERF_PLAN 7.3). The pause's pending stall
            // event still halves it once, so the net resume window is
            // about half the healthy value, floored by the swarm size.
            if (lookaheadHealthy_)
                lookaheadWindow_ = std::min(
                    lookaheadMax_,
                    std::max(lookaheadWindow_, lookaheadHealthy_));
            log_msg("[install] request gate resumed after %llu ms "
                    "buffered=%zu throttled=%d\n",
                    static_cast<unsigned long long>(stateMs), buffered,
                    state == GateState::Throttled ? 1 : 0);
            telemetry_log("request_gate", taskId_.c_str(),
                "event=resume paused_ms=%llu buffered_bytes=%zu throttled=%d",
                static_cast<unsigned long long>(stateMs), buffered,
                state == GateState::Throttled ? 1 : 0);
        } else if (previous == GateState::Throttled) {
            telemetryThrottledMs_ += stateMs;
        }
        if (state == GateState::Paused) {
            lookaheadStallEvents_++;
            telemetryPauseCount_++;
            log_msg("[install] request gate paused buffered=%zu limit=%zu\n",
                    buffered, maxBufferedBytes_);
            telemetry_log("request_gate", taskId_.c_str(),
                "event=pause buffered_bytes=%zu limit_bytes=%zu",
                buffered, maxBufferedBytes_);
        } else if (state == GateState::Throttled) {
            telemetryThrottleCount_++;
            telemetry_log("request_gate", taskId_.c_str(),
                "event=throttle buffered_bytes=%zu limit_bytes=%zu "
                "drain_bps=%llu",
                buffered, maxBufferedBytes_,
                static_cast<unsigned long long>(requestGate_.drainBps()));
        } else if (previous == GateState::Throttled) {
            telemetry_log("request_gate", taskId_.c_str(),
                "event=throttle_end throttled_ms=%llu buffered_bytes=%zu",
                static_cast<unsigned long long>(stateMs), buffered);
        }
    }

    bool canRequestPiece(uint32_t piece) const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!error_.empty() || stopping_)
            return false;
        if (requestGate_.paused())
            return false;
        if (piece >= pieceGates_.size())
            return true;
        const PieceGate& gate = pieceGates_[piece];
        if (!gate.package)
            return producerOrdinal_ >= packageCount_;
        if (gate.ordinal < producerOrdinal_)
            return true;
        if (gate.ordinal > producerOrdinal_)
            return false;
        return requestGate_.allows(gate.offset);
    }

    install::PackageCallbacks makeCallbacks() {
        install::PackageCallbacks callbacks;
        callbacks.skipFile = [this](const std::string& name) {
            return backend_->shouldSkipFile(name);
        };
        callbacks.beginFile = [this](const std::string& name,
                                     uint64_t fileSize) {
            bool ok = backend_->beginFile(name, fileSize);
            if (!ok)
                setError(backend_->error());
            return ok;
        };
        callbacks.setFileSize = [this](uint64_t fileSize) {
            bool ok = backend_->setFileSize(fileSize);
            if (!ok)
                setError(backend_->error());
            return ok;
        };
        callbacks.writeFile = [this](const uint8_t* bytes,
                                     size_t byteCount) {
            bool ok = backend_->writeFile(bytes, byteCount);
            if (ok) {
                if (progress_) {
                    progress_(completedPackages_, currentPackage_,
                              backend_->installedBytes(),
                              backend_->expectedBytes(),
                              DownloadStatus::Installing);
                }
            } else {
                setError(backend_->error());
            }
            return ok;
        };
        callbacks.endFile = [this] {
            bool ok = backend_->endFile();
            if (!ok)
                setError(backend_->error());
            return ok;
        };
        return callbacks;
    }

    // IMPROVEMENT_PLAN F-B: re-attach to an interrupted install of the
    // package at ordinal completedPackages_. On any mismatch the journal is
    // discarded and the package restarts from scratch. Pieces that lie
    // entirely below the journaled stream position are excluded from
    // download (storage_range_skipped); the piece straddling it is
    // re-downloaded and storage clips sink delivery at ready_bytes, so the
    // restored stream sees its next byte exactly at state.consumed.
    void tryResume() {
        install::InstallJournal journal;
        if (!install::loadInstallJournal(journalPath_, journal))
            return;
        uint32_t fileIndex = UINT32_MAX;
        for (const auto& item : packageOrdinals_) {
            if (item.second == completedPackages_) {
                fileIndex = item.first;
                break;
            }
        }
        if (fileIndex == UINT32_MAX) {
            install::removeInstallJournal(journalPath_);
            return;
        }
        const mi_file_t& file = metainfo_.files[fileIndex];
        if (journal.packageId != file.path ||
            journal.packageSize != static_cast<uint64_t>(file.length) ||
            journal.compressed != isCompressedPackage(file.path) ||
            journal.state.consumed == 0 ||
            journal.state.consumed >= static_cast<uint64_t>(file.length) ||
            journal.backendState.empty()) {
            log_msg("[install] stale journal discarded package='%s'\n",
                    file.path);
            install::removeInstallJournal(journalPath_);
            return;
        }
        if (!backend_->resumePackage(taskId_, file.path,
                                     journal.backendState)) {
            log_msg("[install] backend resume rejected package='%s': %s\n",
                    file.path, backend_->error().c_str());
            install::removeInstallJournal(journalPath_);
            return;
        }
        auto stream = std::make_unique<install::PackageStream>(
            journal.compressed, makeCallbacks(), taskId_);
        if (!stream->restore(journal.state)) {
            log_msg("[install] stream restore failed package='%s': %s\n",
                    file.path, stream->error().c_str());
            backend_->rollbackPackage();
            install::removeInstallJournal(journalPath_);
            return;
        }
        stream_ = std::move(stream);
        activeFileIndex_ = fileIndex;
        currentPackage_ = file.path;
        configs_[fileIndex].ready_bytes = journal.state.consumed;
        producerOffset_ = journal.state.consumed;
        journalConsumed_ = journal.state.consumed;
        journalValid_ = true;
        log_msg("[install] resuming package='%s' at %llu of %llu bytes\n",
                file.path,
                static_cast<unsigned long long>(journal.state.consumed),
                static_cast<unsigned long long>(file.length));
        telemetry_log("install_resume", taskId_.c_str(),
            "package=%s consumed_bytes=%llu total_bytes=%llu",
            file.path,
            static_cast<unsigned long long>(journal.state.consumed),
            static_cast<unsigned long long>(file.length));
    }

    // F-B: periodically persist a resume point from the install worker.
    // Cheap when the stream is between safe points — checkpoint() just
    // declines and we retry after the next chunk.
    void maybeCheckpoint() {
        if (!stream_ || activeFileIndex_ == UINT32_MAX)
            return;
        uint64_t consumed = stream_->consumed();
        if (consumed < journalConsumed_ + kJournalIntervalBytes)
            return;
        install::InstallJournal journal;
        if (!stream_->checkpoint(journal.state) ||
            journal.state.consumed == 0)
            return;
        journal.backendState = backend_->checkpointPackage();
        if (journal.backendState.empty())
            return;
        const mi_file_t& file = metainfo_.files[activeFileIndex_];
        journal.packageId = file.path;
        journal.packageSize = static_cast<uint64_t>(file.length);
        journal.compressed = isCompressedPackage(file.path);
        if (!saveInstallJournal(journalPath_, journal)) {
            log_msg("[install] journal write failed '%s'\n",
                    journalPath_.c_str());
            return;
        }
        journalConsumed_ = consumed;
        journalValid_ = true;
    }

    void clearJournal() {
        if (!journalPath_.empty())
            install::removeInstallJournal(journalPath_);
        journalValid_ = false;
        journalConsumed_ = 0;
    }

    bool processChunk(const InstallChunk& chunk) {
        const mi_file_t& file = metainfo_.files[chunk.fileIndex];
        if (!stream_) {
            if (chunk.fileOffset != 0)
                return setError("Package stream did not start at offset zero.");
            if (!backend_->beginPackage(taskId_, file.path)) {
                return setError(backend_->error());
            }
            activeFileIndex_ = chunk.fileIndex;
            currentPackage_ = file.path;
            stream_ = std::make_unique<install::PackageStream>(
                isCompressedPackage(file.path), makeCallbacks(), taskId_);
            if (progress_)
                progress_(completedPackages_, currentPackage_, 0, 0,
                          DownloadStatus::Installing);
        }
        if (activeFileIndex_ != chunk.fileIndex ||
            chunk.fileOffset != stream_->consumed())
            return setError("Install worker received bytes out of order.");
        if (!stream_->write(chunk.data.data(), chunk.data.size())) {
            if (error().empty())
                setError(stream_->error());
            log_msg("[install] stream error package='%s' offset=%lld: %s\n",
                    currentPackage_.c_str(),
                    static_cast<long long>(chunk.fileOffset), error().c_str());
            backend_->rollbackPackage();
            clearJournal();
            return false;
        }
        if (chunk.final) {
            if (cancelRequested_)
                return false;
            if (progress_) {
                progress_(completedPackages_, currentPackage_,
                          backend_->installedBytes(),
                          backend_->expectedBytes(),
                          DownloadStatus::Committing);
            }
            if (!stream_->finish()) {
                if (error().empty())
                    setError(stream_->error());
                log_msg("[install] finalize error package='%s': %s\n",
                        currentPackage_.c_str(), error().c_str());
                backend_->rollbackPackage();
                clearJournal();
                return false;
            }
            bool alreadyInstalled = false;
            if (!backend_->commitPackage(alreadyInstalled)) {
                setError(backend_->error());
                log_msg("[install] commit error package='%s': %s\n",
                        currentPackage_.c_str(), error().c_str());
                backend_->rollbackPackage();
                clearJournal();
                return false;
            }
            ++completedPackages_;
            stream_.reset();
            activeFileIndex_ = UINT32_MAX;
            // The finished package's resume point is obsolete.
            clearJournal();
            if (progress_)
                progress_(completedPackages_, currentPackage_, 0, 0,
                          DownloadStatus::Downloading);
            currentPackage_.clear();
        } else {
            maybeCheckpoint();
        }
        return true;
    }

    bool setError(const std::string& message) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return setErrorLocked(message);
    }

    bool setErrorLocked(const std::string& message) {
        if (error_.empty()) {
            error_ = message.empty() ? "Installation pipeline failed." : message;
            log_msg("[install] pipeline error: %s\n", error_.c_str());
        }
        accepting_ = false;
        queueReady_.notify_all();
        drained_.notify_all();
        return false;
    }

    void installMain() {
        log_msg("[install] worker started queue=%zu buffer=%zu window=%llu bytes\n",
                maxQueuedBytes_, maxBufferedBytes_,
                static_cast<unsigned long long>(requestAheadBytes_));
        while (true) {
            InstallChunk chunk;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueReady_.wait(lock, [this] {
                    return stopping_ || !queue_.empty();
                });
                if (stopping_)
                    break;
                chunk = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= chunk.data.size();
                processingBytes_ = chunk.data.size();
                processing_ = true;
            }

            uint64_t processStartedUs = telemetry_enabled() ? now_us() : 0;
            bool ok = processChunk(chunk);
            uint64_t processUs = processStartedUs ? now_us() - processStartedUs : 0;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (processStartedUs) {
                    telemetryProcessedBytes_ += chunk.data.size();
                    telemetryProcessedChunks_++;
                    telemetryProcessUs_ += processUs;
                    telemetryProcessMaxUs_ = std::max(
                        telemetryProcessMaxUs_, processUs);
                }
                processingBytes_ = 0;
                processing_ = false;
                requestGate_.onProcessed(chunk.data.size());
                if (!ok) {
                    queue_.clear();
                    pending_.clear();
                    queuedBytes_ = 0;
                    pendingBytes_ = 0;
                } else {
                    enqueueReadyLocked();
                }
                uint64_t now = now_ms();
                updateRequestGateLocked(now);
                if (pending_.empty() && queue_.empty())
                    drained_.notify_all();
                maybeEmitTelemetryLocked(now, false);
            }
            if (!ok)
                break;
        }
        log_msg("[install] worker stopped\n");
        std::lock_guard<std::mutex> lock(queueMutex_);
        processing_ = false;
        drained_.notify_all();
    }

    void resetTelemetryLocked(uint64_t now) {
        telemetryGeneration_ = telemetry_generation();
        telemetryLastMs_ = now;
        telemetrySinkBytes_ = 0;
        telemetryProcessedBytes_ = 0;
        telemetrySinkChunks_ = 0;
        telemetryProcessedChunks_ = 0;
        telemetryPauseCount_ = 0;
        telemetryPausedMs_ = 0;
        telemetryPauseMaxMs_ = 0;
        telemetryThrottleCount_ = 0;
        telemetryThrottledMs_ = 0;
        telemetryProcessUs_ = 0;
        telemetryProcessMaxUs_ = 0;
        telemetryHighBufferedBytes_ = bufferedBytesLocked();
    }

    void maybeEmitTelemetryLocked(uint64_t now, bool force) {
        uint32_t generation = telemetry_generation();
        if (!telemetry_enabled()) {
            if (telemetryGeneration_ != generation)
                resetTelemetryLocked(now);
            return;
        }
        if (telemetryGeneration_ != generation) {
            resetTelemetryLocked(now);
            return;
        }
        if (!telemetryLastMs_)
            resetTelemetryLocked(now);
        uint64_t elapsedMs = now - telemetryLastMs_;
        if (!force && elapsedMs < 5000)
            return;
        if (!elapsedMs)
            elapsedMs = 1;
        uint64_t sinkBps = telemetrySinkBytes_ * 1000 / elapsedMs;
        uint64_t processedBps = telemetryProcessedBytes_ * 1000 / elapsedMs;
        telemetry_log("buffer", taskId_.c_str(),
            "interval_ms=%llu sink_bps=%llu processed_bps=%llu "
            "sink_chunks=%u processed_chunks=%u pending_bytes=%zu "
            "pending_chunks=%zu queued_bytes=%zu queued_chunks=%zu "
            "processing_bytes=%zu high_bytes=%zu limit_bytes=%zu "
            "pauses=%u paused_ms=%llu pause_max_ms=%llu gate_paused=%d "
            "throttles=%u throttled_ms=%llu gate_throttled=%d drain_bps=%llu "
            "process_total_us=%llu process_max_us=%llu "
            "producer_ordinal=%u producer_offset=%llu force=%d",
            (unsigned long long)elapsedMs,
            (unsigned long long)sinkBps,
            (unsigned long long)processedBps,
            telemetrySinkChunks_, telemetryProcessedChunks_, pendingBytes_,
            pending_.size(), queuedBytes_, queue_.size(), processingBytes_,
            telemetryHighBufferedBytes_, maxBufferedBytes_,
            telemetryPauseCount_, (unsigned long long)telemetryPausedMs_,
            (unsigned long long)telemetryPauseMaxMs_,
            requestGate_.paused() ? 1 : 0,
            telemetryThrottleCount_,
            (unsigned long long)telemetryThrottledMs_,
            requestGate_.state() == pipensx::RequestGate::State::Throttled
                ? 1 : 0,
            (unsigned long long)requestGate_.drainBps(),
            (unsigned long long)telemetryProcessUs_,
            (unsigned long long)telemetryProcessMaxUs_, producerOrdinal_,
            (unsigned long long)producerOffset_, force ? 1 : 0);
        resetTelemetryLocked(now);
    }

    void cancel() {
        if (!installWorker_.joinable())
            return;
        cancelRequested_ = true;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopping_ = true;
            accepting_ = false;
            if (!drainComplete_) {
                queue_.clear();
                pending_.clear();
                queuedBytes_ = 0;
                pendingBytes_ = 0;
            }
        }
        queueReady_.notify_all();
        installWorker_.join();
    }

    bool enqueueReadyLocked() {
        bool queued = false;
        while (true) {
            PendingKey key {producerOrdinal_, producerOffset_};
            auto item = pending_.find(key);
            if (item == pending_.end())
                break;
            size_t size = item->second.data.size();
            if (queuedBytes_ > 0 && queuedBytes_ + size > maxQueuedBytes_)
                break;
            const mi_file_t& file = metainfo_.files[item->second.fileIndex];
            bool final = item->second.final;
            pendingBytes_ -= size;
            queuedBytes_ += size;
            producerOffset_ += size;
            queue_.push_back(std::move(item->second));
            pending_.erase(item);
            queued = true;
            if (final) {
                producerOffset_ = 0;
                ++producerOrdinal_;
            } else if (producerOffset_ >= static_cast<uint64_t>(file.length)) {
                setErrorLocked("Package stream missed final chunk.");
                break;
            }
        }
        if (queued)
            queueReady_.notify_one();
        return queued;
    }

    void markPieceGate(uint32_t fileIndex, uint32_t ordinal) {
        const mi_file_t& file = metainfo_.files[fileIndex];
        if (file.length <= 0 || pieceLengthBytes_ == 0)
            return;
        uint64_t fileOffset = static_cast<uint64_t>(file.offset);
        uint64_t fileLength = static_cast<uint64_t>(file.length);
        uint32_t first = static_cast<uint32_t>(fileOffset / pieceLengthBytes_);
        uint32_t last = static_cast<uint32_t>(
            (fileOffset + fileLength - 1) / pieceLengthBytes_);
        for (uint32_t piece = first;
             piece <= last && piece < pieceGates_.size(); ++piece) {
            uint64_t pieceStart = static_cast<uint64_t>(piece) *
                                  pieceLengthBytes_;
            uint64_t localOffset = pieceStart > fileOffset
                ? pieceStart - fileOffset : 0;
            PieceGate& gate = pieceGates_[piece];
            if (!gate.package || ordinal < gate.ordinal ||
                (ordinal == gate.ordinal && localOffset < gate.offset)) {
                gate.package = true;
                gate.ordinal = ordinal;
                gate.offset = localOffset;
            }
        }
    }

    void buildPieceOrder() {
        std::vector<uint8_t> added(metainfo_.num_pieces, 0);
        pieceGates_.assign(metainfo_.num_pieces, PieceGate {});
        for (const auto& item : packageOrdinals_) {
            if (item.second < completedPackages_)
                continue;
            const mi_file_t& file = metainfo_.files[item.first];
            if (file.length <= 0)
                continue;
            markPieceGate(item.first, item.second);
            uint32_t first = static_cast<uint32_t>(
                file.offset / pieceLengthBytes_);
            uint32_t last = static_cast<uint32_t>(
                (file.offset + file.length - 1) / pieceLengthBytes_);
            for (uint32_t piece = first;
                 piece <= last && piece < metainfo_.num_pieces; ++piece) {
                if (!added[piece]) {
                    added[piece] = 1;
                    pieceOrder_.push_back(piece);
                }
            }
        }
        for (uint32_t piece = 0; piece < metainfo_.num_pieces; ++piece)
            if (!added[piece])
                pieceOrder_.push_back(piece);
    }

    const metainfo_t& metainfo_;
    std::string taskId_;
    std::unique_ptr<install::InstallBackend> backend_;
    // F-B resume journal (IMPROVEMENT_PLAN F-B). Touched only by the
    // constructor (before the worker starts), the install worker and the
    // destructor (after the worker joined) — no lock needed.
    static constexpr uint64_t kJournalIntervalBytes = 32ull * 1024 * 1024;
    std::string journalPath_;
    uint64_t journalConsumed_ = 0;
    bool journalValid_ = false;
    bool abandonResume_ = false;
    bool streamInstall_ = false;
    std::vector<storage_file_config_t> configs_;
    std::vector<uint32_t> pieceOrder_;
    std::vector<PieceGate> pieceGates_;
    std::map<uint32_t, uint32_t> packageOrdinals_;
    std::unique_ptr<install::PackageStream> stream_;
    uint32_t completedPackages_ = 0;
    uint32_t initialCompletedPackages_ = 0;
    uint32_t packageCount_ = 0;
    uint32_t activeFileIndex_ = UINT32_MAX;
    std::string currentPackage_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::condition_variable drained_;
    std::deque<InstallChunk> queue_;
    std::map<PendingKey, InstallChunk> pending_;
    std::thread installWorker_;
    size_t queuedBytes_ = 0;
    size_t pendingBytes_ = 0;
    size_t processingBytes_ = 0;
    size_t maxQueuedBytes_ = 32 * 1024 * 1024;
    size_t maxBufferedBytes_ = 64 * 1024 * 1024;
    uint64_t pieceLengthBytes_ = 4 * 1024 * 1024;
    uint64_t requestAheadBytes_ = 64 * 1024 * 1024;
    uint32_t producerOrdinal_ = 0;
    uint64_t producerOffset_ = 0;
    // Adaptive strict-order lookahead state (PERF_PLAN 5.1); guarded by
    // queueMutex_. Window bounds in pieces: at 4 MiB pieces the max adds at
    // most 128 MiB of concurrently pending piece buffers, and the
    // requestAheadBytes_ gate still caps the total in-flight span.
    static constexpr uint32_t kLookaheadStep = 4;
    static constexpr uint64_t kLookaheadAdaptIntervalMs = 1000;
    uint32_t lookaheadMin_ = 8;
    uint32_t lookaheadMax_ = 32;
    uint32_t lookaheadWindow_ = 32;
    uint32_t lookaheadHealthy_ = 0;
    uint32_t lookaheadStallEvents_ = 0;
    uint64_t lookaheadLastAdaptMs_ = 0;
    // Request gate state (PERF_PLAN 5.3 + 7.1); guarded by queueMutex_.
    pipensx::RequestGate requestGate_;
    pipensx::RequestGate::State requestGateState_ =
        pipensx::RequestGate::State::Free;
    uint64_t requestGateStateSinceMs_ = 0;
    bool accepting_ = true;
    bool stopping_ = false;
    bool processing_ = false;
    bool drainComplete_ = false;
    uint32_t telemetryGeneration_ = 0;
    uint64_t telemetryLastMs_ = 0;
    uint64_t telemetrySinkBytes_ = 0;
    uint64_t telemetryProcessedBytes_ = 0;
    uint64_t telemetryPausedMs_ = 0;
    uint64_t telemetryPauseMaxMs_ = 0;
    uint64_t telemetryThrottledMs_ = 0;
    uint64_t telemetryProcessUs_ = 0;
    uint64_t telemetryProcessMaxUs_ = 0;
    size_t telemetryHighBufferedBytes_ = 0;
    uint32_t telemetrySinkChunks_ = 0;
    uint32_t telemetryProcessedChunks_ = 0;
    uint32_t telemetryPauseCount_ = 0;
    uint32_t telemetryThrottleCount_ = 0;
    std::atomic<bool> cancelRequested_ {false};
    std::string error_;
    Progress progress_;
};

DownloadStatus persistedStatus(const std::string& value) {
    if (value == "paused")
        return DownloadStatus::Paused;
    if (value == "completed")
        return DownloadStatus::Completed;
    if (value == "installed")
        return DownloadStatus::Installed;
    if (value == "error")
        return DownloadStatus::Error;
    return DownloadStatus::Queued;
}

std::string persistedStatus(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Paused: return "paused";
        case DownloadStatus::Completed: return "completed";
        case DownloadStatus::Installed: return "installed";
        case DownloadStatus::Error: return "error";
        default: return "queued";
    }
}

TransferMode persistedMode(const std::string& value) {
    return value == "install" ? TransferMode::StreamInstall
                              : TransferMode::DownloadOnly;
}

const char* persistedMode(TransferMode mode) {
    return mode == TransferMode::StreamInstall ? "install" : "download";
}

} // namespace

const char* statusName(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Queued: return "Queued";
        case DownloadStatus::Checking: return "Checking";
        case DownloadStatus::Downloading: return "Downloading";
        case DownloadStatus::Paused: return "Paused";
        case DownloadStatus::Verifying: return "Verifying";
        case DownloadStatus::Completed: return "Completed";
        case DownloadStatus::Installing: return "Installing";
        case DownloadStatus::Committing: return "Committing";
        case DownloadStatus::Installed: return "Installed";
        case DownloadStatus::Error: return "Error";
        case DownloadStatus::Removing: return "Removing";
    }
    return "Unknown";
}

DownloadManager::DownloadManager(std::string rootPath, bool startWorker)
    : rootPath_(std::move(rootPath)),
      torrentRoot_(rootPath_ + "/torrents"),
      downloadRoot_(rootPath_ + "/downloads"),
      statePath_(rootPath_ + "/queue.bencode") {
    makeDirectories(rootPath_);
    makeDirectories(torrentRoot_);
    makeDirectories(downloadRoot_);
    load();
    if (startWorker) {
        workerStarted_ = true;
        worker_ = std::thread(&DownloadManager::workerMain, this);
    }
}

DownloadManager::~DownloadManager() {
    shutdown();
}

bool DownloadManager::previewTorrent(const std::string& path,
                                     TorrentPreview& preview,
                                     std::string& error) {
    metainfo_t metainfo;
    if (!metainfo_load(path.c_str(), &metainfo)) {
        error = "The selected file is not a valid or safe .torrent file.";
        return false;
    }
    char hash[41];
    hex20(hash, metainfo.info_hash);
    preview.name = metainfo.name;
    preview.infoHash = hash;
    preview.totalBytes = static_cast<uint64_t>(metainfo.total_length);
    preview.fileCount = metainfo.num_files;
    preview.trackerCount = metainfo.num_trackers;
    preview.files.reserve(metainfo.num_files);
    for (uint32_t i = 0; i < metainfo.num_files; ++i) {
        TorrentPreview::File file;
        file.path = metainfo.files[i].path;
        file.length = static_cast<uint64_t>(metainfo.files[i].length);
        file.package = hasPackageExtension(metainfo.files[i].path);
        file.compressed = isCompressedPackage(metainfo.files[i].path);
        if (file.package)
            ++preview.packageCount;
        file.cartridge = isCartridgeDump(metainfo.files[i].path);
        if (file.cartridge)
            ++preview.cartridgeCount;
        preview.files.push_back(std::move(file));
    }
    metainfo_free(&metainfo);
    return true;
}

bool DownloadManager::importTorrent(const std::string& path,
                                    TransferMode mode,
                                    const std::vector<uint8_t>& selectedFiles,
                                    std::string& taskId,
                                    std::string& error,
                                    const std::vector<uint8_t>& initialPeers) {
    TorrentPreview preview;
    if (!previewTorrent(path, preview, error))
        return false;
    if (!selectedFiles.empty() && selectedFiles.size() != preview.files.size()) {
        error = "Selected file list does not match torrent contents.";
        return false;
    }
    return importTorrentActions(path,
                                actionsFromLegacySelection(preview, mode,
                                                           selectedFiles),
                                taskId, error, initialPeers);
}

bool DownloadManager::importTorrentActions(
                                    const std::string& path,
                                    const std::vector<uint8_t>& fileActions,
                                    std::string& taskId,
                                    std::string& error,
                                    const std::vector<uint8_t>& initialPeers) {
    TorrentPreview preview;
    if (!previewTorrent(path, preview, error))
        return false;
    if (!fileActions.empty() && fileActions.size() != preview.files.size()) {
        error = "Selected file actions do not match torrent contents.";
        return false;
    }
    if (initialPeers.size() % 6 != 0) {
        error = "Initial peer endpoint list is malformed.";
        return false;
    }

    std::vector<uint8_t> selection = fileActions;
    bool useSelection = !selection.empty();
    uint32_t installPackageCount = 0;
    bool hasSelectedFiles = false;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        uint8_t action = useSelection
            ? selection[i]
            : actionValue(FileAction::Download);
        if (!isValidFileAction(action)) {
            error = "Selected file action is invalid.";
            return false;
        }
        if (action != actionValue(FileAction::Skip))
            hasSelectedFiles = true;
        if (action == actionValue(FileAction::Install)) {
            if (!preview.files[i].package) {
                error = "Only NSP/NSZ package files can be installed.";
                return false;
            }
            ++installPackageCount;
        }
    }
    if (!hasSelectedFiles) {
        error = "Select at least one file.";
        return false;
    }
    TransferMode mode = installPackageCount > 0
        ? TransferMode::StreamInstall
        : TransferMode::DownloadOnly;

    std::lock_guard<std::mutex> lock(mutex_);
    if (findLocked(preview.infoHash)) {
        error = "This torrent is already in the download manager.";
        return false;
    }

    std::string metainfoPath = torrentRoot_ + "/" + preview.infoHash + ".torrent";
    std::string dataPath = downloadRoot_ + "/" + safeComponent(preview.name) +
                           "-" + preview.infoHash.substr(0, 8);
    if (!copyFile(path, metainfoPath)) {
        error = "Unable to copy the torrent file into application storage.";
        return false;
    }
    if (!makeDirectories(dataPath)) {
        unlink(metainfoPath.c_str());
        error = "Unable to create the download directory.";
        return false;
    }

    DownloadTask task;
    task.id = preview.infoHash;
    task.name = preview.name;
    task.metainfoPath = metainfoPath;
    task.dataPath = dataPath;
    task.totalBytes = preview.totalBytes;
    task.status = DownloadStatus::Queued;
    task.mode = mode;
    task.packageCount = installPackageCount;
    task.fileSelection = std::move(selection);
    task.initialPeers = initialPeers;
    tasks_.push_back(std::move(task));
    taskId = preview.infoHash;

    if (!saveLocked(error)) {
        tasks_.pop_back();
        unlink(metainfoPath.c_str());
        removeTree(dataPath);
        return false;
    }
    condition_.notify_all();
    return true;
}

bool DownloadManager::pause(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask* task = findLocked(taskId);
    if (!task)
        return false;
    if (task->status != DownloadStatus::Queued &&
        task->status != DownloadStatus::Checking &&
        task->status != DownloadStatus::Downloading &&
        task->status != DownloadStatus::Installing &&
        task->status != DownloadStatus::Committing &&
        task->status != DownloadStatus::Verifying)
        return false;
    task->status = DownloadStatus::Paused;
    task->speedBytesPerSecond = 0;
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::resume(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask* task = findLocked(taskId);
    if (!task || (task->status != DownloadStatus::Paused &&
                  task->status != DownloadStatus::Error))
        return false;
    task->status = DownloadStatus::Queued;
    task->error.clear();
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::retry(const std::string& taskId) {
    return resume(taskId);
}

bool DownloadManager::verify(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask* task = findLocked(taskId);
    if (!task || task->status != DownloadStatus::Completed)
        return false;
    task->status = DownloadStatus::Queued;
    task->error.clear();
    task->piecesVerified = 0;
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::remove(const std::string& taskId, bool deleteData,
                             std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask* task = findLocked(taskId);
    if (!task) {
        error = "Download task not found.";
        return false;
    }

    if (task->status == DownloadStatus::Checking ||
        task->status == DownloadStatus::Downloading ||
        task->status == DownloadStatus::Installing ||
        task->status == DownloadStatus::Committing ||
        task->status == DownloadStatus::Verifying) {
        task->status = DownloadStatus::Removing;
        task->error = deleteData ? "delete-data" : "keep-data";
        condition_.notify_all();
        return true;
    }
    return removeLocked(taskId, deleteData, error);
}

std::vector<DownloadTask> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_;
}

bool DownloadManager::hasActiveTransfer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const DownloadTask& task : tasks_) {
        switch (task.status) {
            case DownloadStatus::Queued:
            case DownloadStatus::Checking:
            case DownloadStatus::Downloading:
            case DownloadStatus::Verifying:
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:
                return true;
            case DownloadStatus::Paused:
            case DownloadStatus::Completed:
            case DownloadStatus::Installed:
            case DownloadStatus::Error:
            case DownloadStatus::Removing:
                break;
        }
    }
    return false;
}

bool DownloadManager::save(std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return saveLocked(error);
}

bool DownloadManager::saveLocked(std::string& error) const {
    std::ostringstream state;
    state << "d5:tasks";
    state << "l";
    for (const DownloadTask& task : tasks_) {
        if (task.status == DownloadStatus::Removing)
            continue;
        state << "d";
        state << "4:data" << bstr(task.dataPath);
        state << "5:error" << bstr(task.error);
        state << "2:id" << bstr(task.id);
        state << "8:metainfo" << bstr(task.metainfoPath);
        state << "4:mode" << bstr(persistedMode(task.mode));
        state << "4:name" << bstr(task.name);
        state << "13:package-count" << bint(task.packageCount);
        state << "13:packages-done" << bint(task.packagesInstalled);
        state << "9:selection" << bstr(std::string(task.fileSelection.begin(),
                                                   task.fileSelection.end()));
        state << "6:status" << bstr(persistedStatus(task.status));
        state << "5:total" << bint(task.totalBytes);
        state << "e";
    }
    state << "e";
    state << "7:versioni4e";
    state << "e";

    std::string temporary = statePath_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open queue state for writing.";
            return false;
        }
        output << state.str();
        output.flush();
        if (!output.good()) {
            error = "Unable to write queue state.";
            return false;
        }
    }
    if (rename(temporary.c_str(), statePath_.c_str()) != 0) {
        int renameErrno = errno;
        if (renameErrno != EEXIST)
            log_msg("[manager] queue state rename failed: %s\n",
                    std::strerror(renameErrno));
        if (unlink(statePath_.c_str()) != 0 && errno != ENOENT) {
            int unlinkErrno = errno;
            unlink(temporary.c_str());
            error = std::string("Unable to remove old queue state: ") +
                    std::strerror(unlinkErrno);
            return false;
        }
        if (rename(temporary.c_str(), statePath_.c_str()) == 0)
            return true;
        int replaceErrno = errno;
        unlink(temporary.c_str());
        error = std::string("Unable to replace queue state: ") +
                std::strerror(replaceErrno);
        return false;
    }
    return true;
}

void DownloadManager::load() {
    std::ifstream input(statePath_, std::ios::binary);
    if (!input)
        return;
    std::string data((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const char* cursor = data.data();
    const char* end = cursor + data.size();
    be_node_t root;
    if (!be_decode(&cursor, end, &root) || root.type != BE_DICT)
        return;

    be_node_t version;
    if (!be_dict_get(root.buf, root.buf + root.raw_len, "version", 7,
                     &version) ||
        version.type != BE_INT ||
        (version.ival != 1 && version.ival != 2 && version.ival != 3 &&
         version.ival != 4))
        return;

    be_node_t list;
    if (!be_dict_get(root.buf, root.buf + root.raw_len, "tasks", 5, &list) ||
        list.type != BE_LIST)
        return;

    const char* itemCursor = list.buf + 1;
    const char* itemEnd = list.buf + list.raw_len - 1;
    be_node_t item;
    while (be_list_next(&itemCursor, itemEnd, &item)) {
        if (item.type != BE_DICT)
            continue;
        DownloadTask task;
        std::string status;
        if (!dictionaryString(item, "id", task.id) ||
            !dictionaryString(item, "name", task.name) ||
            !dictionaryString(item, "metainfo", task.metainfoPath) ||
            !dictionaryString(item, "data", task.dataPath) ||
            !dictionaryString(item, "status", status) ||
            !dictionaryInteger(item, "total", task.totalBytes))
            continue;
        dictionaryString(item, "error", task.error);
        if (version.ival >= 2) {
            std::string mode;
            uint64_t packageCount = 0;
            uint64_t packagesDone = 0;
            if (dictionaryString(item, "mode", mode))
                task.mode = persistedMode(mode);
            if (dictionaryInteger(item, "package-count", packageCount))
                task.packageCount = static_cast<uint32_t>(packageCount);
            if (dictionaryInteger(item, "packages-done", packagesDone))
                task.packagesInstalled =
                    static_cast<uint32_t>(packagesDone);
        }
        if (version.ival >= 3) {
            std::string selection;
            if (dictionaryString(item, "selection", selection)) {
                task.fileSelection.assign(selection.begin(), selection.end());
            }
        }
        task.status = persistedStatus(status);
        if (task.status == DownloadStatus::Completed ||
            task.status == DownloadStatus::Installed)
            task.completedBytes = task.totalBytes;
        if (!isManagedChild(torrentRoot_, task.metainfoPath) ||
            !isManagedChild(downloadRoot_, task.dataPath)) {
            task.status = DownloadStatus::Error;
            task.error = "The stored task contains an invalid path.";
        } else if (access(task.metainfoPath.c_str(), R_OK) != 0) {
            task.status = DownloadStatus::Error;
            task.error = "The stored .torrent file is missing.";
        }
        if (version.ival == 3 && task.status != DownloadStatus::Error)
            upgradeLegacySelection(task);
        tasks_.push_back(std::move(task));
    }
}

DownloadTask* DownloadManager::findLocked(const std::string& id) {
    for (DownloadTask& task : tasks_)
        if (task.id == id)
            return &task;
    return nullptr;
}

const DownloadTask* DownloadManager::findLocked(const std::string& id) const {
    for (const DownloadTask& task : tasks_)
        if (task.id == id)
            return &task;
    return nullptr;
}

bool DownloadManager::removeLocked(const std::string& id, bool deleteData,
                                   std::string& error) {
    for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
        if (it->id != id)
            continue;
        if (!isManagedChild(torrentRoot_, it->metainfoPath) ||
            !isManagedChild(downloadRoot_, it->dataPath)) {
            error = "Refusing to remove a path outside application storage.";
            it->status = DownloadStatus::Error;
            it->error = error;
            std::string ignored;
            saveLocked(ignored);
            return false;
        }
        if (deleteData && !removeTree(it->dataPath)) {
            error = "Unable to remove all downloaded data.";
            it->status = DownloadStatus::Error;
            it->error = error;
            std::string ignored;
            saveLocked(ignored);
            return false;
        }
        unlink(it->metainfoPath.c_str());
        install::removeInstallJournal(installJournalPath(rootPath_, it->id));
        tasks_.erase(it);
        return saveLocked(error);
    }
    error = "Download task not found.";
    return false;
}

void DownloadManager::workerMain() {
    while (!stopping_) {
        std::string activeId;
        std::string metainfoPath;
        std::string dataPath;
        TransferMode mode = TransferMode::DownloadOnly;
        uint32_t packagesInstalled = 0;
        std::vector<uint8_t> fileSelection;
        std::vector<uint8_t> initialPeers;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                if (stopping_)
                    return true;
                for (const DownloadTask& task : tasks_)
                    if (task.status == DownloadStatus::Queued)
                        return true;
                return false;
            });
            if (stopping_)
                break;
            for (DownloadTask& task : tasks_) {
                if (task.status == DownloadStatus::Queued) {
                    task.status = DownloadStatus::Checking;
                    activeId = task.id;
                    metainfoPath = task.metainfoPath;
                    dataPath = task.dataPath;
                    mode = task.mode;
                    packagesInstalled = task.packagesInstalled;
                    fileSelection = task.fileSelection;
                    initialPeers = task.initialPeers;
                    break;
                }
            }
        }

        metainfo_t metainfo;
        if (!metainfo_load(metainfoPath.c_str(), &metainfo)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId)) {
                task->status = DownloadStatus::Error;
                task->error = "Unable to read the stored .torrent file.";
                std::string ignored;
                saveLocked(ignored);
            }
            continue;
        }

        std::unique_ptr<PackageCoordinator> coordinator;
        torrent_options_t options {};
        {
            coordinator = std::make_unique<PackageCoordinator>(
                metainfo, activeId, rootPath_,
                mode == TransferMode::StreamInstall, fileSelection,
                packagesInstalled,
                installTarget_.load(std::memory_order_relaxed),
                [this, activeId](uint32_t completed,
                                 const std::string& package,
                                 uint64_t installed, uint64_t expected,
                                 DownloadStatus status) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    DownloadTask* task = findLocked(activeId);
                    if (!task || task->status == DownloadStatus::Removing ||
                        task->status == DownloadStatus::Paused)
                        return;
                    bool packageCommitted =
                        completed != task->packagesInstalled;
                    task->packagesInstalled = completed;
                    task->currentPackage = package;
                    task->installedBytes = installed;
                    task->installTotalBytes = expected;
                    task->status = status;
                    if (packageCommitted) {
                        std::string ignored;
                        saveLocked(ignored);
                    }
                });
            if (!coordinator->error().empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (DownloadTask* task = findLocked(activeId)) {
                    task->status = DownloadStatus::Error;
                    task->error = coordinator->error();
                    std::string ignored;
                    saveLocked(ignored);
                }
                coordinator.reset();
                metainfo_free(&metainfo);
                continue;
            }
            options.files = coordinator->configs().data();
            if (mode == TransferMode::StreamInstall) {
                options.strict_piece_order = 1;
                options.piece_order = coordinator->pieceOrder().data();
                options.piece_order_count =
                    static_cast<uint32_t>(coordinator->pieceOrder().size());
                options.request_allowed = &PackageCoordinator::requestAllowedThunk;
                options.request_allowed_user = coordinator.get();
                // Initial window only; the loop below resizes it from the
                // install sink's backlog (PERF_PLAN 5.1).
                options.strict_order_lookahead = coordinator->initialLookahead();
                options.strict_fill_pending_first = 1;
                // Per-peer in-flight ceiling (4 MiB = 256 x 16 KiB blocks =
                // MAX_PIPELINE). The engine scales the actual window per peer by
                // measured speed (PERF_PLAN 5.2); this is only the fast-peer cap.
                // Was 64 (1 MiB), which pinned even 2 MB/s peers at the ceiling.
                options.request_pipeline_limit = 256;
                options.hedge_after_ms = 5000;
            }
            options.telemetry_tag = activeId.c_str();
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId))
                task->packageCount = coordinator->packageCount();
        }

        torrent_t* torrent = torrent_create_ex(
            &metainfo, 51413, dataPath.c_str(), &options);
        if (!torrent) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId)) {
                task->status = DownloadStatus::Error;
                task->error = "Unable to initialize torrent storage or network.";
                std::string ignored;
                saveLocked(ignored);
            }
            coordinator.reset();
            metainfo_free(&metainfo);
            continue;
        }
        if (!initialPeers.empty()) {
            torrent_add_initial_peers(
                torrent, initialPeers.data(),
                static_cast<uint32_t>(initialPeers.size() / 6));
        }

        bool finished = false;
        while (!stopping_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                DownloadTask* task = findLocked(activeId);
                if (!task || task->status == DownloadStatus::Paused ||
                    task->status == DownloadStatus::Removing)
                    break;
            }

            std::string installError = coordinator
                ? coordinator->error() : std::string();
            int running = installError.empty() ? torrent_tick(torrent) : -1;
            torrent_stat_t stat;
            torrent_stat(torrent, &stat);
            if (running > 0 && mode == TransferMode::StreamInstall) {
                torrent_set_strict_lookahead(
                    torrent,
                    coordinator->adaptiveLookahead(stat.num_active_peers));
                torrent_set_rate_freeze(
                    torrent, coordinator->requestsCurtailed() ? 1 : 0);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                DownloadTask* task = findLocked(activeId);
                if (!task)
                    break;
                task->completedBytes = stat.completed_bytes;
                task->totalBytes = stat.total_bytes;
                task->speedBytesPerSecond = stat.speed_bps;
                task->peers = stat.num_peers;
                task->dhtGood = stat.dht_good;
                task->dhtDubious = stat.dht_dubious;
                task->piecesDone = stat.num_pieces_done;
                task->piecesTotal = stat.num_pieces;
                task->piecesVerified = stat.num_pieces_verified;
                if (task->status != DownloadStatus::Removing &&
                    task->status != DownloadStatus::Paused &&
                    task->status != DownloadStatus::Installing &&
                    task->status != DownloadStatus::Committing) {
                    if (stat.verifying)
                        task->status = DownloadStatus::Verifying;
                    else
                        task->status = DownloadStatus::Downloading;
                }
                if (running < 0) {
                    task->status = DownloadStatus::Error;
                    task->error = !installError.empty()
                        ? installError : torrent_last_error(torrent);
                    task->speedBytesPerSecond = 0;
                }
            }
            if (running < 0)
                break;
            if (!running) {
                bool installOk = mode != TransferMode::StreamInstall ||
                                 coordinator->finish();
                std::lock_guard<std::mutex> lock(mutex_);
                DownloadTask* task = findLocked(activeId);
                if (task && task->status != DownloadStatus::Removing &&
                    task->status != DownloadStatus::Paused) {
                    if (!installOk) {
                        task->status = DownloadStatus::Error;
                        task->error = coordinator->error();
                    } else if (mode == TransferMode::StreamInstall &&
                               task->packagesInstalled != task->packageCount) {
                        task->status = DownloadStatus::Error;
                        task->error =
                            "Torrent ended before all packages were installed.";
                    } else {
                        task->status = mode == TransferMode::StreamInstall
                            ? DownloadStatus::Installed
                            : DownloadStatus::Completed;
                        finished = true;
                    }
                    task->completedBytes = task->totalBytes;
                    task->speedBytesPerSecond = 0;
                    log_msg("[manager] completed %s, destroying torrent\n",
                            activeId.c_str());
                }
                break;
            }
        }

        torrent_destroy(torrent);
        log_msg("[manager] torrent destroyed %s\n", activeId.c_str());
        if (coordinator) {
            std::lock_guard<std::mutex> lock(mutex_);
            const DownloadTask* task = findLocked(activeId);
            if (!task || task->status == DownloadStatus::Removing)
                coordinator->abandonResume();
        }
        coordinator.reset();
        metainfo_free(&metainfo);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            DownloadTask* task = findLocked(activeId);
            if (task && task->status == DownloadStatus::Removing) {
                bool deleteData = task->error == "delete-data";
                std::string removeError;
                removeLocked(activeId, deleteData, removeError);
            } else if (task) {
                task->speedBytesPerSecond = 0;
                std::string ignored;
                saveLocked(ignored);
                if (finished)
                    log_msg("[manager] completion saved %s\n",
                            activeId.c_str());
            }
        }
        if (finished)
            condition_.notify_all();
    }
}

void DownloadManager::shutdown() {
    if (!workerStarted_)
        return;
    stopping_ = true;
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
    std::string ignored;
    save(ignored);
    workerStarted_ = false;
}

} // namespace pipensx
