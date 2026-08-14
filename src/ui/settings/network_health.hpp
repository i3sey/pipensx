#pragma once

#include <ctime>
#include <string>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

extern "C" {
#include "../core/dht.h"
}

namespace pipensx::ui {

// Compact diagnostics: connectivity, DHT and provider state. Rows are
// built once; values refresh on appear and on a slow timer.
class NetworkHealthActivity : public brls::Activity {
public:
    NetworkHealthActivity(DownloadManager* manager, AppSettings* settings,
                          std::string ipAddress = {})
        : manager_(manager), settings_(settings),
          ipAddress_(std::move(ipAddress)) {
        if (ipAddress_.empty())
            ipAddress_ = brls::Application::getPlatform()->getIpAddress();
        buildContent();
        timer_.setCallback([this] { refresh(); });
        timer_.start(1000);
    }

    ~NetworkHealthActivity() override { timer_.stop(); }

    brls::View* createContentView() override { return frame_; }

    void willAppear(bool resetState) override {
        brls::Activity::willAppear(resetState);
        refresh();
    }

private:
    brls::Label* addRow(brls::Box* content, const std::string& label,
                        bool focusable) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(focusable);
        if (focusable)
            focusRow_ = row;
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginBottom(8);

        auto* name = new brls::Label();
        name->setSingleLine(true);
        name->setAutoAnimate(false);
        name->setGrow(1);
        name->setFontSize(18);
        name->setTextColor(theme::textSecondary());
        name->setText(label);
        row->addView(name);

        auto* valueLabel = new brls::Label();
        valueLabel->setSingleLine(true);
        valueLabel->setAutoAnimate(false);
        valueLabel->setFontSize(18);
        row->addView(valueLabel);
        content->addView(row);
        return valueLabel;
    }

    void setValue(brls::Label* label, const std::string& text,
                  NVGcolor color) {
        setTextIfChanged(label, text);
        label->setTextColor(color);
    }

    static std::string catalogAge(uint64_t wallSec) {
        if (wallSec == 0)
            return tr("pipensx/diag/never");
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        const uint64_t age = now > wallSec ? now - wallSec : 0;
        if (age < 3600)
            return tr("pipensx/diag/updated_m", age / 60);
        return tr("pipensx/diag/updated_h", age / 3600);
    }

    void refresh() {
        const bool online = !ipAddress_.empty() && ipAddress_ != "-";
        setValue(internet_,
                 online ? tr("pipensx/diag/connected")
                        : tr("pipensx/diag/offline"),
                 online ? theme::success() : theme::error());

        int dhtGood = 0;
        int dhtDubious = 0;
        const bool dhtOn = dht_shared_running();
        if (dhtOn)
            dht_shared_nodes(&dhtGood, &dhtDubious);
        if (!dhtOn) {
            setValue(dht_, tr("pipensx/diag/dht_off"), theme::textSecondary());
        } else if (dhtGood > 0) {
            setValue(dht_, tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                     theme::success());
        } else if (dhtDubious > 0) {
            setValue(dht_, tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                     theme::warning());
        } else {
            setValue(dht_, tr("pipensx/diag/dht_bootstrapping"),
                     theme::warning());
        }

        uint32_t peers = 0;
        for (const DownloadTask& task : manager_->snapshot())
            if (task.status == DownloadStatus::Downloading)
                peers += task.peers;
        setValue(peers_, tr("pipensx/diag/peers_n", peers),
                 peers > 0 ? theme::success() : theme::textSecondary());

        const AppSettingsData values = settings_->get();
        setValue(torbox_,
                 values.torboxApiKey.empty() ? tr("pipensx/diag/not_linked")
                                             : tr("pipensx/diag/linked"),
                 values.torboxApiKey.empty() ? theme::textSecondary()
                                             : theme::success());
        setValue(torrserver_,
                 values.torrserverUrl.empty()
                     ? tr("pipensx/diag/not_configured")
                     : values.torrserverUrl,
                 values.torrserverUrl.empty() ? theme::textSecondary()
                                              : theme::textPrimary());
        setValue(realdebrid_,
                 values.realdebridApiKey.empty()
                     ? tr("pipensx/diag/not_linked")
                     : tr("pipensx/diag/linked"),
                 values.realdebridApiKey.empty() ? theme::textSecondary()
                                                 : theme::success());
        setValue(proxy_,
                 values.proxyUrl.empty() ? tr("pipensx/diag/disabled")
                                         : tr("pipensx/diag/enabled"),
                 values.proxyUrl.empty() ? theme::textSecondary()
                                         : theme::accent());
        setValue(catalog_, catalogAge(values.lastCatalogRefreshWallSec),
                 theme::textSecondary());
    }

    void buildContent() {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        internet_ = addRow(content, tr("pipensx/diag/internet"), true);
        dht_ = addRow(content, tr("pipensx/diag/dht"), false);
        peers_ = addRow(content, tr("pipensx/diag/peers"), false);
        torbox_ = addRow(content, tr("pipensx/diag/torbox"), false);
        torrserver_ = addRow(content, tr("pipensx/diag/torrserver"), false);
        realdebrid_ = addRow(content, tr("pipensx/diag/realdebrid"), false);
        proxy_ = addRow(content, tr("pipensx/diag/proxy"), false);
        catalog_ = addRow(content, tr("pipensx/diag/catalog"), false);
        refresh();

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        frame_ = new brls::AppletFrame(scroll);
        frame_->setTitle(tr("pipensx/diag/title"));
        if (focusRow_)
            brls::Application::giveFocus(focusRow_);
    }

    DownloadManager* manager_;
    AppSettings* settings_;
    std::string ipAddress_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Box* focusRow_ = nullptr;
    brls::RepeatingTimer timer_;
    brls::Label* internet_ = nullptr;
    brls::Label* dht_ = nullptr;
    brls::Label* peers_ = nullptr;
    brls::Label* torbox_ = nullptr;
    brls::Label* torrserver_ = nullptr;
    brls::Label* realdebrid_ = nullptr;
    brls::Label* proxy_ = nullptr;
    brls::Label* catalog_ = nullptr;
};

} // namespace pipensx::ui
