#pragma once

#include "app_settings.hpp"

namespace pipensx {

inline bool shouldKeepTransferScreenOn(const AppSettingsData& settings,
                                       bool transferActive, uint64_t) {
    return transferActive && !settings.blankScreenDuringTransfers;
}

inline bool shouldOpenScreenSaver(const AppSettingsData& settings,
                                  bool transferActive, uint64_t idleMs) {
    return !transferActive && settings.screenSaverEnabled &&
           idleMs >= 5ULL * 60000ULL;
}

class TransferScreenTimer {
public:
    bool update(bool active, bool blank, bool input, uint64_t nowMs) {
        if (!initialized_ || active != active_ || blank != blank_) {
            // A deliberate toggle gets three seconds; resumed work gets seven.
            delayMs_ = initialized_ && active && blank && !blank_ ? 3000 : 7000;
            toggleHeld_ = initialized_ && active && blank && !blank_;
            lastInputMs_ = nowMs;
            initialized_ = true;
        }
        active_ = active;
        blank_ = blank;
        if ((input && !toggleHeld_) || nowMs < lastInputMs_) {
            lastInputMs_ = nowMs;
            delayMs_ = 7000;
        }
        if (!input) toggleHeld_ = false;
        return active && blank && !input && nowMs - lastInputMs_ >= delayMs_;
    }

private:
    bool toggleHeld_ = false;
    bool initialized_ = false;
    bool active_ = false;
    bool blank_ = false;
    uint64_t lastInputMs_ = 0;
    uint64_t delayMs_ = 7000;
};

class WakeInputGuard {
public:
    void begin(uint64_t nowMs) { blocking_ = true; lastInputMs_ = nowMs; }
    bool update(bool input, uint64_t nowMs) {
        if (!blocking_) return false;
        if (input || nowMs < lastInputMs_) lastInputMs_ = nowMs;
        if (!input && nowMs - lastInputMs_ >= 300) blocking_ = false;
        return blocking_;
    }
    bool blocking() const { return blocking_; }
private:
    bool blocking_ = false;
    uint64_t lastInputMs_ = 0;
};

} // namespace pipensx
