#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <borealis.hpp>

#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"

namespace pipensx::ui {

// "Report a bug" screen. Snapshots device state into the log, reads the recent
// log tail, and renders it as a single screenful of QR codes so a reporter can
// send the whole thing in one photo. Y toggles a denser "detailed" layout that
// carries more log (for a clean screenshot); B returns. The dev decodes the
// photo with scripts/decode_report.py — no log file ever leaves the console.
class BugReportActivity : public brls::Activity {
  public:
    // logOverride / startDetailed are the golden-screenshot seam: passing a
    // fixed log makes the screen deterministic (the live path touches statvfs,
    // firmware and timestamps). Production uses the defaults.
    BugReportActivity(DownloadManager* manager, CatalogService* catalog,
                      GameMetadataService* metadata,
                      InstalledTitleService* installed,
                      std::optional<std::string> logOverride = std::nullopt,
                      bool startDetailed = false);

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    void rebuild();

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    std::optional<std::string> logOverride_;
    bool detailed_ = false;

    brls::AppletFrame* frame_ = nullptr;
    brls::Box* gridHost_ = nullptr;
    brls::Label* caption_ = nullptr;
    brls::Label* hint_ = nullptr;
};

}  // namespace pipensx::ui
