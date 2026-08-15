#pragma once

#include "catalog_service.hpp"
#include "game_metadata_service.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pipensx {

struct CatalogPresentation {
    std::string title;
    std::string titleId;
    std::string iconUrl;
    bool iconPreserveAspect = false;
    std::string coverUrl;
    std::string description;
    std::string developer;
    std::string publisher;
    std::string releaseDate;
    std::string genre;
    std::string performance;
    std::string multiplayer;
    std::vector<std::string> screenshots;
};

// One entry of the catalogue's player-mode menu. Any is always offered; the
// rest only when the loaded index has data for them.
enum class PlayerFilter {
    Any,
    Splitscreen,
    LocalCoop,
    Lan,
    Online,
};

// Which source wins for prose the catalogue and the metadata index both carry.
// The metadata index is English; the Langegen catalogue is Russian, so a
// Russian UI reads better from the catalogue. Only `description` differs:
// `releaseDate` is absent from every metadata snapshot we ship or fetch, so
// entry.year already wins unconditionally.
enum class TextPreference {
    Metadata,
    CatalogNative,
};

std::vector<std::string> mergeScreenshotUrls(
    const GameMetadata* metadata, const CatalogEntry& entry,
    size_t limit = 6);

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata,
    TextPreference preference = TextPreference::Metadata);

// Games vs Ports sidebar tabs. An [NRO] title is a port even when it carries
// a Nintendo title id (ports often reuse the original game's id). Package
// markers and a title id without an NRO tag are games. Everything else
// (Linux images, untagged emulators) is a port so it does not vanish.
enum class CatalogSection {
    Games,
    Ports,
};

bool catalogEntryIsGame(const CatalogEntry& entry,
                        const GameMetadata* metadata);

inline bool catalogEntryIsPort(const CatalogEntry& entry,
                               const GameMetadata* metadata) {
    return !catalogEntryIsGame(entry, metadata);
}

inline bool catalogEntryInSection(const CatalogEntry& entry,
                                  const GameMetadata* metadata,
                                  CatalogSection section) {
    return catalogEntryIsGame(entry, metadata) ==
           (section == CatalogSection::Games);
}

bool catalogEntryHasMatchedTitle(const GameMetadata* metadata);

// Does this game belong under `filter`? Everything but Any needs metadata, so
// an unmatched catalogue release never shows up under a player mode.
//
// LocalCoop is the one entry with a fallback: when the index carries no mode
// record for the game (pre-IGDB entries), a titledb player count of 2+ means
// "more than one person can play on this console", which is what the entry is
// for. A game that does have a mode record is judged by it alone.
bool catalogEntryMatchesPlayerFilter(const GameMetadata* metadata,
                                     PlayerFilter filter);

} // namespace pipensx
