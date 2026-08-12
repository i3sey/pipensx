#include "stream_budget_arbiter.hpp"

#include <algorithm>

namespace pipensx {

StreamBudgetArbiter::StreamBudgetArbiter(DetectFn detect,
                                         uint64_t engineSlotOverheadBytes)
    : detect_(detect ? std::move(detect)
                     : DetectFn(&detectStreamRamMemorySnapshot)),
      overheadBytes_(engineSlotOverheadBytes) {}

void StreamBudgetArbiter::ensureGenerationLocked() {
    if (generationActive_)
        return;
    base_ = detect_();
    generationActive_ = true;
}

void StreamBudgetArbiter::endGenerationIfIdleLocked() {
    if (engineSlots_ == 0 && leases_.empty())
        generationActive_ = false;
}

StreamRamBudget StreamBudgetArbiter::shareLocked(uint64_t pieceLengthBytes,
                                                 size_t leaseCount) const {
    const uint64_t availableBytes = base_.heapDetected
        ? base_.heapAvailableBytes : kFallbackStreamRamBytes;
    const size_t leases = std::max<size_t>(leaseCount, 1);
    const uint64_t extraSlots = engineSlots_ > leases
        ? engineSlots_ - leases : 0;
    const uint64_t engineBytes = overheadBytes_ * extraSlots;
    const uint64_t poolBytes = availableBytes > engineBytes
        ? availableBytes - engineBytes : 0;
    StreamRamBudget budget =
        calculateStreamRamBudget(poolBytes / leases, pieceLengthBytes);
    budget.memoryDetected = base_.heapDetected;
    budget.kernelHeadroomDetected = base_.kernelHeadroomDetected;
    budget.kernelHeadroomBytes = base_.kernelHeadroomBytes;
    return budget;
}

void StreamBudgetArbiter::notifyLocked() {
    for (const Lease& lease : leases_)
        if (lease.reconfigure)
            lease.reconfigure(
                shareLocked(lease.pieceLengthBytes, leases_.size()));
}

void StreamBudgetArbiter::engineSlotStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureGenerationLocked();
    ++engineSlots_;
    notifyLocked();
}

void StreamBudgetArbiter::engineSlotFinished() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engineSlots_ > 0)
        --engineSlots_;
    endGenerationIfIdleLocked();
    notifyLocked();
}

uint64_t StreamBudgetArbiter::acquire(uint64_t pieceLengthBytes,
                                      ReconfigureFn cb,
                                      StreamRamBudget& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureGenerationLocked();
    out = shareLocked(pieceLengthBytes, leases_.size() + 1);
    if (!out.valid) {
        endGenerationIfIdleLocked();
        return 0;
    }
    Lease lease;
    lease.id = nextLeaseId_++;
    lease.pieceLengthBytes = pieceLengthBytes;
    lease.reconfigure = std::move(cb);
    const uint64_t id = lease.id;
    leases_.push_back(std::move(lease));
    // Existing leases shrink to make room. The new lease's callback fires
    // too, with the same budget already returned in `out` — harmless.
    notifyLocked();
    return id;
}

void StreamBudgetArbiter::release(uint64_t leaseId) {
    if (!leaseId)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = leases_.begin(); it != leases_.end(); ++it) {
        if (it->id == leaseId) {
            leases_.erase(it);
            break;
        }
    }
    endGenerationIfIdleLocked();
    notifyLocked();
}

} // namespace pipensx
