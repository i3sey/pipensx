#pragma once

// Connectivity ladder orchestrator (RF_ACCESS_PLAN Phase W2).
//
// Runs the first-run connectivity ladder as a state machine: it probes RuTracker
// over each escalating route in order —
//
//   [1] direct  →  [2a] user's manual proxy  →  [2b] antizapret routes  →
//   [3] mirror host
//
// stopping at the first route that reaches a live tracker (W1 probe verdict
// REACHABLE). The winning route becomes the persisted ConnectivityMethod so
// later launches take it silently. If every route fails, the run ends in
// NeedMirror (no mirror configured yet — the wizard should offer one) or, once
// a mirror was tried too, Exhausted (fall back to the bundled stale dump).
//
// The engine is split so the ladder logic is testable without a network:
//   * buildLadder / runLadder / applyLadderResult / manualProxyRoute /
//     deriveMirrorAnnounce are pure (all I/O injected as a ProbeFn).
//   * ConnectivityOrchestrator is the async driver: it runs runLadder on a
//     background thread (probing via tracker_probe), queues progress events for
//     the UI thread to drain with poll(), and is cancellable — it never blocks
//     the borealis event loop. W3 renders the events; W6 restarts it.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_settings.hpp"

extern "C" {
#include "../core/antizapret.h"
#include "../core/tracker.h"
}

namespace pipensx {

// One rung of the connectivity ladder.
enum class LadderRung {
    Direct,       // straight to RuTracker, no proxy
    ManualProxy,  // the user's proxy from settings
    Antizapret,   // an antizapret PAC route
    Mirror,       // a RuTracker mirror host
};

// A single probe the orchestrator wants performed. `route` is applied to the
// curl handle when `useRoute` is set; otherwise the probe goes out direct.
struct LadderAttempt {
    LadderRung rung = LadderRung::Direct;
    std::string announceUrl;
    bool useRoute = false;
    antizapret_route_t route{};
    std::string label;  // human text for UI / telemetry
};

// Injected probe: performs `attempt` and returns a W1 verdict. Real callers bind
// this to tracker_probe; tests bind a stub, so the ladder runs offline.
using ProbeFn = std::function<tracker_probe_result_t(const LadderAttempt&)>;

enum class LadderOutcome {
    Connected,   // a rung reached the tracker; `method` is the winner
    NeedMirror,  // every bypass rung failed and no mirror is configured yet
    Exhausted,   // a configured mirror also failed — fall back to the stale dump
    Cancelled,   // aborted mid-run
};

struct LadderResult {
    LadderOutcome outcome = LadderOutcome::Exhausted;
    ConnectivityMethod method = ConnectivityMethod::Direct;  // valid if Connected
    std::string winningLabel;  // route that won, for the "connected via X" UI
};

// Progress events, emitted in order as the ladder advances. W3 renders these.
enum class LadderEventKind {
    RunStarted,
    AttemptStarted,   // about to probe `attempt`
    AttemptFinished,  // `attempt` returned `verdict`
    RunFinished,      // terminal; `result` is set
};

struct LadderEvent {
    LadderEventKind kind = LadderEventKind::RunStarted;
    LadderAttempt attempt;                                   // Attempt* kinds
    tracker_probe_result_t verdict = TRACKER_PROBE_TIMEOUT;  // AttemptFinished
    LadderResult result;                                     // RunFinished
};

using LadderEventFn = std::function<void(const LadderEvent&)>;

// Inputs the ladder derives its rungs from — plain data so the planner needs no
// settings or antizapret globals. ladderConfigFromSettings() fills it at runtime.
struct LadderConfig {
    std::string directAnnounceUrl;  // e.g. http://bt.t-ru.org/ann?magnet
    bool hasManualProxy = false;
    antizapret_route_t manualProxy{};
    std::string manualProxyLabel;
    std::vector<antizapret_route_t> antizapretRoutes;  // supported, non-direct
    bool hasMirror = false;
    std::string mirrorAnnounceUrl;
    std::string mirrorLabel;
    int probeTimeoutSeconds = 0;  // <=0 → tracker_probe's built-in default
};

// Pure planner: the ordered rungs for a config (the heart of the ladder).
std::vector<LadderAttempt> buildLadder(const LadderConfig& config);

// Pure runner: probe each rung in order until one is REACHABLE, emitting events.
// `cancel` is checked before and after every probe (and, for the real probe, is
// threaded into curl for prompt abort). No globals, no file I/O.
LadderResult runLadder(const LadderConfig& config, const ProbeFn& probe,
                       const LadderEventFn& onEvent, tracker_cancel_cb cancel,
                       void* cancelUser);

// Fold a finished run into settings: Connected records the winning method and
// marks the wizard done; every other outcome leaves settings untouched, so the
// wizard runs again next launch rather than silently serving the stale dump.
void applyLadderResult(const LadderResult& result, AppSettingsData& settings);

// Derive a manual-proxy route from settings; false when none is configured
// (ProxyType::Off or an empty URL) or the route type is unsupported here.
bool manualProxyRoute(const AppSettingsData& settings, antizapret_route_t& out);

// Substitute `host` for the host in `announceUrl`, preserving scheme and path.
// Interim mirror derivation until W4 adds a validated known-mirror allowlist:
// only a bare host[:port] is accepted (a value carrying "://" or "/" yields "").
std::string deriveMirrorAnnounce(const std::string& announceUrl,
                                 const std::string& host);

// Build a LadderConfig from settings plus the live antizapret route table. Thin
// adapter over globals (antizapret_get_routes); the planner/runner stay pure.
LadderConfig ladderConfigFromSettings(const AppSettingsData& settings,
                                      const std::string& directAnnounceUrl,
                                      int probeTimeoutSeconds);

// Async driver. Runs the ladder on a background thread and queues events for the
// UI thread to drain with poll(). Cancellable; the destructor cancels and joins.
class ConnectivityOrchestrator {
public:
    ConnectivityOrchestrator() = default;
    ~ConnectivityOrchestrator();

    ConnectivityOrchestrator(const ConnectivityOrchestrator&) = delete;
    ConnectivityOrchestrator& operator=(const ConnectivityOrchestrator&) = delete;

    // Launch a run probing via tracker_probe (cancellation wired into curl).
    // Returns false if a run is still active — drain it to completion first.
    bool start(LadderConfig config);

    // Test seam: launch with an injected probe (no network).
    bool startWithProbe(LadderConfig config, ProbeFn probe);

    // Drain queued events onto `sink` in order (call each UI frame). Returns
    // true while the run is active or events were delivered this call; false
    // once the run has finished and its queue is empty.
    bool poll(const LadderEventFn& sink);

    // Request abort; safe from any thread. The current probe aborts promptly and
    // the run ends Cancelled.
    void cancel();
    bool running() const { return running_.load(); }

private:
    static int cancelThunk(void* self);
    void launch(LadderConfig config, ProbeFn probe);

    std::thread worker_;
    mutable std::mutex mutex_;
    std::vector<LadderEvent> queue_;
    std::atomic<bool> cancelFlag_{false};
    std::atomic<bool> running_{false};
};

} // namespace pipensx
