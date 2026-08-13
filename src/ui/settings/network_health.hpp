#pragma once

#include <ctime>
#include <string>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

extern "C" {
#include "../core/dht.h"
}

namespace pipensx::ui {

// Network Health: a compact diagnostics screen that shows the current
// connectivity, DHT and provider state without reading logs. Values are
// collected once at construction; the screen is intentionally read-only.
class NetworkHealthActivity : public brls::Activity {
public:
    NetworkHealthActivity(DownloadManager* manager, AppSettings* settings,
                          std::string ipAddress = {})
        : manager_(manager), settings_(settings) {
        if (ipAddress.empty())
            ipAddress = brls::Application::getPlatform()->getIpAddress();
        buildContent(ipAddress);
    }

    brls::View* createContentView() override { return frame_; }

private:
    void buildContent(const std::string& ipAddress) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        const AppSettingsData values = settings_->get();
        const bool online = !ipAddress.empty() && ipAddress != "-";

        int dhtGood = 0;
        int dhtDubious = 0;
        dht_shared_nodes(&dhtGood, &dhtDubious);

        uint32_t peers = 0;
        for (const DownloadTask& task : manager_->snapshot())
            if (task.status == DownloadStatus::Downloading)
                peers += task.peers;

        addRow(content, tr("pipensx/diag/internet"),
               online ? tr("pipensx/diag/connected")
                      : tr("pipensx/diag/offline"),
               online ? theme::success() : theme::error());

        if (dhtGood || dhtDubious)
            addRow(content, tr("pipensx/diag/dht"),
                   tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                   theme::success());
        else
            addRow(content, tr("pipensx/diag/dht"),
                   tr("pipensx/diag/dht_off"), theme::textSecondary());

        addRow(content, tr("pipensx/diag/peers"),
               tr("pipensx/diag/peers_n", peers),
               peers > 0 ? theme::success() : theme::textSecondary());

        addRow(content, tr("pipensx/diag/torbox"),
               values.torboxApiKey.empty()
                   ? tr("pipensx/diag/not_linked")
                   : tr("pipensx/diag/linked"),
               values.torboxApiKey.empty() ? theme::textSecondary()
                                           : theme::success());

        addRow(content, tr("pipensx/diag/torrserver"),
               values.torrserverUrl.empty()
                   ? tr("pipensx/diag/not_configured")
                   : values.torrserverUrl,
               values.torrserverUrl.empty() ? theme::textSecondary()
                                            : theme::textPrimary());

        addRow(content, tr("pipensx/diag/realdebrid"),
               values.realdebridApiKey.empty()
                   ? tr("pipensx/diag/not_linked")
                   : tr("pipensx/diag/linked"),
               values.realdebridApiKey.empty() ? theme::textSecondary()
                                               : theme::success());

        addRow(content, tr("pipensx/diag/proxy"),
               values.proxyUrl.empty() ? tr("pipensx/diag/disabled")
                                       : tr("pipensx/diag/enabled"),
               values.proxyUrl.empty() ? theme::textSecondary()
                                       : theme::accent());

        addRow(content, tr("pipensx/diag/catalog"),
               catalogAge(values.lastCatalogRefreshWallSec),
               theme::textSecondary());

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        frame_ = new brls::AppletFrame(scroll);
        frame_->setTitle(tr("pipensx/diag/title"));
    }

    void addRow(brls::Box* content, const std::string& label,
                const std::string& value, NVGcolor valueColor) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(false);
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
        valueLabel->setTextColor(valueColor);
        valueLabel->setText(value);
        row->addView(valueLabel);

        content->addView(row);
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

    DownloadManager* manager_;
    AppSettings* settings_;
    brls::AppletFrame* frame_ = nullptr;
};

} // namespace pipensx::ui
