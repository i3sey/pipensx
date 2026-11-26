#pragma once

#include "installed_view.hpp"
#include "ui/homebrew/homebrew_view.hpp"
#include "ui/retro/retro_download_view.hpp"
#include <array>

namespace pipensx::ui {

class MyGamesView : public brls::Box {
public:
    using Factory = std::function<brls::View*(size_t)>;
    explicit MyGamesView(Factory factory)
        : brls::Box(brls::Axis::COLUMN), factory_(std::move(factory)) {
        auto* menu = new brls::Box(brls::Axis::ROW);
        menu->setShrink(0);
        menu->setPadding(6, 28, 2, 28);
        const std::array<const char*, 4> keys = {
            "pipensx/nav/games", "pipensx/nav/ports",
            "pipensx/nav/homebrew", "pipensx/nav/retro"};
        for (size_t i = 0; i < buttons_.size(); ++i) {
            auto* button = new theme::Button();
            button->setText(tr(keys[i]));
            button->setFontSize(theme::kFontSmall);
            button->setHeight(40);
            button->setGrow(1);
            button->setShrink(1);
            button->setMarginRight(i + 1 == buttons_.size() ? 0 : 10);
            button->registerClickAction([this, i](brls::View*) {
                select(i);
                return true;
            });
            button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
            buttons_[i] = button;
            menu->addView(button);
        }
        addView(menu);
        host_ = new brls::Box(brls::Axis::COLUMN);
        host_->setGrow(1);
        addView(host_);
        select(0);
        registerAction("", brls::BUTTON_LT, [this](brls::View*) {
            select((active_ + buttons_.size() - 1) % buttons_.size());
            return true;
        }, true);
        registerAction("", brls::BUTTON_RT, [this](brls::View*) {
            select((active_ + 1) % buttons_.size());
            return true;
        }, true);
    }

    ~MyGamesView() override {
        for (size_t i = 0; i < pages_.size(); ++i)
            if (i != active_)
                delete pages_[i];
    }

private:
    void select(size_t index) {
        if (index == active_) return;
        if (!pages_[index]) pages_[index] = factory_(index);
        pages_[index]->setGrow(1);
        if (active_ < pages_.size()) {
            brls::Application::giveFocus(buttons_[index]);
            host_->setLastFocusedView(nullptr);
            auto* outgoing = pages_[active_];
            host_->removeView(outgoing, false);
            outgoing->setParent(nullptr);
        }
        active_ = index;
        host_->addView(pages_[index]);
        for (size_t i = 0; i < buttons_.size(); ++i) {
            buttons_[i]->setBackgroundColor(i == index
                ? theme::sidebarActive() : nvgRGBA(0, 0, 0, 0));
            buttons_[i]->setLineColor(theme::accent());
            buttons_[i]->setLineBottom(i == index ? 3 : 0);
        }
    }

    Factory factory_;
    std::array<brls::View*, 4> pages_{};
    std::array<theme::Button*, 4> buttons_{};
    brls::Box* host_ = nullptr;
    size_t active_ = 4;
};

} // namespace pipensx::ui
