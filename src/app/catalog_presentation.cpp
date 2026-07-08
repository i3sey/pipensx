#include "catalog_presentation.hpp"

#include <algorithm>
#include <cctype>
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

std::string join(const std::vector<std::string>& values) {
    std::ostringstream result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            result << ", ";
        result << values[i];
    }
    return result.str();
}

} // namespace

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata) {
    CatalogPresentation result;
    result.title = metadata && !metadata->name.empty()
        ? metadata->name : entry.title;
    result.titleId = metadata ? metadata->titleId : std::string();
    result.iconUrl = metadata && !metadata->iconUrl.empty()
        ? metadata->iconUrl : entry.posterUrl;
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
    if (metadata)
        return true;
    std::string title = entry.title;
    std::transform(title.begin(), title.end(), title.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return title.find("[nro]") == std::string::npos;
}

} // namespace pipensx
