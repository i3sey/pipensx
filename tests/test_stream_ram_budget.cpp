#include "../src/app/stream_ram_budget.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint64_t MiB = 1024 * 1024;

void test_full_budget_matches_existing_fast_path() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(1024 * MiB, 4 * MiB);

    assert(budget.valid);
    assert(budget.reserveBytes == 256 * MiB);
    assert(budget.peakBytes == 384 * MiB);
    assert(budget.maxBufferedBytes == 256 * MiB);
    assert(budget.maxQueuedBytes == 64 * MiB);
    assert(budget.requestAheadBytes == 192 * MiB);
    assert(budget.lookaheadMin == 8);
    assert(budget.lookaheadStart == 32);
    assert(budget.lookaheadMax == 32);
}

void test_constrained_memory_reduces_both_windows() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(512 * MiB, 4 * MiB);

    assert(budget.valid);
    assert(budget.peakBytes == 128 * MiB);
    assert(budget.maxBufferedBytes == 88 * MiB);
    assert(budget.maxQueuedBytes == 20 * MiB);
    assert(budget.requestAheadBytes == 68 * MiB);
    assert(budget.lookaheadMin == 8);
    assert(budget.lookaheadStart == 10);
    assert(budget.lookaheadMax == 10);
    assert(budget.maxBufferedBytes +
               budget.lookaheadMax * 4 * MiB <=
           budget.peakBytes);
}

void test_reserve_is_preserved_at_low_memory() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(340 * MiB, 4 * MiB);

    assert(budget.valid);
    assert(budget.peakBytes == 84 * MiB);
    assert(budget.maxBufferedBytes == 64 * MiB);
    assert(budget.lookaheadMin == 5);
    assert(budget.lookaheadStart == 5);
    assert(budget.lookaheadMax == 5);
    assert(budget.availableBytes - budget.peakBytes >= budget.reserveBytes);
}

void test_insufficient_memory_is_rejected() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(320 * MiB, 4 * MiB);

    assert(!budget.valid);
    assert(budget.peakBytes == 0);
}

void test_large_pieces_fit_the_same_peak_bound() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(1024 * MiB, 16 * MiB);

    assert(budget.valid);
    assert(budget.peakBytes == 384 * MiB);
    assert(budget.maxBufferedBytes == 256 * MiB);
    assert(budget.lookaheadMin == 8);
    assert(budget.lookaheadStart == 8);
    assert(budget.lookaheadMax == 8);
    assert(budget.maxBufferedBytes +
               budget.lookaheadMax * 16 * MiB <=
           budget.peakBytes);
}

void test_limits_hold_across_piece_sizes() {
    const uint64_t pieceSizes[] = {1 * MiB, 4 * MiB, 10 * MiB, 16 * MiB};
    for (uint64_t pieceSize : pieceSizes) {
        pipensx::StreamRamBudget budget =
            pipensx::calculateStreamRamBudget(1024 * MiB, pieceSize);
        assert(budget.valid);
        assert(budget.maxBufferedBytes <= 256 * MiB);
        assert(budget.lookaheadMax <= 64);
        assert(budget.peakBytes == budget.maxBufferedBytes +
                                       budget.lookaheadMax * pieceSize);
        assert(budget.availableBytes - budget.peakBytes >=
               budget.reserveBytes);
    }
}

void test_piece_larger_than_peak_capacity_is_rejected() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(2048 * MiB, 512 * MiB);
    assert(!budget.valid);
}

void test_pc_detection_uses_conservative_fallback() {
    pipensx::StreamRamBudget budget =
        pipensx::detectStreamRamBudget(4 * MiB);
    assert(budget.valid);
    assert(!budget.memoryDetected);
    assert(budget.availableBytes == 384 * MiB);
    assert(budget.peakBytes == 96 * MiB);
}

} // namespace

int main() {
    test_full_budget_matches_existing_fast_path();
    test_constrained_memory_reduces_both_windows();
    test_reserve_is_preserved_at_low_memory();
    test_insufficient_memory_is_rejected();
    test_large_pieces_fit_the_same_peak_bound();
    test_limits_hold_across_piece_sizes();
    test_piece_larger_than_peak_capacity_is_rejected();
    test_pc_detection_uses_conservative_fallback();
    std::puts("stream RAM budget tests passed");
    return 0;
}
