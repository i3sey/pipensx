#include "catalog_refresh.hpp"

namespace pipensx {

CatalogRefreshAdoption adoptCatalogRefresh(
    CatalogService& catalog, GameMetadataService& metadata,
    CatalogRefreshBatch batch, ModIndexService* mods,
    const std::string& catalogSourceUrl) {
    CatalogRefreshAdoption result;
    if (batch.catalogOk) {
        catalog.adopt(std::move(batch.catalogEntries), catalogSourceUrl);
        result.catalogChanged = true;
    }
    if (batch.metadataOk) {
        metadata.adopt(std::move(batch.metadata));
        metadata.dropMemoryImageCache();
        result.metadataChanged = true;
    }
    if (batch.modsOk && mods) {
        mods->adopt(std::move(batch.mods));
        result.modsChanged = true;
    }
    return result;
}

} // namespace pipensx
