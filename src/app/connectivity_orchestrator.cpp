#include "connectivity_orchestrator.hpp"

extern "C" {
#include "../core/util.h"  // log_msg
}

#include <cstdio>
#include <utility>

namespace pipensx {
namespace {

ConnectivityMethod methodForRung(LadderRung rung) {
    switch (rung) {
    case LadderRung::ManualProxy:
        return ConnectivityMethod::Proxy;
    case LadderRung::Antizapret:
        return ConnectivityMethod::Antizapret;
    case LadderRung::Mirror:
        return ConnectivityMethod::Mirror;
    case LadderRung::Direct:
    default:
        return ConnectivityMethod::Direct;
    }
}

const char* verdictName(tracker_probe_result_t verdict) {
    switch (verdict) {
    case TRACKER_PROBE_REACHABLE:
        return "reachable";
    case TRACKER_PROBE_BLOCKED:
        return "blocked";
    default:
        return "timeout";
    }
}

bool cancelRequested(tracker_cancel_cb cancel, void* user) {
    return cancel && cancel(user) != 0;
}

} // namespace

std::vector<LadderAttempt> buildLadder(const LadderConfig& config) {
    std::vector<LadderAttempt> rungs;

    // [1] Direct — always tried first; if RuTracker is reachable we never proxy.
    LadderAttempt direct;
    direct.rung = LadderRung::Direct;
    direct.announceUrl = config.directAnnounceUrl;
    direct.useRoute = false;
    direct.label = "direct";
    rungs.push_back(std::move(direct));

    // [2a] The user's own proxy, when configured — preferred over antizapret.
    if (config.hasManualProxy) {
        LadderAttempt proxy;
        proxy.rung = LadderRung::ManualProxy;
        proxy.announceUrl = config.directAnnounceUrl;
        proxy.useRoute = true;
        proxy.route = config.manualProxy;
        proxy.label =
            config.manualProxyLabel.empty() ? "manual proxy"
                                            : config.manualProxyLabel;
        rungs.push_back(std::move(proxy));
    }

    // [2b] Antizapret bypass routes, in the order the PAC lists them.
    for (const antizapret_route_t& route : config.antizapretRoutes) {
        LadderAttempt bypass;
        bypass.rung = LadderRung::Antizapret;
        bypass.announceUrl = config.directAnnounceUrl;
        bypass.useRoute = true;
        bypass.route = route;
        bypass.label = std::string("antizapret ") + antizapret_route_name(&route);
        rungs.push_back(std::move(bypass));
    }

    // [3] A RuTracker mirror host, when one is configured (probed direct).
    if (config.hasMirror) {
        LadderAttempt mirror;
        mirror.rung = LadderRung::Mirror;
        mirror.announceUrl = config.mirrorAnnounceUrl;
        mirror.useRoute = false;
        mirror.label = config.mirrorLabel.empty() ? "mirror" : config.mirrorLabel;
        rungs.push_back(std::move(mirror));
    }

    return rungs;
}

LadderResult runLadder(const LadderConfig& config, const ProbeFn& probe,
                       const LadderEventFn& onEvent, tracker_cancel_cb cancel,
                       void* cancelUser) {
    auto emit = [&](const LadderEvent& event) {
        if (onEvent)
            onEvent(event);
    };

    LadderEvent started;
    started.kind = LadderEventKind::RunStarted;
    emit(started);

    LadderResult result;
    for (const LadderAttempt& attempt : buildLadder(config)) {
        if (cancelRequested(cancel, cancelUser)) {
            result.outcome = LadderOutcome::Cancelled;
            break;
        }

        LadderEvent begin;
        begin.kind = LadderEventKind::AttemptStarted;
        begin.attempt = attempt;
        emit(begin);

        tracker_probe_result_t verdict =
            probe ? probe(attempt) : TRACKER_PROBE_TIMEOUT;

        LadderEvent finished;
        finished.kind = LadderEventKind::AttemptFinished;
        finished.attempt = attempt;
        finished.verdict = verdict;
        emit(finished);
        log_msg("[wizard] rung %s -> %s\n", attempt.label.c_str(),
                verdictName(verdict));

        // A cancel during the probe surfaces as TIMEOUT; honor an explicit
        // cancel request as Cancelled rather than treating it as a failed rung.
        if (cancelRequested(cancel, cancelUser)) {
            result.outcome = LadderOutcome::Cancelled;
            break;
        }
        if (verdict == TRACKER_PROBE_REACHABLE) {
            result.outcome = LadderOutcome::Connected;
            result.method = methodForRung(attempt.rung);
            result.winningLabel = attempt.label;
            break;
        }
    }

    if (result.outcome != LadderOutcome::Connected &&
        result.outcome != LadderOutcome::Cancelled) {
        // Every rung failed. A mirror already in the ladder means we are out of
        // options (stale dump); otherwise the wizard should offer one.
        result.outcome =
            config.hasMirror ? LadderOutcome::Exhausted : LadderOutcome::NeedMirror;
    }

    switch (result.outcome) {
    case LadderOutcome::Connected:
        log_msg("[wizard] connected via %s\n", result.winningLabel.c_str());
        break;
    case LadderOutcome::NeedMirror:
        log_msg("[wizard] all bypass routes failed, offering mirror\n");
        break;
    case LadderOutcome::Exhausted:
        log_msg("[wizard] exhausted, falling back to stale dump\n");
        break;
    case LadderOutcome::Cancelled:
        log_msg("[wizard] cancelled\n");
        break;
    }

    LadderEvent done;
    done.kind = LadderEventKind::RunFinished;
    done.result = result;
    emit(done);
    return result;
}

void applyLadderResult(const LadderResult& result, AppSettingsData& settings) {
    if (result.outcome != LadderOutcome::Connected)
        return;
    settings.connectivityMethod = result.method;
    settings.connectivitySetupDone = true;
}

bool manualProxyRoute(const AppSettingsData& settings, antizapret_route_t& out) {
    if (settings.manualProxyType == ProxyType::Off ||
        settings.manualProxyUrl.empty())
        return false;
    out = antizapret_route_t{};
    out.type = settings.manualProxyType == ProxyType::Socks5
                   ? ANTIZAPRET_ROUTE_SOCKS5
                   : ANTIZAPRET_ROUTE_HTTP;
    std::snprintf(out.address, sizeof(out.address), "%s",
                  settings.manualProxyUrl.c_str());
    return antizapret_route_supported(&out) != 0;
}

std::string deriveMirrorAnnounce(const std::string& announceUrl,
                                 const std::string& host) {
    // Reject anything that is not a bare host[:port]; a full URL or a path is
    // W4's job to validate, not ours to blindly probe.
    if (host.empty() || host.find("://") != std::string::npos ||
        host.find('/') != std::string::npos)
        return std::string();

    const std::string scheme = "://";
    size_t schemeEnd = announceUrl.find(scheme);
    if (schemeEnd == std::string::npos)
        return std::string();
    size_t hostStart = schemeEnd + scheme.size();
    size_t pathStart = announceUrl.find('/', hostStart);
    std::string prefix = announceUrl.substr(0, hostStart);
    std::string path =
        pathStart == std::string::npos ? std::string()
                                       : announceUrl.substr(pathStart);
    return prefix + host + path;
}

LadderConfig ladderConfigFromSettings(const AppSettingsData& settings,
                                      const std::string& directAnnounceUrl,
                                      int probeTimeoutSeconds) {
    LadderConfig config;
    config.directAnnounceUrl = directAnnounceUrl;
    config.probeTimeoutSeconds = probeTimeoutSeconds;

    antizapret_route_t proxy;
    if (manualProxyRoute(settings, proxy)) {
        config.hasManualProxy = true;
        config.manualProxy = proxy;
        config.manualProxyLabel = std::string("manual ") +
                                  antizapret_route_name(&proxy) + " " +
                                  settings.manualProxyUrl;
    }

    if (settings.useAntizapret) {
        antizapret_route_t routes[ANTIZAPRET_MAX_ROUTES];
        size_t count = antizapret_get_routes(routes, ANTIZAPRET_MAX_ROUTES);
        for (size_t i = 0; i < count; ++i) {
            if (routes[i].type == ANTIZAPRET_ROUTE_DIRECT)
                continue;  // the direct rung already covers this
            if (antizapret_route_supported(&routes[i]) == 0)
                continue;  // e.g. HTTPS proxy on Switch
            config.antizapretRoutes.push_back(routes[i]);
        }
    }

    std::string mirror =
        deriveMirrorAnnounce(directAnnounceUrl, settings.rutrackerHost);
    if (!mirror.empty()) {
        config.hasMirror = true;
        config.mirrorAnnounceUrl = std::move(mirror);
        config.mirrorLabel = std::string("mirror ") + settings.rutrackerHost;
    }

    return config;
}

// ---- async driver ----

int ConnectivityOrchestrator::cancelThunk(void* self) {
    return static_cast<ConnectivityOrchestrator*>(self)->cancelFlag_.load() ? 1
                                                                            : 0;
}

void ConnectivityOrchestrator::launch(LadderConfig config, ProbeFn probe) {
    cancelFlag_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    running_.store(true);
    worker_ = std::thread([this, config = std::move(config),
                           probe = std::move(probe)]() mutable {
        auto onEvent = [this](const LadderEvent& event) {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(event);
        };
        runLadder(config, probe, onEvent, &ConnectivityOrchestrator::cancelThunk,
                  this);
        running_.store(false);
    });
}

bool ConnectivityOrchestrator::start(LadderConfig config) {
    if (running_.load())
        return false;
    if (worker_.joinable())
        worker_.join();  // reap a previous, already-finished run
    int timeout = config.probeTimeoutSeconds;
    ProbeFn probe = [this, timeout](const LadderAttempt& attempt) {
        return tracker_probe(attempt.announceUrl.c_str(),
                             attempt.useRoute ? &attempt.route : nullptr, timeout,
                             &ConnectivityOrchestrator::cancelThunk, this);
    };
    launch(std::move(config), std::move(probe));
    return true;
}

bool ConnectivityOrchestrator::startWithProbe(LadderConfig config,
                                              ProbeFn probe) {
    if (running_.load())
        return false;
    if (worker_.joinable())
        worker_.join();
    launch(std::move(config), std::move(probe));
    return true;
}

bool ConnectivityOrchestrator::poll(const LadderEventFn& sink) {
    std::vector<LadderEvent> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    if (sink) {
        for (const LadderEvent& event : drained)
            sink(event);
    }
    // RunFinished is enqueued before running_ flips false, so it is never lost:
    // a poll that sees running_ still true will observe it (and its queue) next.
    return running_.load() || !drained.empty();
}

void ConnectivityOrchestrator::cancel() { cancelFlag_.store(true); }

ConnectivityOrchestrator::~ConnectivityOrchestrator() {
    cancelFlag_.store(true);
    if (worker_.joinable())
        worker_.join();
}

} // namespace pipensx
