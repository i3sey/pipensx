#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx {

struct MagnetSpec {
    uint8_t infoHash[20]{};
    std::string infoHashHex;
    std::string trackerUrl;
};

struct MagnetProgress {
    enum class Stage {
        FindingPeers,
        Connecting,
        FetchingMetadata,
        Validating,
    };

    Stage stage = Stage::FindingPeers;
    uint32_t completedPieces = 0;
    uint32_t totalPieces = 0;
    uint32_t peerIndex = 0;
    uint32_t peerCount = 0;
};

class MagnetResolver {
public:
    using ProgressCallback = std::function<void(const MagnetProgress&)>;

    static bool parse(const std::string& uri, MagnetSpec& spec,
                      std::string& error);
    static bool buildTorrent(const MagnetSpec& spec,
                             const std::vector<uint8_t>& info,
                             std::vector<uint8_t>& torrent,
                             std::string& error);

    /* presetInfo: pre-resolved bencoded info dictionary from the catalog
       (RF_ACCESS_PLAN П2.1). When it validates against the magnet hash the
       whole tracker→peer→ut_metadata phase is skipped; otherwise the
       network resolve runs as usual. */
    bool resolveToFile(const std::string& uri, const std::string& path,
                       std::atomic<bool>& cancelled,
                       const ProgressCallback& progress,
                       std::string& error,
                       std::vector<uint8_t>* verifiedPeers = nullptr,
                       const std::vector<uint8_t>* presetInfo = nullptr) const;
};

} // namespace pipensx
