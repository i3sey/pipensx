#pragma once

#include "catalog_service.hpp"
#include "game_metadata_service.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

enum class CatalogSortMode {
    Latest,
    Popular,
    Alphabetical,
    Largest,
};

struct CatalogBrowseResult;

// Everything the background builder needs is owned or immutable. Status
// labels are resolved by the UI before dispatch, so this layer never calls
// borealis translation APIs.
struct CatalogBrowseRequest {
    uint64_t generation = 0;
    std::shared_ptr<const std::vector<CatalogEntry>> catalog;
    std::shared_ptr<const GameMetadataIndexSnapshot> metadata;
    std::shared_ptr<const CatalogBrowseResult> previous;
    CatalogSection section = CatalogSection::Games;
    CatalogSortMode sort = CatalogSortMode::Popular;
    bool sortReversed = false;
    std::string query;
    bool favoritesOnly = false;
    bool fitsOnly = false;
    PlayerFilter playerFilter = PlayerFilter::Any;
    std::unordered_set<std::string> genreFilters;
    // Lower-case info hash -> already localized task status label.
    std::unordered_map<std::string, std::string> taskBadges;
    std::unordered_set<std::string> favoriteHashes;
    std::unordered_set<std::string> selectedHashes;
    std::unordered_set<std::string> installedTitleIds;
    std::string installedBadge;
    uint64_t freeBytes = 0;
    bool freeSpaceAvailable = false;
};

struct CatalogBrowseResult {
    uint64_t generation = 0;
    std::shared_ptr<const std::vector<CatalogEntry>> catalog;
    std::shared_ptr<const GameMetadataIndexSnapshot> metadata;
    std::vector<int> indices;
    // Compact identity vector used by the next worker to decide whether the
    // recycler structure changed.
    std::vector<std::string> structureHashes;
    std::vector<std::string> titles;
    std::vector<std::string> iconUrls;
    std::vector<uint8_t> iconPreserveAspect;
    std::vector<std::string> badges;
    // Badge shown when there is no active download task (currently the
    // installed marker). Keeping it separate lets task changes be patched
    // without rebuilding/filtering/sorting the catalogue.
    std::vector<std::string> baseBadges;
    std::vector<uint8_t> favorite;
    std::vector<uint8_t> selected;
    std::vector<uint8_t> selectable;
    std::vector<std::string> genres;
    std::unordered_map<std::string, size_t> rowByInfoHash;
    // A catalogue may contain more than one release for the same hash. The
    // fast live-state path must update every matching card without scanning
    // the filtered result.
    std::unordered_map<std::string, std::vector<size_t>> rowsByInfoHash;
    std::unordered_set<std::string> selectedHashes;
    size_t count = 0;
    bool hasRegularEntries = false;
    bool structureChanged = true;
};

// Patch presentation-only state in an already built browse result. Returned
// row numbers are the cards that actually changed and can be repainted by a
// virtualized UI without reloading the recycler.
std::vector<size_t> patchCatalogTaskBadge(CatalogBrowseResult& result,
                                          const std::string& infoHash,
                                          bool hasTask,
                                          const std::string& badge);
std::vector<size_t> patchCatalogFavorite(CatalogBrowseResult& result,
                                         const std::string& infoHash,
                                         bool favorite);
std::vector<size_t> patchCatalogInstalledBadge(
    CatalogBrowseResult& result, const std::string& infoHash,
    const std::unordered_set<std::string>& installedTitleIds,
    const std::string& badge);

// Returns false when cancelled. The predicate is sampled throughout joins,
// filtering, cancellable merge-sort passes, presentation and comparison.
bool buildCatalogBrowse(const CatalogBrowseRequest& request,
                        CatalogBrowseResult& result,
                        const std::function<bool()>& cancelled = {});

// Small, UI-independent generation gate used by CatalogView. request()
// starts work only when idle; while active it coalesces any number of calls
// into one pending generation. complete() promotes that latest generation.
class CatalogBrowseGenerationQueue {
public:
    struct Ticket {
        uint64_t generation = 0;
        bool startNow = false;
    };

    Ticket request();
    bool complete(uint64_t generation);
    bool isCurrent(uint64_t generation) const;
    size_t pendingCount() const;
    size_t activeCount() const;
    uint64_t latestGeneration() const;

private:
    mutable std::mutex mutex_;
    uint64_t latest_ = 0;
    uint64_t active_ = 0;
    bool pending_ = false;
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
// but a snapshot exists), instead of the bare "never" badge. A snapshot
// whose injected `isToday` is true is Ok (today's catalog, even without a
// stamp). A truly empty catalogue keeps Never. `isToday` is injected
// (isLocalToday at the call site) to keep this clock-free.
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

// Auto-refresh gate, clock-free like resolveCatalogFreshness. A network stamp
// from today skips the fetch; so does a cached snapshot from today when this
// console never stamped (lost settings, killed mid-stamp). Bundled/empty
// snapshots with no stamp stay due so a first install still pulls live data.
bool catalogAutoRefreshDue(uint64_t wallSec, int64_t cachedSnapshotSec,
                           bool hasCachedEntries, bool wallIsToday,
                           bool cachedIsToday);

} // namespace pipensx
