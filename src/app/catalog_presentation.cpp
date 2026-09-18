#include "catalog_presentation.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <sstream>
#include <unordered_set>

namespace pipensx {

namespace {

std::string foldAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

template <typename Less>
bool stableSortCancelled(std::vector<int>& values, Less less,
                         const std::function<bool()>& cancelled) {
    if (values.size() < 2)
        return !cancelled || !cancelled();
    std::vector<int> scratch(values.size());
    size_t checks = 0;
    for (size_t width = 1; width < values.size();) {
        if (cancelled && cancelled())
            return false;
        for (size_t left = 0; left < values.size(); left += width * 2) {
            const size_t middle = std::min(left + width, values.size());
            const size_t right = std::min(left + width * 2, values.size());
            size_t a = left, b = middle, out = left;
            while (a < middle || b < right) {
                if ((++checks & 255u) == 0 && cancelled && cancelled())
                    return false;
                if (b == right ||
                    (a < middle && !less(values[b], values[a])))
                    scratch[out++] = values[a++];
                else
                    scratch[out++] = values[b++];
            }
        }
        values.swap(scratch);
        if (width > values.size() / 2)
            break;
        width *= 2;
    }
    return !cancelled || !cancelled();
}

bool sameBrowseStructure(const CatalogBrowseResult& before,
                         const CatalogBrowseResult& after,
                         const std::function<bool()>& cancelled) {
    if (before.structureHashes.size() != after.structureHashes.size())
        return false;
    for (size_t row = 0; row < after.structureHashes.size(); ++row) {
        if ((row & 255u) == 0 && cancelled && cancelled())
            return false;
        if (before.structureHashes[row] != after.structureHashes[row])
            return false;
    }
    return true;
}

} // namespace

std::vector<std::string> mergeScreenshotUrls(
    const GameMetadata* metadata, const CatalogEntry& entry, size_t limit) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    result.reserve(limit);
    auto append = [&](const std::vector<std::string>& values) {
        for (const std::string& value : values) {
            if (result.size() >= limit)
                return;
            if (!value.empty() && seen.insert(value).second)
                result.push_back(value);
        }
    };
    if (metadata)
        append(metadata->screenshots);
    append(entry.screenshots);
    return result;
}

namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string join(const std::vector<std::string>& values) {
    std::ostringstream result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            result << ", ";
        result << values[i];
    }
    return result.str();
}

bool containsAny(const std::string& text,
                 std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (text.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

bool hasNroMarker(const std::string& title) {
    return containsAny(title, {"[nro", ".nro"});
}

bool hasPackageMarker(const std::string& title) {
    return containsAny(title, {"[nsp", "[nsz", "[xci", "[xcz",
                               ".nsp", ".nsz", ".xci", ".xcz",
                               "/nsp", "/nsz", "/xci", "/xcz"});
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool looksLikeTitleId(const std::string& titleId) {
    return titleId.size() == 16 &&
           std::all_of(titleId.begin(), titleId.end(), isHex);
}

} // namespace

CatalogRowPresentation resolveCatalogRow(const CatalogEntry& entry,
                                         const GameMetadata* metadata) {
    CatalogRowPresentation result;
    result.title = metadata && !metadata->name.empty()
        ? metadata->name : entry.title;
    result.titleId = metadata && !metadata->titleId.empty()
        ? metadata->titleId : entry.titleId;
    if (metadata && !metadata->iconUrl.empty()) {
        result.iconUrl = metadata->iconUrl;
        result.iconPreserveAspect = false;
    } else {
        result.iconUrl = entry.posterUrl;
        result.iconPreserveAspect = !result.iconUrl.empty();
    }
    return result;
}

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata,
    TextPreference preference) {
    CatalogRowPresentation row = resolveCatalogRow(entry, metadata);
    CatalogPresentation result;
    result.title = std::move(row.title);
    result.titleId = std::move(row.titleId);
    result.iconUrl = std::move(row.iconUrl);
    result.iconPreserveAspect = row.iconPreserveAspect;
    if (metadata && !metadata->bannerUrl.empty())
        result.coverUrl = metadata->bannerUrl;
    else
        result.coverUrl = result.iconUrl;
    // CatalogNative still falls back to the metadata prose: 26 catalogue
    // entries carry no description at all, and neither do test fixtures.
    if (preference == TextPreference::CatalogNative &&
        !entry.description.empty())
        result.description = entry.description;
    else if (metadata && !metadata->description.empty())
        result.description = metadata->description;
    else if (metadata && !metadata->intro.empty())
        result.description = metadata->intro;
    else
        result.description = entry.description;
    result.developer = entry.developer;
    result.publisher = metadata && !metadata->publisher.empty()
        ? metadata->publisher : entry.publisher;
    // No metadata snapshot has ever carried releaseDate, so this is entry.year
    // in every locale — the Release row is already catalogue-native.
    result.releaseDate = metadata && !metadata->releaseDate.empty()
        ? metadata->releaseDate : entry.year;
    result.genre = metadata && !metadata->categories.empty()
        ? join(metadata->categories) : entry.genre;
    result.performance = entry.performance;
    result.multiplayer = entry.multiplayer;
    result.screenshots = mergeScreenshotUrls(metadata, entry, 6);
    return result;
}

bool catalogEntryIsGame(const CatalogEntry& entry,
                        const GameMetadata* metadata) {
    const std::string title = lowerAscii(entry.title);
    if (hasNroMarker(title))
        return false;
    if (metadata && looksLikeTitleId(metadata->titleId))
        return true;
    if (looksLikeTitleId(entry.titleId))
        return true;
    return hasPackageMarker(title);
}

bool catalogEntryHasMatchedTitle(const GameMetadata* metadata) {
    return metadata && looksLikeTitleId(metadata->titleId);
}

bool catalogEntryMatchesPlayerFilter(const GameMetadata* metadata,
                                     PlayerFilter filter) {
    if (filter == PlayerFilter::Any)
        return true;
    if (!metadata)
        return false;
    switch (filter) {
    case PlayerFilter::Splitscreen:
        return (metadata->modes & kPlayerModeSplit) != 0;
    case PlayerFilter::LocalCoop:
        if (metadata->hasModes)
            return (metadata->modes & kPlayerModeCoop) != 0;
        return metadata->players >= 2;
    case PlayerFilter::Lan:
        return (metadata->modes & kPlayerModeLan) != 0;
    case PlayerFilter::Online:
        return (metadata->modes & kPlayerModeOnline) != 0;
    case PlayerFilter::Any:
        break;
    }
    return true;
}

std::string catalogFoldForSearch(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            const char folded = (c >= 'A' && c <= 'Z')
                ? static_cast<char>(c + ('a' - 'A')) : text[i];
            out.push_back(folded);
            ++i;
            continue;
        }
        // Cyrillic capitals (two-byte sequences with a 0xD0 lead):
        //   U+0400-U+040F (D0 80-8F, incl. Ё=U+0401) → U+0450-U+045F
        //   U+0410-U+041F (D0 90-9F, А-П) → U+0430-U+043F (same lead)
        //   U+0420-U+042F (D0 A0-AF, Р-Я) → U+0440-U+044F (lead D0→D1)
        // The Р-Я range crosses the D0/D1 UTF-8 boundary, so a plain
        // "+0x20 on the trail byte" would emit invalid bytes.
        if (c == 0xD0 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x80 && d <= 0x8F) {
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(d + 0x10));
                i += 2;
                continue;
            }
            if (d >= 0x90 && d <= 0x9F) {
                out.push_back(static_cast<char>(c));
                out.push_back(static_cast<char>(d + 0x20));
                i += 2;
                continue;
            }
            if (d >= 0xA0 && d <= 0xAF) {
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(d - 0x20));
                i += 2;
                continue;
            }
        }
        // Any other multi-byte sequence (or truncation): copy the lead byte
        // and let the continuation bytes flow through below.
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool catalogFoldedContains(const std::string& haystack,
                           const std::string& needleFolded) {
    if (needleFolded.empty())
        return true;
    return catalogFoldForSearch(haystack).find(needleFolded) !=
           std::string::npos;
}

bool catalogEntryMatchesSearch(const CatalogEntry& entry,
                               const GameMetadata* metadata,
                               const std::string& needleFolded) {    if (needleFolded.empty())
        return true;
    if (catalogFoldedContains(entry.title, needleFolded))
        return true;
    if (!entry.genre.empty() &&
        catalogFoldedContains(entry.genre, needleFolded))
        return true;
    if (!metadata)
        return false;
    if (!metadata->name.empty() &&
        catalogFoldedContains(metadata->name, needleFolded))
        return true;
    for (const std::string& category : metadata->categories) {
        if (catalogFoldedContains(category, needleFolded))
            return true;
    }
    return false;
}

bool buildCatalogBrowse(const CatalogBrowseRequest& request,
                        CatalogBrowseResult& result,
                        const std::function<bool()>& cancelled) {
    auto isCancelled = [&] { return cancelled && cancelled(); };
    result = {};
    result.generation = request.generation;
    result.catalog = request.catalog
        ? request.catalog
        : std::make_shared<const std::vector<CatalogEntry>>();
    result.metadata = request.metadata
        ? request.metadata
        : std::make_shared<const GameMetadataIndexSnapshot>();
    result.selectedHashes = request.selectedHashes;
    const auto& all = *result.catalog;
    const std::string needle = catalogFoldForSearch(request.query);
    result.indices.reserve(all.size());

    std::unordered_set<std::string> genres;
    for (size_t i = 0; i < all.size(); ++i) {
        if ((i & 127u) == 0 && isCancelled())
            return false;
        const CatalogEntry& entry = all[i];
        if (entry.isHiddenByDefault())
            continue;
        result.hasRegularEntries = true;
        const GameMetadata* meta =
            result.metadata->findByInfoHash(entry.infoHash);
        if (catalogEntryInSection(entry, meta, request.section)) {
            if (meta) {
                for (const std::string& category : meta->categories)
                    if (!category.empty())
                        genres.insert(category);
            } else if (!entry.genre.empty()) {
                genres.insert(entry.genre);
            }
        }

        const std::string hash = foldAscii(entry.infoHash);
        if (request.favoritesOnly &&
            request.favoriteHashes.count(hash) == 0)
            continue;
        if (request.fitsOnly && request.freeSpaceAvailable && entry.size != 0 &&
            entry.size > request.freeBytes)
            continue;
        if (!catalogEntryInSection(entry, meta, request.section))
            continue;
        if (!catalogEntryMatchesPlayerFilter(meta, request.playerFilter))
            continue;
        if (!request.genreFilters.empty()) {
            bool matchedGenre = false;
            if (meta) {
                for (const std::string& category : meta->categories)
                    matchedGenre = matchedGenre ||
                        request.genreFilters.count(category) != 0;
            }
            matchedGenre = matchedGenre || (!entry.genre.empty() &&
                request.genreFilters.count(entry.genre) != 0);
            if (!matchedGenre)
                continue;
        }
        if (!catalogEntryMatchesSearch(entry, meta, needle))
            continue;
        result.indices.push_back(static_cast<int>(i));
    }
    result.genres.assign(genres.begin(), genres.end());
    std::sort(result.genres.begin(), result.genres.end());
    if (isCancelled())
        return false;

    auto entryAt = [&](int index) -> const CatalogEntry& {
        return all[static_cast<size_t>(index)];
    };
    if (request.sort == CatalogSortMode::Alphabetical) {
        std::unordered_map<int, std::string> keys;
        keys.reserve(result.indices.size());
        size_t keyIndex = 0;
        for (int index : result.indices) {
            if ((keyIndex++ & 127u) == 0 && isCancelled())
                return false;
            keys.emplace(index, catalogFoldForSearch(entryAt(index).title));
        }
        if (!stableSortCancelled(result.indices,
                [&](int a, int b) { return keys[a] < keys[b]; }, cancelled))
            return false;
    } else if (request.sort == CatalogSortMode::Largest) {
        if (!stableSortCancelled(result.indices,
                [&](int a, int b) {
                    return entryAt(a).size > entryAt(b).size;
                }, cancelled))
            return false;
    } else if (request.sort == CatalogSortMode::Latest) {
        if (!stableSortCancelled(result.indices,
                [&](int a, int b) {
                    return entryAt(a).publishedAt > entryAt(b).publishedAt;
                }, cancelled))
            return false;
    } else {
        bool hasPeers = false;
        for (size_t i = 0; i < result.indices.size(); ++i) {
            if ((i & 127u) == 0 && isCancelled())
                return false;
            hasPeers = hasPeers || entryAt(result.indices[i]).peerCount > 0;
        }
        if (hasPeers) {
            if (!stableSortCancelled(result.indices,
                    [&](int a, int b) {
                        const CatalogEntry& left = entryAt(a);
                        const CatalogEntry& right = entryAt(b);
                        if (left.peerCount != right.peerCount)
                            return left.peerCount > right.peerCount;
                        return left.publishedAt > right.publishedAt;
                    }, cancelled))
                return false;
        } else {
            std::vector<int> ranked = result.indices;
            std::unordered_map<int, size_t> score;
            score.reserve(ranked.size());
            if (!stableSortCancelled(ranked, [&](int a, int b) {
                    return entryAt(a).publishedAt > entryAt(b).publishedAt;
                }, cancelled))
                return false;
            for (size_t pos = 0; pos < ranked.size(); ++pos)
                score[ranked[pos]] += pos;
            if (!stableSortCancelled(ranked, [&](int a, int b) {
                    return entryAt(a).size > entryAt(b).size;
                }, cancelled))
                return false;
            for (size_t pos = 0; pos < ranked.size(); ++pos)
                score[ranked[pos]] += pos;
            if (!stableSortCancelled(result.indices, [&](int a, int b) {
                    if (score[a] != score[b])
                        return score[a] < score[b];
                    return entryAt(a).publishedAt > entryAt(b).publishedAt;
                }, cancelled))
                return false;
        }
    }
    if (request.sortReversed) {
        for (size_t left = 0, right = result.indices.size();
             left < right && left < --right; ++left) {
            if ((left & 127u) == 0 && isCancelled())
                return false;
            std::swap(result.indices[left], result.indices[right]);
        }
    }
    if (isCancelled())
        return false;

    const size_t count = result.indices.size();
    result.titles.reserve(count);
    result.iconUrls.reserve(count);
    result.iconPreserveAspect.reserve(count);
    result.badges.reserve(count);
    result.baseBadges.reserve(count);
    result.favorite.reserve(count);
    result.selected.reserve(count);
    result.selectable.reserve(count);
    result.rowByInfoHash.reserve(count);
    result.rowsByInfoHash.reserve(count);
    result.structureHashes.reserve(count);
    for (size_t rowIndex = 0; rowIndex < count; ++rowIndex) {
        if ((rowIndex & 127u) == 0 && isCancelled())
            return false;
        const CatalogEntry& entry = entryAt(result.indices[rowIndex]);
        const std::string hash = foldAscii(entry.infoHash);
        result.structureHashes.push_back(hash);
        const auto task = request.taskBadges.find(hash);
        const bool selectable = task == request.taskBadges.end();
        if (!selectable)
            result.selectedHashes.erase(hash);
        const GameMetadata* meta =
            result.metadata->findByInfoHash(entry.infoHash);
        CatalogRowPresentation presentation = resolveCatalogRow(entry, meta);
        std::string baseBadge;
        if (!presentation.titleId.empty() &&
            request.installedTitleIds.count(
                upperAscii(presentation.titleId)) != 0)
            baseBadge = request.installedBadge;
        std::string badge = selectable ? baseBadge : task->second;
        result.titles.push_back(std::move(presentation.title));
        result.iconUrls.push_back(std::move(presentation.iconUrl));
        result.iconPreserveAspect.push_back(
            presentation.iconPreserveAspect ? 1 : 0);
        result.badges.push_back(std::move(badge));
        result.baseBadges.push_back(std::move(baseBadge));
        result.favorite.push_back(request.favoriteHashes.count(hash) ? 1 : 0);
        result.selected.push_back(
            result.selectedHashes.count(hash) ? 1 : 0);
        result.selectable.push_back(selectable ? 1 : 0);
        result.rowByInfoHash.emplace(hash, rowIndex);
        result.rowsByInfoHash[hash].push_back(rowIndex);
    }
    result.count = count;
    if (request.previous) {
        const bool same = sameBrowseStructure(
            *request.previous, result, cancelled);
        if (isCancelled())
            return false;
        result.structureChanged = !same;
    }
    return !isCancelled();
}

std::vector<size_t> patchCatalogTaskBadge(CatalogBrowseResult& result,
                                          const std::string& infoHash,
                                          bool hasTask,
                                          const std::string& badge) {
    std::vector<size_t> changed;
    const auto found = result.rowsByInfoHash.find(foldAscii(infoHash));
    if (found == result.rowsByInfoHash.end())
        return changed;
    for (size_t row : found->second) {
        if (row >= result.badges.size() || row >= result.baseBadges.size() ||
            row >= result.selectable.size() || row >= result.selected.size())
            continue;
        const std::string& nextBadge = hasTask ? badge : result.baseBadges[row];
        const uint8_t nextSelectable = hasTask ? 0 : 1;
        const bool rowChanged = result.badges[row] != nextBadge ||
            result.selectable[row] != nextSelectable ||
            (hasTask && result.selected[row] != 0);
        result.badges[row] = nextBadge;
        result.selectable[row] = nextSelectable;
        if (hasTask) {
            result.selected[row] = 0;
            result.selectedHashes.erase(foldAscii(infoHash));
        }
        if (rowChanged)
            changed.push_back(row);
    }
    return changed;
}

std::vector<size_t> patchCatalogFavorite(CatalogBrowseResult& result,
                                         const std::string& infoHash,
                                         bool favorite) {
    std::vector<size_t> changed;
    const auto found = result.rowsByInfoHash.find(foldAscii(infoHash));
    if (found == result.rowsByInfoHash.end())
        return changed;
    for (size_t row : found->second) {
        if (row >= result.favorite.size() ||
            result.favorite[row] == static_cast<uint8_t>(favorite))
            continue;
        result.favorite[row] = favorite ? 1 : 0;
        changed.push_back(row);
    }
    return changed;
}

std::vector<size_t> patchCatalogInstalledBadge(
    CatalogBrowseResult& result, const std::string& infoHash,
    const std::unordered_set<std::string>& installedTitleIds,
    const std::string& badge) {
    std::vector<size_t> changed;
    const auto found = result.rowsByInfoHash.find(foldAscii(infoHash));
    if (found == result.rowsByInfoHash.end())
        return changed;
    for (size_t row : found->second) {
        if (row >= result.baseBadges.size() || row >= result.badges.size() ||
            row >= result.selectable.size())
            continue;
        if (row >= result.indices.size())
            continue;
        const int entryIndex = result.indices[row];
        if (entryIndex < 0 || !result.catalog ||
            static_cast<size_t>(entryIndex) >= result.catalog->size())
            continue;
        const CatalogEntry& entry =
            (*result.catalog)[static_cast<size_t>(entryIndex)];
        const GameMetadata* meta = result.metadata
            ? result.metadata->findByInfoHash(entry.infoHash) : nullptr;
        const std::string titleId = resolveCatalogRow(entry, meta).titleId;
        const bool installed = !titleId.empty() &&
            installedTitleIds.count(upperAscii(titleId)) != 0;
        const std::string next = installed ? badge : std::string();
        if (result.baseBadges[row] == next)
            continue;
        const std::string oldBase = result.baseBadges[row];
        result.baseBadges[row] = next;
        if (result.selectable[row] != 0 && result.badges[row] == oldBase) {
            result.badges[row] = next;
            changed.push_back(row);
        }
    }
    return changed;
}

CatalogBrowseGenerationQueue::Ticket
CatalogBrowseGenerationQueue::request() {
    std::lock_guard<std::mutex> lock(mutex_);
    Ticket ticket;
    ticket.generation = ++latest_;
    if (active_ == 0) {
        active_ = ticket.generation;
        ticket.startNow = true;
    } else {
        pending_ = true;
    }
    return ticket;
}

bool CatalogBrowseGenerationQueue::complete(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ != generation)
        return false;
    if (pending_) {
        pending_ = false;
        active_ = latest_;
        return true;
    }
    active_ = 0;
    return false;
}

bool CatalogBrowseGenerationQueue::isCurrent(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == latest_;
}

size_t CatalogBrowseGenerationQueue::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ ? 1 : 0;
}

size_t CatalogBrowseGenerationQueue::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ == 0 ? 0 : 1;
}

uint64_t CatalogBrowseGenerationQueue::latestGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

CatalogFreshness resolveCatalogFreshness(bool refreshing, uint64_t wallSec,
                                          int64_t snapshotSec, bool hasEntries,
                                          bool isToday) {
    CatalogFreshness out;
    if (refreshing) {
        out.kind = CatalogFreshness::Kind::Updating;
        return out;
    }
    if (wallSec != 0) {
        out.kind = isToday ? CatalogFreshness::Kind::Ok
                           : CatalogFreshness::Kind::Stale;
        out.epochSec = static_cast<int64_t>(wallSec);
        return out;
    }
    if (snapshotSec > 0 && hasEntries) {
        out.kind = isToday ? CatalogFreshness::Kind::Ok
                           : CatalogFreshness::Kind::Stale;
        out.epochSec = snapshotSec;
        return out;
    }
    out.kind = CatalogFreshness::Kind::Never;
    return out;
}

bool catalogAutoRefreshDue(uint64_t wallSec, int64_t cachedSnapshotSec,
                           bool hasCachedEntries, bool wallIsToday,
                           bool cachedIsToday) {
    if (wallSec != 0)
        return !wallIsToday;
    if (hasCachedEntries && cachedSnapshotSec > 0)
        return !cachedIsToday;
    return true;
}

} // namespace pipensx
