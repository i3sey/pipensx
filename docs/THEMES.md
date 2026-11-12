# Native appearance themes

Choose a theme in **Settings > General > Theme**, then reopen pipensx.
The selection is stored in `settings.json` and can also be changed under
Switch settings in the browser companion. The browser's own light/dark
setting remains independent.

| Theme | Appearance |
| --- | --- |
| Follow console | The console's light or dark appearance |
| Dark / Light | The original pipensx palettes |
| Liquid Glass / Dark or Light | Neutral charcoal or white; frosted navigation, translucent content plates and high-contrast focus |
| Terminal / Dark or Light | Square frames, ruled cover captions, underlined navigation without shadows or cover zoom |
| Material You / Light or Dark | Rounded tonal cards, pill buttons and switches with a thumb and check mark |

All themes use the same layout, typography, spacing and controller navigation.
Corner shapes, outlines, focus treatments, control materials and toggle rendering
vary by theme. Settings and download row backgrounds have an inset painted inside
the existing bounds; hit targets and row heights remain the same.
Terminal and Material You expose **Accent color** below the theme selector:
theme default, green, blue, purple, amber or rose. The default is green for
Terminal and purple for Material You. Accents tint surfaces and selections;
text, focus and button ink use separate light/dark values. Reopen the app to
apply theme or accent changes. These settings are also in the browser companion.

On Switch, Liquid Glass captures the current framebuffer immediately before
drawing the navigation material. A half-resolution GPU texture supplies nine
blur samples, with a rounded-distance-field bevel displacing the samples for
edge refraction. Directional highlights and a navigation-only shadow establish
the raised control layer. The material limits background brightness behind
text. Content surfaces use a subtle translucent fill and a fine neutral border.
This is a custom deko3d material inspired by Apple's navigation-layer guidance,
not Apple's proprietary renderer. Non-Switch renderers use a solid fallback.
Material You derives a tonal palette from the selected accent, without
wallpaper extraction.
QR codes retain black ink on white paper, and the screen-off cover stays black.

The palette tokens and Borealis chrome mappings live in `src/ui/theme.hpp`.
New theme IDs extend the existing settings field without changing the meaning
of saved values, so they require no settings migration. The native selector
order matches `AppTheme`; the companion uses stable string IDs.
The optional `theme_accent` field defaults to `default` when absent; existing
`terminal` (dark) and `material_you` (light) IDs retain their meaning. New IDs
are `terminal_light`, `material_you_dark`, and `liquid_glass_light`. No migration is needed.

Design direction: ENERGY 2 / RHYTHM 1 / MOTION 1. Materials and the accent
give each theme its identity while preserving the established screen rhythm.
Refraction marks the navigation layer; Terminal keeps surfaces matte;
Material You uses tonal separation for the same existing controls.
