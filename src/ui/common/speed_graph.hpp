#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <borealis.hpp>

#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class SpeedGraphView : public brls::View {
public:
    SpeedGraphView() {
        setHeight(82);
        setMarginBottom(13);
    }

    void setSamples(std::vector<uint64_t> download,
                    std::vector<uint64_t> install) {
        download_ = std::move(download);
        install_ = std::move(install);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (width <= 1 || height <= 1)
            return;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 6);
        nvgFillColor(vg, theme::graphBg());
        nvgFill(vg);

        const float pad = 8.0f;
        const float left = x + pad;
        const float right = x + width - pad;
        const float top = y + pad;
        const float bottom = y + height - pad;
        const float plotWidth = std::max(1.0f, right - left);
        const float plotHeight = std::max(1.0f, bottom - top);

        nvgStrokeWidth(vg, 1.0f);
        nvgStrokeColor(vg, theme::graphGrid());
        for (int i = 1; i <= 3; i++) {
            float gy = top + plotHeight * static_cast<float>(i) / 4.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, left, gy);
            nvgLineTo(vg, right, gy);
            nvgStroke(vg);
        }

        uint64_t maxSpeed = 512ULL * 1024ULL;
        for (uint64_t speed : download_)
            maxSpeed = std::max(maxSpeed, speed);
        for (uint64_t speed : install_)
            maxSpeed = std::max(maxSpeed, speed);

        drawSeries(vg, download_, maxSpeed, left, top, plotWidth, plotHeight,
                   theme::accent(), 2.5f);
        drawSeries(vg, install_, maxSpeed, left, top, plotWidth, plotHeight,
                   theme::success(), 2.0f);
    }

private:
    static float yFor(uint64_t speed, uint64_t maxSpeed, float top,
                      float plotHeight) {
        double ratio = maxSpeed ? static_cast<double>(speed) /
                                      static_cast<double>(maxSpeed)
                                : 0.0;
        ratio = std::clamp(ratio, 0.0, 1.0);
        return top + plotHeight * static_cast<float>(1.0 - ratio);
    }

    static void drawSeries(NVGcontext* vg, const std::vector<uint64_t>& samples,
                           uint64_t maxSpeed, float left, float top,
                           float plotWidth, float plotHeight, NVGcolor color,
                           float strokeWidth) {
        if (samples.empty())
            return;
        if (samples.size() == 1) {
            nvgBeginPath(vg);
            nvgCircle(vg, left, yFor(samples.front(), maxSpeed, top, plotHeight),
                      2.5f);
            nvgFillColor(vg, color);
            nvgFill(vg);
            return;
        }

        nvgBeginPath(vg);
        for (size_t i = 0; i < samples.size(); i++) {
            float px = left + plotWidth * static_cast<float>(i) /
                                static_cast<float>(samples.size() - 1);
            float py = yFor(samples[i], maxSpeed, top, plotHeight);
            if (i == 0)
                nvgMoveTo(vg, px, py);
            else
                nvgLineTo(vg, px, py);
        }
        nvgStrokeWidth(vg, strokeWidth);
        nvgStrokeColor(vg, color);
        nvgStroke(vg);
    }

    std::vector<uint64_t> download_;
    std::vector<uint64_t> install_;
};

}  // namespace pipensx::ui
