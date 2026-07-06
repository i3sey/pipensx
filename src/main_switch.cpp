#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "platform/switch_crashlog.h"
#include "platform/switch_performance.hpp"

extern "C" {
#include "core/util.h"
}

#include <borealis.hpp>
#include <curl/curl.h>
#include <switch.h>
#include <switch-ipcext.h>

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "ui/catalog/catalog_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/downloads/downloads_view.hpp"
#include "ui/installed/installed_view.hpp"
#include "ui/settings/about_view.hpp"
#include "ui/settings/settings_view.hpp"
#include "ui/theme.hpp"

using pipensx::AppSettings;
using pipensx::CatalogService;
using pipensx::DownloadManager;
using pipensx::GameMetadataService;
using pipensx::InstalledTitleService;
using pipensx::SwitchPerformanceController;

using namespace pipensx::ui;

namespace {

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata,
                 InstalledTitleService* installed, AppSettings* settings)
        : manager_(manager), catalog_(catalog), metadata_(metadata),
          installed_(installed), settings_(settings) {
        auto* tabs = new brls::TabFrame();
        tabs->addTab("Catalog", [manager, catalog, metadata, installed,
                                  settings, tabs] {
            return new CatalogView(manager, catalog, metadata, installed,
                                   settings, [tabs] { tabs->focusTab(1); });
        });
        tabs->addTab("Downloads", [manager, metadata, settings] {
            return new MainView(manager, metadata, settings);
        });
        tabs->addTab("Installed", [installed, manager, metadata] {
            return new InstalledView(installed, manager, metadata);
        });
        tabs->addTab("Settings", [settings, manager, catalog, metadata,
                                   installed] {
            return new SettingsView(settings, manager, catalog, metadata,
                                    installed);
        });
        tabs->addTab("About", [] {
            return new AboutView();
        });
        frame_ = new brls::AppletFrame(tabs);
        frame_->setTitle("pipensx");
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        registerAction("Exit", brls::BUTTON_START,
            [this](brls::View*) {
                startupStage("quit requested by Plus");
                brls::Application::quit();
                return true;
            });
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    brls::AppletFrame* frame_;
};

}  // namespace

int main(int, char**) {
    switch_crashlog_install();
    switch_crashlog_stage("creating application directories");
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/pipensx", 0755);
    log_init(LogPath);
    AppSettings settings(SettingsPath, TelemetryFlagPath);
    std::string settingsError;
    if (!settings.load(settingsError))
        diagnostic_error("settings", "startup", "error=%s",
                         settingsError.c_str());
    telemetry_set_enabled(settings.get().extendedTelemetry ? 1 : 0);
    log_msg("[telemetry] setting enabled=%d interval_ms=5000 build='%s %s'\n",
            telemetry_enabled(), __DATE__, __TIME__);
    startupStage("entered main");

    openBorealisLog();

    std::set_terminate([] {
        switch_crashlog_stage("uncaught C++ exception");
        log_msg("[crash] std::terminate called\n");
        if (gBorealisLog)
            std::fflush(gBorealisLog);
        std::_Exit(134);
    });

    bool curlReady = false;
    bool ncmReady = false;
    bool nsReady = false;
    bool esReady = false;
    try {
        log_msg("[startup] applet_type=%d operation_mode=%d\n",
                (int)appletGetAppletType(), (int)appletGetOperationMode());

        if (!isApplicationMode()) {
            startupStage("unsupported applet mode");
            showApplicationModeRequired();
            startupStage("applet mode exit");
            if (gBorealisLog) {
                std::fflush(gBorealisLog);
                std::fclose(gBorealisLog);
                gBorealisLog = nullptr;
            }
            log_close();
            return 2;
        }

        startupStage("curl_global_init");
        CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curlResult != CURLE_OK) {
            log_msg("[startup] curl_global_init failed: %d\n",
                    (int)curlResult);
            throw std::runtime_error("curl_global_init failed");
        }
        curlReady = true;

        startupStage("installer services");
        Result rc = ncmInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("ncmInitialize failed");
        ncmReady = true;
        rc = nsInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("nsInitialize failed");
        nsReady = true;
        rc = esInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("esInitialize failed");
        esReady = true;

        startupStage("Borealis Application::init");
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
        if (!brls::Application::init())
            throw std::runtime_error("Borealis Application::init failed");
        pipensx::ui::theme::registerColors();

        startupStage("Borealis createWindow");
        brls::Application::createWindow("pipensx");
        brls::Application::setGlobalQuit(false);

        startupStage("CatalogService construction");
        log_msg("[startup] image relay: relays-first + disk cache (rev4)\n");
        unlink("sdmc:/switch/pipensx/rutracker.cfg");
        unlink("sdmc:/switch/pipensx/rutracker_cookies.txt");
        CatalogService catalog("sdmc:/switch/pipensx");
        std::string catalogError;
        if (!catalog.load(catalogError))
            log_msg("[catalog] initial load failed: %s\n",
                    catalogError.c_str());
        startupStage("GameMetadataService construction");
        GameMetadataService metadata("sdmc:/switch/pipensx");
        std::string metadataError;
        if (!metadata.load(metadataError))
            log_msg("[metadata] initial load failed: %s\n",
                    metadataError.c_str());

        startupStage("InstalledTitleService refresh");
        InstalledTitleService installed("sdmc:/switch/pipensx");
        std::string installedError;
        if (!installed.refresh(installedError))
            diagnostic_error("installed", "startup", "error=%s",
                             installedError.c_str());

        startupStage("DownloadManager construction");
        SwitchPerformanceController performance;
        DownloadManager manager("sdmc:/switch/pipensx");
        manager.setInstallTarget(
            installTargetFor(settings.get().installLocation));
        metadata.setImageNetworkPaused(manager.hasActiveTransfer());

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata,
                                          &installed, &settings);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);

        // First-run disclaimer: the catalog is a third-party RuTracker dump.
        // Shown once on top of the app; acknowledging persists the flag so
        // later launches skip it.
        if (!settings.get().catalogDisclaimerAcknowledged) {
            startupStage("catalog disclaimer");
            auto* dialog = new brls::Dialog(
                "Catalog is a RuTracker dump by @Langegen. Not affiliated.");
            // Narrow the stock 720px dialog frame for this short one-liner.
            if (auto* frame = dialog->getView("brls/dialog/applet"))
                frame->setWidth(520);
            dialog->addButton("OK", [&settings] {
                pipensx::AppSettingsData values = settings.get();
                if (values.catalogDisclaimerAcknowledged)
                    return;
                values.catalogDisclaimerAcknowledged = true;
                std::string error;
                if (!settings.update(values, error))
                    log_msg("[settings] disclaimer ack persist failed: %s\n",
                            error.c_str());
            });
            dialog->open();
        }

        startupStage("first main loop");
        bool firstFrame = true;
        while (true) {
            bool activeTransfer = manager.hasActiveTransfer();
            performance.setActive(activeTransfer);
            metadata.setImageNetworkPaused(activeTransfer);
            if (!brls::Application::mainLoop())
                break;
            if (firstFrame) {
                startupStage("main loop running");
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        manager.shutdown();
        performance.setActive(false);
    } catch (const std::exception& error) {
        log_msg("[crash] exception at stage '%s': %s\n",
                "see previous startup marker", error.what());
    } catch (...) {
        log_msg("[crash] unknown exception\n");
    }

    startupStage("cleanup");
    if (esReady)
        esExit();
    if (nsReady)
        nsExit();
    if (ncmReady)
        ncmExit();
    if (curlReady)
        curl_global_cleanup();
    if (gBorealisLog) {
        std::fflush(gBorealisLog);
        std::fclose(gBorealisLog);
        gBorealisLog = nullptr;
    }
    log_close();
    return 0;
}
