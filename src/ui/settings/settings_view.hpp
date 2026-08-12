#pragma once

#include <atomic>
#include <ctime>
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
#include "app/web_server.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/web_qr.hpp"
#include "ui/debrid_ui.hpp"
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
                 ModIndexService* mods = nullptr, WebServer* webServer = nullptr)
        : brls::Box(brls::Axis::COLUMN), settings_(settings), manager_(manager),
          catalog_(catalog), metadata_(metadata), installed_(installed), updater_(updater),
          mods_(mods), webServer_(webServer),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        addSection(content, tr("pipensx/settings/section_general"));
        language_ = new brls::SelectorCell();
        language_->init(tr("pipensx/settings/language"),
            {tr("pipensx/settings/language_auto"),
             tr("pipensx/settings/language_en"),
             tr("pipensx/settings/language_ru"),
             tr("pipensx/settings/language_pt_br"),
             tr("pipensx/settings/language_fr"),
             tr("pipensx/settings/language_es"),
             tr("pipensx/settings/language_zh")},
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
            tr("pipensx/settings/check_update_detail", PIPENSX_VERSION),
            [this] { checkForUpdateNow(); });
        if (updater_ && updater_->checkCompleted())
            markUpdateChecked();
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

        catalogSource_ = actionCell(tr("pipensx/settings/catalog_source"), "",
            [this] { editCatalogSource(); });
        content->addView(catalogSource_);
        refreshCatalogSourceDetail();

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
                if (!persist(values, "stream_selection")) {
                    streamSelection_->setSelection(
                        previous == StreamSelection::PackagesOnly ? 1 : 0,
                        true);
                    return;
                }
                if (webServer_)
                    webServer_->setStreamSelection(values.streamSelection);
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
                if (manager_)
                    manager_->setInstallTarget(
                        installTargetFor(values.installLocation));
            });
        content->addView(installLocation_);

        maxActiveDownloads_ = new brls::SelectorCell();
        maxActiveDownloads_->init(tr("pipensx/settings/max_active_downloads"),
            {"1", "2", "3", "4"},
            static_cast<int>(settings_->get().maxActiveDownloads) - 1,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                uint32_t previous = values.maxActiveDownloads;
                values.maxActiveDownloads =
                    pipensx::clampMaxActiveDownloads(
                        static_cast<uint64_t>(selected) + 1);
                if (!persist(values, "max_active_downloads")) {
                    maxActiveDownloads_->setSelection(
                        static_cast<int>(previous) - 1, true);
                    return;
                }
                if (manager_)
                    manager_->setMaxActiveDownloads(
                        values.maxActiveDownloads);
            });
        content->addView(maxActiveDownloads_);

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

        addSection(content, tr("pipensx/settings/section_debrid"));
        torrenting_ = new brls::BooleanCell();
        torrenting_->init(tr("pipensx/settings/torrenting"),
            settings_->get().torrentingEnabled,
            [this](bool enabled) { setTorrenting(enabled); });
        content->addView(torrenting_);

        debridProvider_ = new brls::SelectorCell();
        debridProvider_->init(tr("pipensx/settings/debrid_provider"),
            {"TorBox", "TorrServer", "Real-Debrid"},
            settings_->get().debridProvider == DebridProviderKind::TorBox
                ? 0
                : settings_->get().debridProvider == DebridProviderKind::RealDebrid
                ? 2 : 1,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                const DebridProviderKind previous = values.debridProvider;
                values.debridProvider = selected == 1
                    ? DebridProviderKind::TorrServer
                    : selected == 2
                    ? DebridProviderKind::RealDebrid
                    : DebridProviderKind::TorBox;
                if (!persist(values, "debrid_provider"))
                    debridProvider_->setSelection(
                        previous == DebridProviderKind::TorrServer ? 1
                        : previous == DebridProviderKind::RealDebrid ? 2 : 0,
                        true);
                refreshDebridLinkDetail();
            });
        content->addView(debridProvider_);

        debridLink_ = actionCell(tr("pipensx/settings/debrid_link"), "",
            [this] {
                DebridLinkView::push(settings_, manager_,
                                     settings_->get().debridProvider);
            });
        // Named so the golden runner can focus it: the debrid section sits
        // far below the fold, and scrolling to it by counting d-pad presses
        // would break every time a row above it is added.
        debridLink_->setId("settings-debrid-link");
        content->addView(debridLink_);
        refreshDebridLinkDetail();

        addSection(content, tr("pipensx/settings/section_web"));
        webToggle_ = new brls::BooleanCell();
        webToggle_->init(tr("pipensx/settings/web_toggle"),
            settings_->get().webServerEnabled,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.webServerEnabled;
                values.webServerEnabled = enabled;
                if (!persist(values, "web_server")) {
                    webToggle_->setOn(previous, false);
                    return;
                }
                if (webServer_) {
                    if (enabled) {
                        if (!webServer_->start())
                            brls::Application::notify(
                                tr("pipensx/settings/web_start_failed"));
                    } else {
                        webServer_->stop();
                    }
                }
                updateWebCells();
            });
        content->addView(webToggle_);
        webAddress_ = actionCell(tr("pipensx/settings/web_address"),
            "", [this] { showWebQr(); });
        content->addView(webAddress_);
        webPin_ = actionCell(tr("pipensx/settings/web_pin"),
            "", [this] { editWebPin(); });
        content->addView(webPin_);
        updateWebCells();

        content->addView(actionCell(tr("pipensx/settings/advanced"),
            tr("pipensx/settings/advanced_detail"),
            [this] { openAdvanced(); }));

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

    ~SettingsView() override {
        alive_->store(false);
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        // The console may have joined/left Wi-Fi since the last visit.
        updateWebCells();
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

    std::string webAddressText() const {
        if (!settings_->get().webServerEnabled)
            return tr("pipensx/settings/web_disabled");
        std::string url = webCompanionUrl(webServer_, true);
        return url.empty() ? tr("pipensx/settings/web_address_none") : url;
    }

    void updateWebCells() {
        if (webAddress_)
            webAddress_->setDetailText(webAddressText());
        if (webPin_)
            webPin_->setDetailText(
                settings_->get().webServerPin.empty() ? "——" : "••••");
    }

    void showWebQr() {
        const std::string url = webAddressText();
        if (url.rfind("http://", 0) != 0) {
            brls::Application::notify(url);
            return;
        }
        showWebQrDialog(url, settings_->get().webServerPin);
    }

    void editWebPin() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidWebPin(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/web_pin_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.webServerPin = text;
                if (!persist(values, "web_pin"))
                    return;
                if (webServer_)
                    webServer_->setPin(settings_->get().webServerPin);
                updateWebCells();
            },
            tr("pipensx/settings/web_pin"),
            tr("pipensx/settings/web_pin_detail"), 8,
            settings_->get().webServerPin, brls::KEYBOARD_DISABLE_NONE);
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
        if (catalog) {
            values.lastCatalogRefreshMs = now;
            values.lastCatalogRefreshWallSec =
                static_cast<uint64_t>(time(nullptr));
        }
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
        const std::string catalogSourceUrl =
            effectiveCatalogSourceUrl(settings_->get().catalogSourceUrl);
        brls::async([this, alive, catalog, catalogSourceUrl,
                     onDone = std::move(onDone)]() mutable {
            std::vector<CatalogEntry> entries;
            std::string error;
            bool ok = catalog->fetchLatest(entries, error, catalogSourceUrl);
            brls::sync([this, alive, ok, entries = std::move(entries),
                        error = std::move(error), catalogSourceUrl,
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
                catalog_->adopt(std::move(entries), catalogSourceUrl);
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
                markUpdateChecked();
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

    void markUpdateChecked() {
        updateAction_->setTextColor(theme::accent());
        updateAction_->setDetailTextColor(theme::accent());
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

    // Turning torrenting ON is the risky direction, so it goes through a
    // confirmation; turning it off needs none. The toggle is snapped back to
    // false first so the cell never shows "on" while the dialog is up.
    void setTorrenting(bool enabled) {
        const bool previous = settings_->get().torrentingEnabled;
        if (enabled && !previous) {
            torrenting_->setOn(false, false);
            auto* dialog = new brls::Dialog(
                tr("pipensx/settings/torrenting_warning"));
            dialog->addButton(tr("pipensx/settings/torrenting_enable"),
                [this] {
                    AppSettingsData values = settings_->get();
                    values.torrentingEnabled = true;
                    if (persist(values, "torrenting")) {
                        manager_->setTorrentingEnabled(true);
                        torrenting_->setOn(true, false);
                    }
                });
            dialog->addButton(tr("pipensx/common/cancel"), [] {});
            dialog->open();
            return;
        }
        AppSettingsData values = settings_->get();
        values.torrentingEnabled = enabled;
        if (!persist(values, "torrenting")) {
            torrenting_->setOn(previous, false);
            return;
        }
        manager_->setTorrentingEnabled(enabled);
    }

    void refreshCatalogSourceDetail() {
        const std::string& url = settings_->get().catalogSourceUrl;
        catalogSource_->setDetailText(
            url.empty() ? tr("pipensx/settings/catalog_source_default") : url);
    }

    void editCatalogSource() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidCatalogSourceUrl(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/catalog_source_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.catalogSourceUrl = text;
                if (!persist(values, "catalog_source_url"))
                    return;
                refreshCatalogSourceDetail();
            },
            tr("pipensx/settings/catalog_source"),
            tr("pipensx/settings/catalog_source_detail"), 512,
            settings_->get().catalogSourceUrl, brls::KEYBOARD_DISABLE_NONE);
    }

    void refreshDebridLinkDetail() {
        if (!debridLink_)
            return;
        const AppSettingsData& values = settings_->get();
        const char* provider = debridProviderName(values.debridProvider);
        // Spelled out rather than picking the key with a ternary: the i18n
        // checker only sees keys that appear as a literal first argument.
        debridLink_->setDetailText(
            activeDebridKey(values).empty()
                ? tr("pipensx/settings/debrid_not_linked", provider)
                : tr("pipensx/settings/debrid_linked", provider));
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
        refreshCatalogSourceDetail();
        streamSelection_->setSelection(
            values.streamSelection == StreamSelection::PackagesOnly ? 1 : 0,
            true);
        installLocation_->setSelection(
            values.installLocation == InstallLocation::SystemMemory ? 1 : 0,
            true);
        if (manager_)
            manager_->setInstallTarget(
                installTargetFor(values.installLocation));
        maxActiveDownloads_->setSelection(
            static_cast<int>(values.maxActiveDownloads) - 1, true);
        showCompleted_->setOn(values.showCompletedDownloads, false);
        checkForUpdates_->setOn(values.checkForUpdatesOnLaunch, false);
        webToggle_->setOn(values.webServerEnabled, false);
        updateWebCells();
        torrenting_->setOn(values.torrentingEnabled, false);
        debridProvider_->setSelection(
            values.debridProvider == DebridProviderKind::TorrServer ? 1
            : values.debridProvider == DebridProviderKind::RealDebrid ? 2 : 0,
            true);
        manager_->setTorrentingEnabled(values.torrentingEnabled);
        manager_->setTorboxApiKey(values.torboxApiKey);
        manager_->setTorrserverUrl(values.torrserverUrl);
        manager_->setRealdebridApiKey(values.realdebridApiKey);
        refreshDebridLinkDetail();
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    UpdateService* updater_;
    ModIndexService* mods_;
    WebServer* webServer_;
    std::shared_ptr<std::atomic<bool>> alive_;
    brls::SelectorCell* language_ = nullptr;
    brls::SelectorCell* catalogFilter_ = nullptr;
    brls::BooleanCell* refreshCatalog_ = nullptr;
    brls::DetailCell* catalogSource_ = nullptr;
    brls::BooleanCell* checkForUpdates_ = nullptr;
    brls::DetailCell* updateAction_ = nullptr;
    brls::SelectorCell* streamSelection_ = nullptr;
    brls::SelectorCell* installLocation_ = nullptr;
    brls::SelectorCell* maxActiveDownloads_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
    brls::BooleanCell* webToggle_ = nullptr;
    brls::DetailCell* webAddress_ = nullptr;
    brls::DetailCell* webPin_ = nullptr;
    brls::BooleanCell* torrenting_ = nullptr;
    brls::SelectorCell* debridProvider_ = nullptr;
    brls::DetailCell* debridLink_ = nullptr;
    bool refreshInFlight_ = false;
    bool updateInFlight_ = false;
};

}  // namespace pipensx::ui
