#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// Fullscreen screenshot pager (O6)
// ---------------------------------------------------------------------------
// Opened by clicking (A) a screenshot on the game card. One shot fills the
// screen (FIT); page with LB/RB, the D-pad, or a horizontal swipe. B returns —
// AppletFrame registers that for free.
class ScreenshotViewerActivity : public brls::Activity {
  public:
    ScreenshotViewerActivity(GameMetadataService* metadata,
                             std::vector<std::string> urls, size_t index,
                             std::string title)
        : metadata_(metadata), urls_(std::move(urls)),
          title_(std::move(title)) {
        if (urls_.empty())
            urls_.push_back("");
        index_ = std::min(index, urls_.size() - 1);

        auto* root = new brls::Box(brls::Axis::COLUMN);
        root->setGrow(1);
        root->setAlignItems(brls::AlignItems::CENTER);
        root->setJustifyContent(brls::JustifyContent::CENTER);
        root->setPadding(24, 24, 24, 24);

        image_ = new AsyncRgbaImage();
        image_->setScalingType(brls::ImageScalingType::FIT);
        image_->setClipsToBounds(false);  // no letterbox edge bands
        image_->setGrow(1);
        image_->setWidthPercentage(100);
        image_->setFocusable(true);
        root->addView(image_);

        counter_ = new brls::Label();
        counter_->setFontSize(theme::kFontSmall);
        counter_->setTextColor(theme::textSecondary());
        counter_->setMarginTop(12);
        root->addView(counter_);

        // Horizontal swipe pages; only the final delta decides the direction.
        root->addGestureRecognizer(new brls::PanGestureRecognizer(
            [this](brls::PanGestureStatus status, brls::Sound*) {
                if (status.state != brls::GestureState::END)
                    return;
                float dx = status.position.x - status.startPosition.x;
                if (dx <= -kSwipeThreshold)
                    page(1);
                else if (dx >= kSwipeThreshold)
                    page(-1);
            },
            brls::PanAxis::HORIZONTAL));

        frame_ = new brls::AppletFrame(root);
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        registerAction("Previous", brls::BUTTON_LB,
                       [this](brls::View*) { page(-1); return true; }, false,
                       true);
        registerAction("Next", brls::BUTTON_RB,
                       [this](brls::View*) { page(1); return true; }, false,
                       true);
        registerAction("", brls::BUTTON_LEFT,
                       [this](brls::View*) { page(-1); return true; }, true,
                       true);
        registerAction("", brls::BUTTON_RIGHT,
                       [this](brls::View*) { page(1); return true; }, true,
                       true);
        show();
        brls::Application::giveFocus(image_);
    }

  private:
    static constexpr float kSwipeThreshold = 40.0f;

    void page(int delta) {
        if (urls_.size() <= 1)
            return;
        int count = static_cast<int>(urls_.size());
        index_ = static_cast<size_t>(
            (static_cast<int>(index_) + delta + count) % count);
        show();
    }

    void show() {
        loadImageInto(image_, metadata_, urls_[index_]);
        frame_->setTitle(title_.empty() ? "Screenshots" : title_);
        counter_->setText(std::to_string(index_ + 1) + " / " +
                          std::to_string(urls_.size()));
    }

    GameMetadataService* metadata_;
    std::vector<std::string> urls_;
    std::string title_;
    size_t index_ = 0;
    brls::AppletFrame* frame_ = nullptr;
    AsyncRgbaImage* image_ = nullptr;
    brls::Label* counter_ = nullptr;
};

}  // namespace pipensx::ui
