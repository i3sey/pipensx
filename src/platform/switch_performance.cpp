#include "switch_performance.hpp"

extern "C" {
#include "../core/util.h"
}

#include <switch.h>

namespace pipensx {

namespace {

void logFailure(const char* operation, Result result) {
    log_msg("[performance] %s failed: 0x%08x\n", operation,
            static_cast<unsigned int>(result));
}

} // namespace

SwitchPerformanceController::~SwitchPerformanceController() {
    deactivate();
}

void SwitchPerformanceController::setActive(bool active) {
    if (active == active_)
        return;
    active_ = active;
    if (active)
        activate();
    else
        deactivate();
}

void SwitchPerformanceController::activate() {
    if (!cpuBoostApplied_ && hosversionAtLeast(7, 0, 0)) {
        Result result = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        if (R_SUCCEEDED(result))
            cpuBoostApplied_ = true;
        else
            logFailure("enable CPU boost", result);
    }

    if (!autoSleepChanged_ && hosversionAtLeast(5, 0, 0)) {
        ensureAwake();
    }
}

bool SwitchPerformanceController::ensureAwake() {
    if (!active_)
        return true;
    if (!hosversionAtLeast(5, 0, 0))
        return false;
    bool previous = previousAutoSleepDisabled_;
    if (!autoSleepChanged_) {
        Result result = appletIsAutoSleepDisabled(&previous);
        if (R_FAILED(result)) {
            logFailure("read auto-sleep state", result);
            return false;
        }
    }
    Result result = appletSetAutoSleepDisabled(true);
    if (R_SUCCEEDED(result)) {
        previousAutoSleepDisabled_ = previous;
        autoSleepChanged_ = true;
        return true;
    }
    logFailure("disable auto-sleep", result);
    return false;
}

void SwitchPerformanceController::keepDisplayOn(uint64_t nowMs) {
    if (!active_) return;
    if (lastDisplayActivityMs_ && nowMs >= lastDisplayActivityMs_ &&
        nowMs - lastDisplayActivityMs_ < 15000) return;
    lastDisplayActivityMs_ = nowMs;
    ensureAwake();
    if (hosversionAtLeast(2, 0, 0)) {
        const Result result = appletReportUserIsActive();
        if (R_FAILED(result)) logFailure("keep display active", result);
    }
}

void SwitchPerformanceController::deactivate() {
    lastDisplayActivityMs_ = 0;
    if (cpuBoostApplied_) {
        Result result = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        if (R_SUCCEEDED(result))
            cpuBoostApplied_ = false;
        else
            logFailure("disable CPU boost", result);
    }

    if (autoSleepChanged_) {
        Result result =
            appletSetAutoSleepDisabled(previousAutoSleepDisabled_);
        if (R_SUCCEEDED(result))
            autoSleepChanged_ = false;
        else
            logFailure("restore auto-sleep state", result);
    }
}

} // namespace pipensx
