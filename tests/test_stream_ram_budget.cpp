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

void test_low_memory_scales_reserve_instead_of_rejecting_install() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(320 * MiB, 2 * MiB);

    assert(budget.valid);
    assert(budget.reserveBytes == 160 * MiB);
    assert(budget.peakBytes == 80 * MiB);
    assert(budget.maxBufferedBytes == 64 * MiB);
    assert(budget.maxQueuedBytes == 16 * MiB);
    assert(budget.requestAheadBytes == 48 * MiB);
    assert(budget.lookaheadMin == 8);
    assert(budget.lookaheadStart == 8);
    assert(budget.lookaheadMax == 8);
    assert(budget.availableBytes - budget.peakBytes >= budget.reserveBytes);
}

void test_severely_constrained_memory_reduces_buffer_below_preferred_minimum() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(96 * MiB, 2 * MiB);

    assert(budget.valid);
    assert(budget.reserveBytes == 64 * MiB);
    assert(budget.peakBytes == 32 * MiB);
    assert(budget.maxBufferedBytes == 30 * MiB);
    assert(budget.lookaheadMin == 1);
    assert(budget.lookaheadStart == 1);
    assert(budget.lookaheadMax == 1);
    assert(budget.availableBytes - budget.peakBytes >= budget.reserveBytes);
}

void test_truly_insufficient_memory_is_rejected() {
    pipensx::StreamRamBudget budget =
        pipensx::calculateStreamRamBudget(67 * MiB, 2 * MiB);

    assert(!budget.valid);
    assert(budget.reserveBytes == 64 * MiB);
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

void test_limits_hold_across_low_and_high_memory() {
    const uint64_t availableSizes[] = {
        68 * MiB, 80 * MiB, 96 * MiB, 128 * MiB,
        192 * MiB, 320 * MiB, 384 * MiB, 512 * MiB, 1024 * MiB,
    };
    const uint64_t pieceSizes[] = {
        1 * MiB, 2 * MiB, 4 * MiB, 8 * MiB, 16 * MiB,
    };

    for (uint64_t available : availableSizes) {
        for (uint64_t pieceSize : pieceSizes) {
            pipensx::StreamRamBudget budget =
                pipensx::calculateStreamRamBudget(available, pieceSize);
            const bool canFitMinimum =
                available >= budget.reserveBytes + 2 * pieceSize;
            assert(budget.valid == canFitMinimum);
            if (!budget.valid)
                continue;
            assert(budget.reserveBytes <= available);
            assert(budget.peakBytes <= available - budget.reserveBytes);
            assert(budget.peakBytes <= 384 * MiB);
            assert(budget.maxBufferedBytes <= 256 * MiB);
            assert(budget.maxQueuedBytes <= 64 * MiB);
            assert(budget.maxQueuedBytes <= budget.maxBufferedBytes);
            assert(budget.lookaheadMax >= 1);
            assert(budget.peakBytes == budget.maxBufferedBytes +
                                           budget.lookaheadMax * pieceSize);
        }
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

void test_heap_availability_wins_over_post_allocation_kernel_headroom() {
    pipensx::StreamRamMemorySnapshot memory;
    memory.heapDetected = true;
    memory.heapAvailableBytes = 512 * MiB;
    memory.kernelHeadroomDetected = true;
    memory.kernelHeadroomBytes = 4145152;

    pipensx::StreamRamBudget budget =
        pipensx::selectStreamRamBudget(memory, 2 * MiB);

    assert(budget.valid);
    assert(budget.memoryDetected);
    assert(budget.availableBytes == 512 * MiB);
    assert(budget.kernelHeadroomDetected);
    assert(budget.kernelHeadroomBytes == 4145152);
}

} // namespace

int main() {
    test_full_budget_matches_existing_fast_path();
    test_constrained_memory_reduces_both_windows();
    test_low_memory_scales_reserve_instead_of_rejecting_install();
    test_severely_constrained_memory_reduces_buffer_below_preferred_minimum();
    test_truly_insufficient_memory_is_rejected();
    test_large_pieces_fit_the_same_peak_bound();
    test_limits_hold_across_piece_sizes();
    test_limits_hold_across_low_and_high_memory();
    test_piece_larger_than_peak_capacity_is_rejected();
    test_pc_detection_uses_conservative_fallback();
    test_heap_availability_wins_over_post_allocation_kernel_headroom();
    std::puts("stream RAM budget tests passed");
    return 0;
}
