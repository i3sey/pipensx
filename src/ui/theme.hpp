#pragma once

#include <string>

#include <borealis.hpp>

// UI_PLAN O1 — design tokens. Single source of truth for colors, corner
// radii, the font scale and the spacing step.
//
// Colors are registered into both Borealis theme variants under "pipensx/*"
// and resolved through brls::Application::getTheme(), so every view follows
// the console light/dark theme automatically.
//
// Rule (see CONTRIBUTING / UI_PLAN S13): no nvgRGB/nvgRGBA literals anywhere
// in src/ui outside this file.

namespace pipensx::ui::theme {

// --- Corner radii ------------------------------------------------------

inline constexpr float kRadiusSmall  = 6.0f;
inline constexpr float kRadiusMedium = 8.0f;
inline constexpr float kRadiusLarge  = 12.0f;

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
    {"pipensx/meter_used", nvgRGB(0, 195, 227), nvgRGB(0, 195, 227)},   // Joy-Con Neon Blue #00C3E3
    {"pipensx/meter_border", nvgRGBA(0, 0, 0, 50), nvgRGBA(255, 255, 255, 55)},

    {"pipensx/graph_bg", nvgRGBA(208, 210, 216, 120), nvgRGBA(30, 31, 36, 120)},
    {"pipensx/graph_grid", nvgRGBA(60, 60, 70, 35), nvgRGBA(180, 180, 190, 35)},

    // Bug-report QR codes. Deliberately theme-independent pure black on
    // pure white: a camera photographing a TV needs maximum contrast, and a
    // theme-tinted or inverted (dark-mode) code scans far worse. These are
    // the one place a "paper"/"ink" pair is fixed across both themes.
    {"pipensx/qr_paper", nvgRGB(255, 255, 255), nvgRGB(255, 255, 255)},
    {"pipensx/qr_ink", nvgRGB(0, 0, 0), nvgRGB(0, 0, 0)},

    // OLED burn-in saver: pure black backdrop + a barely-visible marker that
    // drifts so static chrome never sits on the same pixels for hours.
    {"pipensx/burn_in_backdrop", nvgRGB(0, 0, 0), nvgRGB(0, 0, 0)},
    {"pipensx/burn_in_marker", nvgRGB(18, 18, 18), nvgRGB(18, 18, 18)},
};

// Indices into kTokens — keep in the same order as the table above.
enum class Tok : size_t {
    Accent, OnAccent, Error, Success, Warning,
    TextPrimary, TextSecondary, TextTertiary, TextDisabled,
    Surface, Overlay, Panel, Track, Sidebar, SidebarActive,
    MeterTrack, MeterUsed, MeterBorder,
    GraphBg, GraphGrid,
    QrPaper, QrInk,
    BurnInBackdrop, BurnInMarker,
};

// Call once after brls::Application::init() and before any view is
// constructed.
inline void registerColors() {
    for (const auto& t : kTokens) {
        brls::Theme::getLightTheme().addColor(t.name, t.light);
        brls::Theme::getDarkTheme().addColor(t.name, t.dark);
    }
}

inline NVGcolor color(Tok t) {
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
inline NVGcolor burnInMarker() { return color(Tok::BurnInMarker); }

} // namespace pipensx::ui::theme
