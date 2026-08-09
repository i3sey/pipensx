#pragma once

#include <cmath>
#include <cstdint>

#include <borealis.hpp>

#include "ui/theme.hpp"

namespace pipensx::ui {

// Full-screen OLED burn-in guard: pure black plus one slowly drifting dim
// marker so static UI chrome (sidebars, progress bars) does not sit on the
// same pixels for hours. Any controller button or touch dismisses it.
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

    void onContentAvailable() override {
        // Register on the content view so any face button / stick click pops.
        getContentView()->registerAction(
            "Dismiss", brls::BUTTON_A, [this](brls::View*) {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                return true;
            },
            true);
        getContentView()->registerAction(
            "Dismiss", brls::BUTTON_B, [this](brls::View*) {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                return true;
            },
            true);
        getContentView()->registerAction(
            "Dismiss", brls::BUTTON_X, [this](brls::View*) {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                return true;
            },
            true);
        getContentView()->registerAction(
            "Dismiss", brls::BUTTON_Y, [this](brls::View*) {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                return true;
            },
            true);
        getContentView()->registerAction(
            "Dismiss", brls::BUTTON_START, [this](brls::View*) {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                return true;
            },
            true);
        getContentView()->registerAction(
            "Dismiss", brls::BUTTON_BACK, [this](brls::View*) {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                return true;
            },
            true);
    }
};

inline bool controllerHasButtonDown(const brls::ControllerState& state) {
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (state.buttons[i])
            return true;
    }
    return false;
}

// Five minutes of no button/touch input — long enough for a download screen
// to sit idle, short enough to protect OLED panels that stay on a static UI.
constexpr uint64_t kBurnInIdleMs = 5ull * 60ull * 1000ull;

} // namespace pipensx::ui
