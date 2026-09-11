#pragma once

// Footer SD indicator for the AppletFrame bottom bar (replaces the clock).
// Much smaller than the old top-bar StorageMeter: a ~72x8 pill plus a single
// "SD: 72GB" label in one row. Draw-only; the caller feeds total/free bytes.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include <borealis.hpp>

#include "ui/theme.hpp"

namespace pipensx::ui {

class FooterStorage : public brls::Box {
public:
    FooterStorage() : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setAlignItems(brls::AlignItems::CENTER);
        bar_ = new MiniBar();
        addView(bar_);
        label_ = new brls::Label();
        label_->setFocusable(false);
        label_->setSingleLine(true);
        label_->setFontSize(theme::kFontCaption);
        label_->setTextColor(theme::textPrimary());
        label_->setMarginLeft(8);
        addView(label_);
        setUnavailable();
    }

    void setStorage(uint64_t total, uint64_t free) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        bar_->setData(total, free);
        label_->setText("SD: " + formatShort(free));
    }

    void setUnavailable() {
        bar_->setData(0, 0);
        label_->setText("SD: --");
    }

private:
    // Same units as formatBytesShort, no fraction — keeps the footer narrow.
    static std::string formatShort(uint64_t bytes) {
        char buffer[32];
        if (bytes >= 1024ULL * 1024 * 1024)
            std::snprintf(buffer, sizeof(buffer), "%.0f GB",
                          bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024ULL * 1024)
            std::snprintf(buffer, sizeof(buffer), "%.0f MB",
                          bytes / (1024.0 * 1024));
        else if (bytes >= 1024ULL)
            std::snprintf(buffer, sizeof(buffer), "%.0f KB", bytes / 1024.0);
        else
            std::snprintf(buffer, sizeof(buffer), "%llu B",
                          static_cast<unsigned long long>(bytes));
        return buffer;
    }

    class MiniBar : public brls::View {
    public:
        MiniBar() {
            setFocusable(false);
            setWidth(kWidth);
            setHeight(kHeight);
            setAlignSelf(brls::AlignSelf::CENTER);
        }

        void setData(uint64_t total, uint64_t free) {
            total_ = total;
            free_ = free;
        }

        void draw(NVGcontext* vg, float x, float y, float width, float height,
                  brls::Style, brls::FrameContext*) override {
            if (width <= 1 || height <= 1)
                return;
            const float radius = std::min(4.0f, height / 2.0f);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, radius);
            nvgFillColor(vg, theme::meterTrack());
            nvgFill(vg);

            if (total_ > 0) {
                const uint64_t used = total_ >= free_ ? total_ - free_ : 0;
                float usedW = static_cast<float>(static_cast<double>(used) *
                                                 static_cast<double>(width) /
                                                 static_cast<double>(total_));
                // A non-zero slice keeps a sliver so a nearly full card still
                // reads at this size.
                if (used > 0 && usedW < kMinSlice)
                    usedW = kMinSlice;
                usedW = std::min(usedW, width);
                if (usedW > 0.0f) {
                    const float right =
                        usedW >= width - 0.5f ? radius : 0.0f;
                    nvgBeginPath(vg);
                    nvgRoundedRectVarying(vg, x, y, usedW, height, radius,
                                          right, right, radius);
                    nvgFillColor(vg, theme::meterUsed());
                    nvgFill(vg);
                }
            }

            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 0.5f, y + 0.5f, width - 1.0f, height - 1.0f,
                           radius);
            nvgStrokeWidth(vg, 1.0f);
            nvgStrokeColor(vg, theme::meterBorder());
            nvgStroke(vg);
        }

    private:
        static constexpr float kWidth = 72.0f;
        static constexpr float kHeight = 8.0f;
        static constexpr float kMinSlice = 3.0f;
        uint64_t total_ = 0;
        uint64_t free_ = 0;
    };

    MiniBar* bar_ = nullptr;
    brls::Label* label_ = nullptr;
};

}  // namespace pipensx::ui
