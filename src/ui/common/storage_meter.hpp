#pragma once

// Segmented SD-storage bar (Switch data-management style): a full-width track
// scaled to the card capacity, split into an existing "used" chunk, an optional
// "this install" chunk (accent, red when it will not fit), and the remaining
// free space. Used four ways:
//   * sidebar footer       -> setStorage(total, free)   (no install chunk)
//   * game page            -> setGameEstimate(...)      (catalog size, then the
//                                                        exact size after the
//                                                        torrent metadata lands)
//   * pre-install screens  -> setEstimate(total, free, install, insufficient)
// All math is done by the caller via pipensx::install_space; this is draw-only.
//
// The root box clips to its bounds and every text row is single-line, so the
// widget can never grow past the column it was given — the sidebar footer sits
// in a 216px (56px collapsed) column and used to spill out of the frame.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include <borealis.hpp>

#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class StorageMeter : public brls::Box {
public:
    StorageMeter() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(false);
        // Hard guarantee against overflow: View::frame() scissors children to
        // this rect when clipsToBounds is set.
        setClipsToBounds(true);

        head_ = new brls::Box(brls::Axis::ROW);
        head_->setFocusable(false);
        head_->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        head_->setAlignItems(brls::AlignItems::CENTER);
        head_->setMarginBottom(6);
        head_->setVisibility(brls::Visibility::GONE);

        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(theme::kFontCaption);
        title_->setTextColor(theme::textTertiary());
        head_->addView(title_);

        value_ = new brls::Label();
        value_->setSingleLine(true);
        value_->setFontSize(theme::kFontCaption);
        value_->setTextColor(theme::textPrimary());
        value_->setMarginLeft(10);
        head_->addView(value_);
        addView(head_);

        bar_ = new MeterBar();
        addView(bar_);

        foot_ = new brls::Box(brls::Axis::ROW);
        foot_->setFocusable(false);
        foot_->setAlignItems(brls::AlignItems::CENTER);
        foot_->setMarginTop(6);

        caption_ = new brls::Label();
        caption_->setSingleLine(true);
        caption_->setGrow(1);
        caption_->setFontSize(theme::kFontCaption);
        caption_->setTextColor(theme::textSecondary());
        foot_->addView(caption_);

        legend_ = new brls::Box(brls::Axis::ROW);
        legend_->setFocusable(false);
        legend_->setAlignItems(brls::AlignItems::CENTER);
        legend_->setMarginLeft(16);
        legend_->setVisibility(brls::Visibility::GONE);
        addLegendEntry(LegendDot::Kind::Used, tr("pipensx/storage/legend_used"));
        installDot_ = addLegendEntry(LegendDot::Kind::Install,
                                     tr("pipensx/storage/legend_install"));
        addLegendEntry(LegendDot::Kind::Free, tr("pipensx/storage/legend_free"));
        foot_->addView(legend_);
        addView(foot_);
    }

    // Caption above the bar. Empty string hides the whole row.
    void setHeader(const std::string& title) {
        headerTitle_ = title;
        hasHeader_ = !title.empty();
        title_->setText(title);
        applyVisibility();
    }

    // Colour key next to the caption. Only worth it on the wide screens.
    void setLegendVisible(bool visible) {
        hasLegend_ = visible;
        applyVisibility();
    }

    // Collapsed sidebar rail: bar only, and a thinner one.
    void setCompact(bool compact) {
        compact_ = compact;
        bar_->setCompact(compact);
        applyVisibility();
    }

    // SD capacity unknown / query failed.
    void setUnavailable() {
        bar_->setData(0, 0, 0, false);
        installDot_->setInsufficient(false);
        title_->setText(headerTitle_);  // drop any "(est.)" qualifier
        value_->setText("");
        caption_->setText(tr("pipensx/storage/unavailable"));
    }

    // Footer: just used vs free of the whole card.
    void setStorage(uint64_t total, uint64_t free) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        bar_->setData(total, free, 0, false);
        installDot_->setInsufficient(false);
        value_->setText(formatBytesShort(free));
        caption_->setText(tr("pipensx/storage/free", formatBytesShort(free)));
    }

    // Game page: what this one release will take out of the card. `exact` is
    // false while the number is still the catalog-declared size.
    void setGameEstimate(uint64_t total, uint64_t free, uint64_t installBytes,
                         bool insufficient, bool exact) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        bar_->setData(total, free, installBytes, insufficient);
        installDot_->setInsufficient(insufficient);
        // The console font draws '~' as a raised accent, so the "still a guess"
        // marker lives in the header title rather than in front of the number.
        title_->setText(exact ? headerTitle_
                              : tr("pipensx/storage/estimate_suffix",
                                   headerTitle_));
        value_->setText(installBytes == 0 ? tr("pipensx/common/unknown")
                                          : formatBytes(installBytes));
        if (insufficient) {
            const uint64_t shortfall =
                installBytes > free ? installBytes - free : 0;
            caption_->setText(tr("pipensx/storage/need_more",
                                 formatBytes(shortfall)));
        } else if (installBytes == 0) {
            caption_->setText(tr("pipensx/storage/free",
                                 formatBytesShort(free)));
        } else {
            const uint64_t left = free >= installBytes ? free - installBytes : 0;
            caption_->setText(tr("pipensx/storage/left_after",
                                 formatBytesShort(left)));
        }
    }

    // Pre-install: overlay how much this install will take on top of used space.
    void setEstimate(uint64_t total, uint64_t free, uint64_t installBytes,
                     bool insufficient, bool nszUnknown = false) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        bar_->setData(total, free, installBytes, insufficient);
        installDot_->setInsufficient(insufficient);
        value_->setText(tr("pipensx/storage/install_bytes",
                           formatBytes(installBytes)));
        const uint64_t used = total >= free ? total - free : 0;
        std::string text = tr("pipensx/storage/caption", formatBytes(used),
                              formatBytes(installBytes), formatBytes(free));
        if (insufficient) {
            const uint64_t shortfall =
                installBytes > free ? installBytes - free : 0;
            text += tr("pipensx/storage/caption_need",
                       formatBytes(shortfall));
        } else if (nszUnknown) {
            text += tr("pipensx/storage/caption_nsz");
        }
        caption_->setText(text);
    }

private:
    // Colour key swatch. The colour is resolved at draw time so it follows the
    // console light/dark theme like every other pipensx view.
    class LegendDot : public brls::View {
    public:
        enum class Kind { Used, Install, Free };

        explicit LegendDot(Kind kind) : kind_(kind) {
            setWidth(10);
            setHeight(10);
            setAlignSelf(brls::AlignSelf::CENTER);
            setMarginRight(6);
            setFocusable(false);
        }

        void setInsufficient(bool insufficient) {
            insufficient_ = insufficient;
        }

        void draw(NVGcontext* vg, float x, float y, float width, float height,
                  brls::Style, brls::FrameContext*) override {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 3.0f);
            switch (kind_) {
                case Kind::Used:
                    nvgFillColor(vg, theme::meterUsed());
                    break;
                case Kind::Install:
                    nvgFillColor(vg, insufficient_ ? theme::error()
                                                   : theme::accent());
                    break;
                case Kind::Free:
                    nvgFillColor(vg, theme::meterTrack());
                    break;
            }
            nvgFill(vg);
            if (kind_ == Kind::Free) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x + 0.5f, y + 0.5f, width - 1.0f,
                               height - 1.0f, 3.0f);
                nvgStrokeWidth(vg, 1.0f);
                nvgStrokeColor(vg, theme::meterBorder());
                nvgStroke(vg);
            }
        }

    private:
        Kind kind_;
        bool insufficient_ = false;
    };

    class MeterBar : public brls::View {
    public:
        MeterBar() {
            setFocusable(false);
            setHeight(kHeight);
            setMarginTop(2);
        }

        void setCompact(bool compact) {
            setHeight(compact ? kCompactHeight : kHeight);
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

            const float radius = std::min(theme::kRadiusSmall, height / 2.0f);

            // Empty slot = remaining free space / unknown capacity.
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, radius);
            nvgFillColor(vg, theme::meterTrack());
            nvgFill(vg);

            float usedW = 0.0f;
            float installW = 0.0f;
            float markerX = -1.0f;
            if (total_ > 0) {
                const uint64_t used = total_ >= free_ ? total_ - free_ : 0;

                // Fits: the bar is the card, install is a slice of the free
                // span. Does not fit: rescale to used+install so the red chunk
                // is drawn at its real size and a marker shows where the card
                // actually ends — otherwise a nearly full card leaves a 3px
                // sliver of red and the overflow reads as nothing.
                const bool overflowScale =
                    insufficient_ && install_ > 0 &&
                    install_ <= std::numeric_limits<uint64_t>::max() - used;
                const uint64_t denom = overflowScale ? used + install_ : total_;
                const double scale =
                    static_cast<double>(width) / static_cast<double>(denom);
                usedW = span(used, scale, width);

                const uint64_t fill =
                    install_ == 0 ? 0
                                  : (insufficient_ ? (denom - used)
                                                   : std::min(install_, free_));
                installW = span(fill, scale, width - usedW);
                if (overflowScale)
                    markerX = x + static_cast<float>(
                                      static_cast<double>(total_) * scale);

                // Segments are drawn as their own rounded rects rather than
                // rect + scissor: a scissor is axis-aligned and would leave the
                // segment corners poking square out of the rounded slot.
                if (usedW > 0.0f) {
                    const bool tail =
                        installW <= 0.0f && usedW >= width - 0.5f;
                    segmentPath(vg, x, y, usedW, height, radius, true, tail);
                    nvgFillColor(vg, theme::meterUsed());
                    nvgFill(vg);
                }
                if (installW > 0.0f) {
                    const bool head = usedW <= 0.0f;
                    const bool tail = usedW + installW >= width - 0.5f;
                    segmentPath(vg, x + usedW, y, installW, height, radius,
                                head, tail);
                    nvgFillColor(vg, insufficient_ ? theme::error()
                                                   : theme::accent());
                    nvgFill(vg);
                }
                // Crisp boundary between the two filled chunks.
                if (usedW >= 4.0f && installW >= 4.0f) {
                    nvgBeginPath(vg);
                    nvgRect(vg, x + usedW - kSeparator / 2.0f, y, kSeparator,
                            height);
                    nvgFillColor(vg, theme::meterTrack());
                    nvgFill(vg);
                }
                // Card capacity, when the bar had to be scaled past it. Skipped
                // when it lands on a rounded end, where it is only noise.
                if (markerX > x + 3.0f && markerX < x + width - 3.0f) {
                    nvgBeginPath(vg);
                    nvgRect(vg, markerX - kMarker / 2.0f, y, kMarker, height);
                    nvgFillColor(vg, theme::textPrimary());
                    nvgFill(vg);
                }
            }

            // Outline last so the bar reads against any background. On a nearly
            // full card the overflow slice is only a few pixels wide, so the
            // error state leans on a heavier red outline to carry the warning.
            const bool failing = insufficient_ && install_ > 0;
            const float stroke = failing ? 2.0f : 1.0f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + stroke / 2.0f, y + stroke / 2.0f,
                           width - stroke, height - stroke, radius);
            nvgStrokeWidth(vg, stroke);
            nvgStrokeColor(vg, failing ? theme::error() : theme::meterBorder());
            nvgStroke(vg);
        }

    private:
        static constexpr float kHeight = 14.0f;
        static constexpr float kCompactHeight = 10.0f;
        static constexpr float kSeparator = 2.0f;
        static constexpr float kMinSegment = 3.0f;
        static constexpr float kMarker = 2.0f;

        // A non-zero slice always keeps a sliver of width, otherwise a small
        // install next to a nearly full card renders as nothing at all.
        static float span(uint64_t bytes, double scale, float maxWidth) {
            if (bytes == 0 || maxWidth <= 0.0f)
                return 0.0f;
            float width = static_cast<float>(static_cast<double>(bytes) * scale);
            if (width < kMinSegment)
                width = kMinSegment;
            return std::min(width, maxWidth);
        }

        static void segmentPath(NVGcontext* vg, float x, float y, float width,
                                float height, float radius, bool roundLeft,
                                bool roundRight) {
            const float left = roundLeft ? std::min(radius, width / 2.0f) : 0.0f;
            const float right =
                roundRight ? std::min(radius, width / 2.0f) : 0.0f;
            nvgBeginPath(vg);
            nvgRoundedRectVarying(vg, x, y, width, height, left, right, right,
                                  left);
        }

        uint64_t total_ = 0;
        uint64_t free_ = 0;
        uint64_t install_ = 0;
        bool insufficient_ = false;
    };

    LegendDot* addLegendEntry(LegendDot::Kind kind, const std::string& text) {
        auto* dot = new LegendDot(kind);
        legend_->addView(dot);
        auto* label = new brls::Label();
        label->setSingleLine(true);
        label->setFontSize(theme::kFontCaption);
        label->setTextColor(theme::textTertiary());
        label->setMarginRight(kind == LegendDot::Kind::Free ? 0.0f : 14.0f);
        label->setText(text);
        legend_->addView(label);
        return dot;
    }

    void applyVisibility() {
        head_->setVisibility(hasHeader_ && !compact_
                                 ? brls::Visibility::VISIBLE
                                 : brls::Visibility::GONE);
        foot_->setVisibility(compact_ ? brls::Visibility::GONE
                                      : brls::Visibility::VISIBLE);
        legend_->setVisibility(hasLegend_ && !compact_
                                   ? brls::Visibility::VISIBLE
                                   : brls::Visibility::GONE);
    }

    brls::Box* head_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* value_ = nullptr;
    MeterBar* bar_ = nullptr;
    brls::Box* foot_ = nullptr;
    brls::Label* caption_ = nullptr;
    brls::Box* legend_ = nullptr;
    LegendDot* installDot_ = nullptr;
    std::string headerTitle_;
    bool hasHeader_ = false;
    bool hasLegend_ = false;
    bool compact_ = false;
};

}  // namespace pipensx::ui
