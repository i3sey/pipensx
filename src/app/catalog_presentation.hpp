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
    std::vector<std::string> screenshots;
};

std::vector<std::string> mergeScreenshotUrls(
    const GameMetadata* metadata, const CatalogEntry& entry,
    size_t limit = 6);

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata);

bool catalogEntryIsGame(const CatalogEntry& entry,
                        const GameMetadata* metadata);

bool catalogEntryHasMatchedTitle(const GameMetadata* metadata);

} // namespace pipensx
