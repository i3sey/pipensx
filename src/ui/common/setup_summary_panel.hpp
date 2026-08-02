#pragma once

#include <string>

#include <borealis.hpp>

#include "app/bug_report.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

struct SetupSummaryFixture {
    std::string lanAddress;
    std::string diagnosticTail;
};

inline std::string setupDiagnosticText(const DiagnosticSummary& summary) {
    if (summary.level.empty())
        return tr("pipensx/setup_summary/no_diagnostics");
    return tr("pipensx/setup_summary/diagnostics",
              static_cast<int>(summary.errorCount), summary.stage, summary.tag,
              summary.level);
}

inline NVGcolor setupDiagnosticColor(const DiagnosticSummary& summary) {
    if (summary.level.empty())
        return theme::textSecondary();
    return summary.errorCount ? theme::error() : theme::success();
}

class SetupSummaryPanel : public brls::Box {
public:
    SetupSummaryPanel() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(false);
        setBackgroundColor(theme::panel());
        setBorderColor(theme::track());
        setBorderThickness(1.0f);
        setCornerRadius(theme::kRadiusLarge);
        setPadding(24, 24, 24, 24);

        connection_ = addSection(tr("pipensx/setup_summary/connection"));
        addSeparator();
        check_ = addSection(tr("pipensx/setup_summary/check"));
        addSeparator();
        selected_ = addSection(tr("pipensx/setup_summary/selected"));
        addSeparator();
        diagnostics_ = addSection(tr("pipensx/setup_summary/last_diagnostic"));
    }

    void setConnection(const std::string& text, NVGcolor color) {
        setSection(connection_, text, color);
    }

    void setCheck(const std::string& text, NVGcolor color) {
        setSection(check_, text, color);
    }

    void setSelected(const std::string& text, NVGcolor color) {
        setSection(selected_, text, color);
    }

    void setDiagnostics(const std::string& text, NVGcolor color) {
        setSection(diagnostics_, text, color);
    }

    std::string renderedState() const {
        return connection_->getFullText() + "|" + check_->getFullText() +
               "|" + selected_->getFullText() + "|" +
               diagnostics_->getFullText();
    }

    bool contentFits() {
        const brls::Rect panel = getFrame();
        for (brls::View* child : getChildren()) {
            const brls::Rect frame = child->getFrame();
            if (frame.getMaxX() > panel.getMaxX() ||
                frame.getMaxY() > panel.getMaxY())
                return false;
        }
        return true;
    }

private:
    brls::Label* addSection(const std::string& title) {
        auto* heading = new brls::Label();
        heading->setText(title);
        heading->setFontSize(theme::kFontCaption);
        heading->setTextColor(theme::textTertiary());
        heading->setMarginBottom(8);
        addView(heading);

        auto* value = new brls::Label();
        value->setFontSize(theme::kFontSmall);
        value->setTextColor(theme::textSecondary());
        value->setSingleLine(false);
        addView(value);
        return value;
    }

    void addSeparator() {
        auto* separator = new brls::Box(brls::Axis::ROW);
        separator->setHeight(1);
        separator->setBackgroundColor(theme::track());
        separator->setMargins(16, 0, 16, 0);
        addView(separator);
    }

    static void setSection(brls::Label* label, const std::string& text,
                           NVGcolor color) {
        label->setText(text);
        label->setTextColor(color);
    }

    brls::Label* connection_ = nullptr;
    brls::Label* check_ = nullptr;
    brls::Label* selected_ = nullptr;
    brls::Label* diagnostics_ = nullptr;
};

}  // namespace pipensx::ui
