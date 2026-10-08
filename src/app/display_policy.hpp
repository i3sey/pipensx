#pragma once

#include "app_settings.hpp"

namespace pipensx {

// Uses real controller/touch idle time, independent of the synthetic activity
// reports sent to Horizon to prevent system dimming while this returns true.
inline bool shouldKeepTransferScreenOn(const AppSettingsData& settings,
                                       bool transferActive, uint64_t idleMs) {
    return transferActive && settings.keepScreenOnDuringTransfers &&
        (settings.screenOnMinutes == 0 ||
         idleMs < static_cast<uint64_t>(settings.screenOnMinutes) * 60000ULL);
}

inline bool shouldOpenScreenSaver(const AppSettingsData& settings,
                                  bool transferActive, uint64_t idleMs) {
    return settings.screenSaverEnabled && idleMs >= 5ULL * 60000ULL &&
        !shouldKeepTransferScreenOn(settings, transferActive, idleMs);
}

} // namespace pipensx
