#pragma once

#include <string>
#include <vector>

namespace pipensx {

// Version source for the game-update check. The metadata index
// (catalog∩titledb) is the only source of candidate versions: it is already
// fetched, cached and trust-listed, so a check never touches the network.
// The check service consumes only the candidate version strings — never the
// full metadata records — which keeps it testable off-console with a fake.
class IUpdateMetadataSource {
public:
    virtual ~IUpdateMetadataSource() = default;

    // Appends the non-empty latestVersion of every index entry matching
    // titleId (a game can have several bundles/regions, each carrying its own
    // version). Returns false when the index has no entry for the title at
    // all — the "source unknown" case. The caller folds the candidates.
    virtual bool collectLatestVersions(const std::string& titleId,
                                       std::vector<std::string>& out) const = 0;
};

} // namespace pipensx
