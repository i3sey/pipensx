#pragma once

#include "catalog_service.hpp"
#include "game_metadata_service.hpp"
#include "mod_index_service.hpp"

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
    bool modsOk = false;
    ModIndexSnapshot mods;
    std::string modsError;
};

struct CatalogRefreshAdoption {
    bool catalogChanged = false;
    bool metadataChanged = false;
    bool modsChanged = false;
};

// UI-thread seam: worker threads fill a batch without touching live maps, then
// the render thread adopts each successful source independently. `mods` is
// optional (null on builds/tests without a mod index).
CatalogRefreshAdoption adoptCatalogRefresh(
    CatalogService& catalog, GameMetadataService& metadata,
    CatalogRefreshBatch batch, ModIndexService* mods = nullptr);

} // namespace pipensx
