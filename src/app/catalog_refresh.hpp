#pragma once

#include "catalog_service.hpp"
#include "game_metadata_service.hpp"

#include <string>
#include <vector>

namespace pipensx {

struct CatalogRefreshBatch {
    bool catalogOk = false;
    std::vector<CatalogEntry> catalogEntries;
    std::string catalogError;
    bool metadataOk = false;
    MetadataSnapshot metadata;
    std::string metadataError;
};

struct CatalogRefreshAdoption {
    bool catalogChanged = false;
    bool metadataChanged = false;
};

// UI-thread seam: worker threads fill a batch without touching live maps, then
// the render thread adopts each successful source independently.
CatalogRefreshAdoption adoptCatalogRefresh(
    CatalogService& catalog, GameMetadataService& metadata,
    CatalogRefreshBatch batch);

} // namespace pipensx
