#include "../src/app/request_gate.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint64_t MiB = 1024 * 1024;
constexpr size_t kLimit = 256 * MiB;
constexpr uint64_t kAhead = 192 * MiB;
constexpr uint64_t kPiece = 4 * MiB;

using pipensx::RequestGate;

RequestGate makeGate() {
    RequestGate gate;
    gate.configure(kLimit, kAhead, kPiece, 0);
    return gate;
}

// Feed a steady drain of `bps` through `ms` of update ticks so the EMA
// converges near the real rate; keeps buffered/producer position fixed.
void runSteadyDrain(RequestGate& gate, uint64_t& now, uint64_t ms,
                    uint64_t bps, size_t buffered, uint64_t producerOffset) {
    for (uint64_t end = now + ms; now < end;) {
        now += 500;
        gate.onProcessed(static_cast<size_t>(bps / 2));
        gate.update(buffered, 0, producerOffset, now);
    }
}

void test_unconfigured_gate_is_wide_open() {
    RequestGate gate;
    assert(!gate.paused());
    assert(gate.allows(0));
    assert(gate.allows(UINT64_MAX));
    gate.update(1024 * MiB, 3, 17 * MiB, 1000);
    assert(!gate.paused());
    assert(gate.allows(UINT64_MAX));
}

void test_free_flow_matches_request_ahead_window() {
    RequestGate gate = makeGate();
    assert(gate.state() == RequestGate::State::Free);
    assert(gate.allows(kAhead));
    assert(!gate.allows(kAhead + kPiece + 1));

    gate.update(0, 0, 10 * MiB, 1000);
    assert(gate.allows(10 * MiB + kAhead));
}

void test_emergency_pause_and_quick_resume() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(kLimit, 0, 0, now);
    assert(gate.state() == RequestGate::State::Paused);
    assert(gate.paused());
    assert(!gate.allows(0));

    // One piece below the limit resumes into the throttle band instead of
    // waiting for the old 75% drain (the 20-28 s rx=0 window).
    now += 100;
    gate.update(kLimit - kPiece, 0, 0, now);
    assert(!gate.paused());
    assert(gate.state() == RequestGate::State::Throttled);
}

void test_throttle_anchors_edge_to_arrival_frontier() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 0, now);
    assert(gate.allows(kAhead));

    gate.onArrived(0, 100 * MiB);
    now += 100;
    gate.update(kLimit / 4 * 3, 0, 50 * MiB, now);
    assert(gate.state() == RequestGate::State::Throttled);
    // Banked free-flow allowance is forfeited: only a small burst beyond
    // the highest arrived byte stays admissible.
    assert(gate.allows(100 * MiB + 2 * kPiece));
    assert(!gate.allows(100 * MiB + 2 * kPiece + 1));
}

void test_throttled_edge_advances_at_drain_rate() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 0, now);
    runSteadyDrain(gate, now, 10000, 16 * MiB, 0, 0);
    uint64_t drain = gate.drainBps();
    assert(drain > 15 * MiB && drain <= 16 * MiB);

    gate.onArrived(0, 100 * MiB);
    now += 100;
    size_t buffered = kLimit / 4 * 3;
    gate.update(buffered, 0, 50 * MiB, now);
    assert(gate.state() == RequestGate::State::Throttled);
    uint64_t edgeAtEntry = gate.edgeOffset();

    // 4 seconds of steady drain must open roughly 4 s worth of new bytes.
    runSteadyDrain(gate, now, 4000, 16 * MiB, buffered, 50 * MiB);
    uint64_t opened = gate.edgeOffset() - edgeAtEntry;
    assert(opened >= 60 * MiB && opened <= 68 * MiB);
    assert(gate.allows(edgeAtEntry + opened));
}

void test_throttle_with_dead_drain_keeps_head_requestable() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 0, now);
    gate.onArrived(0, 120 * MiB);
    now += 100;
    gate.update(kLimit / 4 * 3, 0, 80 * MiB, now);
    assert(gate.state() == RequestGate::State::Throttled);

    // No drain at all: the bucket stays empty, but the piece at the
    // producer head must remain admissible so the pipeline cannot
    // deadlock on its own gate.
    for (int i = 0; i < 20; ++i) {
        now += 500;
        gate.update(kLimit / 4 * 3, 0, 80 * MiB, now);
    }
    assert(gate.drainBps() == 0);
    assert(gate.allows(80 * MiB));
    assert(!gate.allows(130 * MiB));
}

void test_exit_throttle_restores_free_flow() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 0, now);
    gate.onArrived(0, 100 * MiB);
    now += 100;
    gate.update(kLimit / 4 * 3, 0, 50 * MiB, now);
    assert(gate.state() == RequestGate::State::Throttled);
    assert(!gate.allows(50 * MiB + kAhead));

    // Hysteresis: one piece below the enter threshold leaves the band.
    now += 100;
    gate.update(kLimit / 4 * 3 - kPiece, 0, 50 * MiB, now);
    assert(gate.state() == RequestGate::State::Free);
    assert(gate.allows(50 * MiB + kAhead));
}

void test_free_state_creeps_past_exhausted_ahead_window() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 0, now);
    runSteadyDrain(gate, now, 10000, 16 * MiB, 0, 0);
    assert(!gate.allows(kAhead + 5 * kPiece));

    // Producer stuck at 0 with an exhausted window (the third dead window
    // of the 2026-07-03 run): an active drain keeps creeping the edge past
    // the hard ceiling, bounded by the overshoot cap.
    runSteadyDrain(gate, now, 2000, 16 * MiB, 0, 0);
    assert(gate.allows(kAhead + 4 * kPiece));
    assert(!gate.allows(kAhead + 4 * kPiece + 1));
}

void test_package_rollover_resets_edge_to_new_file() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 500 * MiB, now);
    assert(gate.allows(500 * MiB + kAhead));

    // Next package: offsets restart at zero; a free buffer reopens the
    // full request-ahead window immediately.
    now += 100;
    gate.update(0, 1, 0, now);
    assert(gate.allows(kAhead));
    assert(!gate.allows(kAhead + 5 * kPiece));

    // Arrivals of the old package no longer move the frontier.
    gate.onArrived(0, 800 * MiB);
    gate.onArrived(1, 10 * MiB);
    now += 100;
    gate.update(kLimit / 4 * 3, 1, 5 * MiB, now);
    assert(gate.state() == RequestGate::State::Throttled);
    assert(gate.allows(10 * MiB + 2 * kPiece));
    assert(!gate.allows(10 * MiB + 2 * kPiece + 1));
}

void test_paused_gate_banks_no_tokens() {
    RequestGate gate = makeGate();
    uint64_t now = 1000;
    gate.update(0, 0, 0, now);
    runSteadyDrain(gate, now, 10000, 16 * MiB, 0, 0);

    gate.onArrived(0, 150 * MiB);
    now += 100;
    gate.update(kLimit, 0, 100 * MiB, now);
    assert(gate.paused());

    // A long pause with active drain must not accumulate an admission
    // burst: on resume the edge sits at the frontier plus the small burst,
    // not 10 s of banked drain.
    for (int i = 0; i < 20; ++i) {
        now += 500;
        gate.onProcessed(8 * MiB);
        gate.update(kLimit, 0, 100 * MiB, now);
        assert(gate.paused());
    }
    now += 500;
    gate.update(kLimit - kPiece, 0, 100 * MiB, now);
    assert(gate.state() == RequestGate::State::Throttled);
    assert(!gate.allows(150 * MiB + 2 * kPiece + gate.drainBps()));
}

} // namespace

int main() {
    test_unconfigured_gate_is_wide_open();
    test_free_flow_matches_request_ahead_window();
    test_emergency_pause_and_quick_resume();
    test_throttle_anchors_edge_to_arrival_frontier();
    test_throttled_edge_advances_at_drain_rate();
    test_throttle_with_dead_drain_keeps_head_requestable();
    test_exit_throttle_restores_free_flow();
    test_free_state_creeps_past_exhausted_ahead_window();
    test_package_rollover_resets_edge_to_new_file();
    test_paused_gate_banks_no_tokens();
    std::printf("test_request_gate: all tests passed\n");
    return 0;
}
