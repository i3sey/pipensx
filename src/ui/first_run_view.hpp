#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/bug_report.hpp"
#include "app/download_manager.hpp"
#include "ui/common/setup_summary_panel.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class FirstRunOption : public brls::Box {
public:
    FirstRunOption(const std::string& heading, std::function<void()> onChoose,
                   std::function<void()> onFocus)
        : brls::Box(brls::Axis::COLUMN), onChoose_(std::move(onChoose)),
          onFocus_(std::move(onFocus)) {
        setFocusable(true);
        setPadding(16, 24, 16, 24);
        setMarginBottom(8);
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusLarge);
        setHighlightCornerRadius(theme::kRadiusLarge);

        auto* title = new brls::Label();
        title->setText(heading);
        title->setFontSize(theme::kFontBody);
        title->setTextColor(theme::textPrimary());
        addView(title);

        registerClickAction([this](brls::View*) {
            onChoose_();
            return true;
        });
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (onFocus_)
            onFocus_();
    }

private:
    std::function<void()> onChoose_;
    std::function<void()> onFocus_;
};

class FirstRunView : public brls::Box {
public:
    FirstRunView(AppSettings* settings, DownloadManager* manager,
                 std::optional<SetupSummaryFixture> fixture = std::nullopt)
        : brls::Box(brls::Axis::ROW), settings_(settings), manager_(manager) {
        setPadding(24, 40, 24, 40);

        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidthPercentage(54);
        left->setShrink(0);
        left->setMarginRight(24);

        auto* intro = new brls::Label();
        intro->setText(tr("pipensx/first_run/intro"));
        intro->setFontSize(theme::kFontSmall);
        intro->setTextColor(theme::textSecondary());
        intro->setSingleLine(false);
        intro->setMarginBottom(16);
        left->addView(intro);

        left->addView(new FirstRunOption(
            tr("pipensx/first_run/torrserver"),
            [this] { choose(DebridProviderKind::TorrServer, false); },
            [this] { updateSelection(DebridProviderKind::TorrServer, false); }));
        left->addView(new FirstRunOption(
            tr("pipensx/first_run/torbox"),
            [this] { choose(DebridProviderKind::TorBox, false); },
            [this] { updateSelection(DebridProviderKind::TorBox, false); }));
        left->addView(new FirstRunOption(
            tr("pipensx/first_run/direct"),
            [this] { choose(DebridProviderKind::TorBox, true); },
            [this] { updateSelection(DebridProviderKind::TorBox, true); }));
        left->setDefaultFocusedIndex(1);

        auto* note = new brls::Label();
        note->setText(tr("pipensx/first_run/note"));
        note->setFontSize(theme::kFontCaption);
        note->setTextColor(theme::textTertiary());
        note->setSingleLine(false);
        note->setMarginTop(8);
        left->addView(note);
        addView(left);

        summary_ = new SetupSummaryPanel();
        summary_->setGrow(1);
        addView(summary_);

        const std::string ip = fixture
            ? fixture->lanAddress
            : brls::Application::getPlatform()->getIpAddress();
        const bool online = !ip.empty() && ip != "0.0.0.0";
        summary_->setConnection(
            online ? tr("pipensx/setup_summary/network_available")
                   : tr("pipensx/setup_summary/network_unavailable"),
            online ? theme::success() : theme::warning());
        summary_->setCheck(tr("pipensx/setup_summary/not_checked"),
                           theme::textSecondary());

        const std::string tail = fixture
            ? fixture->diagnosticTail
            : readApplicationLogTail(kBugReportMaxTailBytes);
        const DiagnosticSummary diagnostics = summarizeDiagnostics(tail);
        summary_->setDiagnostics(setupDiagnosticText(diagnostics),
                                 setupDiagnosticColor(diagnostics));
        updateSelection(DebridProviderKind::TorrServer, false);
    }

    static void push(AppSettings* settings, DownloadManager* manager) {
        auto* frame = new brls::AppletFrame(new FirstRunView(settings, manager));
        frame->setTitle(tr("pipensx/first_run/title"));
        brls::Application::pushActivity(new brls::Activity(frame));
    }

    std::string summaryState() const {
        return summary_->renderedState();
    }

    bool summaryFits() {
        return summary_->contentFits();
    }

private:
    void updateSelection(DebridProviderKind provider, bool torrenting) {
        if (torrenting)
            summary_->setSelected(
                tr("pipensx/first_run/direct_summary",
                   tr("pipensx/first_run/direct_detail")),
                theme::accent());
        else if (provider == DebridProviderKind::TorrServer)
            summary_->setSelected(
                tr("pipensx/first_run/torrserver_summary",
                   tr("pipensx/first_run/torrserver_detail")),
                theme::accent());
        else
            summary_->setSelected(
                tr("pipensx/first_run/torbox_summary",
                   tr("pipensx/first_run/torbox_detail")),
                theme::accent());
    }

    void choose(DebridProviderKind provider, bool torrenting) {
        AppSettingsData values = settings_->get();
        values.debridProvider = provider;
        values.torrentingEnabled = torrenting;
        values.firstRunCompleted = true;
        std::string error;
        if (!settings_->update(values, error)) {
            brls::Application::notify(error);
            return;
        }
        manager_->setTorrentingEnabled(torrenting);
        AppSettings* settings = settings_;
        DownloadManager* manager = manager_;
        brls::Application::popActivity(brls::TransitionAnimation::FADE,
            [settings, manager, provider, torrenting] {
                if (!torrenting)
                    DebridLinkView::push(settings, manager, provider);
            });
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    SetupSummaryPanel* summary_ = nullptr;
};

inline void showFirstRunChoice(AppSettings* settings,
                               DownloadManager* manager) {
    if (!settings || settings->get().firstRunCompleted)
        return;
    FirstRunView::push(settings, manager);
}

}  // namespace pipensx::ui
