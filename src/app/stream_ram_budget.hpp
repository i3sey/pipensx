#pragma once

#include <cstddef>
#include <cstdint>

namespace pipensx {

struct StreamRamBudget {
    bool valid = false;
    bool memoryDetected = false;
    uint64_t availableBytes = 0;
    uint64_t reserveBytes = 0;
    uint64_t peakBytes = 0;
    size_t maxQueuedBytes = 0;
    size_t maxBufferedBytes = 0;
    uint64_t requestAheadBytes = 0;
    uint32_t lookaheadMin = 0;
    uint32_t lookaheadStart = 0;
    uint32_t lookaheadMax = 0;
};

StreamRamBudget calculateStreamRamBudget(uint64_t availableBytes,
                                         uint64_t pieceLengthBytes);
StreamRamBudget detectStreamRamBudget(uint64_t pieceLengthBytes);

} // namespace pipensx
