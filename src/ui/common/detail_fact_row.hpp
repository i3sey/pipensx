#pragma once

#include <borealis.hpp>
#include "ui/theme.hpp"

namespace pipensx::ui {

inline void detailFactRow(brls::Box* table, const std::string& name, const std::string& value) {
    if (value.empty()) return;
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setMarginTop(8);
    auto* key = new brls::Label();
    key->setFontSize(theme::kFontCaption);
    key->setTextColor(theme::textSecondary());
    key->setWidth(205);
    key->setMarginRight(20);
    key->setSingleLine(false);
    key->setShrink(0);
    key->setText(name);
    row->addView(key);
    auto* val = new brls::Label();
    val->setFontSize(theme::kFontCaption);
    val->setTextColor(theme::textSecondary());
    val->setGrow(1);
    val->setShrink(1);
    val->setMinWidth(0);
    val->setSingleLine(false);
    val->setText(value);
    row->addView(val);
    table->addView(row);
}

} // namespace pipensx::ui
