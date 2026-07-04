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
// Registers the pipensx/* tokens into both Borealis themes. Call once after
// brls::Application::init() and before any view is constructed.

inline void registerColors() {
    struct Token {
        const char* name;
        NVGcolor light;
        NVGcolor dark;
    };
    static const Token kTokens[] = {
        // Brand / status
        {"pipensx/accent", nvgRGB(0, 195, 227), nvgRGB(0, 195, 227)},   // Joy-Con Neon Blue #00C3E3
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
        {"pipensx/graph_bg", nvgRGBA(208, 210, 216, 120), nvgRGBA(30, 31, 36, 120)},
        {"pipensx/graph_grid", nvgRGBA(60, 60, 70, 35), nvgRGBA(180, 180, 190, 35)},
    };
    for (const auto& t : kTokens) {
        brls::Theme::getLightTheme().addColor(t.name, t.light);
        brls::Theme::getDarkTheme().addColor(t.name, t.dark);
    }
}

inline NVGcolor color(const std::string& name) {
    return brls::Application::getTheme().getColor(name);
}

inline NVGcolor accent() { return color("pipensx/accent"); }
inline NVGcolor error() { return color("pipensx/error"); }
inline NVGcolor success() { return color("pipensx/success"); }
inline NVGcolor warning() { return color("pipensx/warning"); }
inline NVGcolor textPrimary() { return color("pipensx/text_primary"); }
inline NVGcolor textSecondary() { return color("pipensx/text_secondary"); }
inline NVGcolor textTertiary() { return color("pipensx/text_tertiary"); }
inline NVGcolor textDisabled() { return color("pipensx/text_disabled"); }
inline NVGcolor surface() { return color("pipensx/surface"); }
inline NVGcolor overlay() { return color("pipensx/overlay"); }
inline NVGcolor panel() { return color("pipensx/panel"); }
inline NVGcolor track() { return color("pipensx/track"); }
inline NVGcolor graphBg() { return color("pipensx/graph_bg"); }
inline NVGcolor graphGrid() { return color("pipensx/graph_grid"); }

} // namespace pipensx::ui::theme
