#pragma once

// Settings hub panels. SettingsView (settings_view.hpp) hosts a section rail
// on the left; each rail entry shows one of the panels defined here on the
// right. The panels absorb the former SettingsView long list, the Advanced
// sub-page (proxy -> Network, diagnostics/reset -> System), the Storage
// Manager screen (Storage) and the Network Health screen (Network). Nothing
// here changes persist/network/web-server logic — it only moves.

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_refresh.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/storage_manager.hpp"
#include "app/update_service.hpp"
#include "app/web_server.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/web_qr.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

extern "C" {
#include "../core/dht.h"
}

namespace pipensx::ui {

enum class SettingsSection : size_t {
    General,
    Downloads,
    Source,
    Network,
    Catalog,
    Storage,
    System,
};

inline constexpr size_t kSettingsSectionCount = 7;

inline const char* settingsSectionTag(SettingsSection section) {
    switch (section) {
        case SettingsSection::General: return "general";
        case SettingsSection::Downloads: return "downloads";
        case SettingsSection::Source: return "source";
        case SettingsSection::Network: return "network";
        case SettingsSection::Catalog: return "catalog";
        case SettingsSection::Storage: return "storage";
        case SettingsSection::System: return "system";
    }
    return "unknown";
}

inline const char* settingsSectionLabelKey(SettingsSection section) {
    switch (section) {
        case SettingsSection::General: return "pipensx/settings/section_general";
        case SettingsSection::Downloads: return "pipensx/settings/section_downloads";
        case SettingsSection::Source: return "pipensx/settings/section_source";
        case SettingsSection::Network: return "pipensx/settings/section_network";
        case SettingsSection::Catalog: return "pipensx/settings/section_catalog";
        case SettingsSection::Storage: return "pipensx/storage/title";
        case SettingsSection::System: return "pipensx/settings/section_system";
    }
    return "pipensx/settings/section_general";
}

// ---------------------------------------------------------------------------
// Section rail (the settings sidebar)
// ---------------------------------------------------------------------------

// Line glyph for one settings section, drawn in the same 24px box and stroke
// style as the MainFrame nav icons. Colour follows the item's active state.
class SettingsSectionIcon : public brls::View {
public:
    explicit SettingsSectionIcon(SettingsSection section) : section_(section) {
        setWidth(28.0f);
        setHeight(28.0f);
        setAlignSelf(brls::AlignSelf::CENTER);
        setFocusable(false);
        setMarginRight(12.0f);
    }

    void setActive(bool active) {
        active_ = active;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const NVGcolor c = active_ ? theme::accent() : theme::textSecondary();
        nvgStrokeColor(vg, c);
        nvgFillColor(vg, c);
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        const float s = 24.0f;
        const float gx = x + (width - s) / 2.0f;
        const float gy = y + (height - s) / 2.0f;
        switch (section_) {
            case SettingsSection::General: drawGeneral(vg, gx, gy, s); break;
            case SettingsSection::Downloads: drawDownloads(vg, gx, gy, s); break;
            case SettingsSection::Source: drawSource(vg, gx, gy, s); break;
            case SettingsSection::Network: drawNetwork(vg, gx, gy, s); break;
            case SettingsSection::Catalog: drawCatalog(vg, gx, gy, s); break;
            case SettingsSection::Storage: drawStorage(vg, gx, gy, s); break;
            case SettingsSection::System: drawSystem(vg, gx, gy, s); break;
        }
    }

private:
    // Three fader lines with offset knobs — the app-wide settings glyph.
    static void drawGeneral(NVGcontext* vg, float gx, float gy, float s) {
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

    // Chain link: two interlocked rounded rings.
    static void drawSource(NVGcontext* vg, float gx, float gy, float s) {
        nvgSave(vg);
        nvgTranslate(vg, gx + s / 2.0f, gy + s / 2.0f);
        nvgRotate(vg, NVG_PI / 4.0f);
        const float w = 13.0f, h = 6.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -w / 2.0f - 1.0f, -h / 2.0f, w, h, 3.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -w / 2.0f + 1.0f, -h / 2.0f, w, h, 3.0f);
        nvgStroke(vg);
        nvgRestore(vg);
    }

    // Wi-fi: three arcs and a dot.
    static void drawNetwork(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, 9.0f, NVG_PI + 0.55f, -0.55f, NVG_CW);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, 6.0f, NVG_PI + 0.55f, -0.55f, NVG_CW);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy + 5.5f, 1.6f);
        nvgFill(vg);
    }

    // Stacked layers: a crest plus two chevrons.
    static void drawCatalog(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 2.0f);
        nvgLineTo(vg, cx + 6.5f, gy + 6.0f);
        nvgLineTo(vg, cx - 6.5f, gy + 6.0f);
        nvgClosePath(vg);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 7.5f, gy + 10.5f);
        nvgLineTo(vg, cx, gy + 13.5f);
        nvgLineTo(vg, cx + 7.5f, gy + 10.5f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 7.5f, gy + 16.5f);
        nvgLineTo(vg, cx, gy + 19.5f);
        nvgLineTo(vg, cx + 7.5f, gy + 16.5f);
        nvgStroke(vg);
    }

    // SD card: body, corner notch, contact pins.
    static void drawStorage(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 2.0f, gy + 3.0f, s - 4.0f, s - 6.0f, 2.0f);
        nvgStroke(vg);
        // Corner cut at the top right.
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + s - 2.0f, gy + 3.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 3.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 6.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 5.0f, gy + 12.5f);
        nvgLineTo(vg, gx + 10.0f, gy + 12.5f);
        nvgMoveTo(vg, gx + 14.0f, gy + 12.5f);
        nvgLineTo(vg, gx + s - 5.0f, gy + 12.5f);
        nvgStroke(vg);
    }

    // Chip: body plus pins on four sides.
    static void drawSystem(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 3.2f, cy - 3.2f, 6.4f, 6.4f, 1.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 2.0f);
        nvgLineTo(vg, cx, cy - 5.5f);
        nvgMoveTo(vg, cx, cy + 5.5f);
        nvgLineTo(vg, cx, gy + s - 2.0f);
        nvgMoveTo(vg, gx + 2.0f, cy);
        nvgLineTo(vg, cx - 5.5f, cy);
        nvgMoveTo(vg, cx + 5.5f, cy);
        nvgLineTo(vg, gx + s - 2.0f, cy);
        nvgStroke(vg);
    }

    SettingsSection section_;
    bool active_ = false;
};

// Accent bar pinned to the left edge of an active section item. Always
// occupies its slot so labels keep a constant inset across the rail.
class SettingsSectionBar : public brls::View {
public:
    SettingsSectionBar() {
        setWidth(4.0f);
        setHeight(36.0f);
        setAlignSelf(brls::AlignSelf::CENTER);
        setFocusable(false);
        setMarginRight(18.0f);  // total left inset 22, matching the rail pad
    }

    void setActive(bool active) {
        active_ = active;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (!active_)
            return;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 2.0f);
        nvgFillColor(vg, theme::accent());
        nvgFill(vg);
    }

private:
    bool active_ = false;
};

// One rail entry. Focus is the selection: gaining focus switches the hub to
// this section, exactly like the main sidebar selects tabs.
class SettingsNavItem : public brls::Box {
public:
    SettingsNavItem(SettingsSection section, std::string label,
                    std::function<void(SettingsSection)> onSelected)
        : brls::Box(brls::Axis::ROW), section_(section),
          onSelected_(std::move(onSelected)) {
        setFocusable(true);
        setHeight(56.0f);
        setAlignItems(brls::AlignItems::CENTER);
        setBackgroundColor(theme::sidebar());

        bar_ = new SettingsSectionBar();
        addView(bar_);

        icon_ = new SettingsSectionIcon(section);
        addView(icon_);

        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setGrow(1);
        label_->setFontSize(18);
        label_->setTextColor(theme::textSecondary());
        label_->setText(std::move(label));
        addView(label_);
    }

    void setActive(bool active) {
        active_ = active;
        bar_->setActive(active);
        icon_->setActive(active);
        label_->setTextColor(active ? theme::textPrimary()
                                    : theme::textSecondary());
        setBackgroundColor(active ? theme::sidebarActive()
                                  : theme::sidebar());
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (onSelected_)
            onSelected_(section_);
    }

    SettingsSection section() const { return section_; }

private:
    SettingsSection section_;
    std::function<void(SettingsSection)> onSelected_;
    SettingsSectionBar* bar_ = nullptr;
    SettingsSectionIcon* icon_ = nullptr;
    brls::Label* label_ = nullptr;
    bool active_ = false;
};

// The rail: a header plus the seven section items. Fixed width, full height.
class SettingsSidebar : public brls::Box {
public:
    explicit SettingsSidebar(std::function<void(SettingsSection)> onSelected)
        : brls::Box(brls::Axis::COLUMN),
          onSelected_(std::move(onSelected)) {
        setWidth(kWidth);
        setAlignSelf(brls::AlignSelf::STRETCH);
        setBackgroundColor(theme::sidebar());
        setPadding(26, 0, 20, 0);

        auto* head = new brls::Label();
        head->setText(tr("pipensx/nav/settings"));
        head->setSingleLine(true);
        head->setFontSize(theme::kFontCaption);
        head->setTextColor(theme::textTertiary());
        head->setMarginBottom(10);
        head->setMarginLeft(24);
        addView(head);

        for (size_t i = 0; i < kSettingsSectionCount; ++i) {
            const auto section = static_cast<SettingsSection>(i);
            auto* item = new SettingsNavItem(
                section, tr(settingsSectionLabelKey(section)),
                [this](SettingsSection picked) {
                    if (onSelected_)
                        onSelected_(picked);
                });
            item->setId(std::string("settings-nav-") +
                        settingsSectionTag(section));
            items_[i] = item;
            addView(item);
        }
    }

    SettingsNavItem* item(SettingsSection section) {
        return items_[static_cast<size_t>(section)];
    }

    void setActive(SettingsSection section) {
        for (size_t i = 0; i < kSettingsSectionCount; ++i)
            items_[i]->setActive(static_cast<SettingsSection>(i) == section);
    }

    static constexpr float kWidth = 296.0f;

private:
    std::function<void(SettingsSection)> onSelected_;
    SettingsNavItem* items_[kSettingsSectionCount] = {};
};

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

// Common pane scaffold: padded content inside a scrolling frame that fills
// the right-hand column. Panels stay constructed (and hidden) so switching
// sections is instant and every cell keeps its state.
class SettingsPanel : public brls::Box {
public:
    SettingsPanel() : brls::Box(brls::Axis::COLUMN) {
        setGrow(1);
        content_ = new brls::Box(brls::Axis::COLUMN);
        content_->setPadding(24, 34, 24, 34);
        scroll_ = new brls::ScrollingFrame();
        scroll_->setGrow(1);
        scroll_->setContentView(content_);
        addView(scroll_);
    }

    // The panel became the visible one.
    virtual void onShown() {}

    // Re-sync cells from persisted settings (after a factory reset, a
    // first-run chooser round-trip, or the tab coming back on screen).
    virtual void applyValues() {}

protected:
    brls::Box* content_ = nullptr;
    brls::ScrollingFrame* scroll_ = nullptr;
};

// --- General: language + launch behaviour --------------------------------

class GeneralPanel : public SettingsPanel {
public:
    explicit GeneralPanel(AppSettings* settings) : settings_(settings) {
        addSection(content_, tr("pipensx/settings/language"));
        language_ = new brls::SelectorCell();
        language_->init(tr("pipensx/settings/language"),
            {tr("pipensx/settings/language_auto"),
             tr("pipensx/settings/language_en"),
             tr("pipensx/settings/language_ru"),
             tr("pipensx/settings/language_pt_br"),
             tr("pipensx/settings/language_fr"),
             tr("pipensx/settings/language_es"),
             tr("pipensx/settings/language_zh"),
             tr("pipensx/settings/language_it")},
            languageIndex(settings_->get().language),
            [this](int selected) {
                AppSettingsData values = settings_->get();
                const std::string previous = values.language;
                values.language = kLanguageValues[selected];
                if (!persistSettings(settings_, values, "language")) {
                    language_->setSelection(languageIndex(previous), true);
                    return;
                }
                // Borealis loads translations once, inside Application::init().
                brls::Application::notify(
                    tr("pipensx/settings/language_restart"));
            });
        content_->addView(language_);

        addSection(content_, tr("pipensx/settings/section_launch"));
        checkForUpdates_ = new brls::BooleanCell();
        checkForUpdates_->init(tr("pipensx/settings/check_updates"),
            settings_->get().checkForUpdatesOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.checkForUpdatesOnLaunch;
                values.checkForUpdatesOnLaunch = enabled;
                if (!persistSettings(settings_, values, "update_check"))
                    checkForUpdates_->setOn(previous, false);
            });
        content_->addView(checkForUpdates_);

        addSection(content_, tr("pipensx/settings/section_exit"));
        confirmExit_ = new brls::BooleanCell();
        confirmExit_->init(tr("pipensx/settings/confirm_exit"),
            settings_->get().confirmExit,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previousConfirm = values.confirmExit;
                bool previousWarn = values.warnOnActiveDownload;
                values.confirmExit = enabled;
                if (!enabled)
                    values.warnOnActiveDownload = false;
                if (!persistSettings(settings_, values, "confirm_exit")) {
                    confirmExit_->setOn(previousConfirm, false);
                    return;
                }
                if (!enabled) {
                    warnActiveDownload_->setOn(false, false);
                }
                warnActiveDownload_->setEnabled(enabled);
            });
        content_->addView(confirmExit_);

        warnActiveDownload_ = new brls::BooleanCell();
        warnActiveDownload_->init(tr("pipensx/settings/warn_active_download"),
            settings_->get().warnOnActiveDownload,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.warnOnActiveDownload;
                values.warnOnActiveDownload = enabled;
                if (!persistSettings(settings_, values, "warn_active_download"))
                    warnActiveDownload_->setOn(previous, false);
            });
        warnActiveDownload_->setEnabled(settings_->get().confirmExit);
        content_->addView(warnActiveDownload_);
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        language_->setSelection(languageIndex(values.language), true);
        checkForUpdates_->setOn(values.checkForUpdatesOnLaunch, false);
        confirmExit_->setOn(values.confirmExit, false);
        warnActiveDownload_->setOn(values.warnOnActiveDownload, false);
        warnActiveDownload_->setEnabled(values.confirmExit);
    }

private:
    // Settings-selector row for a stored language value; falls back to the
    // "auto" row so a value from a newer build cannot leave the cell blank.
    static int languageIndex(const std::string& value) {
        for (size_t i = 0; i < std::size(kLanguageValues); ++i) {
            if (value == kLanguageValues[i])
                return static_cast<int>(i);
        }
        return 0;
    }

    AppSettings* settings_;
    brls::SelectorCell* language_ = nullptr;
    brls::BooleanCell* checkForUpdates_ = nullptr;
    brls::BooleanCell* confirmExit_ = nullptr;
    brls::BooleanCell* warnActiveDownload_ = nullptr;
};

// --- Downloads: queue behaviour + install target -------------------------

class DownloadsPanel : public SettingsPanel {
public:
    DownloadsPanel(AppSettings* settings, DownloadManager* manager)
        : settings_(settings), manager_(manager) {
        addSection(content_, tr("pipensx/settings/section_download"));
        streamSelection_ = new brls::SelectorCell();
        streamSelection_->init(tr("pipensx/settings/stream_selection"),
            {tr("pipensx/settings/stream_all"),
             tr("pipensx/settings/stream_packages")},
            settings_->get().streamSelection == StreamSelection::PackagesOnly
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                StreamSelection previous = values.streamSelection;
                values.streamSelection = selected == 1
                    ? StreamSelection::PackagesOnly
                    : StreamSelection::AllFiles;
                if (!persistSettings(settings_, values, "stream_selection")) {
                    streamSelection_->setSelection(
                        previous == StreamSelection::PackagesOnly ? 1 : 0,
                        true);
                    return;
                }
            });
        content_->addView(streamSelection_);

        maxActiveDownloads_ = new brls::SelectorCell();
        maxActiveDownloads_->init(tr("pipensx/settings/max_active_downloads"),
            {"1", "2", "3", "4"},
            static_cast<int>(settings_->get().maxActiveDownloads) - 1,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                uint32_t previous = values.maxActiveDownloads;
                values.maxActiveDownloads =
                    pipensx::clampMaxActiveDownloads(
                        static_cast<uint64_t>(selected) + 1);
                if (!persistSettings(settings_, values,
                                     "max_active_downloads")) {
                    maxActiveDownloads_->setSelection(
                        static_cast<int>(previous) - 1, true);
                    return;
                }
                if (manager_)
                    manager_->setMaxActiveDownloads(
                        values.maxActiveDownloads);
            });
        content_->addView(maxActiveDownloads_);

        showCompleted_ = new brls::BooleanCell();
        showCompleted_->init(tr("pipensx/settings/show_completed"),
            settings_->get().showCompletedDownloads,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.showCompletedDownloads;
                values.showCompletedDownloads = enabled;
                if (!persistSettings(settings_, values, "show_completed"))
                    showCompleted_->setOn(previous, false);
            });
        content_->addView(showCompleted_);

        addSection(content_, tr("pipensx/settings/section_install"));
        installLocation_ = new brls::SelectorCell();
        installLocation_->init(tr("pipensx/settings/install_location"),
            {tr("pipensx/settings/install_sd"),
             tr("pipensx/settings/install_nand")},
            settings_->get().installLocation == InstallLocation::SystemMemory
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                InstallLocation previous = values.installLocation;
                values.installLocation = selected == 1
                    ? InstallLocation::SystemMemory
                    : InstallLocation::SdCard;
                if (!persistSettings(settings_, values, "install_location")) {
                    installLocation_->setSelection(
                        previous == InstallLocation::SystemMemory ? 1 : 0,
                        true);
                    return;
                }
                if (manager_)
                    manager_->setInstallTarget(
                        installTargetFor(values.installLocation));
            });
        content_->addView(installLocation_);
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        streamSelection_->setSelection(
            values.streamSelection == StreamSelection::PackagesOnly ? 1 : 0,
            true);
        maxActiveDownloads_->setSelection(
            static_cast<int>(values.maxActiveDownloads) - 1, true);
        showCompleted_->setOn(values.showCompletedDownloads, false);
        installLocation_->setSelection(
            values.installLocation == InstallLocation::SystemMemory ? 1 : 0,
            true);
        if (manager_)
            manager_->setInstallTarget(
                installTargetFor(values.installLocation));
    }

private:
    AppSettings* settings_;
    DownloadManager* manager_;
    brls::SelectorCell* streamSelection_ = nullptr;
    brls::SelectorCell* maxActiveDownloads_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
    brls::SelectorCell* installLocation_ = nullptr;
};

// --- Source: the fetch method + current-source status --------------------

class SourcePanel : public SettingsPanel {
public:
    SourcePanel(AppSettings* settings,
                std::function<void()> onChangeSource)
        : settings_(settings), onChangeSource_(std::move(onChangeSource)) {
        addSection(content_, tr("pipensx/settings/section_debrid"));
        // The source picker lives in the first-run chooser; Settings only
        // links to it so the two never drift apart (or duplicate each other).
        downloadSource_ = actionCell(tr("pipensx/settings/source_title"), "",
            [this] {
                if (onChangeSource_)
                    onChangeSource_();
            });
        // Same id as the old debrid-link cell so the golden runner keeps a
        // stable way to scroll this section into view.
        downloadSource_->setId("settings-debrid-link");
        content_->addView(downloadSource_);
        addNote(content_, tr("pipensx/settings/source_hint"));

        addSection(content_, tr("pipensx/settings/source_current"));
        statusValue_ = addStatusRow(content_,
                                    tr("pipensx/settings/source_status"));
        providerValue_ = addStatusRow(content_,
                                      tr("pipensx/settings/source_provider"));
        torrentingValue_ =
            addStatusRow(content_,
                         tr("pipensx/settings/source_torrenting"));
        refreshDetail();
    }

    void refreshDetail() {
        const AppSettingsData& values = settings_->get();
        if (values.torrentingEnabled) {
            downloadSource_->setDetailText(tr("pipensx/first_run/direct"));
        } else {
            const char* provider = debridProviderName(values.debridProvider);
            // Spelled out rather than picking the key with a ternary: the i18n
            // checker only sees keys that appear as a literal first argument.
            downloadSource_->setDetailText(
                activeDebridKey(values).empty()
                    ? tr("pipensx/settings/debrid_not_linked", provider)
                    : tr("pipensx/settings/debrid_linked", provider));
        }
        const bool configured = values.torrentingEnabled ||
                                !activeDebridKey(values).empty();
        statusValue_->setText(configured
            ? tr("pipensx/settings/source_connected")
            : tr("pipensx/settings/source_not_set_up"));
        statusValue_->setTextColor(configured ? theme::success()
                                              : theme::textSecondary());
        providerValue_->setText(providerText(values));
        torrentingValue_->setText(values.torrentingEnabled
            ? tr("pipensx/settings/on")
            : tr("pipensx/settings/off"));
    }

private:
    static std::string providerText(const AppSettingsData& values) {
        if (values.torrentingEnabled)
            return tr("pipensx/first_run/direct");
        return std::string(debridProviderName(values.debridProvider)) + " · " +
               (values.debridProvider == DebridProviderKind::TorrServer
                    ? tr("pipensx/first_run/chip_free")
                    : tr("pipensx/first_run/chip_paid"));
    }

    // Read-only label/value row for the "current source" block.
    static brls::Label* addStatusRow(brls::Box* content,
                                     const std::string& label) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(false);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginBottom(8);

        auto* name = new brls::Label();
        name->setSingleLine(true);
        name->setGrow(1);
        name->setFontSize(18);
        name->setTextColor(theme::textSecondary());
        name->setText(label);
        row->addView(name);

        auto* value = new brls::Label();
        value->setSingleLine(true);
        value->setFontSize(18);
        value->setTextColor(theme::textPrimary());
        row->addView(value);
        content->addView(row);
        return value;
    }

    AppSettings* settings_;
    std::function<void()> onChangeSource_;
    brls::DetailCell* downloadSource_ = nullptr;
    brls::Label* statusValue_ = nullptr;
    brls::Label* providerValue_ = nullptr;
    brls::Label* torrentingValue_ = nullptr;
};

// --- Network: web companion, proxy, live status --------------------------

class NetworkPanel : public SettingsPanel {
public:
    NetworkPanel(AppSettings* settings, DownloadManager* manager,
                 WebServer* webServer, std::string ipAddress)
        : settings_(settings), manager_(manager), webServer_(webServer),
          ipAddress_(std::move(ipAddress)) {
        if (ipAddress_.empty())
            ipAddress_ = brls::Application::getPlatform()->getIpAddress();

        addSection(content_, tr("pipensx/settings/section_web"));
        webToggle_ = new brls::BooleanCell();
        webToggle_->init(tr("pipensx/settings/web_toggle"),
            settings_->get().webServerEnabled,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.webServerEnabled;
                values.webServerEnabled = enabled;
                if (!persistSettings(settings_, values, "web_server")) {
                    webToggle_->setOn(previous, false);
                    return;
                }
                if (webServer_) {
                    if (enabled) {
                        if (!webServer_->start())
                            brls::Application::notify(
                                tr("pipensx/settings/web_start_failed"));
                    } else {
                        webServer_->stop();
                    }
                }
                updateWebCells();
            });
        content_->addView(webToggle_);
        webAddress_ = actionCell(tr("pipensx/settings/web_address"),
            "", [this] { showWebQr(); });
        content_->addView(webAddress_);
        webPin_ = actionCell(tr("pipensx/settings/web_pin"),
            "", [this] { editWebPin(); });
        content_->addView(webPin_);
        updateWebCells();

        addSection(content_, tr("pipensx/settings/proxy"));
        proxy_ = actionCell(tr("pipensx/settings/proxy"), "",
            [this] { editProxy(); });
        content_->addView(proxy_);
        refreshProxyDetail();
        addNote(content_, tr("pipensx/settings/proxy_note"));

        addSection(content_, tr("pipensx/diag/title"));
        internet_ = addHealthRow(tr("pipensx/diag/internet"));
        dht_ = addHealthRow(tr("pipensx/diag/dht"));
        peers_ = addHealthRow(tr("pipensx/diag/peers"));
        source_ = addHealthRow(tr("pipensx/diag/source"));
        catalog_ = addHealthRow(tr("pipensx/diag/catalog"));
        proxyHealth_ = addHealthRow(tr("pipensx/diag/proxy"));

        auto* recheck = new brls::Button();
        recheck->setText(tr("pipensx/settings/recheck"));
        recheck->setMarginTop(12);
        recheck->registerClickAction([this](brls::View*) {
            refreshHealth();
            return true;
        });
        content_->addView(recheck);
    }

    void onShown() override {
        // The console may have joined/left Wi-Fi since the last visit, and
        // the health rows are only refreshed on demand — never by a timer.
        refreshHealth();
    }

    void updateWebCells() {
        if (webAddress_)
            webAddress_->setDetailText(webAddressText());
        if (webPin_)
            webPin_->setDetailText(
                settings_->get().webServerPin.empty() ? "——" : "••••");
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        webToggle_->setOn(values.webServerEnabled, false);
        updateWebCells();
        // A reset clears the proxy, so the environment has to follow it.
        pipensx::applyProxySetting(values.proxyUrl);
        refreshProxyDetail();
        refreshHealth();
    }

private:
    std::string webAddressText() const {
        if (!settings_->get().webServerEnabled)
            return tr("pipensx/settings/web_disabled");
        std::string url = webCompanionUrl(webServer_, true);
        return url.empty() ? tr("pipensx/settings/web_address_none") : url;
    }

    void showWebQr() {
        const std::string url = webAddressText();
        if (url.rfind("http://", 0) != 0) {
            brls::Application::notify(url);
            return;
        }
        showWebQrDialog(url, settings_->get().webServerPin);
    }

    void editWebPin() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidWebPin(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/web_pin_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.webServerPin = text;
                if (!persistSettings(settings_, values, "web_pin"))
                    return;
                if (webServer_)
                    webServer_->setPin(settings_->get().webServerPin);
                updateWebCells();
            },
            tr("pipensx/settings/web_pin"),
            tr("pipensx/settings/web_pin_detail"), 8,
            settings_->get().webServerPin, brls::KEYBOARD_DISABLE_NONE);
    }

    void refreshProxyDetail() {
        const std::string& url = settings_->get().proxyUrl;
        proxy_->setDetailText(url.empty()
            ? tr("pipensx/settings/proxy_direct") : url);
    }

    void editProxy() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidProxyUrl(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/proxy_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.proxyUrl = text;
                if (!persistSettings(settings_, values, "proxy_url"))
                    return;
                // Takes effect on the next request, not the next launch.
                pipensx::applyProxySetting(text);
                refreshProxyDetail();
            },
            tr("pipensx/settings/proxy"),
            tr("pipensx/settings/proxy_detail"), 128,
            settings_->get().proxyUrl, brls::KEYBOARD_DISABLE_NONE);
    }

    // Health rows: built once, values replaced in place (the old Network
    // Health screen minus its 1-second timer — refresh is on demand).
    brls::Label* addHealthRow(const std::string& label) {
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

        auto* value = new brls::Label();
        value->setSingleLine(true);
        value->setAutoAnimate(false);
        value->setFontSize(18);
        value->setTextColor(theme::textPrimary());
        row->addView(value);
        content_->addView(row);
        return value;
    }

    void setHealthValue(brls::Label* label, const std::string& text,
                        NVGcolor color) {
        setTextIfChanged(label, text);
        label->setTextColor(color);
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

    void refreshHealth() {
        const bool online = !ipAddress_.empty() && ipAddress_ != "-";
        setHealthValue(internet_,
                       online ? tr("pipensx/diag/connected")
                              : tr("pipensx/diag/offline"),
                       online ? theme::success() : theme::error());

        int dhtGood = 0;
        int dhtDubious = 0;
        const bool dhtOn = dht_shared_running();
        if (dhtOn)
            dht_shared_nodes(&dhtGood, &dhtDubious);
        if (!dhtOn) {
            setHealthValue(dht_, tr("pipensx/diag/dht_off"),
                           theme::textSecondary());
        } else if (dhtGood > 0) {
            setHealthValue(dht_,
                           tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                           theme::success());
        } else if (dhtDubious > 0) {
            setHealthValue(dht_,
                           tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                           theme::warning());
        } else {
            setHealthValue(dht_, tr("pipensx/diag/dht_bootstrapping"),
                           theme::warning());
        }

        uint32_t peers = 0;
        if (manager_) {
            for (const DownloadTask& task : manager_->snapshotUi())
                if (task.status == DownloadStatus::Downloading)
                    peers += task.peers;
        }
        setHealthValue(peers_, tr("pipensx/diag/peers_n", peers),
                       peers > 0 ? theme::success() : theme::textSecondary());

        const AppSettingsData values = settings_->get();
        if (values.torrentingEnabled) {
            setHealthValue(source_, tr("pipensx/first_run/direct"),
                           theme::accent());
        } else {
            const bool linked = !activeDebridKey(values).empty();
            const std::string text =
                linked ? debridProviderName(values.debridProvider)
                       : tr("pipensx/diag/not_linked");
            setHealthValue(source_, text,
                           linked ? theme::success()
                                  : theme::textSecondary());
        }
        setHealthValue(proxyHealth_,
                       values.proxyUrl.empty() ? tr("pipensx/diag/disabled")
                                               : tr("pipensx/diag/enabled"),
                       values.proxyUrl.empty() ? theme::textSecondary()
                                               : theme::accent());
        setHealthValue(catalog_, catalogAge(values.lastCatalogRefreshWallSec),
                       theme::textSecondary());
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    WebServer* webServer_;
    std::string ipAddress_;
    brls::BooleanCell* webToggle_ = nullptr;
    brls::DetailCell* webAddress_ = nullptr;
    brls::DetailCell* webPin_ = nullptr;
    brls::DetailCell* proxy_ = nullptr;
    brls::Label* internet_ = nullptr;
    brls::Label* dht_ = nullptr;
    brls::Label* peers_ = nullptr;
    brls::Label* source_ = nullptr;
    brls::Label* catalog_ = nullptr;
    brls::Label* proxyHealth_ = nullptr;
};

// --- Catalog: refresh behaviour, source URL, manual refresh ---------------

class CatalogPanel : public SettingsPanel {
public:
    CatalogPanel(AppSettings* settings, CatalogService* catalog,
                 GameMetadataService* metadata,
                 std::shared_ptr<std::atomic<bool>> alive,
                 std::function<void()> onMetadataRefreshed)
        : settings_(settings), catalog_(catalog), metadata_(metadata),
          alive_(std::move(alive)),
          onMetadataRefreshed_(std::move(onMetadataRefreshed)) {
        addSection(content_, tr("pipensx/settings/section_catalog"));
        refreshCatalog_ = new brls::BooleanCell();
        refreshCatalog_->init(tr("pipensx/settings/auto_refresh"),
            settings_->get().refreshCatalogOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.refreshCatalogOnLaunch;
                values.refreshCatalogOnLaunch = enabled;
                if (!persistSettings(settings_, values, "catalog_refresh"))
                    refreshCatalog_->setOn(previous, false);
            });
        content_->addView(refreshCatalog_);

        catalogSource_ = actionCell(tr("pipensx/settings/catalog_source"), "",
            [this] { editCatalogSource(); });
        content_->addView(catalogSource_);
        refreshCatalogSourceDetail();

        content_->addView(actionCell(tr("pipensx/settings/update_now"),
            tr("pipensx/settings/update_now_detail"),
            [this] { updateAllNow(); }));
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        refreshCatalog_->setOn(values.refreshCatalogOnLaunch, false);
        refreshCatalogSourceDetail();
    }

private:
    // The manual "Update now" action chains catalog then artwork.
    void updateAllNow() {
        if (refreshInFlight_)
            return;
        refreshCatalogNow([this] { refreshMetadataNow(); });
    }

    void refreshCatalogNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_)
            return;
        if (!tryBeginCatalogRefresh())
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_catalog"));
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        AppSettings* settings = settings_;
        const std::string catalogSourceUrl =
            effectiveCatalogSourceUrl(settings_->get().catalogSourceUrl);
        brls::async([this, alive, catalog, settings, catalogSourceUrl,
                     onDone = std::move(onDone)]() mutable {
            std::vector<CatalogEntry> entries;
            std::string error;
            bool ok = catalog->fetchLatest(entries, error, catalogSourceUrl);
            brls::sync([this, alive, ok, entries = std::move(entries),
                        error = std::move(error), catalogSourceUrl, catalog,
                        settings, onDone = std::move(onDone)]() mutable {
                if (ok) {
                    catalog->adopt(std::move(entries), catalogSourceUrl);
                    std::string stampError;
                    if (!recordCatalogRefreshSuccess(settings, true, false,
                                                     stampError) &&
                        !stampError.empty()) {
                        diagnostic_error("settings", "catalog_refresh_time",
                                         "error=%s", stampError.c_str());
                    }
                }
                endCatalogRefresh();
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("catalog", "settings_refresh", "error=%s",
                                     error.c_str());
                    brls::Application::notify(formatCatalogRefreshError(error));
                    return;
                }
                brls::Application::notify(
                    tr("pipensx/catalog/updated_catalog",
                       catalog->entries().size()));
                if (onDone)
                    onDone();
            });
        });
    }

    void refreshMetadataNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_ || !metadata_)
            return;
        if (!tryBeginCatalogRefresh())
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_artwork"));
        auto alive = alive_;
        GameMetadataService* metadata = metadata_;
        AppSettings* settings = settings_;
        brls::async([this, alive, metadata, settings,
                     onDone = std::move(onDone)]() mutable {
            MetadataSnapshot snapshot;
            std::string error;
            bool ok = metadata->fetchLatest(snapshot, error);
            brls::sync([this, alive, ok, snapshot = std::move(snapshot),
                        error = std::move(error), metadata, settings,
                        onDone = std::move(onDone)]() mutable {
                if (ok) {
                    metadata->adopt(std::move(snapshot));
                    metadata->dropMemoryImageCache();
                    std::string stampError;
                    if (!recordCatalogRefreshSuccess(settings, false, true,
                                                     stampError) &&
                        !stampError.empty()) {
                        diagnostic_error("settings", "metadata_refresh_time",
                                         "error=%s", stampError.c_str());
                    }
                }
                endCatalogRefresh();
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("metadata", "settings_refresh",
                                     "error=%s", error.c_str());
                    brls::Application::notify(formatCatalogRefreshError(error));
                    return;
                }
                if (onMetadataRefreshed_)
                    onMetadataRefreshed_();
                brls::Application::notify(
                    tr("pipensx/catalog/updated_artwork", metadata->size()));
                if (onDone)
                    onDone();
            });
        });
    }

    void refreshCatalogSourceDetail() {
        const std::string& url = settings_->get().catalogSourceUrl;
        catalogSource_->setDetailText(
            url.empty() ? tr("pipensx/settings/catalog_source_default") : url);
    }

    void editCatalogSource() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidCatalogSourceUrl(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/catalog_source_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.catalogSourceUrl = text;
                if (!persistSettings(settings_, values, "catalog_source_url"))
                    return;
                refreshCatalogSourceDetail();
            },
            tr("pipensx/settings/catalog_source"),
            tr("pipensx/settings/catalog_source_detail"), 512,
            settings_->get().catalogSourceUrl, brls::KEYBOARD_DISABLE_NONE);
    }

    AppSettings* settings_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::function<void()> onMetadataRefreshed_;
    brls::BooleanCell* refreshCatalog_ = nullptr;
    brls::DetailCell* catalogSource_ = nullptr;
    bool refreshInFlight_ = false;
};

// --- Storage: SD breakdown + cleanup (the former Storage screen) -----------

class StoragePanel : public SettingsPanel {
public:
    StoragePanel(DownloadManager* manager, GameMetadataService* metadata,
                 std::shared_ptr<std::atomic<bool>> alive)
        : manager_(manager), metadata_(metadata),
          alive_(std::move(alive)) {
        meter_ = new StorageMeter();
        meter_->setHeader(tr("pipensx/storage/title_sd_card"));
        content_->addView(meter_);

        addSection(content_, tr("pipensx/storage/breakdown"));
        breakdown_ = new brls::Box(brls::Axis::COLUMN);
        breakdown_->setMarginBottom(10);
        content_->addView(breakdown_);

        addSection(content_, tr("pipensx/storage/cleanup"));
        clearCompleted_ = actionCell(
            tr("pipensx/storage/clear_completed"), "", [this] {
                confirmClearCompleted();
            });
        content_->addView(clearCompleted_);
        clearImages_ = actionCell(
            tr("pipensx/storage/clear_images"), "", [this] {
                confirmClearImages();
            });
        content_->addView(clearImages_);
        clearTorrents_ = actionCell(
            tr("pipensx/storage/clear_torrents"), "", [this] {
                confirmClearTorrents();
            });
        content_->addView(clearTorrents_);
        clearTemporary_ = actionCell(
            tr("pipensx/storage/clear_temporary"), "", [this] {
                confirmClearTemporary();
            });
        content_->addView(clearTemporary_);

        canFree_ = new brls::Label();
        canFree_->setFontSize(17);
        canFree_->setTextColor(theme::success());
        canFree_->setMarginTop(14);
        canFree_->setVisibility(brls::Visibility::GONE);
        content_->addView(canFree_);
        meter_->setUnavailable();
        setLoading(true);
    }

    void onShown() override {
        refresh();
    }

private:
    struct ScanPayload {
        StorageBreakdown snapshot;
        uint64_t completedBytes = 0;
        uint64_t orphanBytes = 0;
        bool hasFinished = false;
    };

    static ScanPayload collectScan(DownloadManager* manager) {
        ScanPayload payload;
        payload.snapshot = scanStorageBreakdown(manager->rootPath());
        std::vector<DownloadTask> tasks = manager->snapshot();
        std::vector<std::string> active;
        active.reserve(tasks.size());
        for (const DownloadTask& task : tasks) {
            active.push_back(task.id);
            if (task.status != DownloadStatus::Completed &&
                task.status != DownloadStatus::Installed)
                continue;
            payload.hasFinished = true;
            uint64_t size = 0;
            if (directorySize(task.dataPath, size))
                payload.completedBytes =
                    size > UINT64_MAX - payload.completedBytes
                        ? UINT64_MAX
                        : payload.completedBytes + size;
        }
        payload.orphanBytes =
            pipensx::orphanTorrentBytes(manager->torrentRoot(), active);
        return payload;
    }

    void applyScan(const ScanPayload& payload) {
        snapshot_ = payload.snapshot;
        completedBytes_ = payload.completedBytes;
        orphanBytes_ = payload.orphanBytes;
        hasFinished_ = payload.hasFinished;

        if (meter_) {
            if (snapshot_.available)
                meter_->setStorage(snapshot_.totalBytes, snapshot_.freeBytes);
            else
                meter_->setUnavailable();
        }

        rebuildRows();

        if (clearCompleted_)
            clearCompleted_->setDetailText(completedDownloadsDetail());
        if (clearImages_)
            clearImages_->setDetailText(
                recoverableDetail(snapshot_.imageCacheBytes));
        if (clearTorrents_)
            clearTorrents_->setDetailText(recoverableDetail(orphanBytes_));
        if (clearTemporary_)
            clearTemporary_->setDetailText(
                recoverableDetail(snapshot_.temporaryBytes));

        if (canFree_) {
            uint64_t total = orphanBytes_;
            const uint64_t parts[3] = {
                hasFinished_ ? completedBytes_ : 0,
                snapshot_.imageCacheBytes, snapshot_.temporaryBytes};
            for (uint64_t part : parts)
                total = part > UINT64_MAX - total ? UINT64_MAX
                                                  : total + part;
            if (total > 0) {
                canFree_->setText(
                    tr("pipensx/storage/can_free", formatBytes(total)));
                canFree_->setVisibility(brls::Visibility::VISIBLE);
            } else {
                canFree_->setVisibility(brls::Visibility::GONE);
            }
        }
    }

    void setLoading(bool loading) {
        const std::string detail =
            loading ? tr("pipensx/settings/checking") : std::string();
        brls::DetailCell* actions[] = {
            clearCompleted_, clearImages_, clearTorrents_, clearTemporary_};
        for (brls::DetailCell* action : actions) {
            if (!action)
                continue;
            action->setFocusable(!loading);
            if (loading)
                action->setDetailText(detail);
        }
        if (loading && canFree_)
            canFree_->setVisibility(brls::Visibility::GONE);
    }

    void refresh() {
        if (!manager_)
            return;
        if (refreshInFlight_) {
            refreshPending_ = true;
            return;
        }
        refreshInFlight_ = true;
        setLoading(true);
        auto alive = alive_;
        DownloadManager* manager = manager_;
        brls::async([this, alive, manager] {
            const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;
            ScanPayload payload = collectScan(manager);
            if (startedUs)
                telemetry_log(
                    "ui", "storage", "event=scan duration_us=%llu",
                    (unsigned long long)(now_us() - startedUs));
            brls::sync([this, alive, payload = std::move(payload)] {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                applyScan(payload);
                if (refreshPending_) {
                    refreshPending_ = false;
                    refresh();
                } else
                    setLoading(false);
            });
        });
    }

    void rebuildRows() {
        if (!breakdown_)
            return;
        breakdown_->clearViews();
        addRow(tr("pipensx/storage/cat_downloads"), snapshot_.downloadsBytes);
        addRow(tr("pipensx/storage/cat_torrents"), snapshot_.torrentBytes);
        addRow(tr("pipensx/storage/cat_images"), snapshot_.imageCacheBytes);
        addRow(tr("pipensx/storage/cat_metadata"),
               snapshot_.metadataCacheBytes);
        addRow(tr("pipensx/storage/cat_temporary"),
               snapshot_.temporaryBytes);
        addRow(tr("pipensx/storage/cat_icons"), snapshot_.iconsBytes);
        addRow(tr("pipensx/storage/cat_other"), snapshot_.otherBytes);
    }

    void addRow(const std::string& label, uint64_t bytes) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(false);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginBottom(4);

        auto* name = new brls::Label();
        name->setSingleLine(true);
        name->setGrow(1);
        name->setFontSize(17);
        name->setTextColor(theme::textSecondary());
        name->setText(label);
        row->addView(name);

        auto* value = new brls::Label();
        value->setSingleLine(true);
        value->setFontSize(17);
        value->setTextColor(theme::textPrimary());
        value->setText(formatBytes(bytes));
        row->addView(value);

        breakdown_->addView(row);
    }

    std::string recoverableDetail(uint64_t bytes) {
        return bytes == 0 ? tr("pipensx/storage/nothing_to_recover")
                          : tr("pipensx/storage/recoverable",
                               formatBytes(bytes));
    }

    std::string completedDownloadsDetail() {
        if (!hasFinished_)
            return tr("pipensx/storage/nothing_to_recover");
        return tr("pipensx/storage/recoverable",
                  formatBytes(completedBytes_));
    }

    void confirmClearCompleted() {
        if (refreshInFlight_)
            return;
        if (!hasFinished_) {
            brls::Application::notify(
                tr("pipensx/storage/nothing_to_recover"));
            return;
        }
        confirm(tr("pipensx/storage/clear_completed_confirm",
                   formatBytes(completedBytes_)),
                [this] { clearCompleted(); });
    }

    void confirmClearImages() {
        if (refreshInFlight_)
            return;
        confirmAction(snapshot_.imageCacheBytes, [this] { clearImages(); });
    }

    void confirmClearTorrents() {
        if (refreshInFlight_)
            return;
        confirmAction(orphanBytes_, [this] { clearTorrents(); });
    }

    void confirmClearTemporary() {
        if (refreshInFlight_)
            return;
        if (manager_->hasActiveTransfer()) {
            brls::Application::notify(tr("pipensx/storage/busy_transfer"));
            return;
        }
        confirmAction(snapshot_.temporaryBytes, [this] { clearTemporary(); });
    }

    void confirmAction(uint64_t bytes, const std::function<void()>& action) {
        if (bytes == 0) {
            brls::Application::notify(
                tr("pipensx/storage/nothing_to_recover"));
            return;
        }
        confirm(tr("pipensx/storage/confirm_recover", formatBytes(bytes)),
                action);
    }

    void confirm(const std::string& message,
                 const std::function<void()>& action) {
        auto* dialog = new brls::Dialog(message);
        dialog->addButton(tr("pipensx/common/clear"), [action] { action(); });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void clearCompleted() {
        std::string error;
        if (!manager_->clearCompleted(true, error)) {
            diagnostic_error("storage", "completed", "error=%s",
                             error.c_str());
            if (!error.empty())
                brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearImages() {
        std::string error;
        if (!metadata_->clearImageCache(error)) {
            diagnostic_error("storage", "images", "error=%s", error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearTorrents() {
        std::vector<DownloadTask> tasks = manager_->snapshot();
        std::vector<std::string> active;
        active.reserve(tasks.size());
        for (const DownloadTask& task : tasks)
            active.push_back(task.id);
        std::string error;
        uint64_t recovered = 0;
        if (!clearOrphanTorrents(manager_->torrentRoot(), active, error,
                                 recovered)) {
            diagnostic_error("storage", "torrents", "error=%s", error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearTemporary() {
        if (manager_->hasActiveTransfer()) {
            brls::Application::notify(tr("pipensx/storage/busy_transfer"));
            return;
        }
        std::string error;
        uint64_t recovered = 0;
        if (!clearTemporaryFiles(manager_->rootPath(), error, recovered)) {
            diagnostic_error("storage", "temporary", "error=%s",
                             error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    DownloadManager* manager_;
    GameMetadataService* metadata_;
    std::shared_ptr<std::atomic<bool>> alive_;
    StorageMeter* meter_ = nullptr;
    brls::Box* breakdown_ = nullptr;
    brls::DetailCell* clearCompleted_ = nullptr;
    brls::DetailCell* clearImages_ = nullptr;
    brls::DetailCell* clearTorrents_ = nullptr;
    brls::DetailCell* clearTemporary_ = nullptr;
    brls::Label* canFree_ = nullptr;
    StorageBreakdown snapshot_;
    uint64_t completedBytes_ = 0;
    uint64_t orphanBytes_ = 0;
    bool hasFinished_ = false;
    bool refreshInFlight_ = false;
    bool refreshPending_ = false;
};

// --- System: update check, diagnostics, factory reset ---------------------

class SystemPanel : public SettingsPanel {
public:
    SystemPanel(AppSettings* settings, DownloadManager* manager,
                CatalogService* catalog, GameMetadataService* metadata,
                InstalledTitleService* installed, UpdateService* updater,
                std::shared_ptr<std::atomic<bool>> alive,
                std::function<void()> onReset)
        : settings_(settings), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed), updater_(updater),
          alive_(std::move(alive)), onReset_(std::move(onReset)) {
        addSection(content_, tr("pipensx/settings/section_updates"));
        updateAction_ = actionCell(tr("pipensx/settings/check_update_now"),
            tr("pipensx/settings/check_update_detail", PIPENSX_VERSION),
            [this] { checkForUpdateNow(); });
        if (updater_ && updater_->checkCompleted())
            markUpdateChecked();
        content_->addView(updateAction_);

        addSection(content_, tr("pipensx/settings/section_diagnostics"));
        addNote(content_, tr("pipensx/settings/diagnostics_note"));
        extendedTelemetry_ = new brls::BooleanCell();
        extendedTelemetry_->init(tr("pipensx/settings/extended_telemetry"),
            settings_->get().extendedTelemetry,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.extendedTelemetry;
                values.extendedTelemetry = enabled;
                if (!persistSettings(settings_, values,
                                     "extended_telemetry")) {
                    extendedTelemetry_->setOn(previous, false);
                    return;
                }
                telemetry_set_enabled(enabled ? 1 : 0);
                brls::Application::notify(enabled
                    ? tr("pipensx/settings/telemetry_on")
                    : tr("pipensx/settings/telemetry_off"));
            });
        content_->addView(extendedTelemetry_);

        content_->addView(actionCell(tr("pipensx/settings/capture_snapshot"),
            tr("pipensx/settings/capture_snapshot_detail"),
            [this] { captureSnapshot(); }));
        content_->addView(actionCell(tr("pipensx/settings/clear_log"),
            tr("pipensx/settings/clear_log_detail"),
            [this] { confirmClearLog(); }));

        auto* path = new brls::Label();
        path->setText(tr("pipensx/settings/log_path", LogPath));
        path->setFontSize(15);
        path->setTextColor(theme::textTertiary());
        path->setMarginTop(18);
        content_->addView(path);

        addSection(content_, tr("pipensx/settings/section_reset"));
        content_->addView(actionCell(tr("pipensx/settings/reset"),
            tr("pipensx/settings/reset_detail"),
            [this] { confirmReset(); }));
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        extendedTelemetry_->setOn(values.extendedTelemetry, false);
        telemetry_set_enabled(values.extendedTelemetry ? 1 : 0);
    }

private:
    void checkForUpdateNow() {
        if (updateInFlight_ || !updater_)
            return;
        updateInFlight_ = true;
        updateAction_->setDetailText(tr("pipensx/settings/checking"));
        auto alive = alive_;
        UpdateService* updater = updater_;
        updater->checkAsync([this, alive](UpdateCheckResult result) {
            brls::sync([this, alive, result = std::move(result)]() mutable {
                if (!alive->load())
                    return;
                updateInFlight_ = false;
                markUpdateChecked();
                if (!result.ok) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/check_failed"));
                    diagnostic_error("update", "check", "error=%s",
                                     result.error.c_str());
                    brls::Application::notify(result.error);
                    return;
                }
                if (!result.updateAvailable) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/up_to_date"));
                    brls::Application::notify(
                        tr("pipensx/settings/up_to_date_notify"));
                    return;
                }
                updateAction_->setDetailText(
                    tr("pipensx/settings/version_detail",
                       result.release.version));
                confirmInstallUpdate(std::move(result.release));
            });
        });
    }

    void confirmInstallUpdate(ReleaseInfo release) {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/update_available", release.version));
        dialog->addButton(tr("pipensx/settings/install_and_restart"),
                          [this, release = std::move(release)] {
            installUpdate(release);
        });
        dialog->addButton(tr("pipensx/common/later"), [] {});
        dialog->open();
    }

    void markUpdateChecked() {
        updateAction_->setTextColor(theme::accent());
        updateAction_->setDetailTextColor(theme::accent());
    }

    void installUpdate(const ReleaseInfo& release) {
        if (updateInFlight_ || !updater_)
            return;
        updateInFlight_ = true;
        updateAction_->setDetailText(tr("pipensx/settings/downloading"));
        auto alive = alive_;
        UpdateService* updater = updater_;
        auto lastPercent = std::make_shared<std::atomic<int>>(-1);
        updater->onInstallProgress(
            [this, alive, lastPercent](uint64_t received, uint64_t total) {
                const int percent =
                    static_cast<int>((received * 100) / total);
                if (lastPercent->exchange(percent) == percent)
                    return;
                brls::sync([this, alive, percent] {
                    if (!alive->load())
                        return;
                    updateAction_->setDetailText(
                        tr("pipensx/settings/downloading_percent", percent));
                });
            });
        updater->installAsync(release, [this, alive](bool installed,
                                                       std::string error) {
            brls::sync([this, alive, installed, error = std::move(error)] {
                if (!alive->load())
                    return;
                updateInFlight_ = false;
                if (!installed) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/install_failed"));
                    diagnostic_error("update", "install", "error=%s",
                                     error.c_str());
                    brls::Application::notify(formatCatalogRefreshError(error));
                    return;
                }
                updateAction_->setDetailText(
                    tr("pipensx/settings/restart_required"));
#ifdef __SWITCH__
                if (!envHasNextLoad()) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_no_restart"));
                    return;
                }
                const std::string helper = updater_->helperPath();
                const std::string arguments =
                    "\"" + helper + "\" --finish-update";
                const Result result = envSetNextLoad(helper.c_str(),
                                                     arguments.c_str());
                if (R_FAILED(result)) {
                    diagnostic_error("update", "restart", "result=0x%08x",
                                     result);
                    brls::Application::notify(
                        tr("pipensx/settings/update_restart_failed"));
                    return;
                }
#endif
                // The helper swaps the NRO after we exit, then drops to HOME
                // instead of relaunching (an in-session relaunch of the full
                // app crashes). Gate the quit behind an acknowledged dialog so
                // the close reads as intentional rather than a crash.
                auto* dialog = new brls::Dialog(
                    tr("pipensx/settings/update_close_body"));
                dialog->setCancelable(false);
                dialog->addButton(tr("pipensx/settings/update_close_button"),
                                  [] { brls::Application::quit(); });
                dialog->open();
            });
        });
    }

    void captureSnapshot() {
        writeSystemSnapshot(manager_, catalog_, metadata_, installed_,
                            "manual");
        brls::Application::notify(tr("pipensx/settings/snapshot_written"));
    }

    void confirmClearLog() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/clear_log_question"));
        dialog->addButton(tr("pipensx/settings/clear_log"), [] {
            if (!clearApplicationLog())
                brls::Application::notify(
                    tr("pipensx/settings/clear_log_failed"));
            else
                brls::Application::notify(
                    tr("pipensx/settings/clear_log_done"));
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    // Factory reset. Every panel re-syncs through the hub's onReset callback
    // (the old Advanced page's applyOwnValues + SettingsView::applyValues).
    void confirmReset() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/reset_question"));
        dialog->addButton(tr("pipensx/settings/reset_action"), [this] {
            std::string error;
            if (!settings_->reset(error)) {
                diagnostic_error("settings", "reset", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
                return;
            }
            if (onReset_)
                onReset_();
            brls::Application::notify(tr("pipensx/settings/reset_done"));
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    UpdateService* updater_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::function<void()> onReset_;
    brls::BooleanCell* extendedTelemetry_ = nullptr;
    brls::DetailCell* updateAction_ = nullptr;
    bool updateInFlight_ = false;
};

}  // namespace pipensx::ui
