#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <borealis.hpp>

extern "C" {
#include "core/util.h"
}
#include "app/app_settings.hpp"
#include "app/direct_hint.hpp"
#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// One arrow of the mode diagram: a horizontal line with an arrowhead on its
// right edge and a caption label below the line. The chips it connects are
// real views on either side; this view draws only the connector between them.
class ModeDiagramArrow : public brls::Box {
public:
    explicit ModeDiagramArrow(const std::string& caption)
        : brls::Box(brls::Axis::COLUMN) {
        setFocusable(false);
        setGrow(1);
        // The chips take 3x100 of the panel's ~480; a plain grow would leave
        // each arrow ~90px — which is exactly the caption's minimum width.
        // Below that, "своя сеть" wraps letter-by-letter (no spaces to break
        // on), so pin the floor.
        setMinWidth(90);
        setAlignItems(brls::AlignItems::CENTER);

        auto* pad = new brls::Box();
        pad->setGrow(1);
        addView(pad);

        caption_ = new brls::Label();
        caption_->setFontSize(theme::kFontCaption);
        caption_->setTextColor(theme::textTertiary());
        caption_->setText(caption);
        addView(caption_);

        // Fixed floor under the caption: the top spacer grows instead, so the
        // arrow line (drawn at the vertical centre) always lands just above
        // the caption, whatever the panel width is.
        auto* bottom = new brls::Box();
        bottom->setHeight(28);
        addView(bottom);
    }

    void setCaption(const std::string& text) { caption_->setText(text); }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const float cy = y + height / 2.0f;
        const float right = x + width - 2.0f;
        nvgSave(vg);
        nvgLineCap(vg, NVG_ROUND);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x + 2.0f, cy);
        nvgLineTo(vg, right, cy);
        nvgStrokeColor(vg, theme::textSecondary());
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, right - 6.0f, cy - 4.0f);
        nvgLineTo(vg, right, cy);
        nvgLineTo(vg, right - 6.0f, cy + 4.0f);
        nvgStroke(vg);
        nvgRestore(vg);
        // The caption is a real child view; Box::draw paints the children,
        // which this override must not swallow.
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    brls::Label* caption_ = nullptr;
};

// How a mode gets files onto the console: a console chip, an optional server
// chip (TorrServer or TorBox) and the swarm chip, joined by arrows. The
// Direct mode hides the server, so the diagram collapses to a single hop.
class ModeDiagram : public brls::Box {
public:
    enum class Kind { TorrServer, TorBox, RealDebrid, Direct };

    ModeDiagram() : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setHeight(96);
        setAlignItems(brls::AlignItems::CENTER);

        makeChip(tr("pipensx/first_run/diagram_console"));
        serverHop_ = new ModeDiagramArrow(tr("pipensx/first_run/diagram_lan"));
        addView(serverHop_);
        serverChip_ = makeChip(tr("pipensx/first_run/diagram_torrserver"),
                               &serverLabel_);
        swarmHop_ =
            new ModeDiagramArrow(tr("pipensx/first_run/diagram_internet"));
        addView(swarmHop_);
        makeChip(tr("pipensx/first_run/diagram_swarm"));
        setKind(Kind::TorrServer);
    }

    void setKind(Kind kind) {
        kind_ = kind;
        const bool direct = kind == Kind::Direct;
        const brls::Visibility visible = direct ? brls::Visibility::GONE
                                                : brls::Visibility::VISIBLE;
        serverChip_->setVisibility(visible);
        serverHop_->setVisibility(visible);
        switch (kind) {
            case Kind::TorrServer:
                serverLabel_->setText(
                    tr("pipensx/first_run/diagram_torrserver"));
                serverHop_->setCaption(tr("pipensx/first_run/diagram_lan"));
                swarmHop_->setCaption(
                    tr("pipensx/first_run/diagram_internet"));
                break;
            case Kind::TorBox:
            case Kind::RealDebrid:
                serverLabel_->setText(
                    kind == Kind::RealDebrid
                        ? tr("pipensx/first_run/diagram_realdebrid")
                        : tr("pipensx/first_run/diagram_torbox"));
                serverHop_->setCaption(
                    tr("pipensx/first_run/diagram_internet"));
                swarmHop_->setCaption(
                    tr("pipensx/first_run/diagram_internet"));
                break;
            case Kind::Direct:
                swarmHop_->setCaption(tr("pipensx/first_run/diagram_torrent"));
                break;
        }
    }

    Kind kind() const { return kind_; }

private:
    // Fixed-width rounded chip with a centred label; returns the chip (for
    // GONE visibility) and optionally fills `label` so setKind can rename the
    // server chip in place. 100px leaves room for the arrows (see
    // ModeDiagramArrow) while still fitting "TorrServer".
    brls::Box* makeChip(const std::string& text, brls::Label** label = nullptr) {
        auto* chip = new brls::Box();
        chip->setFocusable(false);
        chip->setWidth(100);
        chip->setHeight(44);
        chip->setCornerRadius(theme::kRadiusMedium);
        chip->setBackgroundColor(theme::surface());
        chip->setBorderColor(theme::track());
        chip->setBorderThickness(1.0f);
        chip->setAlignItems(brls::AlignItems::CENTER);
        chip->setJustifyContent(brls::JustifyContent::CENTER);

        auto* chipLabel = new brls::Label();
        chipLabel->setFontSize(theme::kFontCaption);
        chipLabel->setTextColor(theme::textPrimary());
        chipLabel->setText(text);
        chip->addView(chipLabel);
        addView(chip);
        if (label)
            *label = chipLabel;
        return chip;
    }

    Kind kind_ = Kind::TorrServer;
    brls::Box* serverChip_ = nullptr;
    brls::Label* serverLabel_ = nullptr;
    ModeDiagramArrow* serverHop_ = nullptr;
    ModeDiagramArrow* swarmHop_ = nullptr;
};

// Right-hand panel of the first-run chooser: the picked method's name, a
// diagram of how it moves bytes, and three short paragraphs (how, who sees
// the console, setup). Replaces the generic SetupSummaryPanel, which keeps
// serving the provider link screen.
class ModeSummaryPanel : public brls::Box {
public:
    ModeSummaryPanel() : brls::Box(brls::Axis::COLUMN) {
        setFocusable(false);
        setBackgroundColor(theme::panel());
        setBorderColor(theme::track());
        setBorderThickness(1.0f);
        setCornerRadius(theme::kRadiusLarge);
        setPadding(24, 24, 24, 24);

        auto* heading = new brls::Label();
        heading->setText(tr("pipensx/setup_summary/selected"));
        heading->setFontSize(theme::kFontCaption);
        heading->setTextColor(theme::textTertiary());
        heading->setMarginBottom(8);
        addView(heading);

        name_ = new brls::Label();
        name_->setFontSize(theme::kFontBody);
        name_->setTextColor(theme::accent());
        name_->setSingleLine(false);
        name_->setMarginBottom(12);
        addView(name_);

        diagram_ = new ModeDiagram();
        diagram_->setMarginBottom(16);
        addView(diagram_);

        for (int i = 0; i < 3; ++i) {
            auto* paragraph = new brls::Label();
            paragraph->setFontSize(theme::kFontSmall);
            paragraph->setTextColor(theme::textSecondary());
            paragraph->setSingleLine(false);
            if (i > 0)
                paragraph->setMarginTop(12);
            addView(paragraph);
            paragraphs_[i] = paragraph;
        }
    }

    void setMode(ModeDiagram::Kind kind, const std::string& name,
                 const std::string& how, const std::string& peer,
                 const std::string& setup) {
        diagram_->setKind(kind);
        name_->setText(name);
        paragraphs_[0]->setText(how);
        paragraphs_[1]->setText(peer);
        paragraphs_[2]->setText(setup);
    }

    std::string renderedState() const {
        std::string state = name_->getFullText();
        for (brls::Label* paragraph : paragraphs_)
            state += "|" + paragraph->getFullText();
        return state;
    }

    bool contentFits() { return overflowDetail().empty(); }

    // First child whose frame escapes its parent's, as a "parent / child +
    // frames" diagnostic for the golden harness. The diagram stretches to the
    // panel's full width, so its own frame always fits — the fixed-width
    // chips inside it are what actually stick out, which is why this must
    // recurse into boxes. Frames are absolute (borealis getFrame), so every
    // level compares like with like.
    std::string overflowDetail() { return firstOverflow(this); }

private:
    static std::string firstOverflow(brls::Box* view) {
        const brls::Rect parent = view->getFrame();
        for (brls::View* child : view->getChildren()) {
            const brls::Rect frame = child->getFrame();
            if (frame.getMaxX() > parent.getMaxX() + 0.5f ||
                frame.getMaxY() > parent.getMaxY() + 0.5f ||
                frame.getMinX() < parent.getMinX() - 0.5f ||
                frame.getMinY() < parent.getMinY() - 0.5f) {
                char detail[192];
                std::snprintf(
                    detail, sizeof(detail),
                    "%s escapes %s (%.0fx%.0f+%.0f+%.0f vs parent "
                    "%.0fx%.0f+%.0f+%.0f)",
                    child->describe().c_str(), view->describe().c_str(),
                    frame.getWidth(), frame.getHeight(), frame.getMinX(),
                    frame.getMinY(), parent.getWidth(), parent.getHeight(),
                    parent.getMinX(), parent.getMinY());
                return detail;
            }
            if (auto* box = dynamic_cast<brls::Box*>(child)) {
                std::string nested = firstOverflow(box);
                if (!nested.empty())
                    return nested;
            }
        }
        return std::string();
    }

    brls::Label* name_ = nullptr;
    ModeDiagram* diagram_ = nullptr;
    brls::Label* paragraphs_[3] = {};
};

class FirstRunOption : public brls::Box {
public:
    FirstRunOption(const std::string& heading, std::function<void()> onChoose,
                   std::function<void()> onFocus, const std::string& hint = {},
                   const std::string& chip = {}, NVGcolor chipColor = {})
        : brls::Box(brls::Axis::COLUMN), onChoose_(std::move(onChoose)),
          onFocus_(std::move(onFocus)) {
        setFocusable(true);
        setPadding(16, 24, 16, 24);
        setMarginBottom(8);
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusLarge);
        setHighlightCornerRadius(theme::kRadiusLarge);

        auto* titleRow = new brls::Box(brls::Axis::ROW);
        titleRow->setAlignItems(brls::AlignItems::CENTER);

        auto* title = new brls::Label();
        title->setText(heading);
        title->setFontSize(theme::kFontBody);
        title->setTextColor(theme::textPrimary());
        title->setGrow(1);
        titleRow->addView(title);

        if (!chip.empty()) {
            auto* badge = new brls::Box();
            badge->setFocusable(false);
            badge->setShrink(0);
            badge->setWidth(104);
            badge->setHeight(26);
            badge->setCornerRadius(theme::kRadiusSmall);
            badge->setBackgroundColor(theme::surface());
            badge->setBorderColor(chipColor);
            badge->setBorderThickness(1.0f);
            badge->setAlignItems(brls::AlignItems::CENTER);
            badge->setJustifyContent(brls::JustifyContent::CENTER);
            auto* chipLabel = new brls::Label();
            chipLabel->setText(chip);
            chipLabel->setFontSize(theme::kFontCaption);
            chipLabel->setTextColor(chipColor);
            badge->addView(chipLabel);
            titleRow->addView(badge);
        }
        addView(titleRow);

        if (!hint.empty()) {
            auto* caption = new brls::Label();
            caption->setText(hint);
            caption->setFontSize(theme::kFontCaption);
            caption->setTextColor(theme::textTertiary());
            caption->setMarginTop(4);
            addView(caption);
        }

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
    // Fires once a method is saved; the caller continues the startup chain
    // (catalog disclaimer, then the provider link screen for server modes).
    using OnComplete = std::function<void(DebridProviderKind, bool)>;

    FirstRunView(AppSettings* settings, DownloadManager* manager,
                 OnComplete onComplete, bool lockBack = true)
        : brls::Box(brls::Axis::ROW), settings_(settings), manager_(manager),
          onComplete_(std::move(onComplete)), lockBack_(lockBack) {
        setPadding(24, 40, 24, 40);

        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidthPercentage(48);
        left->setShrink(0);
        left->setMarginRight(24);

        auto* intro = new brls::Label();
        intro->setText(tr("pipensx/first_run/intro"));
        intro->setFontSize(theme::kFontSmall);
        intro->setTextColor(theme::textSecondary());
        intro->setSingleLine(false);
        intro->setMarginBottom(16);
        left->addView(intro);

        DirectHint hint = classifyDirectHint(consoleTimeZone(), consoleLanguage());
        if (hint == DirectHint::None)
            hint = classifyDirectHint("", brls::Application::getLocale());
        std::string directHint;
        if (hint == DirectHint::Recommended)
            directHint = tr("pipensx/first_run/direct_hint_ok");
        else if (hint == DirectHint::Discouraged)
            directHint = tr("pipensx/first_run/direct_hint_no");

        left->addView(new FirstRunOption(
            tr("pipensx/first_run/torrserver"),
            [this] { choose(DebridProviderKind::TorrServer, false); },
            [this] { updateSelection(DebridProviderKind::TorrServer, false); },
            "", tr("pipensx/first_run/chip_free"), theme::success()));
        left->addView(new FirstRunOption(
            tr("pipensx/first_run/realdebrid"),
            [this] { choose(DebridProviderKind::RealDebrid, false); },
            [this] { updateSelection(DebridProviderKind::RealDebrid, false); },
            "", tr("pipensx/first_run/chip_paid"), theme::warning()));
        left->addView(new FirstRunOption(
            tr("pipensx/first_run/torbox"),
            [this] { choose(DebridProviderKind::TorBox, false); },
            [this] { updateSelection(DebridProviderKind::TorBox, false); },
            "", tr("pipensx/first_run/chip_paid"), theme::warning()));
        left->addView(new FirstRunOption(
            tr("pipensx/first_run/direct"),
            [this] { choose(DebridProviderKind::TorBox, true); },
            [this] { updateSelection(DebridProviderKind::TorBox, true); },
            directHint, tr("pipensx/first_run/chip_free"),
            theme::success()));
        const bool preferDirect = hint == DirectHint::Recommended;
        left->setDefaultFocusedIndex(preferDirect ? 4 : 1);

        auto* note = new brls::Label();
        note->setText(tr("pipensx/first_run/note"));
        note->setFontSize(theme::kFontCaption);
        note->setTextColor(theme::textTertiary());
        note->setSingleLine(false);
        note->setMarginTop(8);
        left->addView(note);
        addView(left);

        summary_ = new ModeSummaryPanel();
        summary_->setGrow(1);
        addView(summary_);

        // B is locked until a method is picked: a first run must end in a
        // choice, or the app would open with no download source at all. The
        // action is hidden so it never rides the hint bar; it sits on this
        // box, closer to the focused option than the frame's dismiss action.
        // From Settings the same view is an editor, so B keeps popping back.
        if (lockBack_)
            registerAction("", brls::BUTTON_B,
                           [](brls::View*) { return true; },
                           /*hidden=*/true);

        if (preferDirect)
            updateSelection(DebridProviderKind::TorBox, true);
        else
            updateSelection(DebridProviderKind::TorrServer, false);
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        if (!lockBack_)
            return;
        // The frame's own B action ("Back", visible in the hint bar) is
        // still registered on the AppletFrame. Replace it with a hidden
        // no-op — registerAction with the same button overwrites — so the
        // bar stops advertising a Back button the lock makes useless.
        for (brls::View* node = getParent(); node; node = node->getParent()) {
            if (auto* frame = dynamic_cast<brls::AppletFrame*>(node)) {
                frame->registerAction("", brls::BUTTON_B,
                                      [](brls::View*) { return true; },
                                      /*hidden=*/true);
                break;
            }
        }
    }

    static void push(AppSettings* settings, DownloadManager* manager,
                     OnComplete onComplete, bool lockBack = true) {
        auto* frame = new brls::AppletFrame(
            new FirstRunView(settings, manager, std::move(onComplete),
                             lockBack));
        frame->setTitle(tr("pipensx/first_run/title"));
        brls::Application::pushActivity(new brls::Activity(frame));
    }

    std::string summaryState() const { return summary_->renderedState(); }

    bool summaryFits() { return summary_->contentFits(); }

    // Golden harness: why summaryFits() fails, if it does.
    std::string summaryOverflow() { return summary_->overflowDetail(); }

    // Golden harness: B must be consumed by this view's hidden action.
    bool backLocked() {
        for (const auto& action : getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_B)
                return true;
        return false;
    }

private:
    void updateSelection(DebridProviderKind provider, bool torrenting) {
        ModeDiagram::Kind kind;
        std::string name, how, peer, setup;
        if (torrenting) {
            kind = ModeDiagram::Kind::Direct;
            name = tr("pipensx/first_run/direct");
            how = tr("pipensx/first_run/direct_how");
            peer = tr("pipensx/first_run/direct_peer");
            setup = tr("pipensx/first_run/direct_setup");
        } else if (provider == DebridProviderKind::TorrServer) {
            kind = ModeDiagram::Kind::TorrServer;
            name = tr("pipensx/first_run/torrserver");
            how = tr("pipensx/first_run/torrserver_how");
            peer = tr("pipensx/first_run/torrserver_peer");
            setup = tr("pipensx/first_run/torrserver_setup");
        } else if (provider == DebridProviderKind::RealDebrid) {
            kind = ModeDiagram::Kind::RealDebrid;
            name = tr("pipensx/first_run/realdebrid");
            how = tr("pipensx/first_run/realdebrid_how");
            peer = tr("pipensx/first_run/realdebrid_peer");
            setup = tr("pipensx/first_run/realdebrid_setup");
        } else {
            kind = ModeDiagram::Kind::TorBox;
            name = tr("pipensx/first_run/torbox");
            how = tr("pipensx/first_run/torbox_how");
            peer = tr("pipensx/first_run/torbox_peer");
            setup = tr("pipensx/first_run/torbox_setup");
        }
        summary_->setMode(kind, name, how, peer, setup);
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
        OnComplete onComplete = std::move(onComplete_);
        brls::Application::popActivity(brls::TransitionAnimation::FADE,
            [onComplete, provider, torrenting] {
                onComplete(provider, torrenting);
            });
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    OnComplete onComplete_;
    ModeSummaryPanel* summary_ = nullptr;
    bool lockBack_ = true;
};

inline void showFirstRunChoice(AppSettings* settings, DownloadManager* manager,
                               FirstRunView::OnComplete onComplete) {
    if (!settings || settings->get().firstRunCompleted)
        return;
    FirstRunView::push(settings, manager, std::move(onComplete));
}

// One-time catalog disclaimer. Non-cancelable on purpose: during the
// first-run chain it guards the step after the method choice (the provider
// link screen), so B must not be able to skip it — the choice is already
// persisted by then, and a dismissed dialog would never show the link setup
// again. `onOk` runs after the acknowledgement is saved.
inline void showCatalogDisclaimer(AppSettings* settings,
                                  std::function<void()> onOk) {
    if (settings->get().catalogDisclaimerAcknowledged) {
        onOk();
        return;
    }
    auto* box = new brls::Box(brls::Axis::COLUMN);
    brls::Style style = brls::Application::getStyle();
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setPadding(style["brls/dialog/paddingTopBottom"],
                    style["brls/dialog/paddingLeftRight"],
                    style["brls/dialog/paddingTopBottom"],
                    style["brls/dialog/paddingLeftRight"]);

    auto* body = new brls::Label();
    body->setText(tr("pipensx/disclaimer/catalog"));
    body->setFontSize(style["brls/dialog/fontSize"]);
    body->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    body->setSingleLine(false);
    box->addView(body);

    auto* warn = new brls::Label();
    warn->setText(tr("pipensx/disclaimer/catalog_direct"));
    warn->setFontSize(style["brls/dialog/fontSize"]);
    warn->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    warn->setSingleLine(false);
    warn->setTextColor(theme::warning());
    warn->setMarginTop(16);
    box->addView(warn);

    auto persist = [settings, onOk] {
        pipensx::AppSettingsData values = settings->get();
        if (!values.catalogDisclaimerAcknowledged) {
            values.catalogDisclaimerAcknowledged = true;
            std::string error;
            if (!settings->update(values, error)) {
                log_msg("[settings] disclaimer ack persist failed: %s\n",
                        error.c_str());
                // The flag did not survive: surface the failure and let the
                // chain continue anyway — the dialog reappears next launch,
                // which is the correct reminder for an unacknowledged
                // disclaimer. A silent return would strand a first-run user
                // who just picked a server mode.
                brls::Application::notify(error);
            }
        }
        onOk();
    };

    constexpr int kReadSec = 5;
    auto* dialog = new brls::Dialog(box);
    dialog->setCancelable(false);
    // Dialog::buttonClick always dismisses; the empty callback is replaced
    // below so A is a no-op until the timer elapses.
    dialog->addButton(tr("pipensx/disclaimer/ok_wait", kReadSec), [] {});

    brls::Button* ok = nullptr;
    std::function<void(brls::View*)> findOk = [&](brls::View* node) {
        if (ok)
            return;
        if (auto* button = dynamic_cast<brls::Button*>(node))
            if (button->getVisibility() == brls::Visibility::VISIBLE)
                ok = button;
        if (auto* parent = dynamic_cast<brls::Box*>(node))
            for (brls::View* child : parent->getChildren())
                findOk(child);
    };
    findOk(dialog);
    if (ok) {
        auto ready = std::make_shared<bool>(false);
        auto live = std::make_shared<bool>(true);
        auto remaining = std::make_shared<int>(kReadSec);
        ok->setState(brls::ButtonState::DISABLED);
        ok->registerClickAction(
            [dialog, persist, ready, live](brls::View*) {
                if (!*ready || !*live)
                    return true;
                *live = false;
                dialog->close(persist);
                return true;
            });
        auto tick = std::make_shared<std::function<void()>>();
        *tick = [ok, remaining, ready, live, tick]() {
            if (!*live)
                return;
            --*remaining;
            if (*remaining <= 0) {
                *ready = true;
                ok->setState(brls::ButtonState::ENABLED);
                ok->setText(tr("pipensx/common/ok"));
                return;
            }
            ok->setText(tr("pipensx/disclaimer/ok_wait", *remaining));
            brls::delay(1000, *tick);
        };
        brls::delay(1000, *tick);
    }
    dialog->open();
}

}  // namespace pipensx::ui
