#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <borealis.hpp>
#include "ui/theme.hpp"

namespace pipensx::ui {

// Keep scroll velocity across focus repeats. The critically damped response
// follows new targets without overshoot, independently of artwork readiness.
class SmoothRecyclerFrame : public brls::RecyclerFrame {
public:
    SmoothRecyclerFrame() {
        setScrollingIndicatorVisible(false);
    }
    void onChildFocusGained(brls::View* child, brls::View* focused) override {
        brls::RecyclerFrame::onChildFocusGained(child, focused);
        if (brls::Application::getInputType() != brls::InputType::GAMEPAD ||
            behavior != brls::ScrollingBehavior::CENTERED || !focused)
            return;
        const float offset = getContentOffsetY();
        const float center = focused->getFrame().getMidY();
        const float limit = std::max(0.0f, getContentHeight() - getScrollingAreaHeight());
        target_ = std::clamp(offset + center - getFrame().getMidY(), 0.0f, limit);
        contentOffsetY.stop();
        if (!moving_) {
            velocity_ = 0;
            lastTick_ = Clock::now();
        }
        lastOffset_ = offset;
        moving_ = true;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const auto now = Clock::now();
        // Touch and explicit position restoration take precedence over motion.
        if (brls::Application::getInputType() != brls::InputType::GAMEPAD ||
            std::abs(getContentOffsetY() - lastOffset_) > 0.01f)
            moving_ = false;
        if (moving_) {
            const float dt = std::clamp(std::chrono::duration<float>(now - lastTick_).count(), 0.0f, 0.05f);
            target_ = std::clamp(target_, 0.0f,
                std::max(0.0f, getContentHeight() - getScrollingAreaHeight()));
            constexpr float omega = 24.0f;
            const float distance = getContentOffsetY() - target_;
            const float impulse = velocity_ + omega * distance;
            const float decay = std::exp(-omega * dt);
            float offset = target_ + (distance + impulse * dt) * decay;
            velocity_ = (velocity_ - omega * impulse * dt) * decay;
            if (std::abs(offset - target_) < 0.1f && std::abs(velocity_) < 1.0f) {
                offset = target_;
                moving_ = false;
            }
            contentOffsetY = offset;
            scrollAnimationTick();
            lastOffset_ = getContentOffsetY();
        }
        lastTick_ = now;
        brls::RecyclerFrame::draw(vg, x, y, width, height, style, ctx);

        const float contentHeight = getContentHeight();
        const float viewHeight = getScrollingAreaHeight();
        if (contentHeight > viewHeight + 1.0f && viewHeight > 24.0f) {
            constexpr float kBarWidth = 6.0f;
            constexpr float kBarRadius = kBarWidth * 0.5f;
            constexpr float kTrackInsetY = 10.0f;
            constexpr float kMinThumbHeight = 44.0f;
            const float trackX = x + width - 14.0f - kBarWidth * 0.5f;
            const float trackY = getScrollingAreaTopBoundary() + kTrackInsetY;
            const float trackH = std::max(1.0f, viewHeight - 2.0f * kTrackInsetY);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, trackX, trackY, kBarWidth, trackH, kBarRadius);
            nvgFillColor(vg, a(theme::track()));
            nvgFill(vg);

            const float maxOffset = std::max(1.0f, contentHeight - viewHeight);
            const float progress = std::clamp(getContentOffsetY() / maxOffset, 0.0f, 1.0f);
            const float thumbH = std::clamp(viewHeight / contentHeight * trackH,
                                            std::min(kMinThumbHeight, trackH), trackH);
            const float thumbY = trackY + progress * (trackH - thumbH);

            NVGcolor thumbColor = theme::textSecondary();
            thumbColor.a *= 0.7f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, trackX, thumbY, kBarWidth, thumbH, kBarRadius);
            nvgFillColor(vg, a(thumbColor));
            nvgFill(vg);
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastTick_ = Clock::now();
    float target_ = 0;
    float velocity_ = 0;
    float lastOffset_ = 0;
    bool moving_ = false;
};

} // namespace pipensx::ui
