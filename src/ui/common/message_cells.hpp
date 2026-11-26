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

        action_ = new theme::Button();
        action_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        action_->setMinWidth(240);
        action_->setPadding(0, 24, 0, 24);
        action_->setHeight(44);
        action_->registerClickAction([this](brls::View*) {
            if (onAction_)
                onAction_();
            return true;
        });
        action_->addGestureRecognizer(new brls::TapGestureRecognizer(action_));
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
        setPadding(24, 28, 24, 28);
        setLineTop(0);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        label_ = new brls::Label();
        label_->setFontSize(20);
        label_->setSingleLine(false);
        addView(label_);
    }
    void setMessage(const std::string& text) { label_->setText(text); }
    void frame(brls::FrameContext* context) override {
        setLineTop(0);
        setLineBottom(0);
        brls::RecyclerCell::frame(context);
    }

private:
    brls::Label* label_;
};

class SkeletonGridRowCell : public brls::RecyclerCell {
public:
    static constexpr int kDefaultColumns = 6;
    static constexpr float kDefaultCardWidth = 190.0f;
    static constexpr float kDefaultCardHeight = 348.0f;
    static constexpr float kDefaultCoverWidth = 182.0f;
    static constexpr float kDefaultCoverHeight = 296.0f;
    static constexpr float kDefaultRowHeight = 364.0f;

    explicit SkeletonGridRowCell(int columns = kDefaultColumns,
                                 float cardWidth = kDefaultCardWidth,
                                 float cardHeight = kDefaultCardHeight,
                                 float coverWidth = kDefaultCoverWidth,
                                 float coverHeight = kDefaultCoverHeight,
                                 float rowHeight = kDefaultRowHeight) {
        setFocusable(false);
        setAxis(brls::Axis::ROW);
        setHeight(rowHeight);
        setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        setLineTop(0);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);

        for (int i = 0; i < columns; ++i) {
            auto* card = new brls::Box(brls::Axis::COLUMN);
            card->setFocusable(i == 0);
            card->setWidth(cardWidth);
            card->setHeight(cardHeight);
            card->setBackgroundColor(theme::surface());
            card->setCornerRadius(theme::kRadiusMedium);
            card->setHighlightCornerRadius(theme::kRadiusMedium);
            card->setClipsToBounds(true);

            auto* cover = new brls::Box();
            cover->setWidth(coverWidth);
            cover->setHeight(coverHeight);
            cover->setCornerRadius(theme::kRadiusMedium);
            cover->setBackgroundColor(theme::panel());
            cover->setAlignSelf(brls::AlignSelf::CENTER);
            cover->setMarginTop(4.0f);
            card->addView(cover);

            auto* line1 = new brls::Box();
            line1->setWidth(coverWidth * 0.72f);
            line1->setHeight(14.0f);
            line1->setCornerRadius(theme::kRadiusSmall);
            line1->setBackgroundColor(theme::panel());
            line1->setMarginTop(10.0f);
            line1->setMarginLeft(theme::kSpacingUnit);
            card->addView(line1);

            auto* line2 = new brls::Box();
            line2->setWidth(coverWidth * 0.44f);
            line2->setHeight(11.0f);
            line2->setCornerRadius(theme::kRadiusSmall);
            line2->setBackgroundColor(theme::track());
            line2->setMarginTop(6.0f);
            line2->setMarginLeft(theme::kSpacingUnit);
            card->addView(line2);

            if (i == 0)
                firstCard_ = card;
            addView(card);
        }
    }

    brls::View* getDefaultFocus() override {
        return firstCard_ ? firstCard_ : brls::Box::getDefaultFocus();
    }

    void frame(brls::FrameContext* context) override {
        setLineTop(0);
        setLineBottom(0);
        brls::RecyclerCell::frame(context);
    }

private:
    brls::Box* firstCard_ = nullptr;
};

class SkeletonListRowCell : public brls::RecyclerCell {
public:
    SkeletonListRowCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 16, 12, 16);
        setHeight(92.0f);
        setLineTop(0);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);

        auto* icon = new brls::Box();
        icon->setWidth(68.0f);
        icon->setHeight(68.0f);
        icon->setShrink(0.0f);
        icon->setCornerRadius(theme::kRadiusSmall);
        icon->setBackgroundColor(theme::surface());
        icon->setMarginRight(16.0f);
        addView(icon);

        auto* labels = new brls::Box(brls::Axis::COLUMN);
        labels->setGrow(1.0f);
        labels->setShrink(1.0f);

        auto* titleBar = new brls::Box();
        titleBar->setWidth(280.0f);
        titleBar->setHeight(18.0f);
        titleBar->setCornerRadius(theme::kRadiusSmall);
        titleBar->setBackgroundColor(theme::surface());
        labels->addView(titleBar);

        auto* subBar = new brls::Box();
        subBar->setWidth(180.0f);
        subBar->setHeight(13.0f);
        subBar->setCornerRadius(theme::kRadiusSmall);
        subBar->setBackgroundColor(theme::track());
        subBar->setMarginTop(8.0f);
        labels->addView(subBar);

        addView(labels);

        auto* chipBar = new brls::Box();
        chipBar->setWidth(76.0f);
        chipBar->setHeight(24.0f);
        chipBar->setShrink(0.0f);
        chipBar->setCornerRadius(theme::kRadiusSmall);
        chipBar->setBackgroundColor(theme::panel());
        chipBar->setMarginLeft(12.0f);
        addView(chipBar);
    }

    void frame(brls::FrameContext* context) override {
        setLineTop(0);
        setLineBottom(0);
        brls::RecyclerCell::frame(context);
    }
};

}  // namespace pipensx::ui
