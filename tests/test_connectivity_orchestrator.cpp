#include "app/connectivity_orchestrator.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pipensx;

namespace {

const char* kDirect = "http://bt.t-ru.org/ann?magnet";

antizapret_route_t makeRoute(antizapret_route_type_t type, const char* address) {
    antizapret_route_t route{};
    route.type = type;
    std::snprintf(route.address, sizeof(route.address), "%s", address);
    return route;
}

LadderConfig baseConfig() {
    LadderConfig config;
    config.directAnnounceUrl = kDirect;
    return config;
}

// A scripted probe: returns verdicts[i] for the i-th probe, recording the
// attempts it saw. Anything past the script is TIMEOUT.
struct ScriptedProbe {
    std::vector<tracker_probe_result_t> verdicts;
    std::vector<LadderAttempt> seen;

    ProbeFn fn() {
        return [this](const LadderAttempt& attempt) {
            size_t index = seen.size();
            seen.push_back(attempt);
            return index < verdicts.size() ? verdicts[index]
                                           : TRACKER_PROBE_TIMEOUT;
        };
    }
};

// The ladder never checks cancellation.
int noCancel(void*) { return 0; }

LadderResult run(const LadderConfig& config, ScriptedProbe& probe) {
    return runLadder(config, probe.fn(), nullptr, noCancel, nullptr);
}

void testBuildLadderOrder() {
    LadderConfig config = baseConfig();
    config.hasManualProxy = true;
    config.manualProxy = makeRoute(ANTIZAPRET_ROUTE_HTTP, "10.0.0.9:8080");
    config.manualProxyLabel = "manual proxy";
    config.antizapretRoutes.push_back(
        makeRoute(ANTIZAPRET_ROUTE_SOCKS5, "az1:1080"));
    config.antizapretRoutes.push_back(
        makeRoute(ANTIZAPRET_ROUTE_HTTP, "az2:3128"));
    config.hasMirror = true;
    config.mirrorAnnounceUrl = "http://bt.mirror.example/ann?magnet";
    config.mirrorLabel = "mirror bt.mirror.example";

    std::vector<LadderAttempt> rungs = buildLadder(config);
    assert(rungs.size() == 5);
    assert(rungs[0].rung == LadderRung::Direct);
    assert(!rungs[0].useRoute);
    assert(rungs[0].announceUrl == kDirect);
    assert(rungs[1].rung == LadderRung::ManualProxy);
    assert(rungs[1].useRoute);
    assert(rungs[1].announceUrl == kDirect);  // proxy tunnels to the same host
    assert(rungs[2].rung == LadderRung::Antizapret);
    assert(rungs[2].route.type == ANTIZAPRET_ROUTE_SOCKS5);
    assert(rungs[3].rung == LadderRung::Antizapret);
    assert(rungs[3].route.type == ANTIZAPRET_ROUTE_HTTP);
    assert(rungs[4].rung == LadderRung::Mirror);
    assert(!rungs[4].useRoute);
    assert(rungs[4].announceUrl == "http://bt.mirror.example/ann?magnet");

    // With nothing configured, only the direct rung exists.
    std::vector<LadderAttempt> bare = buildLadder(baseConfig());
    assert(bare.size() == 1 && bare[0].rung == LadderRung::Direct);
}

void testDirectSuccessStopsEarly() {
    LadderConfig config = baseConfig();
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_HTTP, "az:1"));
    ScriptedProbe probe{{TRACKER_PROBE_REACHABLE}, {}};
    LadderResult result = run(config, probe);
    assert(result.outcome == LadderOutcome::Connected);
    assert(result.method == ConnectivityMethod::Direct);
    assert(probe.seen.size() == 1);  // never touched the antizapret rung
    assert(result.winningLabel == "direct");
}

void testFallsThroughToAntizapret() {
    LadderConfig config = baseConfig();
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_SOCKS5, "a:1"));
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_HTTP, "b:2"));
    // direct blocked, first az times out, second az reaches the tracker.
    ScriptedProbe probe{{TRACKER_PROBE_BLOCKED, TRACKER_PROBE_TIMEOUT,
                         TRACKER_PROBE_REACHABLE},
                        {}};
    LadderResult result = run(config, probe);
    assert(result.outcome == LadderOutcome::Connected);
    assert(result.method == ConnectivityMethod::Antizapret);
    assert(probe.seen.size() == 3);
    assert(probe.seen[2].route.type == ANTIZAPRET_ROUTE_HTTP);
}

void testManualProxyPreferredOverAntizapret() {
    LadderConfig config = baseConfig();
    config.hasManualProxy = true;
    config.manualProxy = makeRoute(ANTIZAPRET_ROUTE_SOCKS5, "user:1080");
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_HTTP, "az:1"));
    // direct blocked, manual proxy reaches the tracker → antizapret untouched.
    ScriptedProbe probe{{TRACKER_PROBE_BLOCKED, TRACKER_PROBE_REACHABLE}, {}};
    LadderResult result = run(config, probe);
    assert(result.outcome == LadderOutcome::Connected);
    assert(result.method == ConnectivityMethod::Proxy);
    assert(probe.seen.size() == 2);
}

void testNeedMirrorWhenBypassExhaustedNoMirror() {
    LadderConfig config = baseConfig();
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_HTTP, "az:1"));
    ScriptedProbe probe{{TRACKER_PROBE_BLOCKED, TRACKER_PROBE_BLOCKED}, {}};
    LadderResult result = run(config, probe);
    assert(result.outcome == LadderOutcome::NeedMirror);
    assert(probe.seen.size() == 2);
}

void testExhaustedWhenMirrorAlsoFails() {
    LadderConfig config = baseConfig();
    config.hasMirror = true;
    config.mirrorAnnounceUrl = "http://bt.mirror.example/ann?magnet";
    ScriptedProbe probe{{TRACKER_PROBE_BLOCKED, TRACKER_PROBE_TIMEOUT}, {}};
    LadderResult result = run(config, probe);
    assert(result.outcome == LadderOutcome::Exhausted);
    assert(probe.seen.size() == 2);
    assert(probe.seen[1].rung == LadderRung::Mirror);
}

void testMirrorSuccess() {
    LadderConfig config = baseConfig();
    config.hasMirror = true;
    config.mirrorAnnounceUrl = "http://bt.mirror.example/ann?magnet";
    config.mirrorLabel = "mirror bt.mirror.example";
    ScriptedProbe probe{{TRACKER_PROBE_BLOCKED, TRACKER_PROBE_REACHABLE}, {}};
    LadderResult result = run(config, probe);
    assert(result.outcome == LadderOutcome::Connected);
    assert(result.method == ConnectivityMethod::Mirror);
    assert(result.winningLabel == "mirror bt.mirror.example");
}

// Cancel callback that trips after `after` probes have run.
struct CountingCancel {
    int after = 0;
    int calls = 0;
    static int thunk(void* self) {
        CountingCancel* c = static_cast<CountingCancel*>(self);
        return c->calls++ >= c->after ? 1 : 0;
    }
};

void testCancellationStopsLadder() {
    LadderConfig config = baseConfig();
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_HTTP, "a:1"));
    config.antizapretRoutes.push_back(makeRoute(ANTIZAPRET_ROUTE_HTTP, "b:2"));
    ScriptedProbe probe{{TRACKER_PROBE_BLOCKED, TRACKER_PROBE_BLOCKED,
                         TRACKER_PROBE_BLOCKED},
                        {}};
    // cancel returns 0 for the pre-direct check and direct's post-probe check,
    // then 1 — so the ladder aborts after the first rung rather than probing on.
    CountingCancel cancel;
    cancel.after = 2;
    LadderResult result = runLadder(config, probe.fn(), nullptr,
                                    &CountingCancel::thunk, &cancel);
    assert(result.outcome == LadderOutcome::Cancelled);
    assert(probe.seen.size() == 1);
}

void testApplyLadderResultOnlyPersistsOnConnected() {
    AppSettingsData base;  // defaults: not done, Direct
    {
        AppSettingsData settings = base;
        LadderResult result;
        result.outcome = LadderOutcome::Connected;
        result.method = ConnectivityMethod::Antizapret;
        applyLadderResult(result, settings);
        assert(settings.connectivitySetupDone);
        assert(settings.connectivityMethod == ConnectivityMethod::Antizapret);
    }
    for (LadderOutcome outcome :
         {LadderOutcome::NeedMirror, LadderOutcome::Exhausted,
          LadderOutcome::Cancelled}) {
        AppSettingsData settings = base;
        LadderResult result;
        result.outcome = outcome;
        result.method = ConnectivityMethod::Mirror;  // must be ignored
        applyLadderResult(result, settings);
        assert(settings == base);  // untouched → wizard runs again, no silent stale
    }
}

void testManualProxyRouteConversion() {
    antizapret_route_t route;

    AppSettingsData off;  // ProxyType::Off by default
    assert(!manualProxyRoute(off, route));

    AppSettingsData emptyUrl;
    emptyUrl.manualProxyType = ProxyType::Http;
    assert(!manualProxyRoute(emptyUrl, route));

    AppSettingsData http;
    http.manualProxyType = ProxyType::Http;
    http.manualProxyUrl = "http://10.0.0.1:8080";
    assert(manualProxyRoute(http, route));
    assert(route.type == ANTIZAPRET_ROUTE_HTTP);
    assert(std::string(route.address) == "http://10.0.0.1:8080");

    AppSettingsData socks;
    socks.manualProxyType = ProxyType::Socks5;
    socks.manualProxyUrl = "10.0.0.2:1080";
    assert(manualProxyRoute(socks, route));
    assert(route.type == ANTIZAPRET_ROUTE_SOCKS5);
    assert(std::string(route.address) == "10.0.0.2:1080");
}

void testDeriveMirrorAnnounce() {
    // Bare host swaps in, scheme + path preserved.
    assert(deriveMirrorAnnounce("http://bt.t-ru.org/ann?magnet",
                                "bt.mirror.example") ==
           "http://bt.mirror.example/ann?magnet");
    // host:port is accepted.
    assert(deriveMirrorAnnounce("http://bt.t-ru.org/ann", "mirror:8080") ==
           "http://mirror:8080/ann");
    // No path is fine.
    assert(deriveMirrorAnnounce("http://bt.t-ru.org", "m.example") ==
           "http://m.example");
    // A value carrying a scheme or a path is rejected (W4 hardens this).
    assert(deriveMirrorAnnounce("http://bt.t-ru.org/ann",
                                "http://evil.example") == "");
    assert(deriveMirrorAnnounce("http://bt.t-ru.org/ann",
                                "evil.example/path") == "");
    assert(deriveMirrorAnnounce("http://bt.t-ru.org/ann", "") == "");
}

// ---- async driver ----

std::vector<LadderEvent> drainToCompletion(ConnectivityOrchestrator& orch) {
    std::vector<LadderEvent> events;
    auto sink = [&](const LadderEvent& event) { events.push_back(event); };
    int guard = 0;
    while (orch.poll(sink)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(++guard < 5000);  // never spin forever
    }
    return events;
}

void testOrchestratorAsyncRun() {
    LadderConfig config = baseConfig();
    ConnectivityOrchestrator orch;
    ProbeFn probe = [](const LadderAttempt&) { return TRACKER_PROBE_REACHABLE; };
    assert(orch.startWithProbe(config, probe));
    assert(!orch.startWithProbe(config, probe));  // rejected while running

    std::vector<LadderEvent> events = drainToCompletion(orch);
    assert(!events.empty());
    assert(events.front().kind == LadderEventKind::RunStarted);
    assert(events.back().kind == LadderEventKind::RunFinished);
    assert(events.back().result.outcome == LadderOutcome::Connected);
    assert(events.back().result.method == ConnectivityMethod::Direct);
    assert(!orch.running());
}

void testOrchestratorCancel() {
    LadderConfig config = baseConfig();  // single direct rung
    ConnectivityOrchestrator orch;

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    // The probe parks inside the first rung until the test cancels and releases,
    // then reports BLOCKED — the post-probe cancel check must end the run.
    ProbeFn probe = [&](const LadderAttempt&) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
        return TRACKER_PROBE_BLOCKED;
    };
    assert(orch.startWithProbe(config, probe));
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    orch.cancel();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();

    std::vector<LadderEvent> events = drainToCompletion(orch);
    assert(events.back().kind == LadderEventKind::RunFinished);
    assert(events.back().result.outcome == LadderOutcome::Cancelled);
}

} // namespace

int main() {
    testBuildLadderOrder();
    testDirectSuccessStopsEarly();
    testFallsThroughToAntizapret();
    testManualProxyPreferredOverAntizapret();
    testNeedMirrorWhenBypassExhaustedNoMirror();
    testExhaustedWhenMirrorAlsoFails();
    testMirrorSuccess();
    testCancellationStopsLadder();
    testApplyLadderResultOnlyPersistsOnConnected();
    testManualProxyRouteConversion();
    testDeriveMirrorAnnounce();
    testOrchestratorAsyncRun();
    testOrchestratorCancel();
    std::puts("connectivity orchestrator tests passed");
    return 0;
}
