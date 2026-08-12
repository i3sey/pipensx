#pragma once

#include <cstddef>
#include <cstdint>

namespace pipensx {

// Rate-matched request gate for the stream-install reorder buffer
// (PERF_PLAN 7.1). Replaces the binary pause-at-100%/resume-at-75%
// hysteresis that produced 20-28 s rx=0 windows: above the throttle
// threshold new piece requests are admitted through a token bucket that
// refills at the measured install drain rate (network bytes), so the
// download trickles at consumption speed instead of stopping dead. The
// binary pause survives as an emergency ceiling at 100% of the buffer
// limit. The same bucket smooths the requestAheadBytes ceiling: when the
// hard window is exhausted while the buffer is healthy, the admission
// edge keeps creeping at the drain rate (bounded overshoot) instead of
// halting until producerOffset jumps.
//
// The admission edge lives in the current package's byte space, matching
// the piece-gate offsets used by PackageCoordinator::canRequestPiece.
// Not thread-safe; the caller serialises access (queueMutex_).
class RequestGate {
public:
    enum class State { Free, Throttled, Paused };

    // Unconfigured gate is wide open and never pauses.
    void configure(size_t bufferLimitBytes, uint64_t requestAheadBytes,
                   uint64_t pieceLengthBytes, uint32_t initialOrdinal);

    // A package chunk for `ordinal` arrived from the network, ending at
    // `endOffset` within the package file. Tracks the arrival frontier
    // the throttle anchors to when it engages.
    void onArrived(uint32_t ordinal, uint64_t endOffset);

    // The install worker drained `bytes` network bytes from the buffer;
    // feeds the drain-rate EMA that refills the token bucket.
    void onProcessed(size_t bytes);

    // Advance state and admission edge. Call whenever buffered bytes
    // change and periodically (heartbeat) so tokens keep refilling
    // during rx lulls.
    void update(size_t bufferedBytes, uint32_t producerOrdinal,
                uint64_t producerOffset, uint64_t nowMs);

    // May a piece whose first needed byte of the current package sits at
    // `offset` be requested?
    bool allows(uint64_t offset) const {
        return state_ != State::Paused && offset <= edgeOffset_;
    }

    State state() const { return state_; }
    bool paused() const { return state_ == State::Paused; }
    uint64_t drainBps() const { return drainBps_; }
    uint64_t edgeOffset() const { return edgeOffset_; }

private:
    static constexpr uint64_t kDrainSampleMs = 500;
    // Allowance ahead of the arrival frontier when the throttle engages.
    static constexpr uint64_t kBurstPieces = 2;
    // How far the edge may creep beyond the hard requestAhead ceiling.
    static constexpr uint64_t kOvershootPieces = 4;

    size_t bufferLimitBytes_ = SIZE_MAX;
    size_t throttleEnterBytes_ = SIZE_MAX;
    uint64_t requestAheadBytes_ = UINT64_MAX;
    uint64_t pieceLengthBytes_ = 1;

    State state_ = State::Free;
    uint32_t edgeOrdinal_ = 0;
    uint64_t edgeOffset_ = UINT64_MAX;
    uint64_t arrivedHigh_ = 0;

    uint64_t drainBps_ = 0;
    uint64_t drainSampleBytes_ = 0;
    uint64_t drainWindowStartMs_ = 0;
    uint64_t lastRefillMs_ = 0;
};

} // namespace pipensx
