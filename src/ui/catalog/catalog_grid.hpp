#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/release_chips.hpp"
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
inline constexpr float kCoverInset = 0.0f;
inline constexpr float kCoverWidth = kCardWidth;
// Cover box fits vertical Switch/retro box art cleanly while leaving room for
// a two-line title and metadata row inside kCardHeight.
inline constexpr float kCoverHeight = 274.0f;
inline constexpr float kCoverSize = kCoverWidth;
inline float iconRadius() { return theme::kRadiusMedium; }
// Cover (274) + gap (8) + 2-line title (38) + gap (2) + metadata (18) + pad (8).
inline constexpr float kCardHeight = 348.0f;
inline constexpr float kRowHeight = 364.0f;
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
    std::string peers;
    std::vector<std::string> diffChips;
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
           left.sub == right.sub && left.peers == right.peers &&
           left.subIsBadge == right.subIsBadge &&
           left.diffChips == right.diffChips &&
           left.iconUrl == right.iconUrl &&
           left.iconPreserveAspect == right.iconPreserveAspect &&
           left.selectionMode == right.selectionMode &&
           left.selected == right.selected &&
           left.selectable == right.selectable &&
           left.favorite == right.favorite;
}

class TwoLineTitle : public brls::View {
public:
    TwoLineTitle() {
        setFocusable(false);
        setHeight(38.0f);
    }

    void setText(const std::string& text) {
        if (text_ == text)
            return;
        text_ = text;
        invalidate();
    }

    const std::string& getFullText() const { return text_; }

    void setFontSize(float size) { fontSize_ = size; }
    void setTextColor(NVGcolor color) { textColor_ = color; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (text_.empty() || width <= 4.0f)
            return;
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);
        nvgFontSize(vg, fontSize_);
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgTextLineHeight(vg, kLineHeight);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, a(textColor_));

        NVGtextRow rows[3];
        const int nrows = nvgTextBreakLines(
            vg, text_.c_str(), nullptr, width, rows, 3);
        const float lineStep = fontSize_ * kLineHeight;
        if (nrows >= 1)
            nvgText(vg, x, y, rows[0].start, rows[0].end);
        if (nrows >= 2) {
            const float y2 = y + lineStep;
            const char* tail = rows[1].end;
            while (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r')
                ++tail;
            const bool truncated = (nrows > 2) || (*tail != '\0');
            if (!truncated) {
                nvgText(vg, x, y2, rows[1].start, rows[1].end);
            } else {
                std::string line(rows[1].start, rows[1].end);
                while (!line.empty() &&
                       (line.back() == ' ' || line.back() == '\t' ||
                        line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                static constexpr const char* kEllipsis = "\xE2\x80\xA6";
                float bounds[4] = {};
                nvgTextBounds(vg, 0, 0, (line + kEllipsis).c_str(), nullptr, bounds);
                while (!line.empty() && (bounds[2] - bounds[0]) > width) {
                    do {
                        line.pop_back();
                    } while (!line.empty() &&
                             (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80);
                    while (!line.empty() && line.back() == ' ')
                        line.pop_back();
                    nvgTextBounds(vg, 0, 0, (line + kEllipsis).c_str(), nullptr, bounds);
                }
                line += kEllipsis;
                nvgText(vg, x, y2, line.c_str(), nullptr);
            }
        }
        nvgRestore(vg);
    }

private:
    static constexpr float kLineHeight = 1.15f;
    std::string text_;
    float fontSize_ = 16.0f;
    NVGcolor textColor_ = theme::textPrimary();
};

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
        setHighlightCornerRadius(theme::kRadiusMedium);
        setClipsToBounds(true);

        // Cover box sits flush with the card edges and stays transparent so
        // only the single card substrate is visible behind any aspect ratio.
        cover_ = new brls::Box();
        cover_->setWidth(grid::kCoverWidth);
        cover_->setHeight(grid::kCoverHeight);
        cover_->setCornerRadius(grid::iconRadius());
        cover_->setBackgroundColor(brls::TRANSPARENT);
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
        image_->setWidth(grid::kCoverWidth);
        image_->setHeight(grid::kCoverHeight);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(grid::iconRadius());
        image_->setScalingType(brls::ImageScalingType::FIT);
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

        favoriteBox_ = new brls::Box();
        favoriteBox_->setPositionType(brls::PositionType::ABSOLUTE);
        favoriteBox_->setPositionTop(theme::kSpacingUnit);
        favoriteBox_->setPositionRight(theme::kSpacingUnit);
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

        diffChips_ = new brls::Box(brls::Axis::ROW);
        diffChips_->setPositionType(brls::PositionType::ABSOLUTE);
        diffChips_->setPositionBottom(0);
        diffChips_->setPositionLeft(0);
        diffChips_->setWidth(grid::kCoverWidth);
        diffChips_->setPadding(4, 0, 0, 4);
        diffChips_->setBackgroundColor(theme::overlay());
        YGNodeStyleSetFlexWrap(diffChips_->getYGNode(), YGWrapWrap);
        diffChips_->setVisibility(brls::Visibility::GONE);
        cover_->addView(diffChips_);

        addView(cover_);

        name_ = new TwoLineTitle();
        name_->setFontSize(16.0f);
        name_->setTextColor(theme::textPrimary());
        name_->setMarginTop(8);
        name_->setMarginLeft(10);
        name_->setMarginRight(10);
        addView(name_);

        auto* metadata = new brls::Box(brls::Axis::ROW);
        metadata->setMarginTop(2);
        metadata->setMarginLeft(10);
        metadata->setMarginRight(10);
        metadata->setAlignItems(brls::AlignItems::CENTER);
        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setAutoAnimate(false);
        sub_->setFontSize(theme::kFontCaption);
        sub_->setGrow(1);
        sub_->setShrink(1);
        sub_->setMinWidth(0);
        sub_->setMarginBottom(0);
        sub_->setTextColor(theme::textSecondary());
        metadata->addView(sub_);
        peers_ = new brls::Label();
        peers_->setSingleLine(true);
        peers_->setAutoAnimate(false);
        peers_->setFontSize(theme::kFontCaption);
        peers_->setTextColor(theme::textSecondary());
        peers_->setShrink(0);
        peers_->setMarginLeft(8);
        peers_->setVisibility(brls::Visibility::GONE);
        metadata->addView(peers_);
        addView(metadata);

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
        setTextIfChanged(peers_, info.peers);
        peers_->setVisibility(info.peers.empty()
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        if (!hasCard_ || painted_.diffChips != info.diffChips) {
            diffChips_->clearViews();
            for (const auto& text : info.diffChips) {
                auto* chip = new StatusChip(text, ChipTone::Neutral, true);
                chip->setMaxWidth(grid::kCoverWidth - 12);
                diffChips_->addView(chip);
            }
            diffChips_->setVisibility(info.diffChips.empty()
                ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        }
        if (!hasCard_ || painted_.subIsBadge != info.subIsBadge)
            sub_->setTextColor(info.subIsBadge ? theme::accent()
                                               : theme::textSecondary());
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
                ? brls::ImageScalingType::FILL
                : brls::ImageScalingType::FIT);
        }
        if (currentIconUrl_ != info.iconUrl ||
            (!info.iconUrl.empty() &&
             (artworkService_ != service || !artworkCurrent))) {
            setArtworkUrl(image_, service, info.iconUrl, currentIconUrl_,
                          imageState_, GameMetadataService::kImageDimCard);
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
                                     : theme::textSecondary());
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
        if (onFocus_)
            onFocus_();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (theme::isCustom())
            theme::drawPlate(vg, x, y, width, height, theme::kRadiusMedium, isFocused());
        brls::Box::draw(vg, x, y, width, height, style, ctx);
        if (isTerminalTheme(theme::activeTheme)) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x + 1, y + grid::kCoverInset + grid::kCoverHeight + 2);
            nvgLineTo(vg, x + width - 1, y + grid::kCoverInset + grid::kCoverHeight + 2);
            nvgStrokeWidth(vg, 1);
            nvgStrokeColor(vg, theme::meterBorder());
            nvgStroke(vg);
        }
        if (isTerminalTheme(theme::activeTheme) ||
            (theme::activeTheme == AppTheme::LiquidGlass && isFocused()))
            theme::drawControlFinish(vg, x, y, width, height, theme::kRadiusMedium, isFocused());
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        applyZoom(false);
    }

    int entryIndex() const { return entryIndex_; }
    const std::string& infoHash() const { return infoHash_; }
    // >= 0 when the card lives in a shelf (value = recycler row of the shelf).
    int shelfRow() const { return shelfRow_; }

private:
    // UI_PLAN F5: subtle cover zoom while the card is focused. The cover box
    // clips, so the art scales in place without shifting the layout.
    void applyZoom(bool focused) {
        const float zoom = focused && !isTerminalTheme(theme::activeTheme) ? grid::kFocusZoom : 1.0f;
        const float width = grid::kCoverWidth * zoom;
        const float height = grid::kCoverHeight * zoom;
        image_->setDimensions(width, height);
        image_->setPositionTop(-(zoom - 1.0f) * grid::kCoverHeight / 2.0f);
        image_->setPositionLeft(-(zoom - 1.0f) * grid::kCoverWidth / 2.0f);
    }

    brls::Box* cover_;
    brls::Label* placeholder_;
    AsyncRgbaImage* image_;
    brls::Box* markBox_;
    brls::Label* mark_;
    brls::Box* favoriteBox_;
    brls::Label* favorite_;
    TwoLineTitle* name_;
    brls::Label* sub_;
    brls::Label* peers_;
    brls::Box* diffChips_;
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
