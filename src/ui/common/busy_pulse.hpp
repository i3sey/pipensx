#pragma once

#include <borealis.hpp>

namespace pipensx::ui {

// Soft alpha pulse on an existing View — no new widget class. Drive the
// view's public Animatable alpha between lo/hi until stopBusyPulse().
inline void startBusyPulse(brls::View* view) {
    if (!view)
        return;
    constexpr float kLo = 0.35f;
    constexpr float kHi = 1.0f;
    constexpr int32_t kHalfMs = 550;
    view->alpha.stop();
    view->alpha.reset(kHi);
    view->alpha.setEndCallback([view](bool done) {
        if (done)
            startBusyPulse(view);
    });
    view->alpha.addStep(kLo, kHalfMs, brls::EasingFunction::quadraticInOut);
    view->alpha.addStep(kHi, kHalfMs, brls::EasingFunction::quadraticInOut);
    view->alpha.start();
}

inline void stopBusyPulse(brls::View* view) {
    if (!view)
        return;
    view->alpha.setEndCallback([](bool) {});
    view->alpha.stop();
    view->setAlpha(1.0f);
}

}  // namespace pipensx::ui
