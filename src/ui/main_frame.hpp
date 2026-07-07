#pragma once

// UI: collapsible navigation frame.
//
// Wraps brls::TabFrame to (1) draw an icon next to every sidebar label and
// (2) fold the sidebar down to a slim icon rail while the user is browsing a
// tab's content, so the catalogue grid gets almost the whole screen. The rail
// re-expands the moment focus returns to the menu (B / left). No extra button:
// the fold is driven purely by focus.
//
// Everything lives here, in-tree — the vendored borealis submodule is left
// untouched. We reach the private sidebar bits we need through the public
// Sidebar::getItem() / Box::getChildren() surface.

#include <functional>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "ui/theme.hpp"

namespace pipensx::ui {

enum class NavIconType { Catalog, Downloads, Installed, Settings, About };

// Shrinks the stock sidebar so the icon rail + expanded menu both look right.
// Style metrics back a shared global table, so this must run once after
// Application::init() and BEFORE the first TabFrame/Sidebar is constructed
// (both read these at inflate time).
inline void installSidebarStyle() {
    brls::Style style = brls::Application::getStyle();
    style.addMetric("brls/tab_frame/sidebar_width", 248.0f);  // was 410
    style.addMetric("brls/sidebar/padding_left", 22.0f);      // was 80
    style.addMetric("brls/sidebar/padding_right", 16.0f);     // was 40
    style.addMetric("brls/sidebar/padding_top", 28.0f);
}

// Decorative glyph shown to the left of a sidebar label. Non-focusable; its
// colour tracks the owning item's active state so it lights up with the accent
// when its tab is selected — matching the label the sidebar already recolours.
class NavIcon : public brls::View {
public:
    NavIcon(NavIconType type, brls::SidebarItem* owner)
        : type_(type), owner_(owner) {
        this->setWidth(28.0f);
        this->setHeight(28.0f);
        this->setAlignSelf(brls::AlignSelf::CENTER);
        this->setFocusable(false);
        this->setMarginRight(12.0f);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const NVGcolor c = (owner_ && owner_->isActive())
                               ? theme::accent()
                               : theme::textSecondary();
        nvgStrokeColor(vg, c);
        nvgFillColor(vg, c);
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        const float s = 24.0f;                    // glyph box side
        const float gx = x + (width - s) / 2.0f;  // glyph origin
        const float gy = y + (height - s) / 2.0f;
        switch (type_) {
            case NavIconType::Catalog:   drawCatalog(vg, gx, gy, s); break;
            case NavIconType::Downloads: drawDownloads(vg, gx, gy, s); break;
            case NavIconType::Installed: drawInstalled(vg, gx, gy, s); break;
            case NavIconType::Settings:  drawSettings(vg, gx, gy, s); break;
            case NavIconType::About:     drawAbout(vg, gx, gy, s); break;
        }
    }

private:
    // 2x2 grid of rounded squares.
    static void drawCatalog(NVGcontext* vg, float gx, float gy, float s) {
        const float cell = 9.0f;
        const float step = s - cell;  // 15 -> 6px gap
        for (int i = 0; i < 4; i++) {
            const float px = gx + (i % 2) * step;
            const float py = gy + (i / 2) * step;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, px, py, cell, cell, 2.0f);
            nvgStroke(vg);
        }
    }

    // Down arrow dropping into a tray.
    static void drawDownloads(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 1.0f);
        nvgLineTo(vg, cx, gy + 14.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 5.0f, gy + 9.0f);
        nvgLineTo(vg, cx, gy + 14.0f);
        nvgLineTo(vg, cx + 5.0f, gy + 9.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 3.0f, gy + 15.0f);
        nvgLineTo(vg, gx + 3.0f, gy + 21.0f);
        nvgLineTo(vg, gx + s - 3.0f, gy + 21.0f);
        nvgLineTo(vg, gx + s - 3.0f, gy + 15.0f);
        nvgStroke(vg);
    }

    // Rounded square with a checkmark.
    static void drawInstalled(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 1.0f, gy + 1.0f, s - 2.0f, s - 2.0f, 4.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 6.0f, gy + 12.0f);
        nvgLineTo(vg, gx + 10.0f, gy + 16.0f);
        nvgLineTo(vg, gx + 17.0f, gy + 8.0f);
        nvgStroke(vg);
    }

    // Three fader lines with offset knobs.
    static void drawSettings(NVGcontext* vg, float gx, float gy, float s) {
        const float ys[3] = {gy + 5.0f, gy + 12.0f, gy + 19.0f};
        const float knob[3] = {gx + 8.0f, gx + 16.0f, gx + 11.0f};
        for (int i = 0; i < 3; i++) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, gx + 2.0f, ys[i]);
            nvgLineTo(vg, gx + s - 2.0f, ys[i]);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, knob[i], ys[i], 2.6f);
            nvgFill(vg);
        }
    }

    // Info circle: dot over a stem.
    static void drawAbout(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, s / 2.0f - 1.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, gy + 7.0f, 1.3f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 11.0f);
        nvgLineTo(vg, cx, gy + 17.0f);
        nvgStroke(vg);
    }

    NavIconType type_;
    brls::SidebarItem* owner_;
};

// TabFrame that carries icons and folds to an icon rail while a tab is focused.
class MainFrame : public brls::TabFrame {
public:
    MainFrame() {
        expandedWidth_ =
            brls::Application::getStyle()["brls/tab_frame/sidebar_width"];
        if (expandedWidth_ < 1.0f)
            expandedWidth_ = 248.0f;
    }

    // Like TabFrame::addTab, but also plants an icon between the active-accent
    // bar and the label, and remembers the label so it can be folded away.
    void addNavTab(const std::string& label, NavIconType icon,
                   brls::TabViewCreator creator) {
        this->addTab(label, std::move(creator));
        const int index = static_cast<int>(this->sidebar->getItemsSize()) - 1;
        brls::SidebarItem* item = this->sidebar->getItem(index);
        if (!item)
            return;

        // Item children start as [accent, label]; capture the label before we
        // splice the icon in at index 1 -> [accent, icon, label].
        std::vector<brls::View*>& kids = item->getChildren();
        brls::View* labelView = kids.size() >= 2 ? kids[1] : nullptr;
        item->addView(new NavIcon(icon, item), 1);
        if (labelView) {
            labels_.push_back(labelView);
            if (collapsed_)
                labelView->setVisibility(brls::Visibility::GONE);
        }
    }

    void setCollapsed(bool collapsed) {
        if (collapsed == collapsed_)
            return;
        collapsed_ = collapsed;
        this->sidebar->setWidth(collapsed ? kCollapsedWidth : expandedWidth_);
        for (brls::View* label : labels_)
            label->setVisibility(collapsed ? brls::Visibility::GONE
                                           : brls::Visibility::VISIBLE);
    }

protected:
    // Focus in the sidebar -> expanded menu; focus in a tab's content -> icon
    // rail. Both subtrees are direct children of this frame, so this fires on
    // every menu<->content crossing.
    void onChildFocusGained(brls::View* directChild,
                            brls::View* focusedView) override {
        brls::TabFrame::onChildFocusGained(directChild, focusedView);
        setCollapsed(!(this->sidebar == directChild));
    }

private:
    // Wide enough for padding + the active-accent bar + the 28px icon.
    static constexpr float kCollapsedWidth = 88.0f;

    bool collapsed_ = false;
    float expandedWidth_ = 248.0f;
    std::vector<brls::View*> labels_;
};

}  // namespace pipensx::ui
