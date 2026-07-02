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
        bool previous = false;
        Result result = appletIsAutoSleepDisabled(&previous);
        if (R_FAILED(result)) {
            logFailure("read auto-sleep state", result);
            return;
        }
        result = appletSetAutoSleepDisabled(true);
        if (R_SUCCEEDED(result)) {
            previousAutoSleepDisabled_ = previous;
            autoSleepChanged_ = true;
        } else {
            logFailure("disable auto-sleep", result);
        }
    }
}

void SwitchPerformanceController::deactivate() {
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
