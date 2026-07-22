#pragma once

#include <functional>
#include <string>

#include <borealis.hpp>

#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class MessageCell : public brls::RecyclerCell {
public:
    MessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(21);
        label_->setText(tr("pipensx/downloads/empty_cell"));
        addView(label_);
    }

private:
    brls::Label* label_;
};

class EmptyStateView : public brls::Box {
public:
    EmptyStateView() : brls::Box(brls::Axis::COLUMN) {
        setGrow(1);
        setPadding(24, 34, 24, 34);
        setJustifyContent(brls::JustifyContent::CENTER);
        setAlignItems(brls::AlignItems::CENTER);

        card_ = new brls::Box(brls::Axis::COLUMN);
        card_->setWidth(560);
        card_->setPadding(32, 32, 32, 32);
        card_->setBackgroundColor(theme::panel());
        card_->setCornerRadius(theme::kRadiusLarge);
        card_->setAlignItems(brls::AlignItems::CENTER);
        addView(card_);

        auto* iconFrame = new brls::Box(brls::Axis::ROW);
        iconFrame->setWidth(96);
        iconFrame->setHeight(96);
        iconFrame->setBackgroundColor(theme::surface());
        iconFrame->setCornerRadius(theme::kRadiusLarge);
        iconFrame->setJustifyContent(brls::JustifyContent::CENTER);
        iconFrame->setAlignItems(brls::AlignItems::CENTER);
        iconFrame->setMarginBottom(20);
        card_->addView(iconFrame);

        auto* icon = new brls::Image();
        icon->setWidth(72);
        icon->setHeight(72);
        icon->setScalingType(brls::ImageScalingType::FIT);
        icon->setImageFromRes("icon.png");
        iconFrame->addView(icon);

        title_ = new brls::Label();
        title_->setFontSize(theme::kFontHeading);
        title_->setSingleLine(false);
        title_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        title_->setTextColor(theme::textPrimary());
        title_->setMarginBottom(10);
        card_->addView(title_);

        body_ = new brls::Label();
        body_->setFontSize(theme::kFontSmall);
        body_->setSingleLine(false);
        body_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        body_->setTextColor(theme::textSecondary());
        body_->setMarginBottom(20);
        card_->addView(body_);

        action_ = new brls::Button();
        action_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        action_->setWidth(240);
        action_->setHeight(44);
        action_->registerClickAction([this](brls::View*) {
            if (onAction_)
                onAction_();
            return true;
        });
        card_->addView(action_);
        card_->setDefaultFocusedIndex(3);
    }

    void setContent(const std::string& title, const std::string& body,
                    const std::string& actionText,
                    std::function<void()> onAction) {
        title_->setText(title);
        body_->setText(body);
        onAction_ = std::move(onAction);
        if (actionText.empty()) {
            action_->setVisibility(brls::Visibility::GONE);
        } else {
            action_->setText(actionText);
            action_->setVisibility(brls::Visibility::VISIBLE);
        }
    }

private:
    brls::Box* card_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* body_ = nullptr;
    brls::Button* action_ = nullptr;
    std::function<void()> onAction_;
};

class TextMessageCell : public brls::RecyclerCell {
public:
    TextMessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(20);
        addView(label_);
    }
    void setMessage(const std::string& text) { label_->setText(text); }

private:
    brls::Label* label_;
};

}  // namespace pipensx::ui
