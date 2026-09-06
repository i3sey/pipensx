// Screen-off without sleep for long downloads (B5). Switch-only: the PC and
// golden builds never compile this unit (see APP_SOURCES in CMakeLists.txt),
// so no __SWITCH__ guard and no PC shim stub are needed.

#include "switch_backlight.hpp"

extern "C" {
#include "../core/util.h"
}

#include <switch.h>

namespace pipensx {

namespace {

// Fast fade: the saver activity is already covering the UI, so there is no
// visible transition to smooth — and a lingering fade would keep the OLED
// lit while the user thinks the console is off.
constexpr u64 kFadeTime = 1;

void logFailure(const char* operation, Result result) {
    log_msg("[backlight] %s failed: 0x%08x\n", operation,
            static_cast<unsigned int>(result));
}

} // namespace

bool switchBacklightOff() {
    const Result result = lblSwitchBacklightOff(kFadeTime);
    if (R_FAILED(result)) {
        logFailure("switch off", result);
        return false;
    }
    return true;
}

bool switchBacklightOn() {
    const Result result = lblSwitchBacklightOn(kFadeTime);
    if (R_FAILED(result)) {
        logFailure("switch on", result);
        return false;
    }
    return true;
}

SwitchBacklightGuard::~SwitchBacklightGuard() {
    if (off_)
        switchBacklightOn();
}

bool SwitchBacklightGuard::turnOff() {
    off_ = switchBacklightOff();
    return off_;
}

void SwitchBacklightGuard::turnOn() {
    if (!off_)
        return;
    if (switchBacklightOn())
        off_ = false;
}

} // namespace pipensx
