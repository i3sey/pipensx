#pragma once

#include <atomic>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

#include <borealis.hpp>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/mod_index_service.hpp"
#include "app/update_service.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/advanced_settings.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class SettingsView : public brls::Box {
public:
    SettingsView(AppSettings* settings, DownloadManager* manager,
                 CatalogService* catalog, GameMetadataService* metadata,
                 InstalledTitleService* installed, UpdateService* updater = nullptr,
                 ModIndexService* mods = nullptr)
        : brls::Box(brls::Axis::COLUMN), settings_(settings), manager_(manager),
          catalog_(catalog), metadata_(metadata), installed_(installed), updater_(updater),
          mods_(mods), alive_(std::make_shared<std::atomic<bool>>(true)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        addSection(content, tr("pipensx/settings/section_general"));
        language_ = new brls::SelectorCell();
        language_->init(tr("pipensx/settings/language"),
            {tr("pipensx/settings/language_auto"),
             tr("pipensx/settings/language_en"),
             tr("pipensx/settings/language_ru")},
            languageIndex(settings_->get().language),
            [this](int selected) {
                AppSettingsData values = settings_->get();
                const std::string previous = values.language;
                values.language = kLanguageValues[selected];
                if (!persist(values, "language")) {
                    language_->setSelection(languageIndex(previous), true);
                    return;
                }
                // Borealis loads translations once, inside Application::init().
                brls::Application::notify(
                    tr("pipensx/settings/language_restart"));
            });
        content->addView(language_);

        checkForUpdates_ = new brls::BooleanCell();
        checkForUpdates_->init(tr("pipensx/settings/check_updates"),
            settings_->get().checkForUpdatesOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.checkForUpdatesOnLaunch;
                values.checkForUpdatesOnLaunch = enabled;
                if (!persist(values, "update_check"))
                    checkForUpdates_->setOn(previous, false);
            });
        content->addView(checkForUpdates_);
        updateAction_ = actionCell(tr("pipensx/settings/check_update_now"),
            tr("pipensx/settings/check_update_detail"),
            [this] { checkForUpdateNow(); });
        content->addView(updateAction_);

        addSection(content, tr("pipensx/settings/section_catalog"));
        catalogFilter_ = new brls::SelectorCell();
        catalogFilter_->init(tr("pipensx/settings/visible_releases"),
            {tr("pipensx/settings/filter_all"),
             tr("pipensx/settings/filter_games")},
            settings_->get().catalogFilter == CatalogFilter::Games ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                CatalogFilter previous = values.catalogFilter;
                values.catalogFilter = selected == 1
                    ? CatalogFilter::Games : CatalogFilter::All;
                if (!persist(values, "catalog_filter"))
                    catalogFilter_->setSelection(
                        previous == CatalogFilter::Games ? 1 : 0, true);
            });
        content->addView(catalogFilter_);

        refreshCatalog_ = new brls::BooleanCell();
        refreshCatalog_->init(tr("pipensx/settings/auto_refresh"),
            settings_->get().refreshCatalogOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.refreshCatalogOnLaunch;
                values.refreshCatalogOnLaunch = enabled;
                if (!persist(values, "catalog_refresh"))
                    refreshCatalog_->setOn(previous, false);
            });
        content->addView(refreshCatalog_);

        content->addView(actionCell(tr("pipensx/settings/update_now"),
            tr("pipensx/settings/update_now_detail"),
            [this] { updateAllNow(); }));

        addSection(content, tr("pipensx/settings/section_downloads"));
        streamSelection_ = new brls::SelectorCell();
        streamSelection_->init(tr("pipensx/settings/stream_selection"),
            {tr("pipensx/settings/stream_all"),
             tr("pipensx/settings/stream_packages")},
            settings_->get().streamSelection == StreamSelection::PackagesOnly
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                StreamSelection previous = values.streamSelection;
                values.streamSelection = selected == 1
                    ? StreamSelection::PackagesOnly
                    : StreamSelection::AllFiles;
                if (!persist(values, "stream_selection"))
                    streamSelection_->setSelection(
                        previous == StreamSelection::PackagesOnly ? 1 : 0,
                        true);
            });
        content->addView(streamSelection_);

        showCompleted_ = new brls::BooleanCell();
        showCompleted_->init(tr("pipensx/settings/show_completed"),
            settings_->get().showCompletedDownloads,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.showCompletedDownloads;
                values.showCompletedDownloads = enabled;
                if (!persist(values, "show_completed"))
                    showCompleted_->setOn(previous, false);
            });
        content->addView(showCompleted_);

        auto* advanced = actionCell(tr("pipensx/settings/advanced"),
            tr("pipensx/settings/advanced_detail"),
            [this] { openAdvanced(); });
        advanced->setMarginTop(18);
        content->addView(advanced);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

    ~SettingsView() override {
        alive_->store(false);
    }

private:
    // Settings-selector row for a stored language value; falls back to the
    // "auto" row so a value from a newer build cannot leave the cell blank.
    static int languageIndex(const std::string& value) {
        for (size_t i = 0; i < std::size(kLanguageValues); ++i) {
            if (value == kLanguageValues[i])
                return static_cast<int>(i);
        }
        return 0;
    }

    void openAdvanced() {
        auto alive = alive_;
        brls::Application::pushActivity(new AdvancedSettingsActivity(
            settings_, manager_, catalog_, metadata_, installed_,
            [this, alive] {
                if (alive->load())
                    applyValues();
            }));
    }

    bool persist(const AppSettingsData& values, const char* tag) {
        std::string error;
        if (settings_->update(values, error))
            return true;
        diagnostic_error("settings", tag, "error=%s", error.c_str());
        brls::Application::notify(error);
        return false;
    }

    void recordRefreshTime(bool catalog, bool metadata, bool mods = false) {
        AppSettingsData values = settings_->get();
        const uint64_t now = now_ms();
        if (catalog)
            values.lastCatalogRefreshMs = now;
        if (metadata)
            values.lastMetadataRefreshMs = now;
        if (mods)
            values.lastModsRefreshMs = now;
        persist(values, catalog ? "catalog_refresh_time"
                                : mods ? "mods_refresh_time"
                                       : "metadata_refresh_time");
    }

    // The manual "Update now" action chains all three sources; each refresh
    // takes an onDone continuation the chain uses to start the next once the
    // previous has cleared refreshInFlight_. A failure stops the chain.
    void updateAllNow() {
        if (refreshInFlight_)
            return;
        refreshCatalogNow([this] {
            refreshMetadataNow([this] { refreshModsNow(); });
        });
    }

    void refreshCatalogNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_catalog"));
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        brls::async([this, alive, catalog, onDone = std::move(onDone)]()
                        mutable {
            std::vector<CatalogEntry> entries;
            std::string error;
            bool ok = catalog->fetchLatest(entries, error);
            brls::sync([this, alive, ok, entries = std::move(entries),
                        error = std::move(error),
                        onDone = std::move(onDone)]() mutable {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("catalog", "settings_refresh", "error=%s",
                                     error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                catalog_->adopt(std::move(entries));
                recordRefreshTime(true, false);
                brls::Application::notify(
                    tr("pipensx/catalog/updated_catalog",
                       catalog_->entries().size()));
                if (onDone)
                    onDone();
            });
        });
    }

    void refreshMetadataNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_ || !metadata_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_artwork"));
        auto alive = alive_;
        GameMetadataService* metadata = metadata_;
        brls::async([this, alive, metadata, onDone = std::move(onDone)]()
                        mutable {
            MetadataSnapshot snapshot;
            std::string error;
            bool ok = metadata->fetchLatest(snapshot, error);
            brls::sync([this, alive, ok, snapshot = std::move(snapshot),
                        error = std::move(error),
                        onDone = std::move(onDone)]() mutable {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("metadata", "settings_refresh",
                                     "error=%s", error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                metadata_->adopt(std::move(snapshot));
                metadata_->dropMemoryImageCache();
                recordRefreshTime(false, true);
                brls::Application::notify(
                    tr("pipensx/catalog/updated_artwork", metadata_->size()));
                if (onDone)
                    onDone();
            });
        });
    }

    void refreshModsNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_ || !mods_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/settings/updating_mods"));
        auto alive = alive_;
        ModIndexService* mods = mods_;
        brls::async([this, alive, mods, onDone = std::move(onDone)]() mutable {
            ModIndexSnapshot snapshot;
            std::string error;
            bool ok = mods->fetchLatest(snapshot, error);
            brls::sync([this, alive, ok, snapshot = std::move(snapshot),
                        error = std::move(error),
                        onDone = std::move(onDone)]() mutable {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("mods", "settings_refresh", "error=%s",
                                     error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                mods_->adopt(std::move(snapshot));
                recordRefreshTime(false, false, true);
                brls::Application::notify(
                    tr("pipensx/settings/updated_mods", mods_->size()));
                if (onDone)
                    onDone();
            });
        });
    }

    void checkForUpdateNow() {
        if (updateInFlight_ || !updater_)
            return;
        updateInFlight_ = true;
        updateAction_->setDetailText(tr("pipensx/settings/checking"));
        auto alive = alive_;
        UpdateService* updater = updater_;
        updater->checkAsync([this, alive](UpdateCheckResult result) {
            brls::sync([this, alive, result = std::move(result)]() mutable {
                if (!alive->load())
                    return;
                updateInFlight_ = false;
                if (!result.ok) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/check_failed"));
                    diagnostic_error("update", "check", "error=%s",
                                     result.error.c_str());
                    brls::Application::notify(result.error);
                    return;
                }
                if (!result.updateAvailable) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/up_to_date"));
                    brls::Application::notify(
                        tr("pipensx/settings/up_to_date_notify"));
                    return;
                }
                updateAction_->setDetailText(
                    tr("pipensx/settings/version_detail", result.release.version));
                confirmInstallUpdate(std::move(result.release));
            });
        });
    }

    void confirmInstallUpdate(ReleaseInfo release) {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/update_available", release.version));
        dialog->addButton(tr("pipensx/settings/install_and_restart"),
                          [this, release = std::move(release)] {
            installUpdate(release);
        });
        dialog->addButton(tr("pipensx/common/later"), [] {});
        dialog->open();
    }

    void installUpdate(const ReleaseInfo& release) {
        if (updateInFlight_ || !updater_)
            return;
        updateInFlight_ = true;
        updateAction_->setDetailText(tr("pipensx/settings/downloading"));
        auto alive = alive_;
        UpdateService* updater = updater_;
        auto lastPercent = std::make_shared<std::atomic<int>>(-1);
        updater->onInstallProgress(
            [this, alive, lastPercent](uint64_t received, uint64_t total) {
                const int percent = static_cast<int>((received * 100) / total);
                if (lastPercent->exchange(percent) == percent)
                    return;
                brls::sync([this, alive, percent] {
                    if (!alive->load())
                        return;
                    updateAction_->setDetailText(
                        tr("pipensx/settings/downloading_percent", percent));
                });
            });
        updater->installAsync(release, [this, alive](bool installed,
                                                       std::string error) {
            brls::sync([this, alive, installed, error = std::move(error)] {
                if (!alive->load())
                    return;
                updateInFlight_ = false;
                if (!installed) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/install_failed"));
                    diagnostic_error("update", "install", "error=%s",
                                     error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                updateAction_->setDetailText(tr("pipensx/settings/restart_required"));
#ifdef __SWITCH__
                if (!envHasNextLoad()) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_no_restart"));
                    return;
                }
                const std::string helper = updater_->helperPath();
                const std::string arguments =
                    "\"" + helper + "\" --finish-update";
                const Result result = envSetNextLoad(helper.c_str(),
                                                     arguments.c_str());
                if (R_FAILED(result)) {
                    diagnostic_error("update", "restart", "result=0x%08x",
                                     result);
                    brls::Application::notify(
                        tr("pipensx/settings/update_restart_failed"));
                    return;
                }
#endif
                // The helper swaps the NRO after we exit, then drops to HOME
                // instead of relaunching (an in-session relaunch of the full
                // app crashes). Gate the quit behind an acknowledged dialog so
                // the close reads as intentional rather than a crash.
                auto* dialog = new brls::Dialog(
                    tr("pipensx/settings/update_close_body"));
                dialog->setCancelable(false);
                dialog->addButton(tr("pipensx/settings/update_close_button"),
                                  [] { brls::Application::quit(); });
                dialog->open();
            });
        });
    }

    // Re-sync the main-list cells. Called after a factory reset performed on the
    // Advanced sub-page (via its onReset callback), since the nested settings tab
    // does not get a lifecycle event when that activity pops.
    void applyValues() {
        const AppSettingsData& values = settings_->get();
        language_->setSelection(languageIndex(values.language), true);
        catalogFilter_->setSelection(
            values.catalogFilter == CatalogFilter::Games ? 1 : 0, true);
        refreshCatalog_->setOn(values.refreshCatalogOnLaunch, false);
        streamSelection_->setSelection(
            values.streamSelection == StreamSelection::PackagesOnly ? 1 : 0,
            true);
        showCompleted_->setOn(values.showCompletedDownloads, false);
        checkForUpdates_->setOn(values.checkForUpdatesOnLaunch, false);
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    UpdateService* updater_;
    ModIndexService* mods_;
    std::shared_ptr<std::atomic<bool>> alive_;
    brls::SelectorCell* language_ = nullptr;
    brls::SelectorCell* catalogFilter_ = nullptr;
    brls::BooleanCell* refreshCatalog_ = nullptr;
    brls::BooleanCell* checkForUpdates_ = nullptr;
    brls::DetailCell* updateAction_ = nullptr;
    brls::SelectorCell* streamSelection_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
    bool refreshInFlight_ = false;
    bool updateInFlight_ = false;
};

}  // namespace pipensx::ui
