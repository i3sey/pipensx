#pragma once

#include <algorithm>

#include <borealis.hpp>
#include "ui/theme.hpp"

namespace pipensx::ui {

class ProgressBar : public brls::View {
public:
    ProgressBar() {
        setHeight(7);
        setCornerRadius(3.5f);
    }

    void setProgress(float progress) {
        progress_ = std::max(0.0f, std::min(1.0f, progress));
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, height / 2);
        nvgFillColor(vg, theme::track());
        nvgFill(vg);
        if (progress_ > 0) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width * progress_, height, height / 2);
            nvgFillColor(vg, theme::accent());
            nvgFill(vg);
        }
    }

private:
    float progress_ = 0;
};

}  // namespace pipensx::ui
