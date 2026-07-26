#pragma once

// Shared bits for surfacing the web companion address in the UI: the URL
// resolver and the QR dialog, used by the Settings row, the global Minus
// action on the main frame, and the sidebar footer status line.

#include <string>

#include <borealis.hpp>

#include "app/web_server.hpp"
#include "ui/common/qr_view.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// "http://<ip>:<port>" when the server is up and the console has an address,
// "" otherwise. A null server (golden runner) yields a fixed fake address so
// screenshot baselines never contain the host's real IP.
inline std::string webCompanionUrl(pipensx::WebServer* server, bool enabled) {
    if (!enabled)
        return "";
    if (!server)
        return "http://192.168.1.2:8080";
    if (!server->running())
        return "";
    std::string ip = brls::Application::getPlatform()->getIpAddress();
    if (ip.empty() || ip == "0.0.0.0")
        return "";
    return "http://" + ip + ":" + std::to_string(server->boundPort());
}

// Big scannable QR + the plain URL underneath. The QR carries the PIN so a
// scan lands authenticated; the SPA stores it and strips it from the URL.
inline void showWebQrDialog(const std::string& url, const std::string& pin) {
    std::string qrUrl = url;
    if (!pin.empty())
        qrUrl += "/?pin=" + pin;
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setPadding(28, 28, 28, 28);
    auto* qr = new QrCodeView(qrUrl);
    qr->setMarginBottom(16);
    box->addView(qr);
    auto* label = new brls::Label();
    label->setText(url);
    label->setFontSize(theme::kFontSmall);
    label->setTextColor(theme::textSecondary());
    box->addView(label);
    auto* hint = new brls::Label();
    hint->setText(tr("pipensx/settings/web_qr_hint"));
    hint->setFontSize(theme::kFontCaption);
    hint->setTextColor(theme::textTertiary());
    hint->setMarginTop(8);
    box->addView(hint);
    auto* dialog = new brls::Dialog(box);
    dialog->addButton(tr("pipensx/common/ok"), [] {});
    dialog->open();
}

}  // namespace pipensx::ui
