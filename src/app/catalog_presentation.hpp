#pragma once

#include "catalog_service.hpp"
#include "game_metadata_service.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pipensx {

const char* catalogContentBadge(const GameMetadata* metadata);

std::vector<std::string> mergeScreenshotUrls(
    const GameMetadata* metadata, const CatalogEntry& entry,
    size_t limit = 6);

} // namespace pipensx
