#pragma once

#include <sys/statvfs.h>

#include <atomic>
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

        addSection(content, tr("pipensx/settings/section_language"));
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

        content->addView(actionCell(tr("pipensx/settings/update_catalog"),
            tr("pipensx/settings/update_catalog_detail"),
            [this] { refreshCatalogNow(); }));
        content->addView(actionCell(tr("pipensx/settings/update_artwork"),
            tr("pipensx/settings/update_artwork_detail"),
            [this] { refreshMetadataNow(); }));
        if (mods_) {
            content->addView(actionCell(tr("pipensx/settings/update_mods"),
                tr("pipensx/settings/update_mods_detail"),
                [this] { refreshModsNow(); }));
        }

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

        installLocation_ = new brls::SelectorCell();
        installLocation_->init(tr("pipensx/settings/install_location"),
            {tr("pipensx/settings/install_sd"),
             tr("pipensx/settings/install_nand")},
            settings_->get().installLocation == InstallLocation::SystemMemory
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                InstallLocation previous = values.installLocation;
                values.installLocation = selected == 1
                    ? InstallLocation::SystemMemory
                    : InstallLocation::SdCard;
                if (!persist(values, "install_location")) {
                    installLocation_->setSelection(
                        previous == InstallLocation::SystemMemory ? 1 : 0,
                        true);
                    return;
                }
                manager_->setInstallTarget(
                    installTargetFor(values.installLocation));
            });
        content->addView(installLocation_);

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

        addSection(content, tr("pipensx/settings/section_updates"));
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

        addSection(content, tr("pipensx/settings/section_diagnostics"));
        auto* description = new brls::Label();
        description->setText(tr("pipensx/settings/diagnostics_note"));
        description->setFontSize(16);
        description->setTextColor(theme::textSecondary());
        description->setMarginBottom(10);
        content->addView(description);

        extendedTelemetry_ = new brls::BooleanCell();
        extendedTelemetry_->init(tr("pipensx/settings/extended_telemetry"),
            settings_->get().extendedTelemetry,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.extendedTelemetry;
                values.extendedTelemetry = enabled;
                if (!persist(values, "extended_telemetry")) {
                    extendedTelemetry_->setOn(previous, false);
                    return;
                }
                telemetry_set_enabled(enabled ? 1 : 0);
                brls::Application::notify(enabled
                    ? tr("pipensx/settings/telemetry_on")
                    : tr("pipensx/settings/telemetry_off"));
            });
        content->addView(extendedTelemetry_);

        content->addView(actionCell(tr("pipensx/settings/capture_snapshot"),
            tr("pipensx/settings/capture_snapshot_detail"),
            [this] { captureSnapshot(); }));
        content->addView(actionCell(tr("pipensx/settings/clear_log"),
            tr("pipensx/settings/clear_log_detail"),
            [this] { confirmClearLog(); }));
        content->addView(actionCell(tr("pipensx/settings/clear_artwork"),
            tr("pipensx/settings/clear_artwork_detail"),
            [this] { confirmClearArtwork(); }));
        content->addView(actionCell(tr("pipensx/settings/reset"),
            tr("pipensx/settings/reset_detail"),
            [this] { confirmReset(); }));

        auto* path = new brls::Label();
        path->setText(tr("pipensx/settings/log_path", LogPath));
        path->setFontSize(15);
        path->setTextColor(theme::textTertiary());
        path->setMarginTop(18);
        content->addView(path);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

    ~SettingsView() override {
        alive_->store(false);
    }

private:
    static void addSection(brls::Box* content, const std::string& text) {
        auto* title = new brls::Label();
        title->setText(text);
        title->setFontSize(25);
        title->setMarginTop(14);
        title->setMarginBottom(8);
        content->addView(title);
    }

    // Settings-selector row for a stored language value; falls back to the
    // "auto" row so a value from a newer build cannot leave the cell blank.
    static int languageIndex(const std::string& value) {
        for (size_t i = 0; i < std::size(kLanguageValues); ++i) {
            if (value == kLanguageValues[i])
                return static_cast<int>(i);
        }
        return 0;
    }

    static brls::DetailCell* actionCell(const std::string& title,
                                        const std::string& detail,
                                        std::function<void()> callback) {
        auto* cell = new brls::DetailCell();
        cell->setText(title);
        cell->setDetailText(detail);
        cell->registerClickAction(
            [callback = std::move(callback)](brls::View*) {
                callback();
                return true;
            });
        return cell;
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

    void refreshCatalogNow() {
        if (refreshInFlight_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_catalog"));
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        brls::async([this, alive, catalog] {
            std::vector<CatalogEntry> entries;
            std::string error;
            bool ok = catalog->fetchLatest(entries, error);
            brls::sync([this, alive, ok, entries = std::move(entries),
                        error = std::move(error)]() mutable {
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
            });
        });
    }

    void refreshMetadataNow() {
        if (refreshInFlight_ || !metadata_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_artwork"));
        auto alive = alive_;
        GameMetadataService* metadata = metadata_;
        brls::async([this, alive, metadata] {
            MetadataSnapshot snapshot;
            std::string error;
            bool ok = metadata->fetchLatest(snapshot, error);
            brls::sync([this, alive, ok, snapshot = std::move(snapshot),
                        error = std::move(error)]() mutable {
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
            });
        });
    }

    void refreshModsNow() {
        if (refreshInFlight_ || !mods_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/settings/updating_mods"));
        auto alive = alive_;
        ModIndexService* mods = mods_;
        brls::async([this, alive, mods] {
            ModIndexSnapshot snapshot;
            std::string error;
            bool ok = mods->fetchLatest(snapshot, error);
            brls::sync([this, alive, ok, snapshot = std::move(snapshot),
                        error = std::move(error)]() mutable {
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

    void applyValues() {
        const AppSettingsData& values = settings_->get();
        language_->setSelection(languageIndex(values.language), true);
        catalogFilter_->setSelection(
            values.catalogFilter == CatalogFilter::Games ? 1 : 0, true);
        refreshCatalog_->setOn(values.refreshCatalogOnLaunch, false);
        streamSelection_->setSelection(
            values.streamSelection == StreamSelection::PackagesOnly ? 1 : 0,
            true);
        installLocation_->setSelection(
            values.installLocation == InstallLocation::SystemMemory ? 1 : 0,
            true);
        manager_->setInstallTarget(installTargetFor(values.installLocation));
        showCompleted_->setOn(values.showCompletedDownloads, false);
        extendedTelemetry_->setOn(values.extendedTelemetry, false);
        checkForUpdates_->setOn(values.checkForUpdatesOnLaunch, false);
        telemetry_set_enabled(values.extendedTelemetry ? 1 : 0);
    }

    void captureSnapshot() {
        size_t active = 0;
        size_t errors = 0;
        for (const DownloadTask& task : manager_->snapshot()) {
            if (task.status == DownloadStatus::Error)
                ++errors;
            else if (task.status != DownloadStatus::Completed &&
                     task.status != DownloadStatus::Installed &&
                     task.status != DownloadStatus::Paused)
                ++active;
        }
        uint64_t freeBytes = 0;
        struct statvfs storage {};
        if (statvfs("sdmc:/", &storage) == 0)
            freeBytes = static_cast<uint64_t>(storage.f_bavail) *
                        static_cast<uint64_t>(storage.f_frsize);
        uint32_t hos = hosversionGet();
        diagnostic_snapshot("system", "manual",
            "version=%s hos=%u.%u.%u operation_mode=%d telemetry=%d "
            "catalog=%zu metadata=%zu installed=%zu active=%zu errors=%zu "
            "sd_free_bytes=%llu",
            PIPENSX_VERSION, HOSVER_MAJOR(hos), HOSVER_MINOR(hos),
            HOSVER_MICRO(hos), static_cast<int>(appletGetOperationMode()),
            telemetry_enabled(), catalog_->entries().size(), metadata_->size(),
            installed_->titles().size(), active, errors,
            static_cast<unsigned long long>(freeBytes));
        brls::Application::notify(tr("pipensx/settings/snapshot_written"));
    }

    void confirmClearLog() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/clear_log_question"));
        dialog->addButton(tr("pipensx/settings/clear_log"), [] {
            if (!clearApplicationLog())
                brls::Application::notify(
                    tr("pipensx/settings/clear_log_failed"));
            else
                brls::Application::notify(tr("pipensx/settings/clear_log_done"));
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void confirmClearArtwork() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/clear_artwork_question"));
        dialog->addButton(tr("pipensx/settings/clear_artwork_action"), [this] {
            std::string error;
            if (!metadata_->clearImageCache(error)) {
                diagnostic_error("metadata", "clear_cache", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
            } else {
                brls::Application::notify(
                    tr("pipensx/settings/clear_artwork_done"));
            }
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void confirmReset() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/reset_question"));
        dialog->addButton(tr("pipensx/settings/reset_action"), [this] {
            std::string error;
            if (!settings_->reset(error)) {
                diagnostic_error("settings", "reset", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
                return;
            }
            applyValues();
            brls::Application::notify(tr("pipensx/settings/reset_done"));
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
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
    brls::SelectorCell* installLocation_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
    brls::BooleanCell* extendedTelemetry_ = nullptr;
    bool refreshInFlight_ = false;
    bool updateInFlight_ = false;
};

}  // namespace pipensx::ui
