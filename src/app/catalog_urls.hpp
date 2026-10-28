#pragma once

namespace pipensx {

inline constexpr char kDefaultCatalogUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-games/refs/heads/main/"
    "switch_games.json";

// The release channel verifies its catalog against the companion manifest.
inline constexpr char kAlternativeCatalogUrl[] =
    "https://github.com/i3sey/pipensx-catalog/releases/latest/download/"
    "catalog.json";
inline constexpr char kDefaultCatalogManifestUrl[] =
    "https://github.com/i3sey/pipensx-catalog/releases/latest/download/"
    "manifest.json";

} // namespace pipensx
