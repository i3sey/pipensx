#pragma once

namespace pipensx {

class SwitchPerformanceController {
public:
    SwitchPerformanceController() = default;
    ~SwitchPerformanceController();

    SwitchPerformanceController(const SwitchPerformanceController&) = delete;
    SwitchPerformanceController& operator=(
        const SwitchPerformanceController&) = delete;

    void setActive(bool active);

private:
    void activate();
    void deactivate();

    bool active_ = false;
    bool cpuBoostApplied_ = false;
    bool autoSleepChanged_ = false;
    bool previousAutoSleepDisabled_ = false;
};

} // namespace pipensx
