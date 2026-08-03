#pragma once

#include "catalog_service.hpp"
#include "download_manager.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pipensx {

// Indices of the packages whose [vN] tag numerically equals latestVersion —
// the update the metadata index points at (the generator derives
// latestVersion from exactly those tags). Empty when no package matches; the
// caller then falls back to selectUpdateFiles. When more than one package
// matches, the caller asks the user which one to install.
std::vector<size_t> updateVersionMatches(const TorrentPreview& preview,
                                         const std::string& latestVersion);

// Per-file action mask installing exactly the packages in `picks` (everything
// else is skipped). `picks` may be empty, yielding an all-skip mask.
std::vector<uint8_t> selectFiles(const TorrentPreview& preview,
                                 const std::vector<size_t>& picks);

// Builds the per-file action mask that installs a release torrent as an
// update of an already-installed title. Release bundles mix base game, update
// and DLC packages; the metadata index derives latestVersion from the [vN]
// tags in release file names.
//
// The package whose [vN] tag equals latestVersion is installed and everything
// else is skipped, so only the update bytes hit the wire. Lookalikes such as a
// mod bundle named "... [v9895936].nsp" beside the real
// "... [v10092544].nsp" are excluded by the exact tag. Without an exact
// match, the highest-tagged update-marked package is used; when no package is
// marked, every package is selected as a fallback — the install backend
// short-circuits content keys that are already installed, so the worst case
// is re-downloading the bundle, never a corrupt install.
std::vector<uint8_t> selectUpdateFiles(const TorrentPreview& preview,
                                       const std::string& latestVersion);

// Magnet used to resolve the update torrent for `infoHash`. Prefers the
// catalog entry's trusted magnet (trackers plus the pre-resolved info dict);
// falls back to a RuTracker magnet for the hash when the catalog does not
// know it — the resolver only accepts RuTracker trackers, and the index is
// RuTracker-derived.
std::string updateMagnetFor(const std::string& infoHash,
                            const CatalogEntry* entry);

} // namespace pipensx
