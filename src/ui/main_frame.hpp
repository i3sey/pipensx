#pragma once

// UI: Switch-style top navigation frame.
//
// Replaces brls::TabFrame. A 72px top bar carries the wordmark
// and icon tabs (the active tab expands to show its label; L/R cycle
// them). Content fills the rest. Tab views are created lazily and kept
// alive across switches so a stream-install is not torn down mid-flight.

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "app/web_server.hpp"
#include "ui/common/footer_storage.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

enum class NavIconType {
    Home, Catalog, Ports, Downloads, Installed, Settings, Help, About
};

inline const char* navTabTag(NavIconType icon) {
    switch (icon) {
        case NavIconType::Home: return "home";
        case NavIconType::Catalog: return "games";
        case NavIconType::Ports: return "ports";
        case NavIconType::Downloads: return "downloads";
        case NavIconType::Installed: return "installed";
        case NavIconType::Settings: return "settings";
        case NavIconType::Help: return "help";
        case NavIconType::About: return "about";
    }
    return "unknown";
}

inline constexpr int kNavTabCount = 8;
inline constexpr float kTopbarHeight = 72.0f;

inline int navTabIndex(NavIconType icon) {
    return static_cast<int>(icon);
}

// No-op kept so older call sites compile until they are deleted.
inline void installSidebarStyle() {}

class TopTab;

class NavIcon : public brls::View {
public:
    NavIcon(NavIconType type, TopTab* owner)
        : type_(type), owner_(owner) {
        this->setWidth(24.0f);
        this->setHeight(24.0f);
        this->setAlignSelf(brls::AlignSelf::CENTER);
        this->setFocusable(false);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override;

private:
    static void drawHome(NVGcontext* vg, float gx, float gy, float s);
    static void drawCatalog(NVGcontext* vg, float gx, float gy, float s);
    static void drawPorts(NVGcontext* vg, float gx, float gy, float s);
    static void drawDownloads(NVGcontext* vg, float gx, float gy, float s);
    static void drawInstalled(NVGcontext* vg, float gx, float gy, float s);
    static void drawSettings(NVGcontext* vg, float gx, float gy, float s);
    static void drawAbout(NVGcontext* vg, float gx, float gy, float s);
    static void drawPulse(NVGcontext* vg, float gx, float gy, float s);

    NavIconType type_;
    TopTab* owner_;
};

class TopTab : public brls::Box {
public:
    TopTab(const std::string& label, NavIconType icon,
           std::function<void()> onSelect, std::function<void()> onEnter,
           std::function<brls::View*()> nextDown = {})
        : brls::Box(brls::Axis::ROW), onSelect_(std::move(onSelect)),
          onEnter_(std::move(onEnter)), nextDown_(std::move(nextDown)) {
        setFocusable(true);
        setHeight(48.0f);
        setMinWidth(44.0f);
        setAlignItems(brls::AlignItems::CENTER);
        setJustifyContent(brls::JustifyContent::CENTER);
        setCornerRadius(24.0f);
        setPadding(0, 8, 0, 8);
        setShrink(0.0f);
        iconWrap_ = new brls::Box();
        iconWrap_->setFocusable(false);
        iconWrap_->setWidth(24.0f);
        iconWrap_->setHeight(24.0f);
        iconWrap_->setShrink(0.0f);
        iconWrap_->setAlignSelf(brls::AlignSelf::CENTER);
        iconWrap_->addView(new NavIcon(icon, this));
        dot_ = new brls::Box();
        dot_->setFocusable(false);
        dot_->setWidth(8.0f);
        dot_->setHeight(8.0f);
        dot_->setCornerRadius(4.0f);
        dot_->setBackgroundColor(theme::accent());
        dot_->setPositionType(brls::PositionType::ABSOLUTE);
        dot_->setPositionTop(0);
        dot_->setPositionRight(0);
        dot_->setVisibility(brls::Visibility::GONE);
        iconWrap_->addView(dot_);
        addView(iconWrap_);
        label_ = new brls::Label();
        label_->setText(label);
        label_->setFontSize(theme::kFontSmall);
        label_->setMarginLeft(8);
        label_->setVisibility(brls::Visibility::GONE);
        addView(label_);
        registerAction("", brls::BUTTON_A,
                       [this](brls::View*) {
                           if (onEnter_)
                               onEnter_();
                           return true;
                       },
                       /*hidden=*/true);
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    bool isTabActive() const { return active_; }

    void setTabActive(bool active) {
        active_ = active;
        // Active tab needs no focus frame: the accent pill is the indicator.
        setHideHighlight(active);
        label_->setVisibility(active ? brls::Visibility::VISIBLE
                                     : brls::Visibility::GONE);
        label_->setTextColor(active ? theme::accent()
                                    : theme::textSecondary());
        setBackgroundColor(active ? theme::sidebarActive()
                                  : brls::TRANSPARENT);
        setPadding(0, active ? 16 : 8, 0, active ? 16 : 8);
    }

    // Update indicator is a small dot in the icon corner, not a number:
    // any non-zero count shows the dot, zero hides it.
    void setBadgeCount(size_t count) {
        dot_->setVisibility(count == 0 ? brls::Visibility::GONE
                                       : brls::Visibility::VISIBLE);
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (onSelect_)
            onSelect_();
    }

    brls::View* getNextFocus(brls::FocusDirection direction,
                             brls::View* current) override {
        if (direction == brls::FocusDirection::DOWN && nextDown_) {
            if (brls::View* down = nextDown_())
                return down;
        }
        return brls::Box::getNextFocus(direction, current);
    }

private:
    brls::Label* label_ = nullptr;
    brls::Box* iconWrap_ = nullptr;
    brls::Box* dot_ = nullptr;
    std::function<void()> onSelect_;
    std::function<void()> onEnter_;
    std::function<brls::View*()> nextDown_;
    bool active_ = false;
};

inline void NavIcon::draw(NVGcontext* vg, float x, float y, float width,
                          float height, brls::Style, brls::FrameContext*) {
    const NVGcolor c = (owner_ && owner_->isTabActive())
                           ? theme::accent()
                           : theme::textSecondary();
    nvgStrokeColor(vg, c);
    nvgFillColor(vg, c);
    nvgStrokeWidth(vg, 2.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    const float s = 24.0f;
    const float gx = x + (width - s) / 2.0f;
    const float gy = y + (height - s) / 2.0f;
    switch (type_) {
        case NavIconType::Home:      drawHome(vg, gx, gy, s); break;
        case NavIconType::Catalog:   drawCatalog(vg, gx, gy, s); break;
        case NavIconType::Ports:     drawPorts(vg, gx, gy, s); break;
        case NavIconType::Downloads: drawDownloads(vg, gx, gy, s); break;
        case NavIconType::Installed: drawInstalled(vg, gx, gy, s); break;
        case NavIconType::Settings:  drawSettings(vg, gx, gy, s); break;
        case NavIconType::Help:      drawPulse(vg, gx, gy, s); break;
        case NavIconType::About:     drawAbout(vg, gx, gy, s); break;
    }
}

inline void NavIcon::drawHome(NVGcontext* vg, float gx, float gy, float s) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, gx + 3.0f, gy + 11.0f);
    nvgLineTo(vg, gx + s / 2.0f, gy + 3.0f);
    nvgLineTo(vg, gx + s - 3.0f, gy + 11.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, gx + 5.5f, gy + 10.2f);
    nvgLineTo(vg, gx + 5.5f, gy + s - 3.0f);
    nvgLineTo(vg, gx + s - 5.5f, gy + s - 3.0f);
    nvgLineTo(vg, gx + s - 5.5f, gy + 10.2f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, gx + 10.0f, gy + s - 3.0f);
    nvgLineTo(vg, gx + 10.0f, gy + 14.0f);
    nvgLineTo(vg, gx + 14.0f, gy + 14.0f);
    nvgLineTo(vg, gx + 14.0f, gy + s - 3.0f);
    nvgStroke(vg);
}

inline void NavIcon::drawCatalog(NVGcontext* vg, float gx, float gy, float s) {
    const float cell = 9.0f;
    const float step = s - cell;
    for (int i = 0; i < 4; i++) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + (i % 2) * step, gy + (i / 2) * step, cell,
                       cell, 2.0f);
        nvgStroke(vg);
    }
}

inline void NavIcon::drawPorts(NVGcontext* vg, float gx, float gy, float s) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, gx + 4.0f, gy + 2.0f, s - 8.0f, s - 4.0f, 3.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, gx + 8.0f, gy + 2.0f);
    nvgLineTo(vg, gx + 8.0f, gy + s - 2.0f);
    nvgStroke(vg);
}

inline void NavIcon::drawDownloads(NVGcontext* vg, float gx, float gy, float s) {
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

inline void NavIcon::drawInstalled(NVGcontext* vg, float gx, float gy, float s) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, gx + 1.0f, gy + 1.0f, s - 2.0f, s - 2.0f, 4.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, gx + 6.0f, gy + 12.0f);
    nvgLineTo(vg, gx + 10.0f, gy + 16.0f);
    nvgLineTo(vg, gx + 17.0f, gy + 8.0f);
    nvgStroke(vg);
}

inline void NavIcon::drawSettings(NVGcontext* vg, float gx, float gy, float s) {
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

inline void NavIcon::drawAbout(NVGcontext* vg, float gx, float gy, float s) {
    const float cx = gx + s / 2.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, cx, gy + s / 2.0f, s / 2.0f - 1.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, gy + 7.0f, 1.3f);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx, gy + 11.0f);
    nvgLineTo(vg, cx, gy + 17.0f);
    nvgStroke(vg);
}

inline void NavIcon::drawPulse(NVGcontext* vg, float gx, float gy, float s) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, gx + 2.0f, gy + 15.0f);
    nvgLineTo(vg, gx + 9.0f, gy + 15.0f);
    nvgLineTo(vg, gx + 11.0f, gy + 7.0f);
    nvgLineTo(vg, gx + 13.0f, gy + 19.0f);
    nvgLineTo(vg, gx + 15.0f, gy + 15.0f);
    nvgLineTo(vg, gx + s - 2.0f, gy + 15.0f);
    nvgStroke(vg);
}

class MainFrame : public brls::Box {
public:
    MainFrame() : brls::Box(brls::Axis::COLUMN) {
        setGrow(1);

        topbar_ = new brls::Box(brls::Axis::ROW);
        topbar_->setHeight(kTopbarHeight);
        topbar_->setShrink(0.0f);
        topbar_->setAlignItems(brls::AlignItems::CENTER);
        topbar_->setPadding(0, 16, 0, 22);
        topbar_->setBackgroundColor(theme::sidebar());
        topbar_->setLineColor(theme::track());
        topbar_->setLineBottom(1.0f);

        auto* brand = new brls::Box(brls::Axis::ROW);
        brand->setFocusable(false);
        brand->setAlignItems(brls::AlignItems::CENTER);
        brand->setShrink(0.0f);
        brand->setMarginRight(8);
        auto* wordmark = new brls::Label();
        wordmark->setText(tr("pipensx/app/title"));
        wordmark->setFontSize(20.0f);
        wordmark->setLineHeight(1.0f);
        wordmark->setSingleLine(true);
        brand->addView(wordmark);
        topbar_->addView(brand);

        auto* navWrap = new brls::Box(brls::Axis::ROW);
        navWrap->setGrow(1.0f);
        navWrap->setAlignItems(brls::AlignItems::CENTER);
        navWrap->setJustifyContent(brls::JustifyContent::CENTER);
        nav_ = new brls::Box(brls::Axis::ROW);
        nav_->setAlignItems(brls::AlignItems::CENTER);
        nav_->setMinWidth(0);
        navWrap->addView(nav_);
        topbar_->addView(navWrap);

        addView(topbar_);

        contentHost_ = new brls::Box(brls::Axis::COLUMN);
        contentHost_->setGrow(1.0f);
        contentHost_->registerAction(
            "", brls::BUTTON_B,
            [this](brls::View*) {
                if (focusInTopbar())
                    return false;
                if (active_ < 0 || active_ >= static_cast<int>(tabs_.size()))
                    return false;
                brls::Application::giveFocus(tabs_[static_cast<size_t>(active_)].item);
                return true;
            },
            /*hidden=*/true, /*allowRepeating=*/false, brls::SOUND_BACK);
        addView(contentHost_);

        registerAction("", brls::BUTTON_LB,
                       [this](brls::View*) {
                           cycleTab(-1);
                           return true;
                       },
                       /*hidden=*/true, /*allowRepeating=*/true);
        registerAction("", brls::BUTTON_RB,
                       [this](brls::View*) {
                           cycleTab(1);
                           return true;
                       },
                       /*hidden=*/true, /*allowRepeating=*/true);
    }

    void addNavTab(const std::string& label, NavIconType icon,
                   brls::TabViewCreator creator, bool countBadge = false) {
        const int display = static_cast<int>(tabs_.size());
        auto* item = new TopTab(
            label, icon,
            [this, display] { selectTab(display, false); },
            [this, display] { selectTab(display, true); },
            [this, display] {
                selectTab(display, false);
                if (!content_)
                    return static_cast<brls::View*>(nullptr);
                brls::View* target = content_->getDefaultFocus();
                return target ? target : content_;
            });
        item->setMarginLeft(1);
        item->setMarginRight(1);
        nav_->addView(item);
        TabEntry entry;
        entry.icon = icon;
        entry.creator = std::move(creator);
        entry.item = item;
        entry.countBadge = countBadge;
        tabs_.push_back(std::move(entry));
        if (countBadge)
            badgeTab_ = item;
        if (tabs_.size() == 1)
            item->setTabActive(true);
    }

    void addSeparator() {
        auto* sep = new brls::Box();
        sep->setWidth(1.0f);
        sep->setHeight(28.0f);
        sep->setFocusable(false);
        sep->setBackgroundColor(theme::track());
        sep->setMarginLeft(9);
        sep->setMarginRight(9);
        sep->setShrink(0.0f);
        nav_->addView(sep);
    }

    void focusTab(int position) {
        if (position < 0 || position >= static_cast<int>(tabs_.size()))
            return;
        brls::Application::giveFocus(tabs_[static_cast<size_t>(position)].item);
    }

    void focusTab(NavIconType icon) {
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i].icon == icon) {
                focusTab(static_cast<int>(i));
                return;
            }
        }
    }

    TopTab* navItem(int position) {
        if (position < 0 || position >= static_cast<int>(tabs_.size()))
            return nullptr;
        return tabs_[static_cast<size_t>(position)].item;
    }

    int navTabCount() const { return static_cast<int>(tabs_.size()); }

    brls::View* tabView(NavIconType icon) {
        const int index = navTabIndex(icon);
        if (index < 0 || index >= kNavTabCount)
            return nullptr;
        return tabViews_[index];
    }

    void setNavTabVisible(NavIconType icon, bool visible) {
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i].icon != icon)
                continue;
            tabs_[i].item->setVisibility(
                visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            tabs_[i].item->setFocusable(visible);
            if (!visible && active_ == static_cast<int>(i))
                selectTab(firstVisibleDisplay(), false);
            return;
        }
    }

    void setUpdateCountBadge(size_t count) {
        if (badgeTab_)
            badgeTab_->setBadgeCount(count);
    }

    // The SD indicator lives in the AppletFrame bottom bar where the clock
    // was: installFooterWidget hides the time label and docks a much smaller
    // FooterStorage (pill + "SD: 72GB") into the same right-hand cluster.
    // Called after the AppletFrame wrapping this frame exists.
    void attachStorageFooter(DownloadManager* manager,
                             brls::AppletFrame* frame = nullptr) {
        manager_ = manager;
        if (frame)
            installFooterWidget(frame);
        queryTimer_.setCallback([this] { scheduleRefresh(); });
        queryTimer_.start(2000);
        scheduleRefresh();
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        if (active_ < 0 && !tabs_.empty())
            selectTab(firstVisibleDisplay(), false);
        for (brls::View* node = this; node; node = node->getParent()) {
            if (dynamic_cast<brls::AppletFrame*>(node)) {
                setShellHintStyle(node);
                break;
            }
        }
    }

    ~MainFrame() override {
        queryTimer_.stop();
        alive_->store(false);
        for (int i = 0; i < kNavTabCount; ++i)
            dropDetachedTab(tabViews_[i]);
    }

private:
    struct TabEntry {
        NavIconType icon = NavIconType::Home;
        brls::TabViewCreator creator;
        TopTab* item = nullptr;
        bool countBadge = false;
    };

    int firstVisibleDisplay() const {
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i].item->getVisibility() != brls::Visibility::GONE)
                return static_cast<int>(i);
        }
        return 0;
    }

    bool focusInTopbar() const {
        brls::View* focus = brls::Application::getCurrentFocus();
        for (brls::View* view = focus; view; view = view->getParent()) {
            if (view == topbar_)
                return true;
        }
        return false;
    }

    bool isDirectChild(brls::View* view) {
        if (!view)
            return false;
        for (brls::View* child : contentHost_->getChildren()) {
            if (child == view)
                return true;
        }
        return false;
    }

    void dropDetachedTab(brls::View*& slot) {
        if (!slot)
            return;
        if (!isDirectChild(slot))
            slot->freeView();
        slot = nullptr;
    }

    brls::View* ensureView(int display) {
        TabEntry& entry = tabs_[static_cast<size_t>(display)];
        const int index = navTabIndex(entry.icon);
        const char* tag = navTabTag(entry.icon);
        if (index >= 0 && index < kNavTabCount && tabViews_[index])
            return tabViews_[index];
        const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;
        log_msg("[ui] tab=%s\n", tag);
        log_flush();
        switch_crashlog_stage(tag);
        brls::View* view = nullptr;
        try {
            view = entry.creator();
        } catch (const std::exception& error) {
            log_msg("[ui] tab=%s failed: %s\n", tag, error.what());
            log_flush();
            view = new brls::Box();
        } catch (...) {
            log_msg("[ui] tab=%s failed: unknown exception\n", tag);
            log_flush();
            view = new brls::Box();
        }
        if (!view)
            view = new brls::Box();
        view->setGrow(1.0f);
        if (index >= 0 && index < kNavTabCount)
            tabViews_[index] = view;
        log_msg("[ui] tab=%s ready\n", tag);
        log_flush();
        if (startedUs)
            telemetry_log(
                "ui", "main",
                "event=tab_create duration_us=%llu tab=%s",
                (unsigned long long)(now_us() - startedUs), tag);
        return view;
    }

    void selectTab(int display, bool focusContent) {
        if (display < 0 || display >= static_cast<int>(tabs_.size()))
            return;
        brls::View* view = ensureView(display);
        if (active_ != display) {
            if (content_)
                contentHost_->removeView(content_, false);
            content_ = view;
            contentHost_->addView(content_);
            active_ = display;
            activeTabTag_ = navTabTag(tabs_[static_cast<size_t>(display)].icon);
            log_msg("[ui] tab=%s\n", activeTabTag_.c_str());
            log_flush();
        }
        for (size_t i = 0; i < tabs_.size(); ++i)
            tabs_[i].item->setTabActive(static_cast<int>(i) == display);
        if (focusContent && content_) {
            brls::View* target = content_->getDefaultFocus();
            brls::Application::giveFocus(target ? target : content_);
        }
    }

    void cycleTab(int delta) {
        if (tabs_.empty())
            return;
        const int n = static_cast<int>(tabs_.size());
        int next = active_ < 0 ? 0 : active_;
        for (int step = 0; step < n; ++step) {
            next = (next + delta % n + n) % n;
            if (tabs_[static_cast<size_t>(next)].item->getVisibility() !=
                brls::Visibility::GONE)
                break;
        }
        const bool fromTop = focusInTopbar();
        if (fromTop)
            brls::Application::giveFocus(tabs_[static_cast<size_t>(next)].item);
        else {
            selectTab(next, true);
        }
    }

    void installFooterWidget(brls::AppletFrame* frame) {
        if (!frame || footerStorage_)
            return;
        brls::Box* bottom = frame->getFooter();
        if (!bottom)
            return;
        brls::View* time = bottom->getView("brls/hints/time");
        brls::Box* slot = nullptr;
        if (time) {
            time->setVisibility(brls::Visibility::GONE);
            slot = time->getParent();
        }
        if (!slot)
            slot = bottom;
        footerStorage_ = new FooterStorage();
        footerStorage_->setShrink(0.0f);
        slot->addView(footerStorage_);
    }

    void scheduleRefresh() {
        if (!footerStorage_ || !manager_ || queryInFlight_)
            return;
        queryInFlight_ = true;
        auto alive = alive_;
        std::string root = manager_->rootPath();
        brls::async([this, alive, root] {
            const pipensx::StorageSpaceSnapshot storage =
                pipensx::queryStorageSpace(root);
            brls::sync([this, alive, storage] {
                if (!alive->load())
                    return;
                queryInFlight_ = false;
                if (storage.available)
                    footerStorage_->setStorage(storage.totalBytes,
                                               storage.freeBytes);
                else
                    footerStorage_->setUnavailable();
            });
        });
    }

    brls::Box* topbar_ = nullptr;
    brls::Box* nav_ = nullptr;
    brls::Box* contentHost_ = nullptr;
    brls::View* content_ = nullptr;
    FooterStorage* footerStorage_ = nullptr;
    TopTab* badgeTab_ = nullptr;
    std::vector<TabEntry> tabs_;
    int active_ = -1;
    DownloadManager* manager_ = nullptr;
    brls::View* tabViews_[kNavTabCount] = {};
    std::string activeTabTag_ = "unknown";
    brls::RepeatingTimer queryTimer_;
    bool queryInFlight_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
};

}  // namespace pipensx::ui
