#include "catalog_presentation.hpp"

#include <unordered_set>

namespace pipensx {

const char* catalogContentBadge(const GameMetadata* metadata) {
    return metadata ? "Contains NSP/NSZ" : "Does not contain NSP/NSZ";
}

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

} // namespace pipensx
