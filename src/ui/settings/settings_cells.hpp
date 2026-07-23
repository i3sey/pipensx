#pragma once

#include <functional>
#include <string>

#include <borealis.hpp>

namespace pipensx::ui {

// Shared row builders for the settings screen and its Advanced sub-page, so the
// two lists render identical section headers and action rows.

inline void addSection(brls::Box* content, const std::string& text) {
    auto* title = new brls::Label();
    title->setText(text);
    title->setFontSize(25);
    title->setMarginTop(14);
    title->setMarginBottom(8);
    content->addView(title);
}

inline brls::DetailCell* actionCell(const std::string& title,
                                    const std::string& detail,
                                    std::function<void()> callback) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->registerClickAction([callback = std::move(callback)](brls::View*) {
        callback();
        return true;
    });
    return cell;
}

}  // namespace pipensx::ui
