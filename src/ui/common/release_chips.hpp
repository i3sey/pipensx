#pragma once

#include <borealis.hpp>
#include <yoga/Yoga.h>

#include "ui/common/release_status.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class StatusChip : public brls::Box {
public:
    explicit StatusChip(const std::string& text,
                        ChipTone tone = ChipTone::Neutral,
                        bool compact = false)
        : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(compact ? 2 : 8, compact ? 4 : 12,
                   compact ? 2 : 8, compact ? 4 : 12);
        setMarginRight(compact ? 4 : 8);
        setMarginBottom(compact ? 4 : 8);
        setMaxWidthPercentage(100);
        setShrink(0);
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusSmall);
        auto* label = new brls::Label();
        label->setFontSize(theme::kFontCaption);
        label->setSingleLine(compact);
        label->setAutoAnimate(false);
        label->setShrink(1);
        label->setMinWidth(0);
        label->setText(text);
        NVGcolor color = theme::textPrimary();
        if (tone == ChipTone::Success) color = theme::color(theme::Tok::ChipSuccess);
        if (tone == ChipTone::Warning) color = theme::color(theme::Tok::ChipWarning);
        if (tone == ChipTone::Error) color = theme::color(theme::Tok::ChipError);
        if (tone != ChipTone::Neutral) {
            auto* mark = new brls::Box();
            mark->setWidth(8);
            mark->setHeight(8);
            mark->setShrink(0);
            mark->setCornerRadius(4);
            mark->setMarginRight(8);
            mark->setBackgroundColor(color);
            addView(mark);
        }
        label->setTextColor(color);
        addView(label);
    }
};

class ReleaseChips : public brls::Box {
public:
    ReleaseChips(const CatalogEntry& entry, uint64_t size,
                 const std::string& format)
        : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setShrink(0);
        setPaddingRight(8);
        setMarginBottom(8);
        YGNodeStyleSetFlexWrap(getYGNode(), YGWrapWrap);
        const auto status = releaseStatus(entry);
        addView(new StatusChip(tr(status.key), status.tone));
        addView(new StatusChip(entry.hasPeerCount || entry.peerCount > 0
            ? tr("pipensx/release_chips/peers", entry.peerCount)
            : tr("pipensx/release_chips/peers_unknown")));
        if (size)
            addView(new StatusChip(formatBytes(size)));
        if (!format.empty())
            addView(new StatusChip(format));
    }
};

} // namespace pipensx::ui
