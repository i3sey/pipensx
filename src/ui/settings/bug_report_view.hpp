#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <borealis.hpp>

#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "ui/common/ui_helpers.hpp"

namespace pipensx::ui {

// "Report a bug" screen. Snapshots device state into the log, reads the recent
// log tail, and renders it as a single screenful of QR codes so a reporter can
// send the whole thing in one photo. Y toggles a denser "detailed" layout that
// carries more log (for a clean screenshot); B returns. The dev decodes the
// photo with scripts/decode_report.py — no log file ever leaves the console.
// The golden-screenshot seam: a fixed log and device state make the screen
// deterministic, which the live path is not (it touches statvfs, firmware and
// the clock). Production passes nothing.
struct BugReportFixture {
    std::string log;
    SystemSnapshot snapshot;
};

class BugReportActivity : public brls::Activity {
  public:
    BugReportActivity(DownloadManager* manager, CatalogService* catalog,
                      GameMetadataService* metadata,
                      InstalledTitleService* installed,
                      std::optional<BugReportFixture> fixture = std::nullopt,
                      bool startDetailed = false);

    ~BugReportActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    // Read the device state and the log once, on entry.
    void captureReport();
    // Re-encode the captured log for the current mode and grid size. Cheap
    // enough to redo on a Y press or a relayout; touches no I/O.
    void renderReport();

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    std::optional<BugReportFixture> fixture_;
    bool detailed_ = false;

    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::string tail_;
    std::uint16_t sessionId_ = 0;
    float gridWidth_ = 0.0f;
    float gridHeight_ = 0.0f;

    brls::AppletFrame* frame_ = nullptr;
    brls::Box* gridHost_ = nullptr;
    brls::Label* summary_ = nullptr;
    brls::Label* caption_ = nullptr;
    brls::Label* hint_ = nullptr;
};

}  // namespace pipensx::ui
