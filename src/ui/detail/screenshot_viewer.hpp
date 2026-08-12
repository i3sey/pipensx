#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// Fullscreen screenshot pager (O6)
// ---------------------------------------------------------------------------
// Opened by clicking (A) a screenshot on the game card. One shot fills the
// screen (FIT); page with LB/RB, the D-pad, or a horizontal swipe. B returns —
// AppletFrame registers that for free.
//
// Resolution: the rail's decode is capped at kImageDimCard (360px), so blowing
// that up over the whole screen is mush. The viewer re-requests the same URL in
// the kImageDimFull class — same cached bytes on disk, sharper decode — and
// paints the card-class decode meanwhile so paging never flashes empty.
// Sources that are themselves thumbnails (the catalogue's fastpic links are
// 300x168) cannot be sharpened, so their frame is capped at 2 physical pixels
// per source pixel and labelled instead of being stretched into mud.
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

        // The stage takes whatever the applet frame leaves between header and
        // footer; the shot is then sized against the stage, not the screen.
        // Weak token: the stage defers its resize by a frame, and that frame
        // can be the one where B pops this activity.
        stage_ = new StageBox([this, alive = std::weak_ptr<bool>(alive_)] {
            if (alive.expired())
                return;
            applyDecodedImage();
        });
        stage_->setGrow(1);
        stage_->setWidthPercentage(100);
        stage_->setAlignItems(brls::AlignItems::CENTER);
        stage_->setJustifyContent(brls::JustifyContent::CENTER);
        root->addView(stage_);

        // Placeholder tile, same trick as the detail page's cover plate: an
        // empty stage while the decode runs (or after it fails) reads as a
        // broken screen, a labelled 16:9 plate reads as "not yet".
        plate_ = new brls::Box();
        plate_->setWidth(kPlateWidth);
        plate_->setHeight(kPlateHeight);
        plate_->setCornerRadius(theme::kRadiusLarge);
        plate_->setBackgroundColor(theme::surface());
        plate_->setAlignItems(brls::AlignItems::CENTER);
        plate_->setJustifyContent(brls::JustifyContent::CENTER);
        plate_->setFocusable(true);
        plateLabel_ = new brls::Label();
        plateLabel_->setFontSize(theme::kFontBody);
        plateLabel_->setTextColor(theme::textSecondary());
        plate_->addView(plateLabel_);
        stage_->addView(plate_);

        image_ = new ViewerImage([this] { onTextureArrived(); });
        image_->setScalingType(brls::ImageScalingType::FIT);
        image_->setClipsToBounds(false);  // no letterbox edge bands
        image_->setFocusable(true);
        stage_->addView(image_);

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
        registerAction(tr("pipensx/common/previous"), brls::BUTTON_LB,
                       [this](brls::View*) { page(-1); return true; }, false,
                       true);
        registerAction(tr("pipensx/common/next"), brls::BUTTON_RB,
                       [this](brls::View*) { page(1); return true; }, false,
                       true);
        registerAction("", brls::BUTTON_LEFT,
                       [this](brls::View*) { page(-1); return true; }, true,
                       true);
        registerAction("", brls::BUTTON_RIGHT,
                       [this](brls::View*) { page(1); return true; }, true,
                       true);
        show();
        contentReady_ = true;
        focusVisible();
    }

  private:
    static constexpr float kSwipeThreshold = 40.0f;
    // Never draw a source pixel bigger than this many physical pixels: past it
    // the picture is bilinear soup, and a smaller sharp frame reads better.
    static constexpr float kMaxUpscale = 2.0f;
    // Sources below this stay thumbnails whatever we do — say so in the counter.
    static constexpr int kPreviewQualityBelow = 640;
    // Placeholder tile, 16:9 so it sits where the shot will.
    static constexpr float kPlateWidth = 640.0f;
    static constexpr float kPlateHeight = 360.0f;

    // Reports its own laid-out size, the way bug_report_view's GridBox does:
    // the shot can only be sized against the rectangle the frame really gave.
    class StageBox : public brls::Box {
      public:
        explicit StageBox(std::function<void()> onResize)
            : brls::Box(brls::Axis::COLUMN), onResize_(std::move(onResize)) {}

        void onLayout() override {
            brls::Box::onLayout();
            const float width = getWidth();
            const float height = getHeight();
            if (!onResize_ || width <= 0.0f || height <= 0.0f)
                return;
            // Resizing the child re-enters layout; only react to a stage that
            // actually changed, or this never settles.
            if (std::abs(width - laidOutWidth_) < 1.0f &&
                std::abs(height - laidOutHeight_) < 1.0f)
                return;
            laidOutWidth_ = width;
            laidOutHeight_ = height;
            // onLayout runs inside yoga's tree walk; defer the resize.
            brls::sync(onResize_);
        }

      private:
        std::function<void()> onResize_;
        float laidOutWidth_ = 0.0f;
        float laidOutHeight_ = 0.0f;
    };

    // Reports every texture swap (both the card-class placeholder and the
    // full-class decode) so the frame can be resized to the pixels it just got.
    class ViewerImage : public AsyncRgbaImage {
      public:
        explicit ViewerImage(std::function<void()> onImage)
            : onImage_(std::move(onImage)) {}

        void innerSetImage(int texture) override {
            AsyncRgbaImage::innerSetImage(texture);
            if (onImage_)
                onImage_();
        }

      private:
        std::function<void()> onImage_;
    };

    void page(int delta) {
        if (urls_.size() <= 1)
            return;
        int count = static_cast<int>(urls_.size());
        index_ = static_cast<size_t>(
            (static_cast<int>(index_) + delta + count) % count);
        show();
    }

    void show() {
        const std::string& url = urls_[index_];
        frame_->setTitle(title_.empty() ? tr("pipensx/detail/screenshots")
                                        : title_);
        showingFull_ = false;
        awaitingFull_ = false;
        updateCounter(0, 0);

        const uint64_t generation = ++state_->generation;
        if (!metadata_ || url.empty()) {
            image_->clear();
            showPlate(tr("pipensx/detail/screenshot_unavailable"));
            return;
        }
        // Already decoded at full size (paging back to a seen shot): straight
        // to the sharp texture, no placeholder step.
        if (GameMetadataService::ImageData full = metadata_->cachedImage(
                url, GameMetadataService::kImageDimFull)) {
            awaitingFull_ = true;
            hidePlate();
            image_->setRgbaNow(full->pixels.data(), full->width, full->height);
            return;
        }
        // Otherwise show the rail's card-class decode (it is in memory — the
        // detail page just drew it) while the full decode is produced, and fall
        // back to the labelled plate when even that is missing.
        if (GameMetadataService::ImageData card = metadata_->cachedImage(url)) {
            hidePlate();
            image_->setRgbaNow(card->pixels.data(), card->width, card->height);
        } else {
            image_->clear();
            showPlate(tr("pipensx/detail/screenshot_loading"));
        }
        requestFull(url, generation);
    }

    // Deliberately not loadImageInto(): that clears the view first, which would
    // throw away the placeholder we just painted, and it swallows the failure
    // the plate needs to report.
    void requestFull(const std::string& url, uint64_t generation) {
        auto state = state_;
        std::weak_ptr<bool> alive = alive_;
        metadata_->requestImage(url, [this, alive, state, generation](
            GameMetadataService::ImageData bytes) {
            // Worker thread: nothing here may touch a view directly.
            brls::sync([this, alive, state, generation, bytes] {
                if (alive.expired() ||
                    state->generation.load() != generation)
                    return;
                if (!bytes || bytes->pixels.empty()) {
                    image_->clear();
                    showPlate(tr("pipensx/detail/screenshot_unavailable"));
                    return;
                }
                awaitingFull_ = true;
                hidePlate();
                image_->setRgbaNow(bytes->pixels.data(), bytes->width,
                                   bytes->height);
            });
        }, GameMetadataService::kImageDimFull);
    }

    // Only one of plate/image is ever laid out, so focus has to follow the
    // visible one — a focused GONE view leaves LB/RB with nowhere to dispatch.
    void showPlate(const std::string& text) {
        plateLabel_->setText(text);
        plate_->setVisibility(brls::Visibility::VISIBLE);
        image_->setVisibility(brls::Visibility::GONE);
        plateVisible_ = true;
        focusVisible();
    }

    void hidePlate() {
        plate_->setVisibility(brls::Visibility::GONE);
        image_->setVisibility(brls::Visibility::VISIBLE);
        plateVisible_ = false;
        focusVisible();
    }

    void focusVisible() {
        if (!contentReady_)
            return;
        brls::Application::giveFocus(
            plateVisible_ ? static_cast<brls::View*>(plate_) : image_);
    }

    // A texture landed: the first one after requestFull() is the sharp decode,
    // anything before it is the card-class placeholder.
    void onTextureArrived() {
        if (awaitingFull_) {
            showingFull_ = true;
            awaitingFull_ = false;
        }
        applyDecodedImage();
    }

    // Size the frame to the decode it is holding. The placeholder keeps the old
    // full-screen FIT behaviour; only the final decode gets the upscale cap, so
    // the frame does not jump from small to large mid-load.
    void applyDecodedImage() {
        const float nativeWidth = image_->getOriginalImageWidth();
        const float nativeHeight = image_->getOriginalImageHeight();
        if (nativeWidth <= 0.0f || nativeHeight <= 0.0f) {
            updateCounter(0, 0);
            return;
        }
        const float availableWidth = stage_->getWidth();
        const float availableHeight = stage_->getHeight();
        if (availableWidth <= 0.0f || availableHeight <= 0.0f) {
            // Pre-layout: StageBox calls back once it has a rectangle.
            updateCounter(static_cast<int>(nativeWidth),
                          static_cast<int>(nativeHeight));
            return;
        }
        float scale = std::min(availableWidth / nativeWidth,
                               availableHeight / nativeHeight);
        if (showingFull_) {
            const float windowScale = brls::Application::windowScale > 0.0f
                                          ? brls::Application::windowScale
                                          : 1.0f;
            scale = std::min(scale, kMaxUpscale / windowScale);
        }
        image_->setWidth(nativeWidth * scale);
        image_->setHeight(nativeHeight * scale);
        updateCounter(static_cast<int>(nativeWidth),
                      static_cast<int>(nativeHeight));
    }

    void updateCounter(int nativeWidth, int nativeHeight) {
        std::string text = std::to_string(index_ + 1) + " / " +
                           std::to_string(urls_.size());
        if (showingFull_ && nativeWidth > 0 && nativeHeight > 0 &&
            std::max(nativeWidth, nativeHeight) < kPreviewQualityBelow) {
            text += "  ·  " + tr("pipensx/detail/screenshot_preview",
                                 nativeWidth, nativeHeight);
        }
        counter_->setText(text);
    }

    GameMetadataService* metadata_;
    std::vector<std::string> urls_;
    std::string title_;
    size_t index_ = 0;
    brls::AppletFrame* frame_ = nullptr;
    StageBox* stage_ = nullptr;
    brls::Box* plate_ = nullptr;
    brls::Label* plateLabel_ = nullptr;
    ViewerImage* image_ = nullptr;
    brls::Label* counter_ = nullptr;
    std::shared_ptr<ImageRequestState> state_ =
        std::make_shared<ImageRequestState>();
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool awaitingFull_ = false;
    bool showingFull_ = false;
    bool plateVisible_ = false;
    bool contentReady_ = false;
};

}  // namespace pipensx::ui
