#include "stream_ram_budget.hpp"

#include <algorithm>
#include <limits>

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#include <unistd.h>

extern "C" char* fake_heap_end;
#endif

namespace pipensx {
namespace {

constexpr uint64_t MiB = 1024 * 1024;
constexpr uint64_t kPreferredReserveBytes = 256 * MiB;
constexpr uint64_t kMinimumReserveBytes = 64 * MiB;
constexpr uint64_t kFallbackAvailableBytes = 384 * MiB;
constexpr uint64_t kPreferredMinBufferedBytes = 64 * MiB;
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

#ifdef __SWITCH__
bool queryHeapAvailableBytes(uint64_t& availableBytes) {
    const struct mallinfo info = mallinfo();
    void* currentBreak = sbrk(0);
    if (currentBreak == reinterpret_cast<void*>(-1) || !fake_heap_end)
        return false;

    const uintptr_t current = reinterpret_cast<uintptr_t>(currentBreak);
    const uintptr_t end = reinterpret_cast<uintptr_t>(fake_heap_end);
    if (end < current)
        return false;

    const uint64_t unclaimedBytes = static_cast<uint64_t>(end - current);
    const uint64_t reusableBytes = static_cast<uint64_t>(info.fordblks);
    availableBytes = reusableBytes >
            std::numeric_limits<uint64_t>::max() - unclaimedBytes
        ? std::numeric_limits<uint64_t>::max()
        : unclaimedBytes + reusableBytes;
    return availableBytes > 0;
}
#endif

} // namespace

StreamRamBudget calculateStreamRamBudget(uint64_t availableBytes,
                                         uint64_t pieceLengthBytes) {
    StreamRamBudget budget;
    budget.availableBytes = availableBytes;
    budget.reserveBytes = std::min(
        availableBytes,
        std::min(kPreferredReserveBytes,
                 std::max(kMinimumReserveBytes, availableBytes / 2)));
    if (pieceLengthBytes == 0 ||
        pieceLengthBytes > kMaxPeakBytes / 2 ||
        availableBytes <= budget.reserveBytes) {
        return budget;
    }

    const uint64_t usableBytes = availableBytes - budget.reserveBytes;
    const uint64_t peakCapacity = std::min(usableBytes, kMaxPeakBytes);
    if (pieceLengthBytes > peakCapacity ||
        pieceLengthBytes > peakCapacity - pieceLengthBytes) {
        return budget;
    }

    uint64_t minimumBufferedBytes = std::min(
        kPreferredMinBufferedBytes, peakCapacity - pieceLengthBytes);
    minimumBufferedBytes = alignDown(minimumBufferedBytes, pieceLengthBytes);
    if (minimumBufferedBytes < pieceLengthBytes)
        return budget;

    uint64_t preferredLookaheadBytes = std::max(
        pieceLengthBytes,
        std::min<uint64_t>(32 * MiB,
                           saturatedMultiply(pieceLengthBytes,
                                             kPreferredLookaheadMin)));
    preferredLookaheadBytes = std::min(
        preferredLookaheadBytes, peakCapacity - minimumBufferedBytes);
    preferredLookaheadBytes = std::max(
        pieceLengthBytes,
        alignDown(preferredLookaheadBytes, pieceLengthBytes));
    uint64_t minimumPeakBytes =
        minimumBufferedBytes + preferredLookaheadBytes;
    uint64_t peakBytes = std::max(usableBytes / 2, minimumPeakBytes);
    peakBytes = std::min(peakBytes, peakCapacity);

    uint64_t lookaheadPieces = std::min<uint64_t>(
        peakBytes / 3 / pieceLengthBytes, kAbsoluteLookaheadMax);
    lookaheadPieces = std::max<uint64_t>(lookaheadPieces, 1);
    while (lookaheadPieces > 1 &&
           peakBytes - lookaheadPieces * pieceLengthBytes <
               minimumBufferedBytes) {
        --lookaheadPieces;
    }

    uint64_t bufferedBytes = peakBytes - lookaheadPieces * pieceLengthBytes;
    if (bufferedBytes < minimumBufferedBytes)
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

StreamRamBudget selectStreamRamBudget(
    const StreamRamMemorySnapshot& memory,
    uint64_t pieceLengthBytes) {
    const uint64_t availableBytes = memory.heapDetected
        ? memory.heapAvailableBytes : kFallbackAvailableBytes;
    StreamRamBudget budget =
        calculateStreamRamBudget(availableBytes, pieceLengthBytes);
    budget.memoryDetected = memory.heapDetected;
    budget.kernelHeadroomDetected = memory.kernelHeadroomDetected;
    budget.kernelHeadroomBytes = memory.kernelHeadroomBytes;
    return budget;
}

StreamRamBudget detectStreamRamBudget(uint64_t pieceLengthBytes) {
    StreamRamMemorySnapshot memory;
#ifdef __SWITCH__
    memory.heapDetected =
        queryHeapAvailableBytes(memory.heapAvailableBytes);

    u64 totalBytes = 0;
    u64 usedBytes = 0;
    if (R_SUCCEEDED(svcGetInfo(&totalBytes, InfoType_TotalMemorySize,
                               CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&usedBytes, InfoType_UsedMemorySize,
                               CUR_PROCESS_HANDLE, 0)) &&
        totalBytes > usedBytes) {
        memory.kernelHeadroomBytes = totalBytes - usedBytes;
        memory.kernelHeadroomDetected = true;
    }
#endif
    return selectStreamRamBudget(memory, pieceLengthBytes);
}

} // namespace pipensx
