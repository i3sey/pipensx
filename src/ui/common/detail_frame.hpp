#pragma once

#include <borealis.hpp>

namespace pipensx::ui {

// Let Borealis own opacity and input blocking for the whole transition.
// Only the body rises slightly; the header and footer remain anchored.
class DetailFrame : public brls::AppletFrame {
public:
    explicit DetailFrame(brls::View* body)
        : brls::AppletFrame(body), body_(body) {}

    bool usesRightStickNavigation() const override { return false; }

    float getShowAnimationDuration(brls::TransitionAnimation) override {
        return 260.0f;
    }

    void show(std::function<void(void)> cb, bool animate, float duration) override {
        entrance_.stop();
        body_->setTranslationY(0.0f);
        if (animate && duration > 0) {
            entrance_.reset(18.0f);
            body_->setTranslationY(18.0f);
            entrance_.addStep(0.0f, duration, brls::EasingFunction::cubicOut);
            entrance_.setTickCallback([this] {
                body_->setTranslationY(entrance_);
            });
            entrance_.start();
        }
        brls::AppletFrame::show(std::move(cb), animate, duration);
    }

    void onShowAnimationEnd() override {
        entrance_.stop();
        body_->setTranslationY(0.0f);
        brls::AppletFrame::onShowAnimationEnd();
    }

private:
    brls::View* body_;
    brls::Animatable entrance_;
};

} // namespace pipensx::ui
