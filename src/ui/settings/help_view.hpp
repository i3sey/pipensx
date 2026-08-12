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
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);
        content->addView(actionCell(tr("pipensx/settings/report_bug"), "",
            [this] {
                brls::Application::pushActivity(new BugReportActivity(
                    manager_, catalog_, metadata_, installed_));
            }));
        addView(content);
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
};

}  // namespace pipensx::ui
