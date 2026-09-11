#pragma once

#include <borealis.hpp>

#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/bug_report_view.hpp"
#include "ui/settings/settings_cells.hpp"

namespace pipensx::ui {

class HelpView : public brls::Box {
public:
    HelpView(DownloadManager* manager, CatalogService* catalog,
             GameMetadataService* metadata, InstalledTitleService* installed)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed) {
        auto* scroller = new brls::ScrollingFrame();
        scroller->setGrow(1);
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);
        const char* rows[][2] = {
            {"pipensx/help/home_title", "pipensx/help/home_body"},
            {"pipensx/help/tabs_title", "pipensx/help/tabs_body"},
            {"pipensx/help/grid_title", "pipensx/help/grid_body"},
            {"pipensx/help/search_title", "pipensx/help/search_body"},
            {"pipensx/help/web_title", "pipensx/help/web_body"},
            {"pipensx/help/exit_title", "pipensx/help/exit_body"},
        };
        for (const auto& row : rows) {
            auto* cell = new brls::DetailCell();
            cell->setText(tr(row[0]));
            cell->setDetailText(tr(row[1]));
            content->addView(cell);
        }
        content->addView(actionCell(tr("pipensx/settings/report_bug"), "",
            [this] {
                brls::Application::pushActivity(new BugReportActivity(
                    manager_, catalog_, metadata_, installed_));
            }));
        scroller->setContentView(content);
        addView(scroller);
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
};

}  // namespace pipensx::ui
