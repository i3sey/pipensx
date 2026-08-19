#pragma once

// Settings hub: a section rail on the left (General, Downloads, Source,
// Network, Catalog, Storage, System) and the selected section's panel on the
// right. Replaces the former long SettingsView list plus the Advanced,
// Storage Manager and Network Health sub-pages, whose logic now lives in the
// panels (settings_panels.hpp) — moved, not changed.
//
// Deliberately not a nested brls::TabFrame: the main MainFrame already folds
// its own sidebar by focus, and a second TabFrame fights it for lifecycle.

#include <atomic>
#include <functional>
#include <memory>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/update_service.hpp"
#include "app/web_server.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/first_run_view.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_panels.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class SettingsView : public brls::Box {
public:
    SettingsView(AppSettings* settings, DownloadManager* manager,
                 CatalogService* catalog, GameMetadataService* metadata,
                 InstalledTitleService* installed, UpdateService* updater = nullptr,
                 WebServer* webServer = nullptr,
                 std::function<void()> onMetadataRefreshed = {},
                 std::string ipAddress = {})
        : brls::Box(brls::Axis::ROW), settings_(settings), manager_(manager),
          onMetadataRefreshed_(std::move(onMetadataRefreshed)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setGrow(1);
        setAlignItems(brls::AlignItems::STRETCH);

        sidebar_ = new SettingsSidebar([this](SettingsSection section) {
            showSection(section);
        });
        addView(sidebar_);

        host_ = new brls::Box(brls::Axis::COLUMN);
        host_->setGrow(1);
        addView(host_);

        panels_[static_cast<size_t>(SettingsSection::General)] =
            new GeneralPanel(settings_);
        panels_[static_cast<size_t>(SettingsSection::Downloads)] =
            new DownloadsPanel(settings_, manager_);
        panels_[static_cast<size_t>(SettingsSection::Source)] =
            source_ = new SourcePanel(settings_,
                                     [this] { openDownloadSource(); });
        panels_[static_cast<size_t>(SettingsSection::Network)] =
            new NetworkPanel(settings_, manager_, webServer,
                             std::move(ipAddress));
        panels_[static_cast<size_t>(SettingsSection::Catalog)] =
            new CatalogPanel(settings_, catalog, metadata, alive_,
                             onMetadataRefreshed_);
        panels_[static_cast<size_t>(SettingsSection::Storage)] =
            new StoragePanel(manager, metadata, alive_);
        panels_[static_cast<size_t>(SettingsSection::System)] =
            new SystemPanel(settings_, manager, catalog, metadata,
                            installed, updater, alive_,
                            [this] { applyValues(); });

        for (size_t i = 0; i < kSettingsSectionCount; ++i) {
            host_->addView(panels_[i]);
            panels_[i]->setVisibility(i == 0 ? brls::Visibility::VISIBLE
                                             : brls::Visibility::GONE);
        }
        sidebar_->setActive(SettingsSection::General);
    }

    ~SettingsView() override {
        alive_->store(false);
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        // The console may have joined/left Wi-Fi, and the first-run
        // chooser / provider link screens are stacked activities above this
        // tab — re-read persisted state whenever the tab comes back.
        applyValues();
    }

    // Switch to a section as if its rail item had been focused (used by the
    // golden runner to pin the non-default panels).
    void selectSection(SettingsSection section) {
        showSection(section);
        brls::Application::giveFocus(sidebar_->item(section));
    }

private:
    void showSection(SettingsSection section) {
        if (section == activeSection_)
            return;
        activeSection_ = section;
        sidebar_->setActive(section);
        for (size_t i = 0; i < kSettingsSectionCount; ++i) {
            panels_[i]->setVisibility(
                i == static_cast<size_t>(section)
                    ? brls::Visibility::VISIBLE
                    : brls::Visibility::GONE);
        }
        panels_[static_cast<size_t>(section)]->onShown();
    }

    // Re-sync every panel's cells from persisted settings, and push the
    // settings that affect the engine down into the manager. Called after a
    // factory reset (System panel) and after a first-run chooser round-trip.
    void applyValues() {
        const AppSettingsData& values = settings_->get();
        for (size_t i = 0; i < kSettingsSectionCount; ++i)
            panels_[i]->applyValues();
        manager_->setTorrentingEnabled(values.torrentingEnabled);
        manager_->setTorboxApiKey(values.torboxApiKey);
        manager_->setTorrserverUrl(values.torrserverUrl);
        manager_->setRealdebridApiKey(values.realdebridApiKey);
        source_->refreshDetail();
    }

    // Opens the same chooser the app shows on first launch, then follows the
    // same post-choice step (the provider link screen) when the picked source
    // still has no key. B pops back to Settings without changing anything.
    void openDownloadSource() {
        auto alive = alive_;
        FirstRunView::push(settings_, manager_,
            [this, alive](DebridProviderKind provider, bool torrenting) {
                if (!alive->load())
                    return;
                applyValues();
                if (!torrenting &&
                    activeDebridKey(settings_->get()).empty())
                    DebridLinkView::push(settings_, manager_, provider);
            },
            /*lockBack=*/false);
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    std::function<void()> onMetadataRefreshed_;
    std::shared_ptr<std::atomic<bool>> alive_;
    SettingsSidebar* sidebar_ = nullptr;
    brls::Box* host_ = nullptr;
    SettingsPanel* panels_[kSettingsSectionCount] = {};
    SourcePanel* source_ = nullptr;
    SettingsSection activeSection_ = SettingsSection::General;
};

}  // namespace pipensx::ui
