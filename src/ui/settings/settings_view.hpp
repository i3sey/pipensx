#pragma once

#include <sys/statvfs.h>

#include <string>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "core/antizapret.h"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class SettingsView : public brls::Box {
public:
    SettingsView(AppSettings* settings, DownloadManager* manager,
                 CatalogService* catalog, GameMetadataService* metadata,
                 InstalledTitleService* installed)
        : brls::Box(brls::Axis::COLUMN), settings_(settings), manager_(manager),
          catalog_(catalog), metadata_(metadata), installed_(installed) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        addSection(content, "Catalog");
        catalogFilter_ = new brls::SelectorCell();
        catalogFilter_->init("Visible releases", {"All", "Games"},
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
        refreshCatalog_->init("Refresh catalog on launch",
            settings_->get().refreshCatalogOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.refreshCatalogOnLaunch;
                values.refreshCatalogOnLaunch = enabled;
                if (!persist(values, "catalog_refresh"))
                    refreshCatalog_->setOn(previous, false);
            });
        content->addView(refreshCatalog_);

        addSection(content, "Downloads");
        streamSelection_ = new brls::SelectorCell();
        streamSelection_->init("Default streaming file selection",
            {"All files", "NSP/NSZ only"},
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
        installLocation_->init("Install location",
            {"SD card", "System memory"},
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
        showCompleted_->init("Show completed downloads",
            settings_->get().showCompletedDownloads,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.showCompletedDownloads;
                values.showCompletedDownloads = enabled;
                if (!persist(values, "show_completed"))
                    showCompleted_->setOn(previous, false);
            });
        content->addView(showCompleted_);

        addSection(content, "Connectivity");
        auto* antizapretHint = new brls::Label();
        antizapretHint->setText(
            "Route RuTracker through antizapret proxies when direct access is "
            "blocked. Turn off if you have unrestricted access.");
        antizapretHint->setFontSize(16);
        antizapretHint->setTextColor(theme::textSecondary());
        antizapretHint->setMarginBottom(10);
        content->addView(antizapretHint);

        useAntizapret_ = new brls::BooleanCell();
        useAntizapret_->init("Antizapret bypass",
            settings_->get().useAntizapret,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.useAntizapret;
                values.useAntizapret = enabled;
                if (!persist(values, "use_antizapret")) {
                    useAntizapret_->setOn(previous, false);
                    return;
                }
                antizapret_set_enabled(enabled ? 1 : 0);
                brls::Application::notify(enabled
                    ? "Antizapret bypass enabled."
                    : "Antizapret bypass disabled.");
            });
        content->addView(useAntizapret_);

        addSection(content, "Diagnostics");
        auto* description = new brls::Label();
        description->setText(
            "Errors are always recorded. Extended mode adds rate-limited "
            "torrent, buffer, decoder, image and NCM metrics every 5 seconds.");
        description->setFontSize(16);
        description->setTextColor(theme::textSecondary());
        description->setMarginBottom(10);
        content->addView(description);

        extendedTelemetry_ = new brls::BooleanCell();
        extendedTelemetry_->init("Extended telemetry",
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
                    ? "Extended telemetry enabled."
                    : "Extended telemetry disabled.");
            });
        content->addView(extendedTelemetry_);

        content->addView(actionCell("Capture diagnostic snapshot", "Write now",
            [this] { captureSnapshot(); }));
        content->addView(actionCell("Clear log", "32 MB rotation",
            [this] { confirmClearLog(); }));
        content->addView(actionCell("Clear artwork cache", "Downloaded images",
            [this] { confirmClearArtwork(); }));
        content->addView(actionCell("Reset settings", "Restore defaults",
            [this] { confirmReset(); }));

        auto* path = new brls::Label();
        path->setText(std::string("Log: ") + LogPath);
        path->setFontSize(15);
        path->setTextColor(theme::textTertiary());
        path->setMarginTop(18);
        content->addView(path);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
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

    void applyValues() {
        const AppSettingsData& values = settings_->get();
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
        telemetry_set_enabled(values.extendedTelemetry ? 1 : 0);
        useAntizapret_->setOn(values.useAntizapret, false);
        antizapret_set_enabled(values.useAntizapret ? 1 : 0);
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
        brls::Application::notify("Diagnostic snapshot written to pipensx.log.");
    }

    void confirmClearLog() {
        auto* dialog = new brls::Dialog("Clear pipensx.log now?");
        dialog->addButton("Clear log", [] {
            if (!clearApplicationLog())
                brls::Application::notify("Unable to clear pipensx.log.");
            else
                brls::Application::notify("Log cleared.");
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    void confirmClearArtwork() {
        auto* dialog = new brls::Dialog("Clear downloaded artwork cache?");
        dialog->addButton("Clear artwork", [this] {
            std::string error;
            if (!metadata_->clearImageCache(error)) {
                diagnostic_error("metadata", "clear_cache", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
            } else {
                brls::Application::notify("Artwork cache cleared.");
            }
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    void confirmReset() {
        auto* dialog = new brls::Dialog("Reset all pipensx settings?");
        dialog->addButton("Reset settings", [this] {
            std::string error;
            if (!settings_->reset(error)) {
                diagnostic_error("settings", "reset", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
                return;
            }
            applyValues();
            brls::Application::notify("Settings restored to defaults.");
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    brls::SelectorCell* catalogFilter_ = nullptr;
    brls::BooleanCell* refreshCatalog_ = nullptr;
    brls::SelectorCell* streamSelection_ = nullptr;
    brls::SelectorCell* installLocation_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
    brls::BooleanCell* extendedTelemetry_ = nullptr;
    brls::BooleanCell* useAntizapret_ = nullptr;
};

}  // namespace pipensx::ui
