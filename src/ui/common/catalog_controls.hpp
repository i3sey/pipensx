#pragma once

#include <borealis.hpp>
#include <functional>
#include "ui/theme.hpp"

namespace pipensx::ui {

inline theme::Button* catalogChip(const std::string& text, std::function<void()> onClick) {
    auto* chip = new theme::Button();
    chip->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    chip->setHeight(40);
    chip->setFontSize(theme::kFontCaption);
    chip->setMarginLeft(8);
    chip->setShrink(0);
    chip->setPaddingLeft(14);
    chip->setPaddingRight(14);
    chip->setText(text);
    chip->registerClickAction([onClick = std::move(onClick)](brls::View*) {
        onClick();
        return true;
    });
    chip->addGestureRecognizer(new brls::TapGestureRecognizer(chip));
    return chip;
}

class SearchIconButton : public theme::Button {
public:
    void setIconColor(NVGcolor color) { iconColor_ = color; }
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        theme::Button::draw(vg, x, y, width, height, style, ctx);
        const float r = 6.5f;
        const float cx = x + width / 2.0f - 2.5f;
        const float cy = y + height / 2.0f - 2.5f;
        nvgStrokeColor(vg, iconColor_);
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgStroke(vg);
        const float k = 0.7071f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + r * k, cy + r * k);
        nvgLineTo(vg, cx + r * k + 5.5f, cy + r * k + 5.5f);
        nvgStroke(vg);
    }
private:
    NVGcolor iconColor_ = theme::textPrimary();
};

} // namespace pipensx::ui
