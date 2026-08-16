#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/companion_settings.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/game_update_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/switch_deploy.hpp"
#include "app/update_service.hpp"
#include "app/web_server.hpp"
#include "platform/switch_crashlog.h"
#include "platform/switch_performance.hpp"

extern "C" {
#include "core/dht.h"
#include "core/util.h"
}

#include <borealis.hpp>
#include <curl/curl.h>
#include <switch.h>
#include <switch-ipcext.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "ui/catalog/catalog_view.hpp"
#include "ui/common/burn_in_saver.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/web_qr.hpp"
#include "ui/first_run_view.hpp"
#include "ui/i18n.hpp"
#include "ui/main_frame.hpp"
#include "ui/downloads/downloads_view.hpp"
#include "ui/downloads/task_files_activity.hpp"
#include "ui/installed/installed_view.hpp"
#include "ui/settings/about_view.hpp"
#include "ui/settings/help_view.hpp"
#include "ui/settings/settings_view.hpp"
#include "ui/theme.hpp"

using pipensx::AppSettings;
using pipensx::CatalogService;
using pipensx::DownloadManager;
using pipensx::SwitchDeployService;
using pipensx::GameMetadataService;
using pipensx::GameUpdateService;
using pipensx::InstalledTitleService;
using pipensx::FavoritesService;
using pipensx::SwitchPerformanceController;
using pipensx::UpdateCheckResult;
using pipensx::UpdateService;
using pipensx::WebServer;

using namespace pipensx::ui;

namespace {

constexpr auto kCompanionSettingsTimeout = std::chrono::seconds(5);

bool runOnUiThread(const std::function<void()>& fn) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    brls::sync([&] {
        fn();
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, kCompanionSettingsTimeout, [&] { return done; });
}

constexpr const char* BundledCatalogPath =
    "romfs:/catalog/switch_games.json.zst";

// AppSettingsData::language -> the borealis locale to load. LOCALE_AUTO makes
// SwitchPlatform read the console's system language, so a Russian console gets
// a Russian UI with no user action; anything we do not ship a locale directory
// for falls back to en-US per key.
// Joins on scope exit so an exception between spawn and the explicit join
// unwinds cleanly instead of hitting std::terminate in ~thread().
struct ThreadJoiner {
    std::thread thread;
    ~ThreadJoiner() {
        if (thread.joinable())
            thread.join();
    }
};

const std::string& borealisLocaleFor(const std::string& language) {
    if (language == "ru")
        return brls::LOCALE_RU;
    if (language == "en-US")
        return brls::LOCALE_EN_US;
    if (language == "pt-BR")
        return brls::LOCALE_PT_BR;
    if (language == "fr")
        return brls::LOCALE_FR;
    if (language == "es")
        return brls::LOCALE_ES;
    if (language == "zh-CN")
        return brls::LOCALE_ZH_CN;
    return brls::LOCALE_AUTO;
}

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata,
                 InstalledTitleService* installed, AppSettings* settings,
                 UpdateService* updater, FavoritesService* favorites, WebServer* webServer,
                 SwitchDeployService* deploy, GameUpdateService* gameUpdates)
        : manager_(manager), catalog_(catalog), metadata_(metadata),
          installed_(installed), settings_(settings), updater_(updater),
          favorites_(favorites), webServer_(webServer),
          deploy_(deploy), gameUpdates_(gameUpdates) {
        auto* tabs = new pipensx::ui::MainFrame();
        using pipensx::ui::NavIconType;
        using pipensx::CatalogSection;
        // Games, Ports, separator, Downloads, ... — Downloads is sidebar index 3.
        tabs->addNavTab(tr("pipensx/nav/games"), NavIconType::Catalog,
                        [this, manager, catalog, metadata, installed,
                         settings, favorites, deploy, tabs] {
            return new CatalogView(manager, catalog, metadata, installed,
                                   settings, [tabs] { tabs->focusTab(3); },
                                   favorites, deploy, CatalogSection::Games, {},
                                   [this] { refreshUpdateBadge(); });
        });
        tabs->addNavTab(tr("pipensx/nav/ports"), NavIconType::Ports,
                        [this, manager, catalog, metadata, installed,
                         settings, favorites, deploy, tabs] {
            return new CatalogView(manager, catalog, metadata, installed,
                                   settings, [tabs] { tabs->focusTab(3); },
                                   favorites, deploy, CatalogSection::Ports,
                                   [tabs] { tabs->focusTab(0); },
                                   [this] { refreshUpdateBadge(); });
        });
        tabs->addSeparator();
        tabs->addNavTab(tr("pipensx/nav/downloads"), NavIconType::Downloads,
                        [manager, metadata, settings, deploy] {
            return new MainView(manager, metadata, settings, deploy);
        });
        tabs->addNavTab(tr("pipensx/nav/installed"), NavIconType::Installed,
                        [installed, manager, metadata, settings, catalog,
                         gameUpdates, tabs, favorites, deploy] {
            auto* view = new InstalledView(installed, manager, metadata,
                                           settings, catalog, gameUpdates,
                                           true, favorites, deploy);
            view->setOnUpdateCount([tabs](size_t count) {
                tabs->setUpdateCountBadge(count);
            });
            return view;
        }, true);
        tabs->addNavTab(tr("pipensx/nav/settings"), NavIconType::Settings,
                        [this, settings, manager, catalog, metadata,
                         installed, updater, webServer] {
            return new SettingsView(settings, manager, catalog, metadata,
                                    installed, updater, webServer,
                                    [this] { refreshUpdateBadge(); });
        });
        tabs->addNavTab(tr("pipensx/nav/help"), NavIconType::Help,
                        [manager, catalog, metadata, installed] {
            return new HelpView(manager, catalog, metadata, installed);
        });
        tabs->addNavTab(tr("pipensx/nav/about"), NavIconType::About, [] {
            return new AboutView();
        });
        tabs->attachStorageFooter(manager, webServer);
        tabs_ = tabs;
        refreshUpdateBadge();
        frame_ = new brls::AppletFrame(tabs);
        frame_->setTitle(tr("pipensx/app/title"));
    }

    // The installed scan runs off-thread so the first frame is not blocked.
    // Call again when that scan lands: titles() is empty in the constructor
    // and availableUpdateCount would otherwise keep the sidebar chip at 0
    // until the user opens the Installed tab (which constructs InstalledView).
    void refreshUpdateBadge() {
        if (!tabs_ || !gameUpdates_ || !installed_ || !settings_)
            return;
        const std::vector<pipensx::InstalledTitle> titles = installed_->titles();
        if (!titles.empty()) {
            std::string saveError;
            gameUpdates_->checkAll(titles, installed_->generation(),
                                   settings_->get().lastMetadataRefreshMs,
                                   saveError);
            if (!saveError.empty())
                diagnostic_error("game_updates", "save", "error=%s",
                                 saveError.c_str());
        }
        tabs_->setUpdateCountBadge(gameUpdates_->availableCount(titles));
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        // Hidden hint: this action sits on the frame, so its label would ride
        // the bottom bar on every screen under MainActivity — and the catalog
        // already registers more hints than a 1280px bar holds in Russian.
        // Plus-to-exit is a console convention, and HOME works regardless.
        registerAction(tr("pipensx/app/exit"), brls::BUTTON_START,
            [this](brls::View*) {
                startupStage("quit requested by Plus");
                if (deploy_ && deploy_->snapshot().active()) {
                    auto* dialog = new brls::Dialog(
                        tr("pipensx/deploy/exit_question"));
                    dialog->addButton(tr("pipensx/common/cancel"), [] {});
                    dialog->addButton(tr("pipensx/deploy/cancel_and_exit"),
                                      [this] {
                        deploy_->cancel();
                        brls::Application::quit();
                    });
                    dialog->open();
                    return true;
                }
                brls::Application::quit();
                return true;
            }, /*hidden=*/true);
        // Visible on every screen: the web companion QR is the whole pairing
        // story, so it must not stay buried three levels deep in Settings.
        registerAction(tr("pipensx/app/web_qr"), brls::BUTTON_BACK,
            [this](brls::View*) {
                const pipensx::AppSettingsData& values = settings_->get();
                const std::string url = pipensx::ui::webCompanionUrl(
                    webServer_, values.webServerEnabled);
                if (url.empty()) {
                    brls::Application::notify(
                        tr(values.webServerEnabled
                               ? "pipensx/settings/web_address_none"
                               : "pipensx/web/off"));
                    return true;
                }
                pipensx::ui::showWebQrDialog(url, values.webServerPin);
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
    FavoritesService* favorites_;
    WebServer* webServer_;
    SwitchDeployService* deploy_;
    GameUpdateService* gameUpdates_;
    pipensx::ui::MainFrame* tabs_ = nullptr;
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
    // A verified download staged by a previous session that quit before the
    // helper finished the swap. Do NOT auto-chain to the helper here: an
    // unconditional envSetNextLoad + quit on every launch turns a single failed
    // helper load into a crash loop that bricks the app. It is surfaced as a
    // user-triggered "install now?" prompt once the UI is up (see below).
    const bool updatePendingFinish = launchUpdater.stagedReady();
    AppSettings settings(SettingsPath, TelemetryFlagPath);
    std::string settingsError;
    if (!settings.load(settingsError))
        diagnostic_error("settings", "startup", "error=%s",
                         settingsError.c_str());
    telemetry_set_enabled(settings.get().extendedTelemetry ? 1 : 0);
    // Before curl_global_init and before any service builds a handle: curl
    // reads the proxy from the environment when it sets up a transfer.
    pipensx::applyProxySetting(settings.get().proxyUrl);
    if (!settings.get().proxyUrl.empty())
        log_msg("[startup] proxy %s\n", settings.get().proxyUrl.c_str());
    log_msg("[telemetry] setting enabled=%d interval_ms=5000 build='%s %s'\n",
            telemetry_enabled(), __DATE__, __TIME__);
    startupStage("entered main");

    openBorealisLog();

    std::set_terminate([] {
        switch_crashlog_stage("uncaught C++ exception");
        // "std::terminate called" on its own names nothing. Rethrowing the
        // in-flight exception is the only way to get its type and message
        // into the log, and it is safe here: we _Exit either way.
        if (std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                log_msg("[crash] std::terminate: %s: %s\n",
                        typeid(e).name(), e.what());
            } catch (...) {
                log_msg("[crash] std::terminate: non-std exception\n");
            }
        } else {
            log_msg("[crash] std::terminate called with no live exception\n");
        }
        log_flush();
        std::_Exit(134);
    });

    bool curlReady = false;
    bool ncmReady = false;
    bool nsReady = false;
    bool esReady = false;
    try {
        log_msg("[startup] applet_type=%d operation_mode=%d\n",
                (int)appletGetAppletType(), (int)appletGetOperationMode());

        // Our own sockets pass MSG_NOSIGNAL, and every curl handle now sets
        // CURLOPT_NOSIGNAL — which also stops libcurl asking the system to
        // ignore SIGPIPE. Do it here instead, as main_pc.c already does.
        signal(SIGPIPE, SIG_IGN);

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

        // The metadata index parse (an ~8 MB JSON) runs on a worker thread in
        // parallel with the catalog parse below; the service is not touched by
        // anything else until the join before MainActivity construction, after
        // which all access is UI-thread as before. Startup pays
        // max(catalog, metadata) instead of their sum.
        startupStage("GameMetadataService construction");
        GameMetadataService metadata("sdmc:/switch/pipensx");
        std::string metadataError;
        bool metadataOk = true;
        ThreadJoiner metadataLoader{
            std::thread([&metadata, &metadataError, &metadataOk] {
                metadataOk = metadata.load(metadataError);
            })};

        std::string catalogError;
        if (!catalog.load(catalogError))
            log_msg("[catalog] initial load failed: %s\n",
                    catalogError.c_str());

        startupStage("FavoritesService construction");
        FavoritesService favorites("sdmc:/switch/pipensx");
        std::string favoritesError;
        if (!favorites.load(favoritesError))
            log_msg("[favorites] initial load skipped: %s\n",
                    favoritesError.c_str());

        // The installed-title scan does one full control-data IPC read (NACP +
        // up to 128 KB icon) per installed title — on a full console this was
        // the single largest startup cost, all before the first frame. The
        // service is internally locked and the UI already refreshes it via
        // brls::async, so run the initial scan on its own thread and let the
        // UI come up immediately; the list fills in when the scan lands.
        startupStage("InstalledTitleService refresh (async)");
        InstalledTitleService installed("sdmc:/switch/pipensx");
        std::atomic<bool> installedScanDone{false};
        ThreadJoiner installedScanner{std::thread([&installed,
                                                   &installedScanDone] {
            std::string installedError;
            if (!installed.refresh(installedError))
                diagnostic_error("installed", "startup", "error=%s",
                                 installedError.c_str());
            installedScanDone.store(true);
        })};

        startupStage("DownloadManager construction");
        SwitchPerformanceController performance;
        dht_engine_set_cache_path("sdmc:/switch/pipensx/dht.cache");
        DownloadManager manager("sdmc:/switch/pipensx");
        SwitchDeployService deploy(manager, "sdmc:/switch/pipensx",
                                   "sdmc:/switch");
        manager.setInstallTarget(
            installTargetFor(settings.get().installLocation));
        manager.setMaxActiveDownloads(settings.get().maxActiveDownloads);
        manager.setTorboxApiKey(settings.get().torboxApiKey);
        manager.setTorrserverUrl(settings.get().torrserverUrl);
        manager.setRealdebridApiKey(settings.get().realdebridApiKey);
        manager.setTorrentingEnabled(settings.get().torrentingEnabled);
        metadata.setImageNetwork(
            manager.hasActiveTransfer()
                ? GameMetadataService::ImageNetwork::Throttled
                : GameMetadataService::ImageNetwork::Full);

        UpdateService updater;

        startupStage("WebServer construction");
        WebServer webServer(manager, "romfs:/web", PIPENSX_VERSION);
        webServer.setPin(settings.get().webServerPin);
        webServer.setStreamSelection(settings.get().streamSelection);
        webServer.setSettingsHandlers(
            [&settings](std::string& json, std::string& error) {
                bool ok = false;
                if (!runOnUiThread([&] {
                        json = pipensx::companionSettingsJson(settings.get());
                        ok = true;
                    })) {
                    error = "timed out";
                    return false;
                }
                return ok;
            },
            [&settings, &manager, &webServer](const std::string& body,
                                              std::string& error,
                                              std::string& json) {
                bool ok = false;
                if (!runOnUiThread([&] {
                        pipensx::AppSettingsData next = settings.get();
                        if (!pipensx::applyCompanionSettingsPatch(next, body,
                                                                  error))
                            return;
                        if (!settings.update(next, error))
                            return;
                        pipensx::applyCompanionSettingsRuntime(settings.get(),
                                                               manager);
                        webServer.setStreamSelection(
                            settings.get().streamSelection);
                        json = pipensx::companionSettingsJson(settings.get());
                        ok = true;
                    })) {
                    error = "timed out";
                    return false;
                }
                return ok;
            });
        webServer.updateCatalog(catalog.sharedEntries());
        // Every later adopt() (launch refresh, settings refresh, catalog tab)
        // lands on the UI thread, so this callback keeps the companion's
        // catalogue reference current from all of them.
        catalog.setOnAdopt(
            [&webServer](
                std::shared_ptr<const std::vector<pipensx::CatalogEntry>> e) {
                webServer.updateCatalog(std::move(e));
            });
        if (settings.get().webServerEnabled)
            webServer.start();

        // Barrier: from here on the UI reads the metadata service, so the
        // parallel index parse must have landed.
        startupStage("join metadata loader");
        metadataLoader.thread.join();
        if (!metadataOk)
            log_msg("[metadata] initial load failed: %s\n",
                    metadataError.c_str());

        startupStage("GameUpdateService load");
        GameUpdateService gameUpdates(&metadata,
                                      "sdmc:/switch/pipensx/game-updates.json");
        std::string gameUpdatesError;
        if (!gameUpdates.load(gameUpdatesError))
            diagnostic_error("game_updates", "load", "error=%s",
                             gameUpdatesError.c_str());

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata,
                                           &installed, &settings, &updater,
                                           &favorites, &webServer,
                                           &deploy, &gameUpdates);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);
        if (updatePendingFinish) {
            // Finish an update staged before a previous quit. User-triggered so
            // a helper that fails to load can never become an automatic loop.
            startupStage("pending update prompt");
            const std::string helper = launchUpdater.helperPath();
            auto* dialog = new brls::Dialog(
                tr("pipensx/settings/update_pending_install"));
            dialog->addButton(tr("pipensx/settings/install_and_restart"),
                              [helper] {
#ifdef __SWITCH__
                if (!envHasNextLoad()) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_no_restart"));
                    return;
                }
                const std::string arguments =
                    "\"" + helper + "\" --finish-update";
                const Result result = envSetNextLoad(helper.c_str(),
                                                     arguments.c_str());
                if (R_FAILED(result)) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_restart_failed"));
                    return;
                }
#endif
                brls::Application::quit();
            });
            dialog->addButton(tr("pipensx/common/later"), [] {});
            dialog->open();
        }
        if (!updatePendingFinish && settings.get().checkForUpdatesOnLaunch) {
            updater.checkAsync([](UpdateCheckResult result) {
                if (!result.ok || !result.updateAvailable)
                    return;
                brls::sync([version = std::move(result.release.version)] {
                    brls::Application::notify(
                        tr("pipensx/settings/update_available_launch", version));
                });
            });
        }

        // First-run: the method choice gates the app, so it comes first on a
        // fresh install — nothing (not even the catalog disclaimer) is shown
        // before it, and B is locked on it until a method is picked. Once the
        // choice is saved, the disclaimer follows (non-cancelable: it guards
        // the provider link step), then — for the server modes — the link
        // screen.
        if (!settings.get().firstRunCompleted) {
            startupStage("first-run method choice");
            pipensx::ui::showFirstRunChoice(
                &settings, &manager,
                [&settings, &manager](
                    pipensx::DebridProviderKind provider, bool torrenting) {
                    pipensx::ui::showCatalogDisclaimer(
                        &settings, [&settings, &manager, provider,
                                    torrenting] {
                            if (!torrenting)
                                pipensx::ui::DebridLinkView::push(
                                    &settings, &manager, provider);
                        });
                });
        } else {
            startupStage("catalog disclaimer");
            pipensx::ui::showCatalogDisclaimer(&settings, [] {});
        }

        startupStage("first main loop");
        bool firstFrame = true;
        uint64_t lastInputMs = now_ms();
        uint64_t lastDeployOfferPollMs = now_ms();
        pipensx::SwitchDeployPhase lastDeployPhase =
            pipensx::SwitchDeployPhase::Idle;
        bool deployOfferDialogOpen = false;
        // Download/install lifecycle notifications (#33): the main loop is the
        // only place that runs regardless of which screen is focused, so
        // terminal status transitions are surfaced here as toasts instead of
        // requiring the Downloads tab to stay open. The map only tracks tasks
        // observed after startup, so pre-existing rows never re-notify.
        std::unordered_map<std::string, pipensx::DownloadStatus> lastTaskStatus;
        uint64_t lastDownloadScanMs = now_ms();
        uint64_t lastPerfMs = 0;
        pipensx::SwitchDeploySnapshot deployState{};
        bool activeTransfer = false;
        bool updateBadgeApplied = false;
        while (true) {
            const uint64_t nowPerf = now_ms();
            if (lastPerfMs == 0 || nowPerf - lastPerfMs >= 250) {
                lastPerfMs = nowPerf;
                deployState = deploy.snapshot();
                activeTransfer = manager.hasActiveTransfer() ||
                                  deployState.active();
                performance.setActive(activeTransfer);
                metadata.setImageNetwork(
                    activeTransfer
                        ? GameMetadataService::ImageNetwork::Throttled
                        : GameMetadataService::ImageNetwork::Full);
            }
            if (!brls::Application::mainLoop())
                break;
            if (!updateBadgeApplied && installedScanDone.load()) {
                updateBadgeApplied = true;
                activity->refreshUpdateBadge();
            }
            if (deployState.phase != lastDeployPhase) {
                if (deployState.phase == pipensx::SwitchDeployPhase::Completed)
                    brls::Application::notify(deployState.detail.empty()
                        ? tr("pipensx/deploy/completed")
                        : tr("pipensx/deploy/completed_warning",
                             deployState.detail));
                else if (deployState.phase ==
                         pipensx::SwitchDeployPhase::Failed)
                    brls::Application::notify(
                        tr("pipensx/deploy/failed") +
                        (deployState.detail.empty()
                             ? std::string() : " " + deployState.detail));
                else if (deployState.phase ==
                         pipensx::SwitchDeployPhase::Cancelled)
                    brls::Application::notify(tr("pipensx/deploy/cancelled"));
                else if (deployState.phase ==
                         pipensx::SwitchDeployPhase::Preparing)
                    brls::Application::notify(tr("pipensx/deploy/phase_preparing"));
                else if (deployState.phase ==
                         pipensx::SwitchDeployPhase::Copying)
                    brls::Application::notify(tr("pipensx/deploy/phase_copying"));
                else if (deployState.phase ==
                         pipensx::SwitchDeployPhase::Extracting)
                    brls::Application::notify(
                        tr("pipensx/deploy/phase_extracting"));
                lastDeployPhase = deployState.phase;
            }

            // Throttled scan of task statuses: notify once per terminal
            // transition (download complete, install complete, download failed).
            const uint64_t frameNowMs = now_ms();
            if (frameNowMs - lastDownloadScanMs >= 1000) {
                lastDownloadScanMs = frameNowMs;
                std::unordered_set<std::string> seen;
                for (const pipensx::DownloadTask& task : manager.snapshotUi()) {
                    seen.insert(task.id);
                    auto previous = lastTaskStatus.find(task.id);
                    if (previous == lastTaskStatus.end()) {
                        lastTaskStatus[task.id] = task.status;
                        continue;
                    }
                    const pipensx::DownloadStatus prevStatus = previous->second;
                    if (prevStatus == task.status)
                        continue;
                    lastTaskStatus[task.id] = task.status;
                    if (task.status == pipensx::DownloadStatus::Completed)
                        brls::Application::notify(
                            tr("pipensx/notify/download_completed", task.name));
                    else if (task.status == pipensx::DownloadStatus::Installed)
                        brls::Application::notify(
                            tr("pipensx/notify/install_completed", task.name));
                    else if (task.status == pipensx::DownloadStatus::Error) {
                        const bool installFailed =
                            prevStatus == pipensx::DownloadStatus::Installing ||
                            prevStatus == pipensx::DownloadStatus::Committing ||
                            task.mode == pipensx::TransferMode::StreamInstall;
                        brls::Application::notify(tr(
                            installFailed ? "pipensx/notify/install_failed"
                                          : "pipensx/notify/download_failed",
                            task.name));
                    }
                }
                for (auto it = lastTaskStatus.begin();
                     it != lastTaskStatus.end();) {
                    if (seen.count(it->first) == 0)
                        it = lastTaskStatus.erase(it);
                    else
                        ++it;
                }
            }

            const uint64_t deployPollNowMs = now_ms();
            if (deployPollNowMs - lastDeployOfferPollMs >= 10000 &&
                !activeTransfer && !deployState.active() &&
                !deployOfferDialogOpen) {
                lastDeployOfferPollMs = deployPollNowMs;
                deploy.scheduleDeployOfferPoll();
            }
            if (!deployOfferDialogOpen) {
                auto offer = deploy.takePendingDeployOffer();
                if (offer) {
                    deployOfferDialogOpen = true;
                    const std::string offerId = offer->taskId;
                    const auto task = manager.snapshotUi(offerId);
                    const std::string name =
                        task ? task->name : offerId.substr(0, 8);
                    auto* dialog = new brls::Dialog(
                        tr("pipensx/deploy/offer_question", name));
                    dialog->addButton(
                        tr("pipensx/deploy/copy"),
                        [&deploy, offerId,
                         inspection = std::move(offer->inspection),
                         &deployOfferDialogOpen]() mutable {
                            deployOfferDialogOpen = false;
                            deploy.dismissDeployOffer(offerId);
                            // Dialog is still alive until this callback
                            // returns. Re-home focus onto the root activity so
                            // pushActivity does not onFocusLost a view that is
                            // about to be deleted with the dialog.
                            const auto stack =
                                brls::Application::getActivitiesStack();
                            if (!stack.empty())
                                brls::Application::giveFocus(
                                    stack.back()->getContentView());
                            if (!inspection.canStart() &&
                                inspection.problem !=
                                    pipensx::SwitchDeployProblem::Conflict &&
                                inspection.problem !=
                                    pipensx::SwitchDeployProblem::NoSpace &&
                                inspection.problem !=
                                    pipensx::SwitchDeployProblem::NoRam) {
                                brls::Application::notify(
                                    inspection.detail.empty()
                                        ? tr("pipensx/deploy/failed")
                                        : inspection.detail);
                                return;
                            }
                            brls::Application::pushActivity(
                                new pipensx::ui::SwitchDeployPreviewActivity(
                                    std::move(inspection), &deploy),
                                brls::TransitionAnimation::NONE);
                        });
                    dialog->addButton(
                        tr("pipensx/common/later"),
                        [&deploy, offerId, &deployOfferDialogOpen] {
                            deployOfferDialogOpen = false;
                            deploy.dismissDeployOffer(offerId);
                        });
                    dialog->open();
                }
            }

            // OLED burn-in guard: after five minutes without a button/touch,
            // cover the UI with a drifting black saver. Any input dismisses it
            // (including D-pad and touch) and resets the idle clock. Open state
            // is derived from the activity stack so a dismiss cannot desync a
            // bool and stack another saver on the next idle period.
            brls::ControllerState pad {};
            std::vector<brls::RawTouchState> touches;
            auto* input = brls::Application::getPlatform()->getInputManager();
            input->updateUnifiedControllerState(&pad);
            input->updateTouchStates(&touches);
            bool touched = false;
            for (const auto& touch : touches) {
                if (touch.pressed) {
                    touched = true;
                    break;
                }
            }
            const bool saverOpen = pipensx::ui::burnInSaverIsTop();
            if (pipensx::ui::controllerHasButtonDown(pad) || touched) {
                lastInputMs = now_ms();
                if (saverOpen)
                    brls::Application::popActivity(
                        brls::TransitionAnimation::NONE);
            } else if (!saverOpen &&
                       now_ms() - lastInputMs >= pipensx::ui::kBurnInIdleMs) {
                brls::Application::pushActivity(
                    new pipensx::ui::BurnInSaverActivity(),
                    brls::TransitionAnimation::NONE);
                lastInputMs = now_ms();
            }

            if (firstFrame) {
                startupStage("main loop running");
                deploy.scheduleDeployOfferPoll();
                if (updatePendingConfirmation) {
                    std::string error;
                    if (!launchUpdater.confirmInstalled(error))
                        diagnostic_error("update", "confirm", "error=%s",
                                         error.c_str());
                    else {
                        log_msg("[update] installed update confirmed\n");
                        brls::Application::notify(
                            tr("pipensx/settings/updated_to", PIPENSX_VERSION));
                    }
                }
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        // The startup title scan references `installed`, which lives on this
        // stack frame — join before anything here is torn down.
        if (installedScanner.thread.joinable())
            installedScanner.thread.join();
        deploy.shutdown();
        // The web server goes first: its threads call into manager, so they
        // must be joined before the manager dies.
        webServer.shutdown();
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
    // Borealis writes into the same handle log_close() owns; drop its pointer
    // before that handle goes away.
    brls::Logger::setLogOutput(nullptr);
    log_close();
    return 0;
}
