#pragma once

#include <algorithm>
#include <cstdint>

#include <borealis.hpp>

#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Static crescent so the warning is a picture, not a screensaver, and so
// golden frames do not depend on the wall clock.
class ScreenOffGlyph : public brls::View {
public:
    ScreenOffGlyph() {
        setWidth(kSize);
        setHeight(kSize);
        setAlignSelf(brls::AlignSelf::CENTER);
        setFocusable(false);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        const float side = std::min(width, height);
        const float scale = side / kGlyph;
        nvgSave(vg);
        nvgTranslate(vg, x + (width - side) / 2.0f, y + (height - side) / 2.0f);
        nvgScale(vg, scale, scale);
        nvgBeginPath(vg);
        nvgCircle(vg, 12.0f, 12.0f, 9.0f);
        nvgCircle(vg, 16.0f, 9.0f, 7.0f);
        nvgPathWinding(vg, NVG_HOLE);
        nvgFillColor(vg, theme::textPrimary());
        nvgFill(vg);
        nvgRestore(vg);
    }

private:
    static constexpr float kSize = 96.0f;
    static constexpr float kGlyph = 24.0f;
};

// Idle cover: a dim warning that the panel will go dark, then (once lbl
// succeeds) opaque black so OLED chrome is not left on. Dismissal and the
// backlight live in main_switch's idle loop — any controller button or touch
// pops this activity AND switches the panel back on through lbl, without
// touching the download queue. Power is Horizon sleep, not our wake, and is
// not in the pad map. The loop also runs a heartbeat watchdog that forces
// the panel back on if the UI thread stalls while the screen is off.
class BurnInSaverView : public brls::Box {
public:
    BurnInSaverView() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(true);
        setHideHighlight(true);
        setGrow(1.f);
        setJustifyContent(brls::JustifyContent::CENTER);
        setAlignItems(brls::AlignItems::CENTER);
        setBackgroundColor(theme::overlay());

        content_ = new brls::Box(brls::Axis::COLUMN);
        content_->setWidth(720.0f);
        content_->setAlignItems(brls::AlignItems::CENTER);
        addView(content_);

        auto* glyph = new ScreenOffGlyph();
        glyph->setMarginBottom(24.0f);
        content_->addView(glyph);

        auto* title = new brls::Label();
        title->setText(tr("pipensx/settings/screen_off_title"));
        title->setFontSize(theme::kFontTitle);
        title->setTextColor(theme::textPrimary());
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        title->setWidth(720.0f);
        title->setMarginBottom(12.0f);
        content_->addView(title);

        auto* hint = new brls::Label();
        hint->setText(tr("pipensx/settings/screen_off_hint"));
        hint->setFontSize(theme::kFontSmall);
        hint->setTextColor(theme::textSecondary());
        hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        hint->setSingleLine(false);
        hint->setWidth(720.0f);
        content_->addView(hint);
    }

    void setPanelOff(bool off) {
        if (panelOff_ == off)
            return;
        panelOff_ = off;
        setBackgroundColor(off ? theme::burnInBackdrop() : theme::overlay());
        content_->setVisibility(off ? brls::Visibility::GONE
                                    : brls::Visibility::VISIBLE);
    }

    bool isTranslucent() override { return !panelOff_; }

    brls::View* getDefaultFocus() override { return this; }

private:
    brls::Box* content_ = nullptr;
    bool panelOff_ = false;
};

class BurnInSaverActivity : public brls::Activity {
public:
    brls::View* createContentView() override {
        view_ = new BurnInSaverView();
        return view_;
    }

    void setPanelOff(bool off) {
        if (view_)
            view_->setPanelOff(off);
    }

private:
    BurnInSaverView* view_ = nullptr;
};

inline bool controllerHasButtonDown(const brls::ControllerState& state) {
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (state.buttons[i])
            return true;
    }
    return false;
}

inline BurnInSaverActivity* burnInSaverTop() {
    const auto stack = brls::Application::getActivitiesStack();
    if (stack.empty())
        return nullptr;
    return dynamic_cast<BurnInSaverActivity*>(stack.back());
}

inline bool burnInSaverIsTop() { return burnInSaverTop() != nullptr; }

// Five minutes of no button/touch input — long enough for a download screen
// to sit idle, short enough to protect OLED panels that stay on a static UI.
constexpr uint64_t kBurnInIdleMs = 5ull * 60ull * 1000ull;
// Grace overlay after idle, before lbl actually kills the panel.
constexpr uint64_t kScreenOffWarnMs = 15ull * 1000ull;

} // namespace pipensx::ui
