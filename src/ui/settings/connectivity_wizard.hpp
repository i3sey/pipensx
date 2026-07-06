#pragma once

// First-run connectivity wizard (RF_ACCESS_PLAN Phase W3).
//
// Borealis flow that gates first launch on the connectivity ladder (Phase W2).
// It drives the pure runLadder engine through the framework's async/sync
// marshaling: a background probe walks direct -> manual proxy -> antizapret ->
// mirror while the UI shows live per-rung progress and a cancel action. When a
// route wins, the method is persisted (settings.connectivityMethod +
// connectivitySetupDone) and the wizard dismisses; when every route fails it
// offers the user a proxy, a mirror, a retry, or continuing offline on the
// bundled (stale) dump. The activity is only pushed while
// !connectivitySetupDone, so later launches skip it and take the saved route.

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/connectivity_orchestrator.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class ConnectivityWizardActivity : public brls::Activity {
public:
    explicit ConnectivityWizardActivity(AppSettings* settings)
        : settings_(settings),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        auto* root = new brls::Box(brls::Axis::COLUMN);
        root->setGrow(1);
        root->setPadding(40, 60, 40, 60);
        root->setJustifyContent(brls::JustifyContent::CENTER);
        root->setAlignItems(brls::AlignItems::CENTER);

        title_ = new brls::Label();
        title_->setFontSize(28);
        title_->setWidth(820);
        title_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        title_->setMarginBottom(16);
        root->addView(title_);

        status_ = new brls::Label();
        status_->setFontSize(19);
        status_->setWidth(820);
        status_->setTextColor(theme::textSecondary());
        status_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        status_->setMarginBottom(28);
        root->addView(status_);

        buttons_ = new brls::Box(brls::Axis::COLUMN);
        buttons_->setWidth(560);
        buttons_->setAlignItems(brls::AlignItems::STRETCH);
        root->addView(buttons_);

        frame_ = new brls::AppletFrame(root);
        frame_->setTitle("Connection setup");
    }

    ~ConnectivityWizardActivity() override {
        // Detach any in-flight probe from the now-dying UI (mirrors the batch
        // installer): sync() callbacks no-op and runLadder stops.
        alive_->store(false);
        cancelled_->store(true);
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
            handleBack();
            return true;
        });
        startCheck();
    }

private:
    enum class State { Checking, Options, Connected };

    static constexpr const char* kDirectAnnounce =
        "http://bt.t-ru.org/ann?magnet";
    static constexpr int kProbeTimeoutSeconds = 8;

    static int cancelThunk(void* user) {
        return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
    }

    void startCheck() {
        state_ = State::Checking;
        cancelled_->store(false);
        title_->setText("Setting up connection");
        status_->setTextColor(theme::textSecondary());
        status_->setText("Checking access to RuTracker...");
        buttons_->clearViews();

        LadderConfig config = ladderConfigFromSettings(
            settings_->get(), kDirectAnnounce, kProbeTimeoutSeconds);
        triedMirror_ = config.hasMirror;

        auto alive = alive_;
        auto cancelled = cancelled_;
        brls::async([this, alive, cancelled, config] {
            int timeout = config.probeTimeoutSeconds;
            auto probe = [cancelled, timeout](const LadderAttempt& attempt) {
                return tracker_probe(
                    attempt.announceUrl.c_str(),
                    attempt.useRoute ? &attempt.route : nullptr, timeout,
                    &ConnectivityWizardActivity::cancelThunk, cancelled.get());
            };
            auto onEvent = [this, alive](const LadderEvent& event) {
                brls::sync([this, alive, event] {
                    if (alive->load())
                        onLadderEvent(event);
                });
            };
            runLadder(config, probe, onEvent,
                      &ConnectivityWizardActivity::cancelThunk, cancelled.get());
        });
    }

    void onLadderEvent(const LadderEvent& event) {
        switch (event.kind) {
        case LadderEventKind::RunStarted:
            status_->setText("Checking access to RuTracker...");
            break;
        case LadderEventKind::AttemptStarted:
            status_->setText("Trying " + event.attempt.label +
                             "...   (B to cancel)");
            break;
        case LadderEventKind::AttemptFinished:
            break;  // the next AttemptStarted / RunFinished carries the state
        case LadderEventKind::RunFinished:
            onRunFinished(event.result);
            break;
        }
    }

    void onRunFinished(const LadderResult& result) {
        switch (result.outcome) {
        case LadderOutcome::Connected: {
            AppSettingsData values = settings_->get();
            applyLadderResult(result, values);
            persist(values);
            showConnected(result.winningLabel);
            break;
        }
        case LadderOutcome::NeedMirror:
        case LadderOutcome::Exhausted:
            showOptions("Couldn't reach RuTracker directly or through the "
                        "available bypass routes.");
            break;
        case LadderOutcome::Cancelled:
            showOptions("Connection check cancelled.");
            break;
        }
    }

    void showConnected(const std::string& label) {
        state_ = State::Connected;
        title_->setText("Connected");
        status_->setTextColor(theme::success());
        status_->setText("RuTracker is reachable via " + label + ".");
        buttons_->clearViews();
        addButton("Continue", brls::BUTTONSTYLE_PRIMARY, [this] { dismiss(); });
    }

    void showOptions(const std::string& message) {
        state_ = State::Options;
        title_->setText("No connection to RuTracker");
        status_->setTextColor(theme::textSecondary());
        status_->setText(message);
        buttons_->clearViews();
        addButton("Enter a proxy", brls::BUTTONSTYLE_PRIMARY,
                  [this] { promptProxy(); });
        // Offer a mirror only when this run did not already try one.
        if (!triedMirror_)
            addButton("Use a RuTracker mirror", brls::BUTTONSTYLE_DEFAULT,
                      [this] { promptMirror(); });
        addButton("Retry", brls::BUTTONSTYLE_DEFAULT, [this] { startCheck(); });
        addButton("Continue offline", brls::BUTTONSTYLE_DEFAULT,
                  [this] { continueOffline(); });
    }

    void promptProxy() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (text.empty())
                    return;
                AppSettingsData values = settings_->get();
                values.manualProxyUrl = text;
                values.manualProxyType =
                    text.find("socks") != std::string::npos ? ProxyType::Socks5
                                                            : ProxyType::Http;
                persist(values);
                startCheck();
            },
            "Proxy for RuTracker", "http://host:port  or  socks5://host:port",
            128, settings_->get().manualProxyUrl);
    }

    void promptMirror() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (text.empty())
                    return;
                AppSettingsData values = settings_->get();
                values.rutrackerHost = text;
                persist(values);
                startCheck();
            },
            "RuTracker mirror host", "host only, e.g. bt.mirror.example", 128,
            settings_->get().rutrackerHost);
    }

    void continueOffline() {
        // Explicit acceptance of the bundled (possibly stale) dump: mark the
        // wizard done so later launches don't re-nag. The staleness banner
        // (W5) keeps that fallback from being silent.
        AppSettingsData values = settings_->get();
        values.connectivitySetupDone = true;
        persist(values);
        dismiss();
    }

    void handleBack() {
        if (state_ == State::Checking) {
            cancelled_->store(true);
            status_->setText("Cancelling...");
            return;  // stay on this screen; the run ends and shows the options
        }
        if (state_ == State::Connected) {
            dismiss();  // already persisted
            return;
        }
        continueOffline();  // Options: back == continue offline
    }

    void dismiss() {
        if (dismissed_)
            return;
        dismissed_ = true;
        cancelled_->store(true);
        brls::Application::popActivity();
    }

    void persist(const AppSettingsData& values) {
        std::string error;
        if (!settings_->update(values, error)) {
            diagnostic_error("settings", "wizard", "error=%s", error.c_str());
            brls::Application::notify(error);
        }
    }

    void addButton(const std::string& text, const brls::ButtonStyle& style,
                   std::function<void()> action) {
        auto* button = new brls::Button();
        button->setStyle(&style);
        button->setHeight(52);
        button->setMarginBottom(10);
        button->setText(text);
        button->registerClickAction(
            [action = std::move(action)](brls::View*) {
                action();
                return true;
            });
        buttons_->addView(button);
    }

    AppSettings* settings_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* status_ = nullptr;
    brls::Box* buttons_ = nullptr;
    State state_ = State::Checking;
    bool triedMirror_ = false;
    bool dismissed_ = false;
};

}  // namespace pipensx::ui
