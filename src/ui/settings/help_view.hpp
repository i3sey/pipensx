#pragma once

#include <borealis.hpp>

#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/bug_report_view.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

namespace {

constexpr float kHelpGap = theme::kSpacingUnit * 2.0f;

inline brls::Box* helpCard(const std::string& title,
                           const std::string& body) {
    auto* card = new brls::Box(brls::Axis::COLUMN);
    card->setGrow(1);
    card->setMinWidth(0);
    card->setMinHeight(112);
    card->setPadding(18, 22, 18, 22);
    card->setBackgroundColor(theme::panel());
    card->setBorderColor(theme::track());
    card->setBorderThickness(1.0f);
    card->setCornerRadius(theme::kRadiusLarge);

    auto* heading = new brls::Label();
    heading->setText(title);
    heading->setSingleLine(false);
    heading->setFontSize(theme::kFontBody);
    heading->setTextColor(theme::accent());
    heading->setMarginBottom(theme::kSpacingUnit);
    card->addView(heading);

    auto* description = new brls::Label();
    description->setText(body);
    description->setSingleLine(false);
    description->setFontSize(theme::kFontSmall);
    description->setTextColor(theme::textSecondary());
    card->addView(description);
    return card;
}

} // namespace

class HelpView : public brls::Box {
public:
    HelpView(DownloadManager* manager, CatalogService* catalog,
             GameMetadataService* metadata, InstalledTitleService* installed)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed) {
        auto* scroller = new brls::ScrollingFrame();
        scroller->setGrow(1);
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(20, 34, 20, 34);
        const char* rows[][2] = {
            {"pipensx/help/home_title", "pipensx/help/home_body"},
            {"pipensx/help/tabs_title", "pipensx/help/tabs_body"},
            {"pipensx/help/grid_title", "pipensx/help/grid_body"},
            {"pipensx/help/search_title", "pipensx/help/search_body"},
            {"pipensx/help/web_title", "pipensx/help/web_body"},
            {"pipensx/help/exit_title", "pipensx/help/exit_body"},
        };
        for (size_t i = 0; i < 6; i += 2) {
            auto* cardRow = new brls::Box(brls::Axis::ROW);
            cardRow->setAlignItems(brls::AlignItems::STRETCH);
            if (i != 0)
                cardRow->setMarginTop(kHelpGap);

            cardRow->addView(helpCard(tr(rows[i][0]), tr(rows[i][1])));
            auto* right = helpCard(tr(rows[i + 1][0]), tr(rows[i + 1][1]));
            right->setMarginLeft(kHelpGap);
            cardRow->addView(right);
            content->addView(cardRow);
        }
        scroller->setContentView(content);
        addView(scroller);

        // Keep the support action outside the scrolling content. It must stay
        // reachable even when a translation makes the help cards taller.
        auto* footer = new brls::Box(brls::Axis::ROW);
        footer->setShrink(0);
        footer->setPadding(12, 34, 12, 34);
        footer->setBackgroundColor(theme::sidebar());
        footer->setLineColor(theme::track());
        footer->setLineTop(1.0f);

        auto* report = new brls::Button();
        report->setGrow(1);
        report->setHeight(52);
        report->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        report->setText(tr("pipensx/settings/report_bug"));
        report->registerClickAction([this](brls::View*) {
            brls::Application::pushActivity(new BugReportActivity(
                manager_, catalog_, metadata_, installed_));
            return true;
        });
        footer->addView(report);
        addView(footer);
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
};

}  // namespace pipensx::ui
