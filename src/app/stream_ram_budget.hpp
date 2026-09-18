#pragma once

#include <cstddef>
#include <cstdint>

namespace pipensx {

struct StreamRamMemorySnapshot {
    bool heapDetected = false;
    uint64_t heapAvailableBytes = 0;
    bool kernelHeadroomDetected = false;
    uint64_t kernelHeadroomBytes = 0;
};

struct StreamRamBudget {
    bool valid = false;
    bool memoryDetected = false;
    bool kernelHeadroomDetected = false;
    uint64_t availableBytes = 0;
    uint64_t kernelHeadroomBytes = 0;
    uint64_t reserveBytes = 0;
    uint64_t peakBytes = 0;
    size_t maxQueuedBytes = 0;
    size_t maxBufferedBytes = 0;
    uint64_t requestAheadBytes = 0;
    /* Picker AIMD band in pieces (PERF_PLAN 5.1). Independent of the
       piece-buffer RAM cap below. */
    uint32_t lookaheadMin = 0;
    uint32_t lookaheadStart = 0;
    uint32_t lookaheadMax = 0;
    /* RAM reserved for in-flight piece buffers (PENDING+HASHING). Not
       lookaheadMax * piece_length: the picker window is in pieces so a
       16 MiB torrent can still AIMD 8/32/64, while this cap stays ~64–128 MiB. */
    uint64_t maxPieceBufferBytes = 0;
};

// Assumed free RAM when heap detection is unavailable (PC builds).
inline constexpr uint64_t kFallbackStreamRamBytes = 384ull * 1024 * 1024;

StreamRamMemorySnapshot detectStreamRamMemorySnapshot();
StreamRamBudget calculateStreamRamBudget(uint64_t availableBytes,
                                         uint64_t pieceLengthBytes);
StreamRamBudget selectStreamRamBudget(
    const StreamRamMemorySnapshot& memory,
    uint64_t pieceLengthBytes);
StreamRamBudget detectStreamRamBudget(uint64_t pieceLengthBytes);

} // namespace pipensx
