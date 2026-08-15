#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/bug_report.hpp"
#include "app/curl_https.hpp"
#include "app/download_manager.hpp"
#include "app/torbox_pairing_server.hpp"
#include "app/torbox_provider.hpp"
#include "app/torrserver_provider.hpp"
#include "app/realdebrid_provider.hpp"
#include "ui/common/qr_view.hpp"
#include "ui/common/setup_summary_panel.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline const std::string& activeDebridKey(const AppSettingsData& values) {
    if (values.debridProvider == DebridProviderKind::TorrServer)
        return values.torrserverUrl;
    if (values.debridProvider == DebridProviderKind::RealDebrid)
        return values.realdebridApiKey;
    return values.torboxApiKey;
}

inline std::unique_ptr<DebridProvider> makeDebridProvider(
    DebridProviderKind kind, const std::string& key) {
    if (kind == DebridProviderKind::TorrServer)
        return std::unique_ptr<DebridProvider>(new TorrserverProvider(key));
    if (kind == DebridProviderKind::RealDebrid)
        return std::unique_ptr<DebridProvider>(new RealdebridProvider(key));
    return std::unique_ptr<DebridProvider>(new TorboxProvider(key));
}

inline const char* debridProviderName(DebridProviderKind kind) {
    if (kind == DebridProviderKind::TorrServer)
        return "TorrServer";
    if (kind == DebridProviderKind::RealDebrid)
        return "Real-Debrid";
    return "TorBox";
}

// User-facing validate failure: SSL cert problems get an explicit clock hint
// (CAINFO may still fail when the console date is wrong).
inline std::string formatDebridValidateError(const std::string& error) {
    if (isSslCertificateErrorMessage(error))
        return tr("pipensx/debrid/ssl_cert_hint");
    if (error.empty())
        return tr("pipensx/debrid/rejected");
    constexpr size_t kMax = 160;
    if (error.size() <= kMax)
        return error;
    return error.substr(0, kMax - 1) + "…";
}

inline std::string debridPairingUrl(const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0")
        return "";
    return "http://" + ip + ":" + std::to_string(kTorboxPairingPort) + "/";
}

inline std::string debridPairingUrl() {
    return debridPairingUrl(
        brls::Application::getPlatform()->getIpAddress());
}

struct DebridLinkFixture {
    SetupSummaryFixture summary;
    bool pairingAvailable = false;
    bool validationSucceeded = false;
};

class DebridLinkView : public brls::Box {
public:
    DebridLinkView(AppSettings* settings, DownloadManager* manager,
                   DebridProviderKind provider,
                   std::optional<DebridLinkFixture> fixture = std::nullopt)
        : brls::Box(brls::Axis::ROW), settings_(settings), manager_(manager),
          provider_(provider), alive_(std::make_shared<std::atomic<bool>>(true)) {
        setPadding(24, 40, 24, 40);

        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidthPercentage(54);
        left->setShrink(0);
        left->setMarginRight(24);

        auto* explanation = new brls::Label();
        explanation->setText(provider_ == DebridProviderKind::TorrServer
            ? tr("pipensx/debrid/link_hint_url")
            : tr("pipensx/debrid/link_hint", debridProviderName(provider_)));
        explanation->setFontSize(theme::kFontSmall);
        explanation->setTextColor(theme::textSecondary());
        explanation->setSingleLine(false);
        explanation->setMarginBottom(16);
        left->addView(explanation);

        const std::string ip = fixture
            ? fixture->summary.lanAddress
            : brls::Application::getPlatform()->getIpAddress();
        const std::string pairingUrl = debridPairingUrl(ip);
        bool pairingAvailable = fixture && fixture->pairingAvailable &&
                                !pairingUrl.empty();
        if (!fixture && !pairingUrl.empty()) {
            server_ = std::make_unique<TorboxPairingServer>(
                kTorboxPairingPort,
                [provider](const std::string& key, std::string& error) {
                    auto done = std::make_shared<
                        std::promise<std::pair<bool, std::string>>>();
                    auto future = done->get_future();
                    brls::async([provider, key, done] {
                        std::string err;
                        bool ok = false;
                        try {
                            ok = makeDebridProvider(provider, key)
                                     ->validate(err);
                        } catch (const std::exception& e) {
                            err = e.what();
                        } catch (...) {
                            err = "Validation failed.";
                        }
                        if (!ok)
                            err = formatDebridValidateError(err);
                        done->set_value({ok, std::move(err)});
                    });
                    const auto result = future.get();
                    error = result.second;
                    return result.first;
                },
                provider == DebridProviderKind::TorrServer
                    ? "Paste the address of your TorrServer, for example "
                      "http://192.168.1.10:8090."
                    : provider == DebridProviderKind::RealDebrid
                    ? "Paste your Real-Debrid API token. Find it at "
                      "real-debrid.com/apitoken."
                    : kTorboxPairingHint);
            std::string error;
            pairingAvailable = server_->start(error);
            if (pairingAvailable) {
                timer_.setCallback([this] { pollPairing(); });
                timer_.start(500);
            } else {
                server_.reset();
            }
        }
        if (pairingAvailable) {
            auto* qr = new QrCodeView(pairingUrl);
            qr->setMarginBottom(8);
            left->addView(qr);
            auto* url = new brls::Label();
            url->setText(pairingUrl);
            url->setFontSize(theme::kFontSmall);
            url->setTextColor(theme::accent());
            url->setMarginBottom(8);
            left->addView(url);
        }

        auto* enter = new brls::DetailCell();
        enter->setText(tr("pipensx/debrid/enter_key"));
        enter->setDetailText(tr("pipensx/debrid/enter_key_detail"));
        enter->registerClickAction([this](brls::View*) {
            openKeyboard();
            return true;
        });
        left->addView(enter);

        unlink_ = new brls::DetailCell();
        unlink_->setText(tr("pipensx/debrid/unlink"));
        unlink_->setDetailText(tr("pipensx/debrid/unlink_detail"));
        unlink_->registerClickAction([this](brls::View*) {
            if (saveKey({}))
                setCheckNeutral();
            return true;
        });
        left->addView(unlink_);
        addView(left);

        summary_ = new SetupSummaryPanel();
        summary_->setGrow(1);
        addView(summary_);

        const bool online = !pairingUrl.empty();
        summary_->setConnection(
            !online ? tr("pipensx/setup_summary/network_unavailable")
            : pairingAvailable ? tr("pipensx/setup_summary/pairing_available")
                               : tr("pipensx/setup_summary/pairing_unavailable"),
            !online ? theme::warning()
                    : pairingAvailable ? theme::success() : theme::warning());
        if (fixture && fixture->validationSucceeded)
            setCheckSuccess();
        else
            setCheckNeutral();

        const std::string tail = fixture
            ? fixture->summary.diagnosticTail
            : readApplicationLogTail(kBugReportMaxTailBytes);
        const DiagnosticSummary diagnostics = summarizeDiagnostics(tail);
        summary_->setDiagnostics(setupDiagnosticText(diagnostics),
                                 setupDiagnosticColor(diagnostics));
        refresh();
    }

    ~DebridLinkView() override {
        alive_->store(false);
        validationGeneration_->fetch_add(1);
        stopPairing();
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
    void stopPairing() {
        timer_.stop();
        if (server_) {
            server_->stop();
            server_.reset();
        }
    }

    void pollPairing() {
        if (!server_ || !server_->keyAccepted())
            return;
        const std::string key = server_->acceptedKey();
        stopPairing();
        setCheckSuccess();
        saveKey(key);
    }

    const std::string& activeKey() const {
        if (provider_ == DebridProviderKind::TorrServer)
            return settings_->get().torrserverUrl;
        if (provider_ == DebridProviderKind::RealDebrid)
            return settings_->get().realdebridApiKey;
        return settings_->get().torboxApiKey;
    }

    void refresh() {
        const bool saved = !activeKey().empty();
        summary_->setSelected(
            saved ? tr("pipensx/setup_summary/provider_saved",
                       debridProviderName(provider_))
                  : tr("pipensx/setup_summary/provider_not_saved",
                       debridProviderName(provider_)),
            saved ? theme::success() : theme::accent());
        unlink_->setVisibility(saved ? brls::Visibility::VISIBLE
                                     : brls::Visibility::GONE);
    }

    void setCheckNeutral() {
        summary_->setCheck(tr("pipensx/setup_summary/not_checked"),
                           theme::textSecondary());
    }

    void setCheckSuccess() {
        summary_->setCheck(tr("pipensx/setup_summary/check_success"),
                           theme::success());
    }

    void openKeyboard() {
        const std::string prompt =
            provider_ == DebridProviderKind::TorrServer
                ? tr("pipensx/debrid/keyboard_prompt_url")
                : tr("pipensx/debrid/keyboard_prompt",
                     debridProviderName(provider_));
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                // SwitchImeManager calls this before swkbdClose(). Starting
                // curl while the keyboard applet is still up takes the
                // process down with std::terminate and no exception.
                brls::sync([this, text = std::move(text)]() mutable {
                    if (!alive_->load())
                        return;
                    validate(std::move(text));
                });
            }, prompt, "",
            128, activeKey(), brls::KEYBOARD_DISABLE_NONE);
    }

    void validate(std::string text) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        const size_t last = text.find_last_not_of(" \t\r\n");
        std::string key = first == std::string::npos
            ? std::string() : text.substr(first, last - first + 1);
        if (key.empty()) {
            validationGeneration_->fetch_add(1);
            summary_->setCheck(tr("pipensx/debrid/no_key"),
                               theme::textSecondary());
            return;
        }
        summary_->setCheck(tr("pipensx/debrid/validating"), theme::accent());
        auto alive = alive_;
        auto generation = validationGeneration_;
        const uint64_t attempt = generation->fetch_add(1) + 1;
        const DebridProviderKind provider = provider_;
        brls::async([this, alive, generation, attempt, provider, key] {
            std::string error;
            bool ok = false;
            try {
                ok = makeDebridProvider(provider, key)->validate(error);
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "Validation failed.";
            }
            if (!ok)
                error = formatDebridValidateError(error);
            brls::sync([this, alive, generation, attempt, ok, key,
                        error = std::move(error)] {
                if (!alive->load() || generation->load() != attempt)
                    return;
                if (ok) {
                    setCheckSuccess();
                    saveKey(key);
                } else {
                    summary_->setCheck(error, theme::error());
                }
            });
        });
    }

    bool saveKey(const std::string& typed) {
        if (typed.empty())
            validationGeneration_->fetch_add(1);
        const std::string key =
            provider_ == DebridProviderKind::TorrServer
                ? TorrserverProvider::normalizeBaseUrl(typed) : typed;
        AppSettingsData values = settings_->get();
        if (provider_ == DebridProviderKind::TorrServer)
            values.torrserverUrl = key;
        else if (provider_ == DebridProviderKind::RealDebrid)
            values.realdebridApiKey = key;
        else
            values.torboxApiKey = key;
        std::string error;
        if (!settings_->update(values, error)) {
            brls::Application::notify(error);
            return false;
        }
        if (provider_ == DebridProviderKind::TorrServer) {
            if (manager_)
                manager_->setTorrserverUrl(key);
        } else if (provider_ == DebridProviderKind::RealDebrid) {
            if (manager_)
                manager_->setRealdebridApiKey(key);
        } else {
            if (manager_)
                manager_->setTorboxApiKey(key);
        }
        if (!key.empty())
            stopPairing();
        brls::Application::notify(key.empty()
            ? tr("pipensx/debrid/unlinked_notify")
            : tr("pipensx/debrid/linked_notify"));
        refresh();
        return true;
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    DebridProviderKind provider_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<uint64_t>> validationGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::unique_ptr<TorboxPairingServer> server_;
    brls::RepeatingTimer timer_;
    SetupSummaryPanel* summary_ = nullptr;
    brls::DetailCell* unlink_ = nullptr;
};

inline void removeDebridTransferAsync(DebridProviderKind provider,
                                      std::string key, std::string id) {
    if (key.empty() || id.empty())
        return;
    log_msg("[DEBUG-debrid-picker] cleanup queued id=%s\n", id.c_str());
    brls::async([provider, key = std::move(key), id = std::move(id)] {
        try {
            log_msg("[DEBUG-debrid-picker] cleanup started id=%s\n", id.c_str());
            std::string ignored;
            makeDebridProvider(provider, key)->remove(id, ignored);
            log_msg("[DEBUG-debrid-picker] cleanup finished id=%s\n", id.c_str());
        } catch (const std::exception& e) {
            log_msg("[DEBUG-debrid-picker] cleanup threw: %s\n", e.what());
        } catch (...) {
            log_msg("[DEBUG-debrid-picker] cleanup threw non-std exception\n");
        }
    });
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

}  // namespace pipensx::ui
