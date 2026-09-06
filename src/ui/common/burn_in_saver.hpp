#pragma once

#include <cmath>
#include <cstdint>

#include <borealis.hpp>

#include "ui/theme.hpp"

namespace pipensx::ui {

// Full-screen OLED burn-in guard: pure black plus one slowly drifting dim
// marker so static UI chrome (sidebars, progress bars) does not sit on the
// same pixels for hours. Dismissal and the panel are owned by main_switch's
// idle loop — any controller button or touch there pops this activity AND
// switches the backlight back on through lbl, without touching the download
// queue — so D-pad and touch work the same as face buttons without racing
// double-pops, and the wake press can never kill a transfer. The loop also
// runs a heartbeat watchdog that forces the panel back on if the UI thread
// ever stalls while the screen is off, so a hang presents as a frozen UI
// with a log line instead of a dead black panel.
class BurnInSaverView : public brls::Box {
public:
    BurnInSaverView() {
        setFocusable(true);
        setGrow(1.f);
        setBackgroundColor(theme::burnInBackdrop());
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, width, height, style, ctx);
        const float t = static_cast<float>(brls::getCPUTimeUsec()) * 1e-6f;
        const float marker = 48.f;
        const float px = x + (width - marker) * (0.5f + 0.45f * std::sin(t * 0.07f));
        const float py = y + (height - marker) * (0.5f + 0.45f * std::cos(t * 0.05f));
        nvgBeginPath(vg);
        nvgRect(vg, px, py, marker, marker);
        nvgFillColor(vg, theme::burnInMarker());
        nvgFill(vg);
    }

    brls::View* getDefaultFocus() override { return this; }
};

class BurnInSaverActivity : public brls::Activity {
public:
    brls::View* createContentView() override { return new BurnInSaverView(); }
};

inline bool controllerHasButtonDown(const brls::ControllerState& state) {
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (state.buttons[i])
            return true;
    }
    return false;
}

inline bool burnInSaverIsTop() {
    const auto stack = brls::Application::getActivitiesStack();
    return !stack.empty() &&
           dynamic_cast<BurnInSaverActivity*>(stack.back()) != nullptr;
}

// Five minutes of no button/touch input — long enough for a download screen
// to sit idle, short enough to protect OLED panels that stay on a static UI.
constexpr uint64_t kBurnInIdleMs = 5ull * 60ull * 1000ull;

} // namespace pipensx::ui
