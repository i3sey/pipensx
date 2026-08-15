# Issue #55 — Games / Ports tabs

GitHub: https://github.com/i3sey/pipensx/issues/55

Request was a PORT tab (and a MODS tab). Mods are not app functionality:
pipensx does not browse or install individual mods, and there is no Mods
section. Games vs Ports only.

## Navigation

- Sidebar: **Games, Ports**, then `addSeparator()`, then Downloads,
  Installed, Updates, Settings, Help, About.
- Eight items at 70px plus a 30px separator fit under the SD footer; do not
  shrink `item_height`.
- Games keeps the 2×2 grid glyph. Ports is a cartridge.

## What each tab contains

| Tab | Predicate |
|-----|-----------|
| Games | `catalogEntryIsGame` |
| Ports | `catalogEntryIsPort` (`!catalogEntryIsGame`) |

`catalogEntryIsGame`:

1. `[NRO]` / `.nro` in the title → **not a game**, even if titledb or the
   catalogue row carries a 16-hex title id (ports reuse the original game's
   id).
2. Else a valid title id on metadata or the catalogue row → game.
3. Else NSP/NSZ/XCI/XCZ markers (`[nsp`, `.nsp`, `/nsp`, …) → game.
4. Else port (Linux images, untagged emulators).

Do not use `catalogEntryHasMatchedTitle` as the Games cut: unmatched NSP/XCI
dumps are still games, and NRO rows with a borrowed title id are still ports.
`isHiddenByDefault()` still hides dead/replaced/unregistered rows on both
tabs.

## Header chrome

- No All / Games chips. Sections live only in the sidebar.
- Same sort chips, ★, Fits, result count, search. Players chip is hidden on
  Ports (no titledb player modes).
- Shelves use the same path; `kMinShelfItems` already hides a shelf that
  cannot fill.

## Empty Ports

Reuse `EmptyStateView`. If the catalogue has visible rows but none are ports:
«К играм» / To Games (`focusTab(0)`). If the whole catalogue is empty, same
Refresh copy as Games.

## Settings

- Drop the **Visible releases** selector. `catalog_filter` is still parsed
  and written so old files load; CatalogView does not apply it. Do not bump
  `kSettingsVersion`.
- Default landing tab is Games. Do not persist last section.
- Web companion: drop the catalog-filter control. The web list is the full
  catalogue. The settings API still accepts `catalogFilter` so old clients
  do not error.

## Out of scope

- A Mods tab, ModCD, or in-app mod file browser.
- Changing how port archives deploy (`isPortArchiveName`, «Select port»).
