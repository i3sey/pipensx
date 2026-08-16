#pragma once

#include "app_settings.hpp"
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
    CatalogRefreshBatch batch, const std::string& catalogSourceUrl = {});

// One in-flight catalogue/metadata fetch across every UI entry (catalog tab,
// settings). A second caller no-ops until endCatalogRefresh().
bool tryBeginCatalogRefresh();
void endCatalogRefresh();
bool catalogRefreshInFlight();

// Stamp last-refresh times after a successful fetch. Safe with a null
// settings pointer (no-op). Returns false when persist fails.
bool recordCatalogRefreshSuccess(AppSettings* settings, bool catalog,
                                 bool metadata, std::string& error);

} // namespace pipensx
