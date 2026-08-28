#pragma once

#include "download_manager.hpp"
#include "install_pacer.hpp"
#include "request_gate.hpp"
#include "stream_budget_arbiter.hpp"
#include "stream_install_flag.hpp"
#include "stream_ram_budget.hpp"
#include "nx_file_types.hpp"
#include "../install/install_backend.hpp"
#include "../install/install_journal.hpp"
#include "../install/package_stream.hpp"

extern "C" {
#include "../core/metainfo.h"
#include "../core/util.h"
}

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pipensx {

// IMPROVEMENT_PLAN F-B: one journal file per task next to the queue state.
inline std::string installJournalPath(const std::string& root,
                                      const std::string& taskId) {
    return root + "/install-journal-" + taskId + ".bencode";
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
                       StreamBudgetArbiter& arbiter,
                       Progress progress)
        : metainfo_(metainfo), taskId_(std::move(taskId)),
          backend_(streamInstall
                       ? install::createInstallBackend(workingRoot, installTarget)
                       : nullptr),
          streamInstall_(streamInstall),
          completedPackages_(completedPackages),
          initialCompletedPackages_(completedPackages),
          producerOrdinal_(completedPackages),
          pacer_(64 * 1024 * 1024),
          progress_(std::move(progress)) {
        arbiter_ = &arbiter;
        bool useSelection = !fileSelection.empty();
        if (useSelection && fileSelection.size() != metainfo_.num_files) {
            error_ = "Selected file actions do not match the torrent.";
            return;
        }
        configs_.resize(metainfo_.num_files);
        uint32_t ordinal = 0;
        for (uint32_t i = 0; i < metainfo_.num_files; ++i) {
            FileAction action = streamInstall_ &&
                                      isPackageName(metainfo_.files[i].path)
                                  ? FileAction::Install
                                  : FileAction::Download;
            if (useSelection) {
                const uint8_t raw = fileSelection[i];
                if (raw != static_cast<uint8_t>(FileAction::Skip) &&
                    raw != static_cast<uint8_t>(FileAction::Download) &&
                    raw != static_cast<uint8_t>(FileAction::Install)) {
                    error_ = "Selected file action is invalid.";
                    return;
                }
                action = static_cast<FileAction>(raw);
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
                !isPackageName(metainfo_.files[i].path)) {
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
            StreamRamBudget budget;
            arbiterLease_ = arbiter_->acquire(
                pieceLengthBytes_,
                [this](const StreamRamBudget& shared) { applyBudget(shared); },
                budget);
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
            pacer_.setMaximumBufferedBytes(maxBufferedBytes_);
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
        arbiter_->release(arbiterLease_);
        if (!backend_)
            return;
        // F-B: an interruption with a journaled safe point keeps the partial
        // install on disk for a later resume; anything else rolls back and
        // drops the journal.
        if (journalValid_ && !abandonResume_ &&
            (error().empty() || recoverableError_.load())) {
            backend_->suspendPackage();
            return;
        }
        backend_->rollbackPackage();
        if (!journalPath_.empty())
            install::removeInstallJournal(journalPath_);
    }

    // The task is going away: never keep partial data or the journal.
    void abandonResume() { abandonResume_ = true; }

    // The package stream is intact, but the transfer source failed (peer loss,
    // timeout, disconnect). Keep the journal/placeholder safe point so retry
    // resumes instead of re-streaming the package from byte zero.
    void markRecoverableError(const std::string& message) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        setErrorLocked(message, true);
    }

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

    // Live budget resize from the arbiter when the active-slot or lease
    // population changes. Runs on whichever thread triggered the change;
    // queueMutex_ serialises it against the sink, the install worker and the
    // adaptive-lookahead tick. The engine-side lookahead follows on the
    // torrent thread's next torrent_set_strict_lookahead call.
    void applyBudget(const StreamRamBudget& budget) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!budget.valid) {
            // Shrunk below viability mid-flight: keep the last workable
            // configuration; the gate's backpressure still bounds RAM.
            return;
        }
        maxQueuedBytes_ = budget.maxQueuedBytes;
        maxBufferedBytes_ = budget.maxBufferedBytes;
        pacer_.setMaximumBufferedBytes(maxBufferedBytes_);
        requestAheadBytes_ = budget.requestAheadBytes;
        lookaheadMin_ = budget.lookaheadMin;
        lookaheadMax_ = budget.lookaheadMax;
        lookaheadWindow_ =
            std::clamp(lookaheadWindow_, lookaheadMin_, lookaheadMax_);
        if (lookaheadHealthy_)
            lookaheadHealthy_ =
                std::clamp(lookaheadHealthy_, lookaheadMin_, lookaheadMax_);
        requestGate_.configure(maxBufferedBytes_, requestAheadBytes_,
                               pieceLengthBytes_, producerOrdinal_);
        // configure() resets the admission edge to the package start;
        // re-anchor it to the live producer state immediately.
        updateRequestGateLocked(now_ms());
        telemetry_log("ram_budget", taskId_.c_str(),
            "event=resize reorder_bytes=%zu queue_bytes=%zu "
            "request_ahead_bytes=%llu lookahead_min=%u lookahead_max=%u "
            "lookahead_window=%u",
            maxBufferedBytes_, maxQueuedBytes_,
            static_cast<unsigned long long>(requestAheadBytes_),
            lookaheadMin_, lookaheadMax_, lookaheadWindow_);
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

        uint32_t ordinal = ordinalIt->second;
        uint64_t offset = static_cast<uint64_t>(fileOffset);
        if (offset >= static_cast<uint64_t>(file.length))
            return setError("Invalid package stream offset.");

        // The multi-MiB copy happens before taking queueMutex_: sink() runs
        // inside the torrent thread's piece callback, and the install worker
        // holds the same mutex while draining, so copying under the lock
        // stalls the whole event loop for the duration of the memcpy.
        InstallChunk chunk;
        chunk.fileIndex = fileIndex;
        chunk.fileOffset = offset;
        chunk.data.assign(data, data + size);
        chunk.final = offset + size == static_cast<uint64_t>(file.length);

        std::unique_lock<std::mutex> lock(queueMutex_);
        if (!error_.empty() || !accepting_)
            return false;
        if (ordinal < producerOrdinal_)
            return true;

        PendingKey key {ordinal, offset};
        if (pending_.find(key) != pending_.end())
            return setErrorLocked("Duplicate package stream chunk.", false);
        // Never wait for buffer space here (PERF_PLAN 5.3): this runs inside
        // the torrent thread's piece callback, so blocking would stall the
        // whole event loop. Chunks already in flight are always accepted —
        // the request gate below stops new requests once the buffer is full,
        // and the strict-order window bounds the overshoot.
        pendingBytes_ += chunk.data.size();
        pending_.emplace(key, std::move(chunk));
        if (requestGate_.state() == pipensx::RequestGate::State::Free)
            pacer_.observeSource(size, now_ms());
        requestGate_.onArrived(ordinal, offset + size);
        telemetrySinkBytes_ += size;
        telemetrySinkChunks_++;
        telemetryHighBufferedBytes_ = std::max(
            telemetryHighBufferedBytes_, bufferedBytesLocked());
        enqueueReadyLocked();
        uint64_t now = now_ms();
        pacer_.setBufferedBytes(pendingBytes_ + queuedBytes_);
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
        pacer_.setSourceMeasurementEnabled(state == GateState::Free, now);
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
            if (!pacer_.waitForWrite(byteCount, [this] {
                    return cancelRequested_.load();
                }))
                return false;
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
            journal.compressed != isCompressedName(file.path) ||
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
        activeOrdinal_.store(completedPackages_);
        currentPackage_ = file.path;
        pacer_.beginPackage(journal.compressed);
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

    // F-B: persist a resume point from the install worker. When `force` is
    // set the 32 MiB interval is ignored — used on worker shutdown so a
    // pause does not discard everything since the last periodic checkpoint.
    void maybeCheckpoint(bool force = false) {
        if (!stream_ || activeFileIndex_ == UINT32_MAX)
            return;
        uint64_t consumed = stream_->consumed();
        if (!force && consumed < journalConsumed_ + kJournalIntervalBytes)
            return;
        if (force && consumed <= journalConsumed_)
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
        journal.compressed = isCompressedName(file.path);
        std::string journalError;
        if (!saveInstallJournal(journalPath_, journal, &journalError)) {
            log_msg("[install] journal write failed '%s': %s\n",
                    journalPath_.c_str(),
                    journalError.empty() ? "unknown error" : journalError.c_str());
            if (!journalDiagnosticSent_) {
                journalDiagnosticSent_ = true;
                diagnostic_error("install", "journal",
                                 "path=%s error=%s", journalPath_.c_str(),
                                 journalError.empty() ? "unknown error"
                                                      : journalError.c_str());
            }
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
            activeOrdinal_.store(packageOrdinals_.at(chunk.fileIndex));
            currentPackage_ = file.path;
            pacer_.beginPackage(isCompressedName(file.path));
            stream_ = std::make_unique<install::PackageStream>(
                isCompressedName(file.path), makeCallbacks(), taskId_);
            if (progress_)
                progress_(completedPackages_, currentPackage_, 0, 0,
                          DownloadStatus::Installing);
        }
        if (activeFileIndex_ != chunk.fileIndex ||
            chunk.fileOffset != stream_->consumed())
            return setError("Install worker received bytes out of order.");
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            const uint32_t ordinal = packageOrdinals_.at(chunk.fileIndex);
            pacer_.setBufferedBytes(pendingBytes_ + queuedBytes_);
            pacer_.setSourceComplete(producerOrdinal_ > ordinal);
        }
        if (!stream_->write(chunk.data.data(), chunk.data.size())) {
            if (cancelRequested_)
                return false;
            if (error().empty())
                setError(stream_->error());
            log_msg("[install] stream error package='%s' offset=%lld: %s\n",
                    currentPackage_.c_str(),
                    static_cast<long long>(chunk.fileOffset), error().c_str());
            backend_->rollbackPackage();
            clearJournal();
            return false;
        }
        pacer_.observeConsumed(stream_->consumed(), backend_->installedBytes(),
                               now_ms());
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
            pacer_.endPackage();
            stream_.reset();
            activeFileIndex_ = UINT32_MAX;
            activeOrdinal_.store(UINT32_MAX);
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
        return setErrorLocked(message, false);
    }

    bool setErrorLocked(const std::string& message, bool recoverable) {
        if (!recoverable)
            recoverableError_.store(false);
        if (error_.empty()) {
            error_ = message.empty() ? "Installation pipeline failed." : message;
            if (recoverable)
                recoverableError_.store(true);
            log_msg("[install] pipeline error: %s\n", error_.c_str());
        }
        accepting_ = false;
        queueReady_.notify_all();
        drained_.notify_all();
        return false;
    }

    void installMain() {
        StreamInstallWorkerGuard streamGuard;
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
                pacer_.setBufferedBytes(pendingBytes_ + queuedBytes_);
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
                pacer_.setBufferedBytes(pendingBytes_ + queuedBytes_);
                uint64_t now = now_ms();
                updateRequestGateLocked(now);
                if (pending_.empty() && queue_.empty())
                    drained_.notify_all();
                maybeEmitTelemetryLocked(now, false);
            }
            if (!ok)
                break;
        }
        // Pause/teardown: persist the latest safe point before the stream dies,
        // even if we are still inside the periodic journal interval.
        maybeCheckpoint(true);
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
                if (activeOrdinal_.load() == producerOrdinal_)
                    pacer_.setSourceComplete(true);
                producerOffset_ = 0;
                ++producerOrdinal_;
            } else if (producerOffset_ >= static_cast<uint64_t>(file.length)) {
                setErrorLocked("Package stream missed final chunk.", false);
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
    StreamBudgetArbiter* arbiter_ = nullptr;
    uint64_t arbiterLease_ = 0;
    uint64_t journalConsumed_ = 0;
    bool journalValid_ = false;
    bool journalDiagnosticSent_ = false;
    bool abandonResume_ = false;
    std::atomic<bool> recoverableError_{false};
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
    std::atomic<uint32_t> activeOrdinal_ {UINT32_MAX};
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
    pipensx::InstallPacer pacer_;
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


} // namespace pipensx
