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

inline constexpr int kColumns = 4;
inline constexpr float kCardWidth = 180.0f;
inline constexpr float kCoverHeight = 180.0f;
// Cover + name line (17px) + sub line (15px) + inner margins.
inline constexpr float kCardHeight = 232.0f;
inline constexpr float kRowHeight = 248.0f;
inline constexpr int kShelfItems = 12;
inline constexpr float kShelfSpacing = 2 * theme::kSpacingUnit;
// Shelf title (21px) + margin + card strip + bottom breathing room.
inline constexpr float kShelfHeight = 284.0f;

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
        name_->setFontSize(theme::kFontSmall);
        name_->setMarginTop(6);
        addView(name_);

        sub_ = new brls::Label();
        sub_->setSingleLine(true);
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
        if (onFocus_)
            onFocus_();
    }

    int entryIndex() const { return entryIndex_; }
    const std::string& infoHash() const { return infoHash_; }
    // >= 0 when the card lives in a shelf (value = recycler row of the shelf).
    int shelfRow() const { return shelfRow_; }

private:
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
    HorizontalShelf() : brls::Box(brls::Axis::ROW) {
        setHeight(grid::kCardHeight);
        setClipsToBounds(true);
        content_ = new brls::Box(brls::Axis::ROW);
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
            if (i < active_)
                cards_[i]->setCard(infos[static_cast<size_t>(i)], service,
                                   onActivate, nullptr, shelfRow);
            else
                cards_[i]->setEmpty();
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
        const float content = active_ > 0
            ? active_ * grid::kCardWidth +
                  (active_ - 1) * grid::kShelfSpacing
            : 0.0f;
        const float maxOffset = std::max(0.0f, content - getWidth());
        offset_ = std::clamp(offset_, 0.0f, maxOffset);
        content_->setTranslationX(-offset_);
    }

    brls::Box* content_;
    GameCard* cards_[grid::kShelfItems] = {};
    int active_ = 0;
    float offset_ = 0;
};

class ShelfCell : public brls::RecyclerCell {
public:
    ShelfCell() {
        setFocusable(false);
        setAxis(brls::Axis::COLUMN);
        setHeight(grid::kShelfHeight);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        title_ = new brls::Label();
        title_->setFontSize(theme::kFontBody);
        title_->setMarginBottom(theme::kSpacingUnit);
        shelf_ = new HorizontalShelf();
        addView(title_);
        addView(shelf_);
    }

    void setShelf(const std::string& title,
                  const std::vector<GridCardInfo>& infos,
                  GameMetadataService* service, GameCard::Activate onActivate,
                  int shelfRow) {
        title_->setText(title);
        shelf_->setItems(infos, service, std::move(onActivate), shelfRow);
    }

private:
    brls::Label* title_;
    HorizontalShelf* shelf_;
};

}  // namespace pipensx::ui
