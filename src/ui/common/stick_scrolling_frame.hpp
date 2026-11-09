#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <borealis.hpp>

namespace pipensx::ui {

// Direct right-stick scrolling never requests focus. CENTERED also prevents
// natural scrolling from moving focus when its item leaves the viewport.
class StickScrollingFrame : public brls::ScrollingFrame {
public:
    StickScrollingFrame() {
        setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    void revealFocusAfterLayout() {
        updateScrollingOnNextFrame = true;
    }

    void willAppear(bool resetState) override {
        brls::ScrollingFrame::willAppear(resetState);
        // Opening the page must not jump to a distant "Show more" action
        // while focus is still on the left-hand install button.
        updateScrollingOnNextFrame = false;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const auto now = Clock::now();
        const float dt = std::clamp(
            std::chrono::duration<float>(now - lastTick_).count(), 0.0f, 0.05f);
        lastTick_ = now;
        const auto stack = brls::Application::getActivitiesStack();
        const bool active = !stack.empty() && stack.back() == getParentActivity() &&
            !brls::Application::isInputBlocks();
        if (active) {
            const float axis = brls::Application::getControllerState().axes[brls::RIGHT_Y];
            constexpr float deadzone = 0.18f;
            const float amount = std::clamp((std::abs(axis) - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
            const float target = std::copysign(amount * amount * 800.0f, axis);
            speed_ += (target - speed_) * (1.0f - std::exp(-18.0f * dt));
            if (std::abs(speed_) > 0.5f)
                setContentOffsetY(getContentOffsetY() + speed_ * dt, false);
            else
                speed_ = 0.0f;
        } else {
            speed_ = 0.0f;
        }
        brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastTick_ = Clock::now();
    float speed_ = 0.0f;
};

} // namespace pipensx::ui
