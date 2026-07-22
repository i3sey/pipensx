#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/update_service.hpp"
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
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "app/mod_index_service.hpp"
#include "ui/catalog/catalog_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/main_frame.hpp"
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
using pipensx::FavoritesService;
using pipensx::ModIndexService;
using pipensx::SwitchPerformanceController;
using pipensx::UpdateCheckResult;
using pipensx::UpdateService;

using namespace pipensx::ui;

namespace {

constexpr const char* BundledCatalogPath =
    "romfs:/catalog/switch_games.json";

// AppSettingsData::language -> the borealis locale to load. LOCALE_AUTO makes
// SwitchPlatform read the console's system language, so a Russian console gets
// a Russian UI with no user action; anything we do not ship a locale directory
// for falls back to en-US per key.
const std::string& borealisLocaleFor(const std::string& language) {
    if (language == "ru")
        return brls::LOCALE_RU;
    if (language == "en-US")
        return brls::LOCALE_EN_US;
    return brls::LOCALE_AUTO;
}

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata,
                 InstalledTitleService* installed, AppSettings* settings,
                 UpdateService* updater, ModIndexService* mods,
                 FavoritesService* favorites)
        : manager_(manager), catalog_(catalog), metadata_(metadata),
          installed_(installed), settings_(settings), updater_(updater),
          mods_(mods), favorites_(favorites) {
        auto* tabs = new pipensx::ui::MainFrame();
        using pipensx::ui::NavIconType;
        tabs->addNavTab(tr("pipensx/nav/catalog"), NavIconType::Catalog,
                        [manager, catalog, metadata, installed,
                         settings, mods, favorites, tabs] {
            return new CatalogView(manager, catalog, metadata, installed,
                                   settings, [tabs] { tabs->focusTab(1); },
                                   mods, favorites);
        });
        tabs->addNavTab(tr("pipensx/nav/downloads"), NavIconType::Downloads,
                        [manager, metadata, settings] {
            return new MainView(manager, metadata, settings);
        });
        tabs->addNavTab(tr("pipensx/nav/installed"), NavIconType::Installed,
                        [installed, manager, metadata] {
            return new InstalledView(installed, manager, metadata);
        });
        tabs->addNavTab(tr("pipensx/nav/settings"), NavIconType::Settings,
                        [settings, manager, catalog, metadata,
                         installed, updater, mods] {
            return new SettingsView(settings, manager, catalog, metadata,
                                    installed, updater, mods);
        });
        tabs->addNavTab(tr("pipensx/nav/about"), NavIconType::About, [] {
            return new AboutView();
        });
        tabs->attachStorageFooter(manager);
        frame_ = new brls::AppletFrame(tabs);
        frame_->setTitle(tr("pipensx/app/title"));
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        registerAction(tr("pipensx/app/exit"), brls::BUTTON_START,
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
    UpdateService* updater_;
    ModIndexService* mods_;
    FavoritesService* favorites_;
    brls::AppletFrame* frame_;
};

}  // namespace

int main(int argc, char** argv) {
    // A library applet must only terminate after qlaunch asks it to close.
    // Keep this path before logging, settings, and custom signal handlers so
    // the unsupported mode uses only libnx's normal applet lifecycle.
    if (!isApplicationMode()) {
        showApplicationModeRequired();
        return 0;
    }

    switch_crashlog_install();
    switch_crashlog_stage("creating application directories");
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/pipensx", 0755);
    log_init(LogPath);

    (void)argc;
    (void)argv;
    UpdateService launchUpdater;
    const bool updatePendingConfirmation =
        launchUpdater.hasPendingConfirmation();
    // A verified download left behind by a session that quit before its
    // restart must chain through the minimal updater, never overwrite the NRO
    // that is currently executing.
    if (launchUpdater.stagedReady()) {
        if (envHasNextLoad() &&
            access(launchUpdater.helperPath().c_str(), F_OK) == 0) {
            const std::string helper = launchUpdater.helperPath();
            const std::string arguments =
                "\"" + helper + "\" --finish-update";
            const Result result = envSetNextLoad(helper.c_str(),
                                                 arguments.c_str());
            if (R_SUCCEEDED(result)) {
                log_msg("[update] staged update ready; launching helper\n");
                log_close();
                return 0;
            }
            diagnostic_error("update", "helper_nextload", "result=0x%08x",
                             result);
        } else {
            diagnostic_error("update", "helper_missing",
                             "nextload=%d helper=%d",
                             envHasNextLoad() ? 1 : 0,
                             access(launchUpdater.helperPath().c_str(), F_OK) == 0);
        }
    }
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
        // Must precede init(): the platform captures the locale in its
        // constructor and Application::init() loads translations exactly once,
        // which is why a language change only lands on the next launch.
        brls::Platform::APP_LOCALE_DEFAULT =
            borealisLocaleFor(settings.get().language);
        if (!brls::Application::init())
            throw std::runtime_error("Borealis Application::init failed");
        pipensx::ui::theme::registerColors();
        pipensx::ui::installSidebarStyle();

        startupStage("Borealis createWindow");
        brls::Application::createWindow("pipensx");
        brls::Application::setGlobalQuit(false);

        startupStage("CatalogService construction");
        log_msg("[startup] image relay: relays-first + disk cache (rev4)\n");
        unlink("sdmc:/switch/pipensx/rutracker.cfg");
        unlink("sdmc:/switch/pipensx/rutracker_cookies.txt");
        CatalogService catalog("sdmc:/switch/pipensx", BundledCatalogPath);
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

        startupStage("ModIndexService construction");
        ModIndexService mods("sdmc:/switch/pipensx");
        std::string modsError;
        if (!mods.load(modsError))
            log_msg("[mods] initial load skipped: %s\n", modsError.c_str());

        startupStage("FavoritesService construction");
        FavoritesService favorites("sdmc:/switch/pipensx");
        std::string favoritesError;
        if (!favorites.load(favoritesError))
            log_msg("[favorites] initial load skipped: %s\n",
                    favoritesError.c_str());

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

        UpdateService updater;

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata,
                                          &installed, &settings, &updater,
                                          &mods, &favorites);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);
        if (settings.get().checkForUpdatesOnLaunch) {
            updater.checkAsync([](UpdateCheckResult result) {
                if (!result.ok || !result.updateAvailable)
                    return;
                brls::sync([version = std::move(result.release.version)] {
                    brls::Application::notify(
                        tr("pipensx/settings/update_available_launch", version));
                });
            });
        }

        // First-run disclaimer: the catalog is a third-party RuTracker dump.
        // Shown once on top of the app; acknowledging persists the flag so
        // later launches skip it.
        if (!settings.get().catalogDisclaimerAcknowledged) {
            startupStage("catalog disclaimer");
            auto* dialog = new brls::Dialog(
                tr("pipensx/disclaimer/catalog"));
            // Narrow the stock 720px dialog frame for this short one-liner.
            if (auto* frame = dialog->getView("brls/dialog/applet"))
                frame->setWidth(520);
            dialog->addButton(tr("pipensx/common/ok"), [&settings] {
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
                if (updatePendingConfirmation) {
                    std::string error;
                    if (!launchUpdater.confirmInstalled(error))
                        diagnostic_error("update", "confirm", "error=%s",
                                         error.c_str());
                    else
                        log_msg("[update] installed update confirmed\n");
                }
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        updater.shutdown();
        manager.shutdown();
        performance.setActive(false);
    } catch (const std::exception& error) {
        log_msg("[crash] exception at stage '%s': %s\n",
                "see previous startup marker", error.what());
    } catch (...) {
        log_msg("[crash] unknown exception\n");
    }

    startupStage("app-owned teardown complete");
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
