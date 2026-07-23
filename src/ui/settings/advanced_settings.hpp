#pragma once

#include <sys/statvfs.h>

#include <functional>
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
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Advanced settings — the rare / troubleshooting controls that used to crowd the
// bottom of SettingsView. Pushed as its own activity so the main list stays
// short. A factory reset here also has to refresh the parent list's cells, which
// is what onReset() is for (see SettingsView).
class AdvancedSettingsActivity : public brls::Activity {
public:
    AdvancedSettingsActivity(AppSettings* settings, DownloadManager* manager,
                             CatalogService* catalog,
                             GameMetadataService* metadata,
                             InstalledTitleService* installed,
                             std::function<void()> onReset)
        : settings_(settings), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed),
          onReset_(std::move(onReset)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        addSection(content, tr("pipensx/settings/section_downloads"));
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

        frame_ = new brls::AppletFrame(scroll);
        frame_->setTitle(tr("pipensx/settings/advanced"));
    }

    brls::View* createContentView() override {
        return frame_;
    }

private:
    bool persist(const AppSettingsData& values, const char* tag) {
        std::string error;
        if (settings_->update(values, error))
            return true;
        diagnostic_error("settings", tag, "error=%s", error.c_str());
        brls::Application::notify(error);
        return false;
    }

    // Re-sync the cells this page owns after a reset restores defaults.
    void applyOwnValues() {
        const AppSettingsData& values = settings_->get();
        installLocation_->setSelection(
            values.installLocation == InstallLocation::SystemMemory ? 1 : 0,
            true);
        manager_->setInstallTarget(installTargetFor(values.installLocation));
        extendedTelemetry_->setOn(values.extendedTelemetry, false);
        telemetry_set_enabled(values.extendedTelemetry ? 1 : 0);
    }

    void captureSnapshot() {
        writeSystemSnapshot(manager_, catalog_, metadata_, installed_, "manual");
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
            applyOwnValues();
            if (onReset_)
                onReset_();
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
    std::function<void()> onReset_;
    brls::AppletFrame* frame_ = nullptr;
    brls::SelectorCell* installLocation_ = nullptr;
    brls::BooleanCell* extendedTelemetry_ = nullptr;
};

}  // namespace pipensx::ui
