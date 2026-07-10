#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// UI_PLAN F2 — eShop-style catalog: cover-art grid + horizontal shelves.
//
// The grid is built with the wiliwili "row of N cells" pattern: every
// RecyclerFrame row is one recycled GridRowCell that hosts kColumns focusable
// GameCards. Shelves are RecyclerFrame rows too (ShelfCell), each wrapping a
// clipped, pannable horizontal strip of GameCards.
// ---------------------------------------------------------------------------

namespace grid {

inline constexpr int kColumns = 5;
inline constexpr float kCardWidth = 180.0f;
inline constexpr float kCoverHeight = 180.0f;
// Langegen/games covers are Nintendo box-art images (~393x640), not square
// eShop icons. Render them at their natural portrait aspect inside the same
// 180px card slot so they do not get stretched/cropped like square icons.
inline constexpr float kGameCoverWidth = 112.0f;
// Cover + name line (17px) + sub line (15px) + inner margins.
inline constexpr float kCardHeight = 232.0f;
inline constexpr float kRowHeight = 248.0f;
inline constexpr int kShelfItems = 12;
inline constexpr float kShelfSpacing = 2 * theme::kSpacingUnit;
// Shelf title (21px) + margin + card strip + bottom breathing room.
inline constexpr float kShelfHeight = 284.0f;
// UI_PLAN F5: minimum cards for an optional shelf (New / Updated / genre)
// to appear at all; "Popular" is exempt — it never hides silently.
inline constexpr int kMinShelfItems = 4;
// Featured hero banner: full-width card + bottom breathing room.
inline constexpr float kHeroCardHeight = 280.0f;
inline constexpr float kHeroHeight = 300.0f;
// Subtle cover zoom while a card is focused (F5 focus-scale).
inline constexpr float kFocusZoom = 1.06f;

}  // namespace grid

// Everything a GameCard needs to render one catalog entry. entryIndex refers
// to CatalogDataSource::entries() so activation flows through the existing
// CatalogView::onEntrySelected() logic (detail page / batch toggle).
struct GridCardInfo {
    int entryIndex = -1;
    std::string infoHash;
    std::string title;
    std::string sub;
    bool subIsBadge = false;
    std::string iconUrl;
    bool iconPreserveAspect = false;
    bool selectionMode = false;
    bool selected = false;
    bool selectable = false;
};

class GameCard : public brls::Box {
public:
    using Activate = std::function<void(int)>;
    using Focused = std::function<void()>;

    GameCard() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(true);
        setWidth(grid::kCardWidth);
        setHeight(grid::kCardHeight);
        setCornerRadius(theme::kRadiusMedium);

        // Cover box doubles as the placeholder tile until art arrives.
        cover_ = new brls::Box();
        cover_->setWidth(grid::kCardWidth);
        cover_->setHeight(grid::kCoverHeight);
        cover_->setCornerRadius(theme::kRadiusMedium);
        cover_->setBackgroundColor(theme::surface());
        cover_->setAlignItems(brls::AlignItems::CENTER);
        cover_->setJustifyContent(brls::JustifyContent::CENTER);
        // Focus zoom scales the cover art past the cover box bounds.
        cover_->setClipsToBounds(true);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(theme::kFontTitle);
        placeholder_->setTextColor(theme::textSecondary());
        cover_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(grid::kCardWidth);
        image_->setHeight(grid::kCoverHeight);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(theme::kRadiusMedium);
        image_->setScalingType(brls::ImageScalingType::FILL);
        cover_->addView(image_);

        // Batch-mode selection chip (top-left corner of the cover).
        markBox_ = new brls::Box();
        markBox_->setPositionType(brls::PositionType::ABSOLUTE);
        markBox_->setPositionTop(theme::kSpacingUnit);
        markBox_->setPositionLeft(theme::kSpacingUnit);
        markBox_->setWidth(36);
        markBox_->setHeight(36);
        markBox_->setCornerRadius(theme::kRadiusSmall);
        markBox_->setBackgroundColor(theme::overlay());
        markBox_->setAlignItems(brls::AlignItems::CENTER);
        markBox_->setJustifyContent(brls::JustifyContent::CENTER);
        mark_ = new brls::Label();
        mark_->setFontSize(theme::kFontBody);
        markBox_->addView(mark_);
        markBox_->setVisibility(brls::Visibility::GONE);
        cover_->addView(markBox_);

        addView(cover_);

        name_ = new brls::Label();
        name_->setSingleLine(true);
        // Keep ellipsis at rest and marquee only the focused card title.
        name_->setAutoAnimate(false);
        name_->setFontSize(theme::kFontSmall);
        name_->setMarginTop(6);
        addView(name_);

        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setAutoAnimate(false);
        sub_->setFontSize(theme::kFontCaption);
        sub_->setMarginTop(2);
        sub_->setTextColor(theme::textTertiary());
        addView(sub_);

        registerClickAction([this](brls::View*) {
            if (onActivate_)
                onActivate_(entryIndex_);
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    void setCard(const GridCardInfo& info, GameMetadataService* service,
                 Activate onActivate, Focused onFocus, int shelfRow = -1) {
        setVisibility(brls::Visibility::VISIBLE);
        setFocusable(true);
        entryIndex_ = info.entryIndex;
        infoHash_ = info.infoHash;
        shelfRow_ = shelfRow;
        onActivate_ = std::move(onActivate);
        onFocus_ = std::move(onFocus);
        name_->setText(info.title);
        placeholder_->setText(placeholderLetter(info.title));
        sub_->setText(info.sub);
        sub_->setTextColor(info.subIsBadge ? theme::accent()
                                           : theme::textTertiary());
        markBox_->setVisibility(info.selectionMode ? brls::Visibility::VISIBLE
                                                   : brls::Visibility::GONE);
        mark_->setText(!info.selectable ? "-" : info.selected ? "x" : " ");
        mark_->setTextColor(info.selectable ? theme::accent()
                                            : theme::textDisabled());
        const bool highlight = info.selectionMode && info.selected;
        cover_->setBorderThickness(highlight ? 4 : 0);
        cover_->setBorderColor(highlight ? theme::accent()
                                         : brls::TRANSPARENT);
        iconPreserveAspect_ = info.iconPreserveAspect;
        applyZoom(false);
        image_->setScalingType(info.iconPreserveAspect
            ? brls::ImageScalingType::FIT
            : brls::ImageScalingType::FILL);
        setArtworkUrl(image_, service, info.iconUrl, currentIconUrl_,
                      imageState_);
    }

    // Unused trailing slot in a row/shelf: keeps its layout space so columns
    // stay aligned, but can neither draw nor take focus.
    void setEmpty() {
        setVisibility(brls::Visibility::INVISIBLE);
        setFocusable(false);
        onActivate_ = nullptr;
        onFocus_ = nullptr;
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        applyZoom(true);
        name_->setAnimated(true);
        if (onFocus_)
            onFocus_();
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        applyZoom(false);
        name_->setAnimated(false);
    }

    int entryIndex() const { return entryIndex_; }
    const std::string& infoHash() const { return infoHash_; }
    // >= 0 when the card lives in a shelf (value = recycler row of the shelf).
    int shelfRow() const { return shelfRow_; }

private:
    // UI_PLAN F5: subtle cover zoom while the card is focused. The cover box
    // clips, so the art scales in place without shifting the layout.
    void applyZoom(bool focused) {
        const float zoom = focused ? grid::kFocusZoom : 1.0f;
        const float baseWidth = iconPreserveAspect_
            ? grid::kGameCoverWidth
            : grid::kCardWidth;
        image_->setDimensions(baseWidth * zoom, grid::kCoverHeight * zoom);
        image_->setPositionTop(-(zoom - 1.0f) * grid::kCoverHeight / 2.0f);
        image_->setPositionLeft((grid::kCardWidth - baseWidth * zoom) / 2.0f);
    }

    brls::Box* cover_;
    brls::Label* placeholder_;
    AsyncRgbaImage* image_;
    brls::Box* markBox_;
    brls::Label* mark_;
    brls::Label* name_;
    brls::Label* sub_;
    std::string currentIconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    int entryIndex_ = -1;
    std::string infoHash_;
    int shelfRow_ = -1;
    bool iconPreserveAspect_ = false;
    Activate onActivate_;
    Focused onFocus_;
};

// One recycled grid row holding kColumns cards. Left/right moves between the
// cards via the regular Box navigation; up/down lands on getDefaultFocus(),
// which preserves the column the user was in (shared focusColumn state).
class GridRowCell : public brls::RecyclerCell {
public:
    explicit GridRowCell(std::shared_ptr<int> focusColumn)
        : focusColumn_(std::move(focusColumn)) {
        setFocusable(false);
        setAxis(brls::Axis::ROW);
        setHeight(grid::kRowHeight);
        setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        for (int i = 0; i < grid::kColumns; ++i) {
            cards_[i] = new GameCard();
            addView(cards_[i]);
        }
    }

    void setRow(const std::vector<GridCardInfo>& infos,
                GameMetadataService* service, GameCard::Activate onActivate) {
        activeCards_ = static_cast<int>(
            std::min<size_t>(infos.size(), grid::kColumns));
        for (int i = 0; i < grid::kColumns; ++i) {
            if (i < activeCards_) {
                std::shared_ptr<int> column = focusColumn_;
                cards_[i]->setCard(infos[static_cast<size_t>(i)], service,
                                   onActivate, [column, i] { *column = i; });
            } else {
                cards_[i]->setEmpty();
            }
        }
    }

    brls::View* getDefaultFocus() override {
        if (getVisibility() != brls::Visibility::VISIBLE || activeCards_ <= 0)
            return brls::Box::getDefaultFocus();
        int column = std::clamp(*focusColumn_, 0, activeCards_ - 1);
        return cards_[column];
    }

private:
    std::shared_ptr<int> focusColumn_;
    GameCard* cards_[grid::kColumns] = {};
    int activeCards_ = 0;
};

// Clipped horizontal strip of cards. Scrolls two ways: focus movement keeps
// the focused card in view (stick / d-pad), and a horizontal pan gesture
// drags the strip directly (touch).
class HorizontalShelf : public brls::Box {
public:
    // focusHash is shared with CatalogView (UI_PLAN O12): the shelf writes
    // the info-hash of whichever card holds the focus, and getDefaultFocus()
    // reads it back, so focus restore survives the cell being recycled.
    explicit HorizontalShelf(std::shared_ptr<std::string> focusHash)
        : brls::Box(brls::Axis::ROW), focusHash_(std::move(focusHash)) {
        setHeight(grid::kCardHeight);
        setClipsToBounds(true);
        content_ = new brls::Box(brls::Axis::ROW);
        // The strip is translated manually. Keep its Yoga bounds at the full
        // card extent so Borealis does not cull labels and images that started
        // outside the shelf viewport and are later scrolled into view.
        content_->setShrink(0.0f);
        for (int i = 0; i < grid::kShelfItems; ++i) {
            cards_[i] = new GameCard();
            if (i + 1 < grid::kShelfItems)
                cards_[i]->setMarginRight(grid::kShelfSpacing);
            content_->addView(cards_[i]);
        }
        addView(content_);
        addGestureRecognizer(new brls::PanGestureRecognizer(
            [this](brls::PanGestureStatus status, brls::Sound*) {
                if (status.state == brls::GestureState::START ||
                    status.state == brls::GestureState::STAY)
                    scrollBy(-status.delta.x);
            },
            brls::PanAxis::HORIZONTAL));
    }

    void setItems(const std::vector<GridCardInfo>& infos,
                  GameMetadataService* service, GameCard::Activate onActivate,
                  int shelfRow) {
        active_ = static_cast<int>(
            std::min<size_t>(infos.size(), grid::kShelfItems));
        for (int i = 0; i < grid::kShelfItems; ++i) {
            if (i < active_) {
                std::shared_ptr<std::string> hash = focusHash_;
                cards_[i]->setCard(
                    infos[static_cast<size_t>(i)], service, onActivate,
                    [hash, h = infos[static_cast<size_t>(i)].infoHash] {
                        *hash = h;
                    },
                    shelfRow);
            } else {
                cards_[i]->setEmpty();
            }
        }
        offset_ = 0;
        applyOffset();
    }

    void onChildFocusGained(brls::View* directChild,
                            brls::View* focusedView) override {
        for (int i = 0; i < active_; ++i) {
            if (cards_[i] == focusedView) {
                scrollToCard(i);
                break;
            }
        }
        brls::Box::onChildFocusGained(directChild, focusedView);
    }

    // O12: focus entering the shelf lands on the remembered card (also
    // scrolled into view via onChildFocusGained), not blindly on the first.
    brls::View* getDefaultFocus() override {
        for (int i = 0; i < active_; ++i) {
            if (cards_[i]->infoHash() == *focusHash_)
                return cards_[i];
        }
        if (active_ > 0)
            return cards_[0];
        return brls::Box::getDefaultFocus();
    }

private:
    void scrollToCard(int index) {
        const float viewport = getWidth();
        if (viewport <= 0)
            return;
        const float left =
            index * (grid::kCardWidth + grid::kShelfSpacing);
        const float right = left + grid::kCardWidth;
        if (left < offset_)
            offset_ = left;
        else if (right > offset_ + viewport)
            offset_ = right - viewport;
        applyOffset();
    }

    void scrollBy(float delta) {
        offset_ += delta;
        applyOffset();
    }

    void applyOffset() {
        const float contentWidth = active_ > 0
            ? active_ * grid::kCardWidth +
                  (active_ - 1) * grid::kShelfSpacing
            : 0.0f;
        content_->setWidth(contentWidth);
        const float maxOffset = std::max(0.0f, contentWidth - getWidth());
        offset_ = std::clamp(offset_, 0.0f, maxOffset);
        content_->setTranslationX(-offset_);
    }

    brls::Box* content_;
    GameCard* cards_[grid::kShelfItems] = {};
    int active_ = 0;
    float offset_ = 0;
    std::shared_ptr<std::string> focusHash_;
};

class ShelfCell : public brls::RecyclerCell {
public:
    explicit ShelfCell(std::shared_ptr<std::string> focusHash) {
        setFocusable(false);
        setAxis(brls::Axis::COLUMN);
        setHeight(grid::kShelfHeight);
        setAlignItems(brls::AlignItems::STRETCH);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        // Section header (UI_PLAN F5): shelf title + optional "See all"
        // action jumping into the grid with the matching sort/filter.
        header_ = new brls::Box(brls::Axis::ROW);
        header_->setWidthPercentage(100);
        header_->setHeight(32);
        header_->setShrink(0.0f);
        header_->setMarginBottom(theme::kSpacingUnit);
        title_ = new brls::Label();
        title_->setFontSize(theme::kFontBody);
        title_->setTextColor(theme::textPrimary());
        title_->setGrow(1);
        seeAll_ = new brls::Button();
        seeAll_->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        seeAll_->setHeight(32);
        seeAll_->setFontSize(theme::kFontCaption);
        seeAll_->setTextColor(theme::textTertiary());
        seeAll_->setPaddingLeft(12);
        seeAll_->setPaddingRight(12);
        seeAll_->setShrink(0.0f);
        seeAll_->setText("See all");
        seeAll_->registerClickAction([this](brls::View*) {
            if (seeAllAction_)
                seeAllAction_();
            return true;
        });
        header_->addView(title_);
        header_->addView(seeAll_);
        shelf_ = new HorizontalShelf(std::move(focusHash));
        shelf_->setWidthPercentage(100);
        addView(header_);
        addView(shelf_);
    }

    // O12: default focus goes to the shelf's remembered card, not to the
    // "See all" label (the first focusable child in tree order).
    brls::View* getDefaultFocus() override {
        return shelf_->getDefaultFocus();
    }

    void setShelf(const std::string& title,
                  const std::vector<GridCardInfo>& infos,
                  GameMetadataService* service, GameCard::Activate onActivate,
                  int shelfRow, std::function<void()> seeAll) {
        title_->setText(title);
        seeAllAction_ = std::move(seeAll);
        const bool hasSeeAll = static_cast<bool>(seeAllAction_);
        seeAll_->setVisibility(hasSeeAll ? brls::Visibility::VISIBLE
                                         : brls::Visibility::GONE);
        seeAll_->setFocusable(hasSeeAll);
        shelf_->setItems(infos, service, std::move(onActivate), shelfRow);
        invalidate();
    }

private:
    brls::Box* header_;
    brls::Label* title_;
    brls::Button* seeAll_;
    HorizontalShelf* shelf_;
    std::function<void()> seeAllAction_;
};

// Full-width featured banner above the shelves (UI_PLAN F5). One focusable
// card: banner art fills the box, a bottom overlay carries the caption,
// title and sub line. Activation routes through the same entry-index path
// as the grid cards.
class HeroCard : public brls::Box {
public:
    using Activate = std::function<void(int)>;

    HeroCard() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(true);
        setHeight(grid::kHeroCardHeight);
        setCornerRadius(theme::kRadiusLarge);
        setClipsToBounds(true);
        setBackgroundColor(theme::surface());
        setJustifyContent(brls::JustifyContent::FLEX_END);

        image_ = new AsyncRgbaImage();
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setWidthPercentage(100);
        image_->setHeightPercentage(100);
        image_->setScalingType(brls::ImageScalingType::FILL);
        addView(image_);

        auto* overlay = new brls::Box(brls::Axis::COLUMN);
        overlay->setWidthPercentage(100);
        overlay->setBackgroundColor(theme::overlay());
        overlay->setPadding(2 * theme::kSpacingUnit, 3 * theme::kSpacingUnit,
                            2 * theme::kSpacingUnit, 3 * theme::kSpacingUnit);
        kicker_ = new brls::Label();
        kicker_->setFontSize(theme::kFontCaption);
        kicker_->setTextColor(theme::accent());
        kicker_->setText("Featured");
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(theme::kFontHeading);
        title_->setMarginTop(2);
        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setFontSize(theme::kFontCaption);
        sub_->setTextColor(theme::textTertiary());
        sub_->setMarginTop(2);
        overlay->addView(kicker_);
        overlay->addView(title_);
        overlay->addView(sub_);
        addView(overlay);

        registerClickAction([this](brls::View*) {
            if (onActivate_)
                onActivate_(entryIndex_);
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    void setHero(const GridCardInfo& info, const std::string& imageUrl,
                 GameMetadataService* service, Activate onActivate) {
        entryIndex_ = info.entryIndex;
        infoHash_ = info.infoHash;
        onActivate_ = std::move(onActivate);
        title_->setText(info.title);
        sub_->setText(info.sub);
        sub_->setTextColor(info.subIsBadge ? theme::accent()
                                           : theme::textTertiary());
        setArtworkUrl(image_, service, imageUrl, currentUrl_, imageState_);
    }

    int entryIndex() const { return entryIndex_; }
    const std::string& infoHash() const { return infoHash_; }

private:
    AsyncRgbaImage* image_;
    brls::Label* kicker_;
    brls::Label* title_;
    brls::Label* sub_;
    std::string currentUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    int entryIndex_ = -1;
    std::string infoHash_;
    Activate onActivate_;
};

class HeroCell : public brls::RecyclerCell {
public:
    HeroCell() {
        setFocusable(false);
        setAxis(brls::Axis::COLUMN);
        setHeight(grid::kHeroHeight);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        hero_ = new HeroCard();
        addView(hero_);
    }

    void setHero(const GridCardInfo& info, const std::string& imageUrl,
                 GameMetadataService* service, HeroCard::Activate onActivate) {
        hero_->setHero(info, imageUrl, service, std::move(onActivate));
    }

private:
    HeroCard* hero_;
};

}  // namespace pipensx::ui
