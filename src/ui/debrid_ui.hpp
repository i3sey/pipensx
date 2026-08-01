#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "app/torbox_pairing_server.hpp"
#include "app/torbox_provider.hpp"
#include "app/torrserver_provider.hpp"
#include "ui/common/qr_view.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline const std::string& activeDebridKey(const AppSettingsData& values) {
    return values.debridProvider == DebridProviderKind::TorrServer
        ? values.torrserverUrl : values.torboxApiKey;
}

inline std::unique_ptr<DebridProvider> makeDebridProvider(
    DebridProviderKind kind, const std::string& key) {
    if (kind == DebridProviderKind::TorrServer)
        return std::unique_ptr<DebridProvider>(new TorrserverProvider(key));
    return std::unique_ptr<DebridProvider>(new TorboxProvider(key));
}

// Brand names stay untranslated — they are the same in every locale, and the
// catalog strings take them as a format argument.
inline const char* debridProviderName(DebridProviderKind kind) {
    return kind == DebridProviderKind::TorrServer ? "TorrServer" : "TorBox";
}

// "http://<ip>:8424/" while the console has a LAN address, "" otherwise. The
// pairing server binds its own port rather than riding on the web companion:
// pairing is only up while this screen is, and the companion may be disabled.
inline std::string debridPairingUrl() {
    const std::string ip = brls::Application::getPlatform()->getIpAddress();
    if (ip.empty() || ip == "0.0.0.0")
        return "";
    return "http://" + ip + ":" + std::to_string(kTorboxPairingPort) + "/";
}

// Links a debrid account, either by typing the key on the console or by
// posting it from a phone on the same LAN (scan the QR, fill the form). The
// pairing server only runs while this screen is open.
class DebridLinkView : public brls::Box {
public:
    DebridLinkView(AppSettings* settings, DownloadManager* manager,
                   DebridProviderKind provider)
        : brls::Box(brls::Axis::COLUMN), settings_(settings), manager_(manager),
          provider_(provider), alive_(std::make_shared<std::atomic<bool>>(true)) {
        setPadding(30, 40, 30, 40);
        auto* explanation = new brls::Label();
        // Spelled out rather than picking the key with a ternary: the i18n
        // checker only sees keys that appear as a literal first argument.
        explanation->setText(provider_ == DebridProviderKind::TorrServer
            ? tr("pipensx/debrid/link_hint_url")
            : tr("pipensx/debrid/link_hint", debridProviderName(provider_)));
        explanation->setFontSize(theme::kFontSmall);
        explanation->setTextColor(theme::textSecondary());
        explanation->setMarginBottom(18);
        addView(explanation);

        const std::string pairingUrl = debridPairingUrl();
        if (!pairingUrl.empty()) {
            server_ = std::make_unique<TorboxPairingServer>(
                kTorboxPairingPort,
                [provider](const std::string& key, std::string& error) {
                    return makeDebridProvider(provider, key)->validate(error);
                },
                provider == DebridProviderKind::TorrServer
                    ? "Paste the address of your TorrServer, for example "
                      "http://192.168.1.10:8090."
                    : kTorboxPairingHint);
            std::string error;
            if (server_->start(error)) {
                auto* qr = new QrCodeView(pairingUrl);
                qr->setMarginBottom(12);
                addView(qr);
                auto* url = new brls::Label();
                url->setText(pairingUrl);
                url->setFontSize(theme::kFontSmall);
                url->setTextColor(theme::accent());
                url->setMarginBottom(12);
                addView(url);
                timer_.setCallback([this] { pollPairing(); });
                timer_.start(500);
            } else {
                // No QR without a listener behind it — a code that leads
                // nowhere is worse than no code.
                server_.reset();
            }
        }

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontSmall);
        status_->setTextColor(theme::textSecondary());
        status_->setMarginBottom(12);
        addView(status_);

        auto* enter = new brls::DetailCell();
        enter->setText(tr("pipensx/debrid/enter_key"));
        enter->setDetailText(tr("pipensx/debrid/enter_key_detail"));
        enter->registerClickAction([this](brls::View*) {
            openKeyboard();
            return true;
        });
        addView(enter);

        unlink_ = new brls::DetailCell();
        unlink_->setText(tr("pipensx/debrid/unlink"));
        unlink_->setDetailText(tr("pipensx/debrid/unlink_detail"));
        unlink_->registerClickAction([this](brls::View*) {
            saveKey({});
            return true;
        });
        addView(unlink_);
        refresh();
    }

    ~DebridLinkView() override {
        alive_->store(false);
        timer_.stop();
        if (server_)
            server_->stop();
    }

    static void push(AppSettings* settings, DownloadManager* manager,
                     DebridProviderKind provider) {
        auto* frame = new brls::AppletFrame(
            new DebridLinkView(settings, manager, provider));
        frame->setTitle(tr("pipensx/debrid/link_title",
                           debridProviderName(provider)));
        brls::Application::pushActivity(new brls::Activity(frame));
    }

private:
    void pollPairing() {
        if (!server_ || !server_->keyAccepted())
            return;
        const std::string key = server_->acceptedKey();
        server_->stop();
        server_.reset();
        saveKey(key);
    }

    // The stored key for the provider this screen links — an API key for
    // TorBox, the server address for TorrServer.
    const std::string& activeKey() const {
        return provider_ == DebridProviderKind::TorrServer
            ? settings_->get().torrserverUrl : settings_->get().torboxApiKey;
    }

    void refresh() {
        const std::string& key = activeKey();
        status_->setText(key.empty() ? tr("pipensx/debrid/not_linked")
                                     : tr("pipensx/debrid/linked"));
        unlink_->setVisibility(key.empty() ? brls::Visibility::GONE
                                           : brls::Visibility::VISIBLE);
    }

    void openKeyboard() {
        const std::string prompt =
            provider_ == DebridProviderKind::TorrServer
                ? tr("pipensx/debrid/keyboard_prompt_url")
                : tr("pipensx/debrid/keyboard_prompt",
                     debridProviderName(provider_));
        brls::Application::getImeManager()->openForText(
            [this](std::string text) { validate(std::move(text)); },
            // Prefilled with what is stored: an address gets corrected far
            // more often than it gets typed from scratch.
            prompt, "", 128, activeKey(), brls::KEYBOARD_DISABLE_NONE);
    }

    void validate(std::string text) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        const size_t last = text.find_last_not_of(" \t\r\n");
        std::string key = first == std::string::npos
            ? std::string() : text.substr(first, last - first + 1);
        if (key.empty()) {
            status_->setText(tr("pipensx/debrid/no_key"));
            return;
        }
        status_->setText(tr("pipensx/debrid/validating"));
        auto alive = alive_;
        const DebridProviderKind provider = provider_;
        std::thread([this, alive, provider, key] {
            std::string error;
            bool ok = makeDebridProvider(provider, key)->validate(error);
            brls::sync([this, alive, ok, key, error] {
                if (!alive->load())
                    return;
                if (ok)
                    saveKey(key);
                else
                    status_->setText(error.empty()
                        ? tr("pipensx/debrid/rejected") : error);
            });
        }).detach();
    }

    void saveKey(const std::string& typed) {
        // "192.168.1.10:8090" is what a user types; the provider needs a URL.
        const std::string key =
            provider_ == DebridProviderKind::TorrServer
                ? TorrserverProvider::normalizeBaseUrl(typed) : typed;
        AppSettingsData values = settings_->get();
        if (provider_ == DebridProviderKind::TorrServer)
            values.torrserverUrl = key;
        else
            values.torboxApiKey = key;
        std::string error;
        if (!settings_->update(values, error)) {
            brls::Application::notify(error);
            return;
        }
        if (provider_ == DebridProviderKind::TorrServer)
            manager_->setTorrserverUrl(key);
        else
            manager_->setTorboxApiKey(key);
        brls::Application::notify(key.empty()
            ? tr("pipensx/debrid/unlinked_notify")
            : tr("pipensx/debrid/linked_notify"));
        refresh();
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    DebridProviderKind provider_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::unique_ptr<TorboxPairingServer> server_;
    brls::RepeatingTimer timer_;
    brls::Label* status_ = nullptr;
    brls::DetailCell* unlink_ = nullptr;
};

inline void removeDebridTransferAsync(DebridProviderKind provider,
                                      std::string key, std::string id) {
    if (key.empty() || id.empty())
        return;
    std::thread([provider, key, id] {
        std::string ignored;
        makeDebridProvider(provider, key)->remove(id, ignored);
    }).detach();
}

inline bool ensureDebridLinked(AppSettings* settings,
                               DownloadManager* manager) {
    if (!settings || !activeDebridKey(settings->get()).empty())
        return true;
    const DebridProviderKind provider = settings->get().debridProvider;
    auto* dialog = new brls::Dialog(tr("pipensx/debrid/needs_account"));
    dialog->addButton(tr("pipensx/debrid/link_now"),
        [settings, manager, provider] {
            DebridLinkView::push(settings, manager, provider);
        });
    dialog->addButton(tr("pipensx/common/cancel"), [] {});
    dialog->open();
    return false;
}

inline bool debridModeActive(const AppSettings* settings) {
    return settings && !settings->get().torrentingEnabled;
}

inline void showFirstRunChoice(AppSettings* settings,
                               DownloadManager* manager) {
    if (!settings || settings->get().firstRunCompleted)
        return;
    auto finish = [settings, manager](DebridProviderKind provider,
                                      bool torrenting) {
        AppSettingsData values = settings->get();
        values.debridProvider = provider;
        values.torrentingEnabled = torrenting;
        values.firstRunCompleted = true;
        std::string error;
        if (!settings->update(values, error)) {
            brls::Application::notify(error);
            return;
        }
        manager->setTorrentingEnabled(torrenting);
        if (!torrenting)
            DebridLinkView::push(settings, manager, provider);
    };
    auto* dialog = new brls::Dialog(tr("pipensx/debrid/first_run"));
    dialog->addButton(tr("pipensx/debrid/first_run_torbox"), [finish] {
        finish(DebridProviderKind::TorBox, false);
    });
    dialog->addButton(tr("pipensx/debrid/first_run_torrserver"), [finish] {
        finish(DebridProviderKind::TorrServer, false);
    });
    dialog->addButton(tr("pipensx/debrid/first_run_torrent"), [finish] {
        finish(DebridProviderKind::TorBox, true);
    });
    dialog->open();
}

}  // namespace pipensx::ui
