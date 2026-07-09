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
                               ".nsp", ".nsz", ".xci", ".xcz"});
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

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata) {
    CatalogPresentation result;
    result.title = metadata && !metadata->name.empty()
        ? metadata->name : entry.title;
    result.titleId = metadata ? metadata->titleId : std::string();
    if (metadata && !metadata->iconUrl.empty()) {
        result.iconUrl = metadata->iconUrl;
        result.iconPreserveAspect = false;
    } else {
        result.iconUrl = entry.posterUrl;
        result.iconPreserveAspect = !result.iconUrl.empty();
    }
    if (metadata && !metadata->bannerUrl.empty())
        result.coverUrl = metadata->bannerUrl;
    else
        result.coverUrl = result.iconUrl;
    if (metadata && !metadata->description.empty())
        result.description = metadata->description;
    else if (metadata && !metadata->intro.empty())
        result.description = metadata->intro;
    else
        result.description = entry.description;
    result.developer = entry.developer;
    result.publisher = metadata && !metadata->publisher.empty()
        ? metadata->publisher : entry.publisher;
    result.releaseDate = metadata && !metadata->releaseDate.empty()
        ? metadata->releaseDate : entry.year;
    result.genre = metadata && !metadata->categories.empty()
        ? join(metadata->categories) : entry.genre;
    result.screenshots = mergeScreenshotUrls(metadata, entry, 6);
    return result;
}

bool catalogEntryIsGame(const CatalogEntry& entry,
                        const GameMetadata* metadata) {
    if (metadata && looksLikeTitleId(metadata->titleId))
        return true;
    const std::string title = lowerAscii(entry.title);
    if (hasNroMarker(title))
        return false;
    return hasPackageMarker(title);
}

bool catalogEntryHasMatchedTitle(const GameMetadata* metadata) {
    return metadata && looksLikeTitleId(metadata->titleId);
}

} // namespace pipensx
