#pragma once

#include <cstdint>

namespace pipensx {

class SwitchPerformanceController {
public:
    SwitchPerformanceController() = default;
    ~SwitchPerformanceController();

    SwitchPerformanceController(const SwitchPerformanceController&) = delete;
    SwitchPerformanceController& operator=(
        const SwitchPerformanceController&) = delete;

    void setActive(bool active);
    // Reassert automatic-sleep protection before the display is switched off.
    // Returns false if an active transfer could not be protected.
    bool ensureAwake();
    // Called only while the display policy requests an illuminated panel.
    void keepDisplayOn(uint64_t nowMs);

private:
    void activate();
    void deactivate();

    bool active_ = false;
    bool cpuBoostApplied_ = false;
    bool autoSleepChanged_ = false;
    bool previousAutoSleepDisabled_ = false;
    uint64_t lastDisplayActivityMs_ = 0;
};

} // namespace pipensx
