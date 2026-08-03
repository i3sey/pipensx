#pragma once

#include "catalog_service.hpp"
#include "download_manager.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pipensx {

// Largest byte index <= maxBytes that does not split a UTF-8 code point.
// RuTracker file names are Cyrillic, so truncating a user-visible label to a
// byte count must never leave a partial character.
inline size_t utf8TruncateBoundary(const std::string& text, size_t maxBytes) {
    if (maxBytes >= text.size())
        return text.size();
    if (maxBytes == 0)
        return 0;
    size_t lead = maxBytes - 1;
    while (lead > 0 &&
           (static_cast<unsigned char>(text[lead]) & 0xC0) == 0x80)
        --lead;
    const unsigned char b = static_cast<unsigned char>(text[lead]);
    if (b < 0xC0)
        return maxBytes;
    const size_t need = b >= 0xF0 ? 3 : (b >= 0xE0 ? 2 : 1);
    if (lead + 1 + need <= maxBytes)
        return maxBytes;
    return lead;
}

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

// One page of the chooser shown when a release carries several packages with
// the update's [vN] tag (brls::Dialog fits three buttons, so matches page
// through two at a time). Every page carries up to two file buttons plus the
// continuation for the "more" page.
//
// Every FileButton holds a full copy of `initialPeers`: the bootstrap peers
// from the magnet resolve are the only way an import can start on networks
// where the tracker is unreachable, so no page or button may consume the
// list. `morePeers` hands the same list to the next page.
struct UpdateFileChoicePage {
    struct FileButton {
        size_t index = 0;  // index into preview.files
        std::string label;
        std::vector<uint8_t> peers;  // full bootstrap peer list for this button
    };
    std::vector<FileButton> files;      // at most two entries
    size_t nextStart = 0;               // matches index for the next page
    size_t remaining = 0;               // matches beyond this page (0 = last)
    std::vector<uint8_t> morePeers;     // peers for the "more" continuation
};

UpdateFileChoicePage updateFileChoicePage(const TorrentPreview& preview,
                                          const std::vector<size_t>& matches,
                                          size_t start,
                                          std::vector<uint8_t> initialPeers);

// Magnet used to resolve the update torrent for `infoHash`. Prefers the
// catalog entry's trusted magnet (trackers plus the pre-resolved info dict);
// falls back to a RuTracker magnet for the hash when the catalog does not
// know it — the resolver only accepts RuTracker trackers, and the index is
// RuTracker-derived.
std::string updateMagnetFor(const std::string& infoHash,
                            const CatalogEntry* entry);

} // namespace pipensx
