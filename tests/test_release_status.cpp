#include "ui/common/release_status.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    using namespace pipensx;
    using namespace pipensx::ui;
    CatalogEntry entry;
    assert(releaseStatus(entry).tone == ChipTone::Neutral);
    entry.health = CatalogHealth::Ok;
    assert(releaseStatus(entry).tone == ChipTone::Neutral);
    entry.hasPeerCount = true;
    assert(releaseStatus(entry).tone == ChipTone::Error);
    assert(std::string(releaseStatus(entry).key) == "pipensx/health/no_peers");
    for (uint32_t peers : {1u, 2u}) {
        entry.peerCount = peers;
        assert(releaseStatus(entry).tone == ChipTone::Warning);
    }
    entry.peerCount = 3;
    assert(releaseStatus(entry).tone == ChipTone::Success);
    entry.peerCount = UINT32_MAX;
    assert(releaseStatus(entry).tone == ChipTone::Success);
    for (auto health : {CatalogHealth::Dead, CatalogHealth::TrackerNotRegistered,
                        CatalogHealth::NoPeers}) {
        entry.health = health;
        assert(releaseStatus(entry).tone == ChipTone::Error);
    }
    for (auto health : {CatalogHealth::Replaced, CatalogHealth::MetadataTimeout}) {
        entry.health = health;
        assert(releaseStatus(entry).tone == ChipTone::Warning);
    }
    std::puts("release status tests passed");
}
