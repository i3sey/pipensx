#include "request_gate.hpp"

#include <algorithm>

namespace pipensx {

namespace {

uint64_t saturatingAdd(uint64_t a, uint64_t b) {
    uint64_t sum = a + b;
    return sum < a ? UINT64_MAX : sum;
}

} // namespace

void RequestGate::configure(size_t bufferLimitBytes,
                            uint64_t requestAheadBytes,
                            uint64_t pieceLengthBytes,
                            uint32_t initialOrdinal) {
    bufferLimitBytes_ = bufferLimitBytes;
    throttleEnterBytes_ = bufferLimitBytes / 4 * 3;
    requestAheadBytes_ = requestAheadBytes;
    pieceLengthBytes_ = pieceLengthBytes ? pieceLengthBytes : 1;
    edgeOrdinal_ = initialOrdinal;
    edgeOffset_ = requestAheadBytes;
    arrivedHigh_ = 0;
}

void RequestGate::onArrived(uint32_t ordinal, uint64_t endOffset) {
    if (ordinal == edgeOrdinal_)
        arrivedHigh_ = std::max(arrivedHigh_, endOffset);
}

void RequestGate::onProcessed(size_t bytes) {
    drainSampleBytes_ += bytes;
}

void RequestGate::update(size_t bufferedBytes, uint32_t producerOrdinal,
                         uint64_t producerOffset, uint64_t nowMs) {
    if (producerOrdinal != edgeOrdinal_) {
        // Package rollover: offsets restart at zero in the new package
        // file. Start from a minimal edge; the state logic below snaps it
        // back to free flow unless the buffer is still under pressure.
        edgeOrdinal_ = producerOrdinal;
        arrivedHigh_ = producerOffset;
        edgeOffset_ = saturatingAdd(producerOffset, pieceLengthBytes_);
    }

    // Fold the accumulated drain sample into the EMA once per window. A
    // window without processed bytes decays the rate, so a stalled worker
    // stops refilling tokens within a couple of windows.
    if (!drainWindowStartMs_) {
        drainWindowStartMs_ = nowMs;
    } else if (nowMs - drainWindowStartMs_ >= kDrainSampleMs) {
        uint64_t rate =
            drainSampleBytes_ * 1000 / (nowMs - drainWindowStartMs_);
        drainBps_ = (drainBps_ + rate) / 2;
        drainSampleBytes_ = 0;
        drainWindowStartMs_ = nowMs;
    }

    State previous = state_;
    switch (state_) {
        case State::Paused:
            if (bufferedBytes + pieceLengthBytes_ <= bufferLimitBytes_)
                state_ = bufferedBytes >= throttleEnterBytes_
                       ? State::Throttled : State::Free;
            break;
        case State::Throttled:
            if (bufferedBytes >= bufferLimitBytes_)
                state_ = State::Paused;
            else if (bufferedBytes + pieceLengthBytes_ <= throttleEnterBytes_)
                state_ = State::Free;
            break;
        case State::Free:
            if (bufferedBytes >= bufferLimitBytes_)
                state_ = State::Paused;
            else if (bufferedBytes >= throttleEnterBytes_)
                state_ = State::Throttled;
            break;
    }

    if (state_ == State::Paused) {
        // Emergency ceiling: no admissions and no token banking, so the
        // resume restarts from the frontier instead of releasing a burst.
        lastRefillMs_ = nowMs;
        return;
    }

    if (state_ == State::Throttled && previous != State::Throttled) {
        // Anchor the edge at the arrival frontier plus a small burst;
        // whatever free-flow allowance was banked above it is forfeited.
        uint64_t anchor = std::max(arrivedHigh_, producerOffset);
        edgeOffset_ = std::min(
            edgeOffset_,
            saturatingAdd(anchor, kBurstPieces * pieceLengthBytes_));
    }

    uint64_t freeflow = saturatingAdd(producerOffset, requestAheadBytes_);
    if (state_ == State::Free)
        edgeOffset_ = std::max(edgeOffset_, freeflow);
    if (lastRefillMs_ && nowMs > lastRefillMs_) {
        uint64_t refill = drainBps_ * (nowMs - lastRefillMs_) / 1000;
        edgeOffset_ = saturatingAdd(edgeOffset_, refill);
    }
    lastRefillMs_ = nowMs;
    // The bucket may creep past the hard requestAhead ceiling by a bounded
    // overshoot (third dead window, PERF_PLAN 7.1); actual RAM stays
    // bounded by the buffer states above. The floor keeps the piece at the
    // drain head requestable even with an empty bucket, so the pipeline
    // can never deadlock on its own gate.
    edgeOffset_ = std::min(
        edgeOffset_,
        saturatingAdd(freeflow, kOvershootPieces * pieceLengthBytes_));
    edgeOffset_ = std::max(
        edgeOffset_, saturatingAdd(producerOffset, pieceLengthBytes_));
}

} // namespace pipensx
