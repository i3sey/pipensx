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

// Title/icon/titleId only — the grid rebuilds thousands of rows and must not
// copy description or screenshot URLs.
struct CatalogRowPresentation {
    std::string title;
    std::string titleId;
    std::string iconUrl;
    bool iconPreserveAspect = false;
};

CatalogRowPresentation resolveCatalogRow(const CatalogEntry& entry,
                                         const GameMetadata* metadata);

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

// UTF-8 aware folding for search: ASCII A-Z → a-z plus Cyrillic capitals
// (А-Я → а-я, Ѐ-Џ → ѐ-џ so Ё → ё). The old search folded
// ASCII only, so a Russian query in one case never matched a title in the
// other (Konstantin 29.08: "filtering stopped working"). Invalid UTF-8
// passes through byte by byte, never dropped.
std::string catalogFoldForSearch(const std::string& text);

// True when the already-folded needle occurs in haystack (folded here).
// An empty needle matches everything, so the grid's empty-query fast path
// and this predicate agree.
bool catalogFoldedContains(const std::string& haystack,
                           const std::string& needleFolded);

// Grid search predicate: title, metadata name, metadata categories and the
// catalogue's own genre string. The genre clause matters because only about
// half the Langegen entries join the metadata index; without it a genre
// "See all" shelf hand-off silently drops every unmatched release.
bool catalogEntryMatchesSearch(const CatalogEntry& entry,
                               const GameMetadata* metadata,
                               const std::string& needleFolded);

// Honest freshness badge decision (B7 goal 1), pure so unit tests cover it.
// Only a successful network refresh stamps wallSec; a cache/bundle snapshot
// still dates the data on screen when this console never fetched (wallSec 0
// but a snapshot exists), instead of the bare "never" badge. A truly empty
// catalogue keeps Never. `isToday` is injected (isLocalToday at the call
// site) to keep this clock-free.
struct CatalogFreshness {
    enum class Kind {
        Updating,
        Never,
        Ok,
        Stale,
    };
    Kind kind = Kind::Never;
    // Wall second when kind is Ok/Stale (refresh stamp, else the snapshot
    // fallback); 0 for Updating/Never. The view renders the date.
    int64_t epochSec = 0;
};

CatalogFreshness resolveCatalogFreshness(bool refreshing, uint64_t wallSec,
                                          int64_t snapshotSec, bool hasEntries,
                                          bool isToday);

} // namespace pipensx
