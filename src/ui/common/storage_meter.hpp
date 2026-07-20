#pragma once

// Segmented SD-storage bar (Switch data-management style): a full-width track
// scaled to the card capacity, split into an existing "used" chunk, an optional
// "this install" chunk (accent, red when it will not fit), and the remaining
// free space. Used two ways:
//   * pre-install screens  -> setEstimate(total, free, install, insufficient)
//   * sidebar footer        -> setStorage(total, free)  (no install chunk)
// All math is done by the caller via pipensx::install_space; this is draw-only.

#include <algorithm>
#include <cstdint>
#include <string>

#include <borealis.hpp>

#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class StorageMeter : public brls::Box {
public:
    StorageMeter() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(false);

        bar_ = new MeterBar();
        addView(bar_);

        caption_ = new brls::Label();
        caption_->setFontSize(theme::kFontCaption);
        caption_->setTextColor(theme::textSecondary());
        caption_->setMarginTop(6);
        addView(caption_);
    }

    // SD capacity unknown / query failed.
    void setUnavailable() {
        bar_->setData(0, 0, 0, false);
        caption_->setText("SD space unavailable");
    }

    // Footer: just used vs free of the whole card.
    void setStorage(uint64_t total, uint64_t free) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        bar_->setData(total, free, 0, false);
        caption_->setText("Free " + formatBytes(free) + " / " +
                          formatBytes(total));
    }

    // Pre-install: overlay how much this install will take on top of used space.
    void setEstimate(uint64_t total, uint64_t free, uint64_t installBytes,
                     bool insufficient, bool nszUnknown = false) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        bar_->setData(total, free, installBytes, insufficient);
        const uint64_t used = total >= free ? total - free : 0;
        std::string text = "Used " + formatBytes(used) + "  ·  Install " +
                           formatBytes(installBytes) + "  ·  Free " +
                           formatBytes(free);
        if (insufficient) {
            const uint64_t shortfall =
                installBytes > free ? installBytes - free : 0;
            text += "   Need " + formatBytes(shortfall) + " more";
        } else if (nszUnknown) {
            text += "   NSZ may expand";
        }
        caption_->setText(text);
    }

    // Collapsed sidebar rail: bar only, no byte caption.
    void setCompact(bool compact) {
        caption_->setVisibility(compact ? brls::Visibility::GONE
                                        : brls::Visibility::VISIBLE);
    }

private:
    class MeterBar : public brls::View {
    public:
        MeterBar() {
            setHeight(12);
            setMarginTop(2);
        }

        void setData(uint64_t total, uint64_t free, uint64_t install,
                     bool insufficient) {
            total_ = total;
            free_ = free;
            install_ = install;
            insufficient_ = insufficient;
        }

        void draw(NVGcontext* vg, float x, float y, float width, float height,
                  brls::Style, brls::FrameContext*) override {
            if (width <= 1 || height <= 1)
                return;

            const float radius =
                std::min(theme::kRadiusSmall, height / 2.0f);

            // Full track = remaining free space background / unknown capacity.
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, radius);
            nvgFillColor(vg, theme::track());
            nvgFill(vg);
            if (total_ == 0)
                return;

            // Clip the flat segment rects to the rounded track outline.
            nvgSave(vg);
            nvgIntersectScissor(vg, x, y, width, height);

            const uint64_t used = total_ >= free_ ? total_ - free_ : 0;
            const float scale = width / static_cast<float>(total_);
            const float usedW = static_cast<float>(used) * scale;

            nvgBeginPath(vg);
            nvgRect(vg, x, y, usedW, height);
            nvgFillColor(vg, theme::surface());
            nvgFill(vg);

            if (install_ > 0) {
                // Fits: exactly the install slice after the used chunk.
                // Overflow: fill the whole remaining free span in the error
                // colour so the bar reads as "past the edge".
                const uint64_t fill =
                    insufficient_ ? (total_ - used) : std::min(install_, free_);
                const float installW = static_cast<float>(fill) * scale;
                nvgBeginPath(vg);
                nvgRect(vg, x + usedW, y, installW, height);
                nvgFillColor(vg, insufficient_ ? theme::error()
                                               : theme::accent());
                nvgFill(vg);
            }

            nvgRestore(vg);
        }

    private:
        uint64_t total_ = 0;
        uint64_t free_ = 0;
        uint64_t install_ = 0;
        bool insufficient_ = false;
    };

    MeterBar* bar_ = nullptr;
    brls::Label* caption_ = nullptr;
};

}  // namespace pipensx::ui
