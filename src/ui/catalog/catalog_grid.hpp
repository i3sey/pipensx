#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/i18n.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// eShop-style catalog: square-icon grid.
//
// The grid is built with the wiliwili "row of N cells" pattern: every
// RecyclerFrame row is one recycled GridRowCell that hosts kColumns focusable
// GameCards.
// ---------------------------------------------------------------------------

namespace grid {

inline constexpr int kColumns = 6;
inline constexpr float kCardWidth = 190.0f;
// Icon is slightly smaller than the plate so the substrate frames it.
inline constexpr float kCoverInset = 4.0f;
inline constexpr float kCoverSize = 182.0f;  // kCardWidth - 2 * kCoverInset
inline constexpr float kCoverHeight = kCoverSize;
// Icon inside the card uses the same rounding as the card itself so the
// whole plate reads as one piece.
inline constexpr float kIconRadius = theme::kRadiusMedium;
// Langegen/games covers are Nintendo box-art images (~393x640), not square
// eShop icons. FIT them inside the square slot.
inline constexpr float kGameCoverWidth = 114.0f;
// Inset + cover + name line (17px) + sub line (15px) + inner margins,
// no pad after the size line.
inline constexpr float kCardHeight = 236.0f;
inline constexpr float kRowHeight = 252.0f;
inline constexpr float kTopInsetHeight = 8.0f;
inline constexpr float kFocusZoom = 1.06f;

}  // namespace grid

// Everything a GameCard needs to render one catalog entry. entryIndex refers
// to the visible list so activation flows through the existing
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
    // On the wishlist (ZR on the grid, or the game page button).
    bool favorite = false;
};

inline bool sameGridCardInfo(const GridCardInfo& left,
                             const GridCardInfo& right) {
    return left.entryIndex == right.entryIndex &&
           left.infoHash == right.infoHash && left.title == right.title &&
           left.sub == right.sub && left.subIsBadge == right.subIsBadge &&
           left.iconUrl == right.iconUrl &&
           left.iconPreserveAspect == right.iconPreserveAspect &&
           left.selectionMode == right.selectionMode &&
           left.selected == right.selected &&
           left.selectable == right.selectable &&
           left.favorite == right.favorite;
}

class GameCard : public brls::Box {
public:
    using Activate = std::function<void(int)>;
    using Focused = std::function<void()>;

    GameCard() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(true);
        setWidth(grid::kCardWidth);
        setHeight(grid::kCardHeight);
        // Single substrate for icon + title + size: barely off the app
        // background (surface vs brls/background).
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusMedium);
        setClipsToBounds(true);

        // Cover box doubles as the placeholder tile until art arrives.
        // Same rounding as the card so the icon sits as a rounded plate
        // inside the substrate.
        cover_ = new brls::Box();
        cover_->setWidth(grid::kCoverSize);
        cover_->setHeight(grid::kCoverHeight);
        cover_->setCornerRadius(grid::kIconRadius);
        cover_->setBackgroundColor(theme::surface());
        cover_->setAlignSelf(brls::AlignSelf::CENTER);
        cover_->setMarginTop(grid::kCoverInset);
        cover_->setAlignItems(brls::AlignItems::CENTER);
        cover_->setJustifyContent(brls::JustifyContent::CENTER);
        // Focus zoom scales the cover art past the cover box bounds.
        cover_->setClipsToBounds(true);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(theme::kFontTitle);
        placeholder_->setTextColor(theme::textSecondary());
        cover_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(grid::kCoverSize);
        image_->setHeight(grid::kCoverHeight);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(grid::kIconRadius);
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

        // Wishlist star, bottom-left of the cover: the selection chip owns the
        // top-left corner, so this is the one corner that never collides.
        favoriteBox_ = new brls::Box();
        favoriteBox_->setPositionType(brls::PositionType::ABSOLUTE);
        favoriteBox_->setPositionBottom(theme::kSpacingUnit);
        favoriteBox_->setPositionLeft(theme::kSpacingUnit);
        favoriteBox_->setWidth(28);
        favoriteBox_->setHeight(28);
        favoriteBox_->setCornerRadius(theme::kRadiusSmall);
        favoriteBox_->setBackgroundColor(theme::overlay());
        favoriteBox_->setAlignItems(brls::AlignItems::CENTER);
        favoriteBox_->setJustifyContent(brls::JustifyContent::CENTER);
        favorite_ = new brls::Label();
        favorite_->setFontSize(theme::kFontCaption);
        favorite_->setTextColor(theme::accent());
        favorite_->setText("★");
        favoriteBox_->addView(favorite_);
        favoriteBox_->setVisibility(brls::Visibility::GONE);
        cover_->addView(favoriteBox_);

        addView(cover_);

        name_ = new brls::Label();
        name_->setSingleLine(true);
        // Keep ellipsis at rest and marquee only the focused card title.
        name_->setAutoAnimate(false);
        name_->setFontSize(theme::kFontSmall);
        name_->setMarginTop(6);
        name_->setMarginLeft(theme::kSpacingUnit);
        name_->setMarginRight(theme::kSpacingUnit);
        addView(name_);

        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setAutoAnimate(false);
        sub_->setFontSize(theme::kFontCaption);
        sub_->setMarginTop(2);
        sub_->setMarginLeft(theme::kSpacingUnit);
        sub_->setMarginRight(theme::kSpacingUnit);
        sub_->setMarginBottom(0);
        sub_->setTextColor(theme::textTertiary());
        addView(sub_);

        registerAction("", brls::BUTTON_A, [this](brls::View*) {
            if (onActivate_)
                onActivate_(entryIndex_);
            return true;
        }, /*hidden=*/true);
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    void setCard(const GridCardInfo& info, GameMetadataService* service,
                 Activate onActivate, Focused onFocus, int shelfRow = -1) {
        const bool artworkCurrent =
            info.iconUrl.empty() || image_->hasArtwork() ||
            imageState_->pending.load();
        const bool unchanged =
            hasCard_ && shelfRow_ == shelfRow && artworkService_ == service &&
            artworkCurrent && sameGridCardInfo(painted_, info);
        entryIndex_ = info.entryIndex;
        infoHash_ = info.infoHash;
        shelfRow_ = shelfRow;
        onActivate_ = std::move(onActivate);
        onFocus_ = std::move(onFocus);
        if (unchanged)
            return;
        if (getVisibility() != brls::Visibility::VISIBLE)
            setVisibility(brls::Visibility::VISIBLE);
        if (!isFocusable())
            setFocusable(true);
        if (name_->getFullText() != info.title)
            name_->setText(info.title);
        const std::string placeholder = placeholderLetter(info.title);
        if (placeholder_->getFullText() != placeholder)
            placeholder_->setText(placeholder);
        if (sub_->getFullText() != info.sub)
            sub_->setText(info.sub);
        if (!hasCard_ || painted_.subIsBadge != info.subIsBadge)
            sub_->setTextColor(info.subIsBadge ? theme::accent()
                                               : theme::textTertiary());
        const brls::Visibility markVisibility =
            info.selectionMode ? brls::Visibility::VISIBLE
                               : brls::Visibility::GONE;
        if (markBox_->getVisibility() != markVisibility)
            markBox_->setVisibility(markVisibility);
        const brls::Visibility favoriteVisibility =
            info.favorite ? brls::Visibility::VISIBLE : brls::Visibility::GONE;
        if (favoriteBox_->getVisibility() != favoriteVisibility)
            favoriteBox_->setVisibility(favoriteVisibility);
        const std::string mark =
            !info.selectable ? "-" : info.selected ? "x" : " ";
        if (mark_->getFullText() != mark)
            mark_->setText(mark);
        if (!hasCard_ || painted_.selectable != info.selectable)
            mark_->setTextColor(info.selectable ? theme::accent()
                                                : theme::textDisabled());
        const bool highlight = info.selectionMode && info.selected;
        const bool previousHighlight =
            hasCard_ && painted_.selectionMode && painted_.selected;
        if (!hasCard_ || previousHighlight != highlight) {
            setBorderThickness(highlight ? 4 : 0);
            setBorderColor(highlight ? theme::accent() : brls::TRANSPARENT);
        }
        if (!hasCard_ ||
            painted_.iconPreserveAspect != info.iconPreserveAspect) {
            iconPreserveAspect_ = info.iconPreserveAspect;
            applyZoom(false);
            image_->setScalingType(info.iconPreserveAspect
                ? brls::ImageScalingType::FIT
                : brls::ImageScalingType::FILL);
        }
        if (currentIconUrl_ != info.iconUrl ||
            (!info.iconUrl.empty() &&
             (artworkService_ != service || !artworkCurrent))) {
            setArtworkUrl(image_, service, info.iconUrl, currentIconUrl_,
                          imageState_, GameMetadataService::kImageDimGrid);
        }
        painted_ = info;
        artworkService_ = service;
        hasCard_ = true;
    }

    void setSubLine(const std::string& text, bool badge) {
        if (hasCard_ && painted_.sub == text &&
            painted_.subIsBadge == badge)
            return;
        setTextIfChanged(sub_, text);
        if (!hasCard_ || painted_.subIsBadge != badge)
            sub_->setTextColor(badge ? theme::accent()
                                     : theme::textTertiary());
        painted_.sub = text;
        painted_.subIsBadge = badge;
    }

    // Unused trailing slot in a row/shelf: keeps its layout space so columns
    // stay aligned, but can neither draw nor take focus.
    void setEmpty() {
        if (getVisibility() != brls::Visibility::INVISIBLE)
            setVisibility(brls::Visibility::INVISIBLE);
        if (isFocusable())
            setFocusable(false);
        hasCard_ = false;
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
            : grid::kCoverSize;
        image_->setDimensions(baseWidth * zoom, grid::kCoverHeight * zoom);
        image_->setPositionTop(-(zoom - 1.0f) * grid::kCoverHeight / 2.0f);
        image_->setPositionLeft((grid::kCoverSize - baseWidth * zoom) / 2.0f);
    }

    brls::Box* cover_;
    brls::Label* placeholder_;
    AsyncRgbaImage* image_;
    brls::Box* markBox_;
    brls::Label* mark_;
    brls::Box* favoriteBox_;
    brls::Label* favorite_;
    brls::Label* name_;
    brls::Label* sub_;
    std::string currentIconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    int entryIndex_ = -1;
    std::string infoHash_;
    int shelfRow_ = -1;
    bool iconPreserveAspect_ = false;
    bool hasCard_ = false;
    GridCardInfo painted_;
    GameMetadataService* artworkService_ = nullptr;
    Activate onActivate_;
    Focused onFocus_;
};

// One recycled grid row holding kColumns cards. Left/right moves between the
// cards via the regular Box navigation; up/down lands on getDefaultFocus(),
// which preserves the column the user was in (shared focusColumn state).
class TopInsetCell : public brls::RecyclerCell {
public:
    TopInsetCell() {
        setFocusable(false);
        setHeight(grid::kTopInsetHeight);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
    }
};

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
        registerAction("", brls::BUTTON_A, [](brls::View*) { return false; },
                       /*hidden=*/true);
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


}  // namespace pipensx::ui
