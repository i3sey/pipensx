#pragma once

#include "app_settings.hpp"
#include "catalog_service.hpp"
#include "game_metadata_service.hpp"

#include <string>
#include <vector>

namespace pipensx {

struct CatalogRefreshBatch {
    bool cancelled = false;
    bool catalogOk = false;
    CatalogSnapshot catalog;
    std::string catalogError;
    bool metadataOk = false;
    MetadataSnapshot metadata;
    std::string metadataError;
};

struct CatalogRefreshAdoption {
    bool catalogChanged = false;
    bool metadataChanged = false;
    RetiredCatalogSnapshot retiredCatalog;
    RetiredMetadataSnapshot retiredMetadata;
};

// UI-thread seam: workers fill fully indexed snapshots without touching live
// maps, then the render thread publishes each successful source independently.
// The rvalue reference deliberately leaves cancelled payload ownership with
// the caller so it can retire those large buffers on a worker.
CatalogRefreshAdoption adoptCatalogRefresh(
    CatalogService& catalog, GameMetadataService& metadata,
    CatalogRefreshBatch&& batch, const std::string& catalogSourceUrl = {});

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
