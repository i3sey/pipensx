#include "stream_ram_budget.hpp"

#include <algorithm>
#include <limits>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace pipensx {
namespace {

constexpr uint64_t MiB = 1024 * 1024;
constexpr uint64_t kReserveBytes = 256 * MiB;
constexpr uint64_t kFallbackAvailableBytes = 384 * MiB;
constexpr uint64_t kMinBufferedBytes = 64 * MiB;
constexpr uint64_t kMaxBufferedBytes = 256 * MiB;
constexpr uint64_t kMaxPeakBytes = 384 * MiB;
constexpr uint64_t kMaxQueuedBytes = 64 * MiB;
constexpr uint32_t kPreferredLookaheadMin = 8;
constexpr uint32_t kPreferredLookaheadStart = 32;
constexpr uint32_t kAbsoluteLookaheadMax = 64;

uint64_t saturatedMultiply(uint64_t value, uint64_t multiplier) {
    if (value > std::numeric_limits<uint64_t>::max() / multiplier)
        return std::numeric_limits<uint64_t>::max();
    return value * multiplier;
}

uint64_t alignDown(uint64_t value, uint64_t alignment) {
    return alignment > 0 ? value - value % alignment : value;
}

} // namespace

StreamRamBudget calculateStreamRamBudget(uint64_t availableBytes,
                                         uint64_t pieceLengthBytes) {
    StreamRamBudget budget;
    budget.availableBytes = availableBytes;
    budget.reserveBytes = kReserveBytes;
    if (pieceLengthBytes == 0 ||
        pieceLengthBytes > kMaxPeakBytes - kMinBufferedBytes ||
        availableBytes <= kReserveBytes) {
        return budget;
    }

    uint64_t usableBytes = availableBytes - kReserveBytes;
    if (pieceLengthBytes > usableBytes ||
        kMinBufferedBytes > usableBytes - pieceLengthBytes) {
        return budget;
    }

    uint64_t preferredLookaheadBytes = std::max(
        pieceLengthBytes,
        std::min<uint64_t>(32 * MiB,
                           saturatedMultiply(pieceLengthBytes,
                                             kPreferredLookaheadMin)));
    uint64_t minimumPeakBytes = kMinBufferedBytes + preferredLookaheadBytes;
    uint64_t peakBytes = std::max(usableBytes / 2, minimumPeakBytes);
    peakBytes = std::min({peakBytes, usableBytes, kMaxPeakBytes});

    uint64_t lookaheadPieces = std::min<uint64_t>(
        peakBytes / 3 / pieceLengthBytes, kAbsoluteLookaheadMax);
    lookaheadPieces = std::max<uint64_t>(lookaheadPieces, 1);
    while (lookaheadPieces > 1 &&
           peakBytes - lookaheadPieces * pieceLengthBytes <
               kMinBufferedBytes) {
        --lookaheadPieces;
    }

    uint64_t bufferedBytes = peakBytes - lookaheadPieces * pieceLengthBytes;
    if (bufferedBytes < kMinBufferedBytes)
        return budget;
    if (bufferedBytes > kMaxBufferedBytes) {
        uint64_t excessBytes = bufferedBytes - kMaxBufferedBytes;
        uint64_t extraPieces =
            (excessBytes + pieceLengthBytes - 1) / pieceLengthBytes;
        lookaheadPieces = std::min<uint64_t>(
            lookaheadPieces + extraPieces, kAbsoluteLookaheadMax);
        bufferedBytes = peakBytes - lookaheadPieces * pieceLengthBytes;
        bufferedBytes = std::min(bufferedBytes, kMaxBufferedBytes);
    }

    uint64_t queuedBytes = std::min({
        saturatedMultiply(pieceLengthBytes, 16), kMaxQueuedBytes,
        bufferedBytes / 4});
    queuedBytes = alignDown(queuedBytes, pieceLengthBytes);
    if (queuedBytes < pieceLengthBytes)
        queuedBytes = std::min(pieceLengthBytes, bufferedBytes);

    budget.valid = true;
    budget.peakBytes = bufferedBytes + lookaheadPieces * pieceLengthBytes;
    budget.maxQueuedBytes = static_cast<size_t>(queuedBytes);
    budget.maxBufferedBytes = static_cast<size_t>(bufferedBytes);
    budget.requestAheadBytes = bufferedBytes > queuedBytes
        ? bufferedBytes - queuedBytes : bufferedBytes;
    budget.lookaheadMax = static_cast<uint32_t>(lookaheadPieces);
    budget.lookaheadMin = std::min(kPreferredLookaheadMin,
                                   budget.lookaheadMax);
    budget.lookaheadStart = std::min(kPreferredLookaheadStart,
                                     budget.lookaheadMax);
    return budget;
}

StreamRamBudget detectStreamRamBudget(uint64_t pieceLengthBytes) {
    uint64_t availableBytes = kFallbackAvailableBytes;
    bool detected = false;
#ifdef __SWITCH__
    u64 totalBytes = 0;
    u64 usedBytes = 0;
    if (R_SUCCEEDED(svcGetInfo(&totalBytes, InfoType_TotalMemorySize,
                               CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&usedBytes, InfoType_UsedMemorySize,
                               CUR_PROCESS_HANDLE, 0)) &&
        totalBytes > usedBytes) {
        availableBytes = totalBytes - usedBytes;
        detected = true;
    }
#endif
    StreamRamBudget budget =
        calculateStreamRamBudget(availableBytes, pieceLengthBytes);
    budget.memoryDetected = detected;
    return budget;
}

} // namespace pipensx
