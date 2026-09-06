#include "catalog_refresh.hpp"

extern "C" {
#include "../core/util.h"
}

#include <atomic>
#include <ctime>

namespace pipensx {
namespace {

std::atomic<bool> catalogRefreshHeld{false};

} // namespace

CatalogRefreshAdoption adoptCatalogRefresh(
    CatalogService& catalog, GameMetadataService& metadata,
    CatalogRefreshBatch batch, const std::string& catalogSourceUrl) {
    CatalogRefreshAdoption result;
    if (batch.catalogOk) {
        catalog.adopt(std::move(batch.catalogEntries), catalogSourceUrl);
        result.catalogChanged = true;
    }
    if (batch.metadataOk) {
        // Covers are keyed by URL, so a refresh that delivers the same index
        // (same manifest SHA) must not evict the 96 MB decoded cache: every
        // visible card would re-decode and flicker on the next scroll.
        const std::string incomingSha = batch.metadata.manifest.indexSha256;
        const std::string currentSha = metadata.manifest().indexSha256;
        metadata.adopt(std::move(batch.metadata));
        if (incomingSha.empty() || incomingSha != currentSha)
            metadata.dropMemoryImageCache();
        result.metadataChanged = true;
    }
    return result;
}

bool tryBeginCatalogRefresh() {
    bool expected = false;
    return catalogRefreshHeld.compare_exchange_strong(expected, true);
}

void endCatalogRefresh() { catalogRefreshHeld.store(false); }

bool catalogRefreshInFlight() {
    return catalogRefreshHeld.load();
}

bool recordCatalogRefreshSuccess(AppSettings* settings, bool catalog,
                                 bool metadata, std::string& error) {
    error.clear();
    if (!settings || (!catalog && !metadata))
        return true;
    AppSettingsData values = settings->get();
    const uint64_t now = now_ms();
    if (catalog) {
        values.lastCatalogRefreshMs = now;
        values.lastCatalogRefreshWallSec =
            static_cast<uint64_t>(time(nullptr));
    }
    if (metadata)
        values.lastMetadataRefreshMs = now;
    return settings->update(values, error);
}

} // namespace pipensx
