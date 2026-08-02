#pragma once

#include <functional>
#include <string>
#include <utility>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// One choice on the first-run screen: a heading plus the paragraph that says
// when to pick it. A focusable Box rather than a DetailCell — the cells put
// their detail text on the right and clip it, and the whole point here is the
// two lines under the heading.
class FirstRunOption : public brls::Box {
public:
    FirstRunOption(const std::string& heading, const std::string& detail,
                   std::function<void()> onChoose)
        : brls::Box(brls::Axis::COLUMN), onChoose_(std::move(onChoose)) {
        setFocusable(true);
        setPadding(18, 24, 18, 24);
        setMarginBottom(12);
        // surface(), not panel(): panel is translucent and lands within a
        // couple of RGB steps of the page background in the dark theme, which
        // makes the cards read as loose paragraphs rather than choices.
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusLarge);
        setHighlightCornerRadius(theme::kRadiusLarge);

        auto* title = new brls::Label();
        title->setText(heading);
        title->setFontSize(theme::kFontBody);
        title->setTextColor(theme::textPrimary());
        title->setMarginBottom(6);
        addView(title);

        auto* body = new brls::Label();
        body->setText(detail);
        body->setFontSize(theme::kFontSmall);
        body->setTextColor(theme::textSecondary());
        body->setSingleLine(false);
        addView(body);

        registerClickAction([this](brls::View*) {
            onChoose_();
            return true;
        });
    }

private:
    std::function<void()> onChoose_;
};

// Shown once, after the catalog disclaimer. Three ways to fetch a release,
// each with the reason to pick it — the choice turns on whether the console
// joins the swarm itself, and that has consequences a one-line dialog button
// cannot explain.
class FirstRunView : public brls::Box {
public:
    FirstRunView(AppSettings* settings, DownloadManager* manager)
        : brls::Box(brls::Axis::COLUMN), settings_(settings),
          manager_(manager) {
        setPadding(20, 40, 24, 40);

        auto* content = new brls::Box(brls::Axis::COLUMN);

        auto* intro = new brls::Label();
        intro->setText(tr("pipensx/first_run/intro"));
        intro->setFontSize(theme::kFontSmall);
        intro->setTextColor(theme::textSecondary());
        intro->setSingleLine(false);
        intro->setMarginBottom(18);
        content->addView(intro);

        content->addView(new FirstRunOption(
            tr("pipensx/first_run/torrserver"),
            tr("pipensx/first_run/torrserver_detail"),
            [this] { choose(DebridProviderKind::TorrServer, false); }));
        content->addView(new FirstRunOption(
            tr("pipensx/first_run/torbox"),
            tr("pipensx/first_run/torbox_detail"),
            [this] { choose(DebridProviderKind::TorBox, false); }));
        content->addView(new FirstRunOption(
            tr("pipensx/first_run/direct"),
            tr("pipensx/first_run/direct_detail"),
            [this] { choose(DebridProviderKind::TorBox, true); }));
        // The intro Label is index 0 and invisible to the focus walk; land on
        // the first option instead of leaving focus on the frame.
        content->setDefaultFocusedIndex(1);

        auto* note = new brls::Label();
        note->setText(tr("pipensx/first_run/note"));
        note->setFontSize(theme::kFontCaption);
        note->setTextColor(theme::textTertiary());
        note->setSingleLine(false);
        note->setMarginTop(6);
        content->addView(note);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

    static void push(AppSettings* settings, DownloadManager* manager) {
        auto* frame = new brls::AppletFrame(
            new FirstRunView(settings, manager));
        frame->setTitle(tr("pipensx/first_run/title"));
        brls::Application::pushActivity(new brls::Activity(frame));
    }

private:
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
        // Pop first, then push: the setup screen replaces this one instead of
        // stacking on it, so backing out of setup lands on the app rather
        // than on a choice that has already been made.
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
};

inline void showFirstRunChoice(AppSettings* settings,
                               DownloadManager* manager) {
    if (!settings || settings->get().firstRunCompleted)
        return;
    FirstRunView::push(settings, manager);
}

}  // namespace pipensx::ui
