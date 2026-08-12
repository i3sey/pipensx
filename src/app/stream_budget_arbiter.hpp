#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "stream_ram_budget.hpp"

namespace pipensx {

// Splits the stream-install RAM budget between concurrent download slots.
// calculateStreamRamBudget is written as "one consumer takes all free RAM";
// with several torrents active that would over-commit the Switch heap, so the
// arbiter snapshots free memory once per busy period, subtracts a fixed
// estimate for each extra engine's unbudgeted allocations, divides the rest
// equally between stream-install leases, and pushes recomputed budgets to
// running coordinators whenever the slot or lease population changes.
class StreamBudgetArbiter {
public:
    using DetectFn = std::function<StreamRamMemorySnapshot()>;
    using ReconfigureFn = std::function<void(const StreamRamBudget&)>;

    // Rough per-torrent engine cost the budget math cannot see: lazily
    // allocated peer receive buffers (96 x 256 KiB), the piece buffer pool
    // and the request tables. Only charged for slots beyond the leases —
    // a lease's own engine is already covered by the reserve inside
    // calculateStreamRamBudget, which keeps N=1 identical to the
    // pre-arbiter behavior.
    static constexpr uint64_t kDefaultEngineOverheadBytes =
        64ull * 1024 * 1024;

    explicit StreamBudgetArbiter(
        DetectFn detect = {},
        uint64_t engineSlotOverheadBytes = kDefaultEngineOverheadBytes);

    // Every active torrent slot (stream-install or download-only) brackets
    // the engine's lifetime with these so its unbudgeted RAM is subtracted
    // from the shared pool.
    void engineSlotStarted();
    void engineSlotFinished();

    // Lease a share for one stream-install coordinator. Returns the lease id,
    // or 0 when the share is too small — nothing is registered then and
    // out.valid is false. cb fires with a recomputed budget whenever slots or
    // leases change; it may run on any thread and must not call back into the
    // arbiter.
    uint64_t acquire(uint64_t pieceLengthBytes, ReconfigureFn cb,
                     StreamRamBudget& out);
    void release(uint64_t leaseId);

private:
    struct Lease {
        uint64_t id = 0;
        uint64_t pieceLengthBytes = 0;
        ReconfigureFn reconfigure;
    };

    StreamRamBudget shareLocked(uint64_t pieceLengthBytes,
                                size_t leaseCount) const;
    void ensureGenerationLocked();
    void endGenerationIfIdleLocked();
    void notifyLocked();

    mutable std::mutex mutex_;
    DetectFn detect_;
    uint64_t overheadBytes_;
    uint32_t engineSlots_ = 0;
    // A generation spans one busy period. The memory snapshot is taken when
    // the arbiter leaves idle — before any torrent buffers exist — and stays
    // frozen until it is idle again: a live re-detect mid-generation would
    // count budget a running slot has merely not filled yet as free and
    // over-commit.
    bool generationActive_ = false;
    StreamRamMemorySnapshot base_;
    std::vector<Lease> leases_;
    uint64_t nextLeaseId_ = 1;
};

} // namespace pipensx
