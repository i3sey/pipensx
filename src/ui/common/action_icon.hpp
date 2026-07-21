#pragma once

// Row-state glyphs for the pre-install selection lists (torrent file selection,
// batch install). Hand-drawn with NanoVG rather than an icon font: the Material
// fallback is registered per platform (desktop_font.cpp vs switch_font.cpp), so
// a font glyph would not be guaranteed identical between the Switch build and
// the golden-screenshot harness. Same drawing conventions as NavIcon in
// ui/main_frame.hpp — 24px glyph box inside a 28px view, 2px round stroke,
// colour resolved at draw time so it follows the light/dark theme.
//
// The three selection states are separated by FILL WEIGHT, not by glyph detail:
// at row scale a solid badge, an outline glyph and a thin hollow ring are
// told apart instantly, whereas two similar arrows are not.

#include <algorithm>

#include <borealis.hpp>

#include "ui/theme.hpp"

namespace pipensx::ui {

enum class ActionIconKind {
    Install,    // solid accent badge, arrow knocked out
    Download,   // outline arrow dropping into a tray
    Skip,       // thin hollow ring, dimmed
    Checked,    // solid accent badge, checkmark knocked out
    Unchecked,  // thin hollow ring, dimmed
    Error,      // outline warning triangle
};

class ActionIcon : public brls::View {
public:
    static constexpr float kSize = 28.0f;

    explicit ActionIcon(ActionIconKind kind = ActionIconKind::Skip,
                        float size = kSize)
        : kind_(kind) {
        setWidth(size);
        setHeight(size);
        setAlignSelf(brls::AlignSelf::CENTER);
        setFocusable(false);
    }

    void setKind(ActionIconKind kind) { kind_ = kind; }
    ActionIconKind kind() const { return kind_; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        // Every glyph below is authored in a fixed 24-unit box; the transform
        // maps it onto whatever size the view was given, so the same paths
        // serve both the 28px row icons and the smaller legend swatches.
        const float side = std::min(width, height);
        const float scale = side / kGlyph;

        nvgSave(vg);
        nvgTranslate(vg, x + (width - side) / 2.0f, y + (height - side) / 2.0f);
        nvgScale(vg, scale, scale);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        switch (kind_) {
            case ActionIconKind::Install:
                drawBadge(vg, 0.0f, 0.0f, kGlyph, theme::accent());
                knockOutArrow(vg, 0.0f, 0.0f, kGlyph);
                break;
            case ActionIconKind::Download:
                drawTrayArrow(vg, 0.0f, 0.0f, kGlyph, theme::accent());
                break;
            case ActionIconKind::Skip:
            case ActionIconKind::Unchecked:
                drawRing(vg, 0.0f, 0.0f, kGlyph, theme::textTertiary());
                break;
            case ActionIconKind::Checked:
                drawBadge(vg, 0.0f, 0.0f, kGlyph, theme::accent());
                knockOutCheck(vg, 0.0f, 0.0f, kGlyph);
                break;
            case ActionIconKind::Error:
                drawWarning(vg, 0.0f, 0.0f, kGlyph, theme::error());
                break;
        }
        nvgRestore(vg);
    }

private:
    static constexpr float kGlyph = 24.0f;  // authoring box for every path

    // Filled plate the knocked-out glyphs are cut into.
    static void drawBadge(NVGcontext* vg, float gx, float gy, float s,
                          NVGcolor color) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 1.0f, gy + 1.0f, s - 2.0f, s - 2.0f,
                       theme::kRadiusSmall);
        nvgFillColor(vg, color);
        nvgFill(vg);
    }

    // Down arrow landing on a baseline, stroked in the on-accent ink so it
    // reads as a hole in the badge on either theme.
    static void knockOutArrow(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        nvgStrokeColor(vg, theme::onAccent());
        nvgStrokeWidth(vg, 2.2f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 6.0f);
        nvgLineTo(vg, cx, gy + 14.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 4.0f, gy + 10.0f);
        nvgLineTo(vg, cx, gy + 14.5f);
        nvgLineTo(vg, cx + 4.0f, gy + 10.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 7.0f, gy + 17.5f);
        nvgLineTo(vg, gx + s - 7.0f, gy + 17.5f);
        nvgStroke(vg);
    }

    static void knockOutCheck(NVGcontext* vg, float gx, float gy, float s) {
        nvgStrokeColor(vg, theme::onAccent());
        nvgStrokeWidth(vg, 2.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 6.5f, gy + 12.0f);
        nvgLineTo(vg, gx + 10.5f, gy + 16.0f);
        nvgLineTo(vg, gx + 17.5f, gy + 8.0f);
        nvgStroke(vg);
    }

    // Same shape as the Downloads tab icon, so "download only" reads the same
    // everywhere in the app.
    static void drawTrayArrow(NVGcontext* vg, float gx, float gy, float s,
                              NVGcolor color) {
        const float cx = gx + s / 2.0f;
        nvgStrokeColor(vg, color);
        nvgStrokeWidth(vg, 2.0f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 2.0f);
        nvgLineTo(vg, cx, gy + 13.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 4.5f, gy + 8.5f);
        nvgLineTo(vg, cx, gy + 13.5f);
        nvgLineTo(vg, cx + 4.5f, gy + 8.5f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 3.5f, gy + 15.0f);
        nvgLineTo(vg, gx + 3.5f, gy + 20.5f);
        nvgLineTo(vg, gx + s - 3.5f, gy + 20.5f);
        nvgLineTo(vg, gx + s - 3.5f, gy + 15.0f);
        nvgStroke(vg);
    }

    static void drawRing(NVGcontext* vg, float gx, float gy, float s,
                         NVGcolor color) {
        nvgStrokeColor(vg, color);
        nvgStrokeWidth(vg, 1.6f);
        nvgBeginPath(vg);
        nvgCircle(vg, gx + s / 2.0f, gy + s / 2.0f, s / 2.0f - 3.0f);
        nvgStroke(vg);
    }

    static void drawWarning(NVGcontext* vg, float gx, float gy, float s,
                            NVGcolor color) {
        const float cx = gx + s / 2.0f;
        nvgStrokeColor(vg, color);
        nvgFillColor(vg, color);
        nvgStrokeWidth(vg, 2.0f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 3.0f);
        nvgLineTo(vg, gx + s - 2.0f, gy + s - 4.0f);
        nvgLineTo(vg, gx + 2.0f, gy + s - 4.0f);
        nvgClosePath(vg);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 9.0f);
        nvgLineTo(vg, cx, gy + 14.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, gy + 17.5f, 1.3f);
        nvgFill(vg);
    }

    ActionIconKind kind_;
};

}  // namespace pipensx::ui
