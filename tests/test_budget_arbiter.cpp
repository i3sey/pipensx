#include "app/stream_budget_arbiter.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using pipensx::StreamBudgetArbiter;
using pipensx::StreamRamBudget;
using pipensx::StreamRamMemorySnapshot;
using pipensx::calculateStreamRamBudget;
using pipensx::kFallbackStreamRamBytes;

namespace {

constexpr uint64_t MiB = 1024 * 1024;
constexpr uint64_t kPiece = 4 * MiB;
constexpr uint64_t kOverhead = 64 * MiB;

struct FakeDetect {
    uint64_t availableBytes = 1024 * MiB;
    bool heapDetected = true;
    int calls = 0;

    StreamBudgetArbiter::DetectFn fn() {
        return [this] {
            ++calls;
            StreamRamMemorySnapshot memory;
            memory.heapDetected = heapDetected;
            memory.heapAvailableBytes = availableBytes;
            memory.kernelHeadroomDetected = true;
            memory.kernelHeadroomBytes = 42;
            return memory;
        };
    }
};

bool sameBudget(const StreamRamBudget& a, const StreamRamBudget& b) {
    return a.valid == b.valid && a.peakBytes == b.peakBytes &&
           a.maxQueuedBytes == b.maxQueuedBytes &&
           a.maxBufferedBytes == b.maxBufferedBytes &&
           a.requestAheadBytes == b.requestAheadBytes &&
           a.lookaheadMin == b.lookaheadMin &&
           a.lookaheadStart == b.lookaheadStart &&
           a.lookaheadMax == b.lookaheadMax;
}

// One stream-install lease in one engine slot must see exactly the budget the
// pre-arbiter code computed from the full snapshot: the lease's own engine
// overhead stays covered by the reserve inside calculateStreamRamBudget.
void testSingleLeaseMatchesLegacyBudget() {
    FakeDetect detect;
    StreamBudgetArbiter arbiter(detect.fn(), kOverhead);
    arbiter.engineSlotStarted();
    StreamRamBudget budget;
    uint64_t lease = arbiter.acquire(kPiece, {}, budget);
    assert(lease != 0);
    assert(budget.valid);
    assert(budget.memoryDetected);
    assert(budget.kernelHeadroomDetected);
    assert(budget.kernelHeadroomBytes == 42);
    assert(sameBudget(budget,
                      calculateStreamRamBudget(detect.availableBytes, kPiece)));
    arbiter.release(lease);
    arbiter.engineSlotFinished();
}

void testDetectOncePerGeneration() {
    FakeDetect detect;
    StreamBudgetArbiter arbiter(detect.fn(), kOverhead);
    arbiter.engineSlotStarted();
    assert(detect.calls == 1);
    arbiter.engineSlotStarted();
    StreamRamBudget budget;
    uint64_t lease = arbiter.acquire(kPiece, {}, budget);
    assert(detect.calls == 1); // frozen for the whole busy period
    arbiter.release(lease);
    arbiter.engineSlotFinished();
    assert(detect.calls == 1);
    arbiter.engineSlotFinished();
    // Idle again: the next busy period takes a fresh snapshot.
    arbiter.engineSlotStarted();
    assert(detect.calls == 2);
    arbiter.engineSlotFinished();
}

void testEqualSharesBetweenLeases() {
    FakeDetect detect;
    StreamBudgetArbiter arbiter(detect.fn(), kOverhead);
    arbiter.engineSlotStarted();
    arbiter.engineSlotStarted();
    StreamRamBudget first;
    StreamRamBudget firstResized;
    uint64_t a = arbiter.acquire(
        kPiece,
        [&firstResized](const StreamRamBudget& b) { firstResized = b; },
        first);
    assert(a != 0);
    // Two engine slots but one lease: the leaseless slot is charged as
    // engine overhead.
    assert(sameBudget(first,
                      calculateStreamRamBudget(
                          detect.availableBytes - kOverhead, kPiece)));
    StreamRamBudget second;
    uint64_t b = arbiter.acquire(kPiece, {}, second);
    assert(b != 0);
    // Both leases now hold half the pool; the first was reconfigured live.
    StreamRamBudget half =
        calculateStreamRamBudget(detect.availableBytes / 2, kPiece);
    assert(sameBudget(second, half));
    assert(sameBudget(firstResized, half));
    // Releasing the second turns its slot back into pure overhead.
    arbiter.release(b);
    assert(sameBudget(firstResized,
                      calculateStreamRamBudget(
                          detect.availableBytes - kOverhead, kPiece)));
    arbiter.release(a);
    arbiter.engineSlotFinished();
    arbiter.engineSlotFinished();
}

// A download-only slot has no lease, but its engine RAM (peer buffers, piece
// pool) must still come out of the shared pool.
void testExtraEngineSlotShrinksPool() {
    FakeDetect detect;
    StreamBudgetArbiter arbiter(detect.fn(), kOverhead);
    arbiter.engineSlotStarted(); // the stream-install slot
    StreamRamBudget budget;
    StreamRamBudget resized;
    uint64_t lease = arbiter.acquire(
        kPiece, [&resized](const StreamRamBudget& b) { resized = b; }, budget);
    assert(lease != 0);
    arbiter.engineSlotStarted(); // a download-only slot joins
    assert(sameBudget(resized,
                      calculateStreamRamBudget(
                          detect.availableBytes - kOverhead, kPiece)));
    arbiter.engineSlotFinished(); // and leaves again
    assert(sameBudget(resized,
                      calculateStreamRamBudget(detect.availableBytes, kPiece)));
    arbiter.release(lease);
    arbiter.engineSlotFinished();
}

void testTooSmallPoolRejectsLease() {
    FakeDetect detect;
    detect.availableBytes = 16 * MiB;
    StreamBudgetArbiter arbiter(detect.fn(), kOverhead);
    arbiter.engineSlotStarted();
    StreamRamBudget budget;
    uint64_t lease = arbiter.acquire(kPiece, {}, budget);
    assert(lease == 0);
    assert(!budget.valid);
    arbiter.release(0); // no-op
    arbiter.engineSlotFinished();
}

void testFallbackWhenHeapUndetected() {
    FakeDetect detect;
    detect.heapDetected = false;
    detect.availableBytes = 0;
    StreamBudgetArbiter arbiter(detect.fn(), kOverhead);
    arbiter.engineSlotStarted();
    StreamRamBudget budget;
    uint64_t lease = arbiter.acquire(kPiece, {}, budget);
    assert(lease != 0);
    assert(!budget.memoryDetected);
    assert(sameBudget(budget,
                      calculateStreamRamBudget(kFallbackStreamRamBytes,
                                               kPiece)));
    arbiter.release(lease);
    arbiter.engineSlotFinished();
}

} // namespace

int main() {
    testSingleLeaseMatchesLegacyBudget();
    testDetectOncePerGeneration();
    testEqualSharesBetweenLeases();
    testExtraEngineSlotShrinksPool();
    testTooSmallPoolRejectsLease();
    testFallbackWhenHeapUndetected();
    std::puts("budget arbiter tests passed");
    return 0;
}
