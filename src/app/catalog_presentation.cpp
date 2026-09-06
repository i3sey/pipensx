#include "catalog_presentation.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <sstream>
#include <unordered_set>

namespace pipensx {

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
    // entries carry no description at all, and neither does any golden fixture.
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
        out.kind = CatalogFreshness::Kind::Stale;
        out.epochSec = snapshotSec;
        return out;
    }
    out.kind = CatalogFreshness::Kind::Never;
    return out;
}

} // namespace pipensx
