#pragma once

#include <ctime>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/catalog_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// RuTracker catalog tab
// ---------------------------------------------------------------------------

inline std::string formatCatalogDate(int64_t timestamp) {
    if (timestamp <= 0)
        return "Unknown date";
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm result{};
    localtime_r(&value, &result);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &result);
    return buffer;
}

class CatalogCell : public brls::RecyclerCell {
public:
    CatalogCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(10, 18, 10, 18);
        setHeight(96);

        mark_ = new brls::Label();
        mark_->setWidth(0);
        mark_->setFontSize(21);
        mark_->setTextColor(theme::accent());
        addView(mark_);

        // Left: rounded box-art thumbnail. The box's background color is the
        // placeholder shown until art loads (or for entries without metadata).
        thumb_ = new brls::Box();
        thumb_->setWidth(64);
        thumb_->setHeight(64);
        thumb_->setCornerRadius(6);
        thumb_->setBackgroundColor(theme::surface());
        thumb_->setMarginRight(16);
        thumb_->setAlignItems(brls::AlignItems::CENTER);
        thumb_->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(28);
        placeholder_->setTextColor(theme::textSecondary());
        thumb_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(64);
        image_->setHeight(64);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(6);
        image_->setScalingType(brls::ImageScalingType::FILL);
        thumb_->addView(image_);
        addView(thumb_);

        // Right: title + state on top, content classification underneath.
        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setGrow(1);
        right->setJustifyContent(brls::JustifyContent::CENTER);

        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(21);
        title_->setGrow(1);
        badge_ = new brls::Label();
        badge_->setSingleLine(true);
        badge_->setFontSize(16);
        badge_->setMarginLeft(12);
        badge_->setTextColor(theme::accent());
        top->addView(title_);
        top->addView(badge_);

        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setFontSize(16);
        sub_->setMarginTop(6);
        sub_->setTextColor(theme::textTertiary());

        right->addView(top);
        right->addView(sub_);
        addView(right);
    }

    void setEntry(const CatalogEntry& entry, const std::string& stateBadge,
                  const std::string& iconUrl, GameMetadataService* service,
                  bool selectionMode, bool selected, bool selectable) {
        mark_->setWidth(selectionMode ? 42 : 0);
        mark_->setMarginRight(selectionMode ? 8 : 0);
        mark_->setText(!selectionMode ? "" : !selectable ? "[-]"
                                             : selected ? "[x]" : "[ ]");
        mark_->setTextColor(selectable ? theme::accent()
                                       : theme::textDisabled());
        title_->setText(entry.title);
        placeholder_->setText(placeholderLetter(entry.title));
        badge_->setText(stateBadge);
        std::string sub = entry.size ? formatBytes(entry.size) : "Unknown size";
        sub += "   " + formatCatalogDate(entry.publishedAt);
        sub_->setText(sub);
        setArtworkUrl(image_, service, iconUrl, currentIconUrl_, imageState_);
    }

private:
    brls::Label* mark_;
    brls::Box* thumb_;
    brls::Label* placeholder_;
    AsyncRgbaImage* image_;
    brls::Label* title_;
    brls::Label* badge_;
    brls::Label* sub_;
    std::string currentIconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
};

}  // namespace pipensx::ui
