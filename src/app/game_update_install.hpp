#pragma once

#include "catalog_service.hpp"
#include "download_manager.hpp"

#include <string>
#include <vector>

namespace pipensx {

// Builds the per-file action mask that installs a release torrent as an
// update of an already-installed title. Release bundles mix base game, update
// and DLC packages; the metadata index derives latestVersion from the [vN]
// tags in release file names, so update packages carry a version tag while
// the base package usually does not (or carries [v0]).
//
// Files whose name carries an update marker are selected for Install and
// everything else is skipped, so only the update bytes hit the wire. When no
// package matches, every package is selected as a fallback — the install
// backend short-circuits content keys that are already installed, so the
// worst case is re-downloading the bundle, never a corrupt install.
std::vector<uint8_t> selectUpdateFiles(const TorrentPreview& preview);

// Magnet used to resolve the update torrent for `infoHash`. Prefers the
// catalog entry's trusted magnet (trackers plus the pre-resolved info dict);
// falls back to a RuTracker magnet for the hash when the catalog does not
// know it — the resolver only accepts RuTracker trackers, and the index is
// RuTracker-derived.
std::string updateMagnetFor(const std::string& infoHash,
                            const CatalogEntry* entry);

} // namespace pipensx
