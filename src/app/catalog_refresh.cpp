#include "catalog_refresh.hpp"

namespace pipensx {

CatalogRefreshAdoption adoptCatalogRefresh(
    CatalogService& catalog, GameMetadataService& metadata,
    CatalogRefreshBatch batch) {
    CatalogRefreshAdoption result;
    if (batch.catalogOk) {
        catalog.adopt(std::move(batch.catalogEntries));
        result.catalogChanged = true;
    }
    if (batch.metadataOk) {
        metadata.adopt(std::move(batch.metadata));
        metadata.dropMemoryImageCache();
        result.metadataChanged = true;
    }
    return result;
}

} // namespace pipensx
