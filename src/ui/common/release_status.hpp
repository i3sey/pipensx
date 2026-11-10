#pragma once

#include "app/catalog_service.hpp"

namespace pipensx::ui {

enum class ChipTone { Neutral, Success, Warning, Error };

struct ReleaseStatus {
    const char* key;
    ChipTone tone;
};

inline ReleaseStatus releaseStatus(const CatalogEntry& entry) {
    switch (entry.health) {
        case CatalogHealth::Dead:
        case CatalogHealth::TrackerNotRegistered:
            return {"pipensx/health/dead", ChipTone::Error};
        case CatalogHealth::Replaced:
            return {"pipensx/health/replaced", ChipTone::Warning};
        case CatalogHealth::NoPeers:
            return {"pipensx/health/no_peers", ChipTone::Error};
        case CatalogHealth::MetadataTimeout:
            return {"pipensx/health/no_metadata", ChipTone::Warning};
        default:
            break;
    }
    if (entry.hasPeerCount || entry.peerCount > 0) {
        if (entry.peerCount == 0)
            return {"pipensx/health/no_peers", ChipTone::Error};
        if (entry.peerCount < 3)
            return {"pipensx/release_chips/few_peers", ChipTone::Warning};
        return {"pipensx/release_chips/available", ChipTone::Success};
    }
    if (entry.health == CatalogHealth::Ok)
        return {"pipensx/health/checked", ChipTone::Neutral};
    return {"pipensx/health/unchecked", ChipTone::Neutral};
}

} // namespace pipensx::ui
