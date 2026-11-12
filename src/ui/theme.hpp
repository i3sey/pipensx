#pragma once

#include <string>

#include <borealis.hpp>
#include "app/app_settings.hpp"

// Colors, corner radii, font scale and spacing shared by the native UI.
// Custom palettes also replace Borealis chrome before views are constructed.
//
// Rule (see CONTRIBUTING / UI_PLAN S13): no nvgRGB/nvgRGBA literals anywhere
// in src/ui outside this file.

namespace pipensx::ui::theme {

// --- Corner radii ------------------------------------------------------

inline float kRadiusSmall  = 6.0f;
inline float kRadiusMedium = 8.0f;
inline float kRadiusLarge  = 12.0f;

// --- Font scale ---------------------------------------------------------

inline constexpr float kFontTitle   = 32.0f;
inline constexpr float kFontHeading = 25.0f;
inline constexpr float kFontBody    = 21.0f;
inline constexpr float kFontSmall   = 17.0f;
inline constexpr float kFontCaption = 15.0f;

// --- Spacing ------------------------------------------------------------
// All paddings/margins should be multiples of this step.

inline constexpr float kSpacingUnit = 8.0f;

// --- Colors -------------------------------------------------------------
// The token table is the single source of truth. registerColors() publishes
// it into both Borealis themes so XML can reference "pipensx/*"; the C++
// accessors below read the table directly — resolving through the theme's
// string-keyed map would cost a std::string heap allocation plus two hash
// lookups per call, and these run several times per view per frame.

struct Token {
    const char* name;
    NVGcolor light;
    NVGcolor dark;
};

inline const Token kTokens[] = {
    // Brand / status
    {"pipensx/accent", nvgRGB(0, 195, 227), nvgRGB(0, 195, 227)},   // Joy-Con Neon Blue #00C3E3
    // Ink for glyphs knocked out of a solid accent plate. The neon blue is
    // bright enough that dark ink beats white on it, and it is theme-
    // independent because the accent itself is.
    {"pipensx/on_accent", nvgRGB(12, 34, 40), nvgRGB(12, 34, 40)},
    {"pipensx/error", nvgRGB(255, 69, 84), nvgRGB(255, 69, 84)},    // Neon Red #FF4554
    {"pipensx/success", nvgRGB(40, 170, 90), nvgRGB(96, 220, 130)}, // #60DC82 (darkened on light)
    {"pipensx/warning", nvgRGB(196, 110, 22), nvgRGB(230, 150, 80)},

    // Text levels
    {"pipensx/text_primary", nvgRGB(45, 45, 45), nvgRGB(245, 245, 250)},
    {"pipensx/text_secondary", nvgRGB(90, 90, 95), nvgRGB(185, 185, 195)},
    {"pipensx/text_tertiary", nvgRGB(125, 125, 130), nvgRGB(150, 150, 160)},
    {"pipensx/text_disabled", nvgRGB(175, 175, 180), nvgRGB(115, 115, 125)},

    // Surfaces
    {"pipensx/surface", nvgRGB(225, 225, 230), nvgRGB(58, 58, 66)},
    {"pipensx/overlay", nvgRGBA(240, 240, 244, 235), nvgRGBA(35, 35, 40, 235)},
    {"pipensx/panel", nvgRGBA(228, 228, 234, 180), nvgRGBA(45, 45, 50, 180)},
    {"pipensx/track", nvgRGBA(128, 128, 128, 70), nvgRGBA(128, 128, 128, 70)},
    // Settings sub-sidebar: a shade off the app background so the section
    // column reads as a rail, and the translucent accent wash behind the
    // active section item.
    {"pipensx/sidebar", nvgRGB(232, 232, 238), nvgRGB(50, 50, 50)},
    {"pipensx/sidebar_active", nvgRGBA(0, 195, 227, 26), nvgRGBA(0, 195, 227, 26)},

    // Storage meter. The segmented SD bar needs far more separation than
    // the generic track/surface pair: the dark sidebar sits at rgb(50,50,50)
    // and the app background at rgb(45,45,45), so the empty slot goes well
    // below both and the used chunk well above.
    {"pipensx/meter_track", nvgRGB(205, 207, 213), nvgRGB(24, 24, 27)},
    {"pipensx/meter_used", nvgRGB(0, 195, 227), nvgRGB(0, 195, 227)}, // Switch turquoise.
    {"pipensx/meter_border", nvgRGBA(0, 0, 0, 50), nvgRGBA(255, 255, 255, 55)},

    {"pipensx/graph_bg", nvgRGBA(208, 210, 216, 120), nvgRGBA(30, 31, 36, 120)},
    {"pipensx/graph_grid", nvgRGBA(60, 60, 70, 35), nvgRGBA(180, 180, 190, 35)},

    // Bug-report QR codes. Deliberately theme-independent pure black on
    // pure white: a camera photographing a TV needs maximum contrast, and a
    // theme-tinted or inverted (dark-mode) code scans far worse. These are
    // the one place a "paper"/"ink" pair is fixed across both themes.
    {"pipensx/qr_paper", nvgRGB(255, 255, 255), nvgRGB(255, 255, 255)},
    {"pipensx/qr_ink", nvgRGB(0, 0, 0), nvgRGB(0, 0, 0)},

    // Screen-off cover after the idle warning: pure black so OLED chrome is
    // not left on the same pixels while the panel is still driven.
    {"pipensx/burn_in_backdrop", nvgRGB(0, 0, 0), nvgRGB(0, 0, 0)},
    {"pipensx/chip_success", nvgRGB(20, 100, 48), nvgRGB(120, 230, 150)},
    {"pipensx/chip_warning", nvgRGB(125, 65, 8), nvgRGB(255, 190, 110)},
    {"pipensx/chip_error", nvgRGB(165, 28, 42), nvgRGB(255, 155, 165)},
};

// Indices into kTokens — keep in the same order as the table above.
enum class Tok : size_t {
    Accent, OnAccent, Error, Success, Warning,
    TextPrimary, TextSecondary, TextTertiary, TextDisabled,
    Surface, Overlay, Panel, Track, Sidebar, SidebarActive,
    MeterTrack, MeterUsed, MeterBorder,
    GraphBg, GraphGrid,
    QrPaper, QrInk,
    BurnInBackdrop,
    ChipSuccess, ChipWarning, ChipError,
};

inline AppTheme activeTheme = AppTheme::System;

struct Palette {
    NVGcolor background, sidebar, surface, panel, overlay;
    NVGcolor primary, secondary, tertiary, disabled;
    NVGcolor accent, onAccent, selected, border, track;
    NVGcolor error, success, warning;
};

inline Palette basePalette(AppTheme theme) {
    if (theme == AppTheme::TerminalLight) {
        return {
            nvgRGB(247, 249, 246), nvgRGB(235, 240, 234),
            nvgRGB(225, 233, 223), nvgRGB(239, 244, 237), nvgRGB(247, 249, 246),
            nvgRGB(20, 32, 25), nvgRGB(53, 72, 59), nvgRGB(75, 93, 79), nvgRGB(110, 125, 113),
            nvgRGB(32, 106, 57), nvgRGB(255, 255, 255), nvgRGB(214, 233, 216),
            nvgRGB(99, 123, 104), nvgRGB(189, 205, 191),
            nvgRGB(179, 38, 30), nvgRGB(32, 106, 57), nvgRGB(133, 77, 14)
        };
    }
    if (theme == AppTheme::MaterialYouDark) {
        return {
            nvgRGB(20, 18, 24), nvgRGB(29, 27, 32),
            nvgRGB(54, 48, 65), nvgRGB(35, 31, 41), nvgRGB(43, 38, 50),
            nvgRGB(234, 225, 239), nvgRGB(207, 196, 216), nvgRGB(182, 170, 193), nvgRGB(130, 120, 141),
            nvgRGB(208, 188, 255), nvgRGB(56, 30, 114), nvgRGB(67, 52, 89),
            nvgRGB(151, 140, 162), nvgRGB(74, 65, 84),
            nvgRGB(255, 180, 171), nvgRGB(151, 218, 165), nvgRGB(244, 193, 119)
        };
    }
    if (theme == AppTheme::MaterialYou) {
        return {
            nvgRGB(255, 251, 254), nvgRGB(243, 237, 247),
            nvgRGB(232, 222, 248), nvgRGB(243, 237, 247), nvgRGB(247, 242, 250),
            nvgRGB(33, 28, 43), nvgRGB(73, 69, 79), nvgRGB(98, 91, 113), nvgRGB(121, 116, 126),
            nvgRGB(103, 80, 164), nvgRGB(255, 255, 255), nvgRGB(232, 222, 248),
            nvgRGB(121, 116, 126), nvgRGB(202, 196, 208),
            nvgRGB(179, 38, 30), nvgRGB(38, 105, 62), nvgRGB(133, 77, 14)
        };
    }
    if (theme == AppTheme::Terminal) {
        return {
            nvgRGB(10, 16, 13), nvgRGB(15, 25, 19),
            nvgRGB(26, 42, 32), nvgRGB(20, 32, 25), nvgRGB(20, 32, 25),
            nvgRGB(222, 239, 225), nvgRGB(175, 199, 180), nvgRGB(151, 176, 158), nvgRGB(102, 125, 108),
            nvgRGB(143, 229, 165), nvgRGB(10, 29, 16), nvgRGB(35, 66, 44),
            nvgRGB(105, 143, 114), nvgRGB(62, 87, 69),
            nvgRGB(255, 157, 151), nvgRGB(143, 229, 165), nvgRGB(235, 199, 131)
        };
    }
    if (theme == AppTheme::LiquidGlassLight) {
        return {
            nvgRGB(240, 240, 240), nvgRGBA(255, 255, 255, 150),
            nvgRGBA(255, 255, 255, 175), nvgRGBA(255, 255, 255, 85), nvgRGB(248, 248, 248),
            nvgRGB(28, 28, 28), nvgRGB(80, 80, 80), nvgRGB(105, 105, 105), nvgRGB(145, 145, 145),
            nvgRGB(40, 40, 40), nvgRGB(255, 255, 255), nvgRGBA(0, 0, 0, 14),
            nvgRGBA(0, 0, 0, 30), nvgRGBA(0, 0, 0, 22),
            nvgRGB(45, 45, 45), nvgRGB(65, 65, 65), nvgRGB(85, 85, 85)
        };
    }
    return {
        nvgRGB(16, 16, 16), nvgRGBA(42, 42, 42, 185),
        nvgRGBA(255, 255, 255, 24), nvgRGBA(255, 255, 255, 12), nvgRGB(40, 40, 40),
        nvgRGB(245, 245, 245), nvgRGB(205, 205, 205), nvgRGB(175, 175, 175), nvgRGB(120, 120, 120),
        nvgRGB(235, 235, 235), nvgRGB(24, 24, 24), nvgRGBA(255, 255, 255, 26),
        nvgRGBA(255, 255, 255, 24), nvgRGBA(255, 255, 255, 28),
        nvgRGB(235, 235, 235), nvgRGB(215, 215, 215), nvgRGB(195, 195, 195)
    };
}

inline Palette palette(AppTheme theme, const std::string& choice = "default") {
    Palette p = basePalette(theme);
    if (!isTerminalTheme(theme) && !isMaterialTheme(theme)) return p;
    const bool dark = theme == AppTheme::Terminal || theme == AppTheme::MaterialYouDark;
    const std::string hue = choice == "default" ? (isTerminalTheme(theme) ? "green" : "purple") : choice;
    NVGcolor ink, pastel;
    if (hue == "blue") { ink = nvgRGB(25, 83, 154); pastel = nvgRGB(168, 204, 255); }
    else if (hue == "purple") { ink = nvgRGB(103, 80, 164); pastel = nvgRGB(208, 188, 255); }
    else if (hue == "amber") { ink = nvgRGB(126, 81, 0); pastel = nvgRGB(244, 201, 117); }
    else if (hue == "rose") { ink = nvgRGB(154, 48, 85); pastel = nvgRGB(255, 177, 200); }
    else { ink = nvgRGB(32, 106, 57); pastel = nvgRGB(143, 229, 165); }
    p.accent = dark ? pastel : ink;
    p.onAccent = dark ? nvgRGB(19, 23, 26) : nvgRGB(255, 255, 255);
    const NVGcolor neutral = dark ? nvgRGB(18, 19, 21) : nvgRGB(253, 252, 250);
    p.background = nvgLerpRGBA(neutral, p.accent, dark ? .025f : .015f);
    p.sidebar = nvgLerpRGBA(neutral, p.accent, dark ? .06f : .07f);
    p.surface = nvgLerpRGBA(neutral, p.accent, .12f);
    p.panel = nvgLerpRGBA(neutral, p.accent, dark ? .07f : .045f);
    p.overlay = p.panel;
    p.selected = nvgLerpRGBA(neutral, p.accent, dark ? .19f : .14f);
    p.track = nvgLerpRGBA(neutral, p.accent, .25f);
    p.border = dark ? nvgRGB(144, 150, 155) : nvgRGB(108, 113, 118);
    return p;
}

inline Palette activePalette = palette(AppTheme::LiquidGlass);

inline bool isCustom() {
    return isGlassTheme(activeTheme) || isTerminalTheme(activeTheme) ||
           isMaterialTheme(activeTheme);
}

inline NVGcolor customColor(Tok token, const Palette& p) {
    switch (token) {
        case Tok::Accent: case Tok::MeterUsed: return p.accent;
        case Tok::OnAccent: return p.onAccent;
        case Tok::Error: case Tok::ChipError: return p.error;
        case Tok::Success: case Tok::ChipSuccess: return p.success;
        case Tok::Warning: case Tok::ChipWarning: return p.warning;
        case Tok::TextPrimary: return p.primary;
        case Tok::TextSecondary: return p.secondary;
        case Tok::TextTertiary: return p.tertiary;
        case Tok::TextDisabled: return p.disabled;
        case Tok::Surface: return p.surface;
        case Tok::Overlay: return p.overlay;
        case Tok::Panel: return p.panel;
        case Tok::Track: case Tok::GraphGrid: return p.track;
        case Tok::Sidebar: return p.sidebar;
        case Tok::SidebarActive: return p.selected;
        case Tok::MeterTrack: case Tok::GraphBg: return p.background;
        case Tok::MeterBorder: return p.border;
        case Tok::QrPaper: return nvgRGB(255, 255, 255);
        case Tok::QrInk: case Tok::BurnInBackdrop: return nvgRGB(0, 0, 0);
    }
    return p.primary;
}

inline void registerChrome(brls::Theme& theme, const Palette& p) {
    const auto set = [&theme](const char* name, NVGcolor color) { theme.addColor(name, color); };
    set("brls/background", p.background);
    set("brls/text", p.primary);
    set("brls/text_disabled", p.disabled);
    set("brls/accent", p.accent);
    set("brls/highlight/background", isGlassTheme(activeTheme) ? p.overlay : p.panel);
    set("brls/highlight/color1", p.accent);
    set("brls/highlight/color2", p.accent);
    NVGcolor pulse = p.accent;
    pulse.a = 0.15f;
    set("brls/click_pulse", pulse);
    set("brls/applet_frame/separator", p.border);
    set("brls/sidebar/background", p.sidebar);
    set("brls/sidebar/active_item", p.accent);
    set("brls/sidebar/separator", p.track);
    set("brls/header/border", p.track);
    set("brls/header/rectangle", p.border);
    set("brls/header/subtitle", p.secondary);
    set("brls/button/primary_enabled_background", p.accent);
    set("brls/button/primary_enabled_text", p.onAccent);
    set("brls/button/primary_disabled_background", p.surface);
    set("brls/button/primary_disabled_text", p.disabled);
    set("brls/button/default_enabled_background", p.surface);
    set("brls/button/default_enabled_text", p.primary);
    set("brls/button/default_disabled_background", p.panel);
    set("brls/button/default_disabled_text", p.disabled);
    set("brls/button/highlight_enabled_text", p.accent);
    set("brls/button/highlight_disabled_text", p.disabled);
    set("brls/button/enabled_border_color", p.border);
    set("brls/button/disabled_border_color", p.track);
    set("brls/list/listItem_value_color", p.accent);
    set("brls/slider/pointer_color", p.accent);
    set("brls/slider/pointer_border_color", p.onAccent);
    set("brls/slider/line_filled", p.accent);
    set("brls/slider/line_empty", p.track);
    set("brls/spinner/bar_color", p.secondary);
}

// Call once after brls::Application::init() and before any view is
// constructed.
inline void registerColors(AppTheme selected = AppTheme::System, const std::string& accent = "default") {
    activeTheme = selected;
    const Palette p = palette(selected, accent);
    activePalette = p;
    for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i) {
        const auto& t = kTokens[i];
        const NVGcolor custom = customColor(static_cast<Tok>(i), p);
        brls::Theme::getLightTheme().addColor(t.name, isCustom() ? custom : t.light);
        brls::Theme::getDarkTheme().addColor(t.name, isCustom() ? custom : t.dark);
    }
    if (isCustom()) {
        registerChrome(brls::Theme::getLightTheme(), p);
        registerChrome(brls::Theme::getDarkTheme(), p);
        const bool terminal = isTerminalTheme(selected);
        const bool material = isMaterialTheme(selected);
        kRadiusSmall = terminal ? 0 : 10;
        kRadiusMedium = terminal ? 0 : material ? 20 : 16;
        kRadiusLarge = terminal ? 0 : material ? 28 : 24;
        auto style = brls::Application::getStyle();
        style.addMetric("brls/button/corner_radius", terminal ? 0 : material ? 22 : 16);
        style.addMetric("brls/highlight/corner_radius", terminal ? 0 : material ? 20 : 16);
        style.addMetric("brls/highlight/stroke_width", terminal ? 2 : material ? 3 : 2);
        style.addMetric("brls/highlight/shadow_opacity", 0);
        style.addMetric("brls/shadow/opacity", 0);
        style.addMetric("brls/listitem/selectRadius", terminal ? 0 : 12);
    }
}

inline NVGcolor color(Tok t) {
    if (isCustom()) return customColor(t, activePalette);
    const Token& tok = kTokens[static_cast<size_t>(t)];
    return brls::Application::getThemeVariant() == brls::ThemeVariant::DARK
               ? tok.dark
               : tok.light;
}

inline NVGcolor accent() { return color(Tok::Accent); }
inline NVGcolor onAccent() { return color(Tok::OnAccent); }
inline NVGcolor error() { return color(Tok::Error); }
inline NVGcolor success() { return color(Tok::Success); }
inline NVGcolor warning() { return color(Tok::Warning); }
inline NVGcolor textPrimary() { return color(Tok::TextPrimary); }
inline NVGcolor textSecondary() { return color(Tok::TextSecondary); }
inline NVGcolor textTertiary() { return color(Tok::TextTertiary); }
inline NVGcolor textDisabled() { return color(Tok::TextDisabled); }
inline NVGcolor surface() { return color(Tok::Surface); }
inline NVGcolor overlay() { return color(Tok::Overlay); }
inline NVGcolor panel() { return color(Tok::Panel); }
inline NVGcolor track() { return color(Tok::Track); }
inline NVGcolor sidebar() { return color(Tok::Sidebar); }
inline NVGcolor sidebarActive() { return color(Tok::SidebarActive); }
inline NVGcolor meterTrack() { return color(Tok::MeterTrack); }
inline NVGcolor meterUsed() { return color(Tok::MeterUsed); }
inline NVGcolor meterBorder() { return color(Tok::MeterBorder); }
inline NVGcolor graphBg() { return color(Tok::GraphBg); }
inline NVGcolor graphGrid() { return color(Tok::GraphGrid); }
inline NVGcolor qrPaper() { return color(Tok::QrPaper); }
inline NVGcolor qrInk() { return color(Tok::QrInk); }
inline NVGcolor burnInBackdrop() { return color(Tok::BurnInBackdrop); }

inline float navigationRadius() {
    return isTerminalTheme(activeTheme) ? 0 : 24;
}

inline void drawBackdrop(NVGcontext* vg, float x, float y, float width, float height) {
    if (!isGlassTheme(activeTheme)) return;
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y + height, x + width, y,
        activePalette.background, activeTheme == AppTheme::LiquidGlassLight ? nvgRGB(255, 255, 255) : nvgRGB(32, 32, 32)));
    nvgFill(vg);
    if (activeTheme == AppTheme::LiquidGlass) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, nvgRadialGradient(vg, x + width * .72f, y + height * .22f,
            0, width * .7f, nvgRGBA(255, 255, 255, 10), nvgRGBA(255, 255, 255, 0)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

inline void drawPlate(NVGcontext* vg, float x, float y, float width, float height,
                      float radius, bool focused = false) {
    if (!isCustom()) return;
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, width - 1, height - 1, radius);
    NVGcolor fill = focused && !isGlassTheme(activeTheme) ? sidebarActive() : panel();
    if (isGlassTheme(activeTheme)) {
        NVGcolor upper = fill;
        upper.a += 0.035f;
        nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x, y + height, upper, fill));
    } else {
        nvgFillColor(vg, fill);
    }
    nvgFill(vg);
    if (isTerminalTheme(activeTheme) || isGlassTheme(activeTheme)) {
        nvgStrokeWidth(vg, 1);
        nvgStrokeColor(vg, activePalette.border);
        nvgStroke(vg);
    }
    nvgRestore(vg);
}

inline void drawGlassNavigation(NVGcontext* vg, int image, float x, float y,
                                float width, float height) {
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x - 12, y - 12, width + 24, height + 32);
    nvgRoundedRect(vg, x, y, width, height, 28);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, nvgBoxGradient(vg, x, y + 5, width, height, 28, 18,
        nvgRGBA(0, 0, 0, 28), nvgRGBA(0, 0, 0, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 28);
    if (image > 0) {
        auto paint = nvgBoxGradient(vg, x, y, width, height, 28, 3.5f,
            activePalette.sidebar, activePalette.sidebar);
        paint.image = image;
        nvgFillPaint(vg, paint);
    } else {
        nvgFillColor(vg, activePalette.panel);
    }
    nvgFill(vg);
    nvgRestore(vg);
}

inline void drawControlFinish(NVGcontext* vg, float x, float y, float width, float height,
                              float radius, bool = false) {
    if (!isTerminalTheme(activeTheme)) return;
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + .5f, y + .5f, width - 1, height - 1, radius);
    nvgStrokeWidth(vg, 1);
    nvgStrokeColor(vg, activePalette.border);
    nvgStroke(vg);
    nvgRestore(vg);
}

class Button : public brls::Button {
public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Button::draw(vg, x, y, width, height, style, ctx);
        drawControlFinish(vg, x, y, width, height, getCornerRadius(), isFocused());
    }
};

class SettingsContent : public brls::Box {
public:
    SettingsContent() : brls::Box(brls::Axis::COLUMN) {}

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (isCustom()) {
            for (auto* child : getChildren()) {
                auto* row = dynamic_cast<brls::DetailCell*>(child);
                if (!row || row->getVisibility() != brls::Visibility::VISIBLE) continue;
                row->setLineBottom(0);
                row->setHighlightCornerRadius(kRadiusMedium);
                if (isMaterialTheme(activeTheme) && dynamic_cast<brls::BooleanCell*>(row) &&
                    row->detail->getVisibility() != brls::Visibility::INVISIBLE)
                    row->detail->setVisibility(brls::Visibility::INVISIBLE);
                const auto f = row->getFrame();
                drawPlate(vg, f.getMinX(), f.getMinY() + 4, f.getWidth(),
                    f.getHeight() - 8, kRadiusMedium, row->isFocused());
            }
        }
        brls::Box::draw(vg, x, y, width, height, style, ctx);
        if (isMaterialTheme(activeTheme)) {
            for (auto* child : getChildren()) {
                auto* toggle = dynamic_cast<brls::BooleanCell*>(child);
                if (!toggle || toggle->getVisibility() != brls::Visibility::VISIBLE) continue;
                const auto f = toggle->getFrame();
                const float tx = f.getMaxX() - 62, ty = f.getMinY() + (f.getHeight() - 28) / 2;
                const bool on = toggle->isOn();
                nvgBeginPath(vg);
                nvgRoundedRect(vg, tx, ty, 46, 28, 14);
                nvgFillColor(vg, on ? accent() : activePalette.track);
                nvgFill(vg);
                if (!on) {
                    nvgStrokeColor(vg, activePalette.border);
                    nvgStrokeWidth(vg, 2);
                    nvgStroke(vg);
                }
                nvgBeginPath(vg);
                nvgCircle(vg, tx + (on ? 32 : 14), ty + 14, on ? 10 : 8);
                nvgFillColor(vg, on ? onAccent() : activePalette.secondary);
                nvgFill(vg);
                if (on) {
                    nvgBeginPath(vg);
                    nvgMoveTo(vg, tx + 28, ty + 14);
                    nvgLineTo(vg, tx + 31, ty + 17);
                    nvgLineTo(vg, tx + 36, ty + 11);
                    nvgStrokeWidth(vg, 2);
                    nvgStrokeColor(vg, accent());
                    nvgStroke(vg);
                }
            }
        }
    }
};

} // namespace pipensx::ui::theme
