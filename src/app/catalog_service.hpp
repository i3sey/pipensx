#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

enum class CatalogHealth {
    Unknown,
    Ok,
    NoPeers,
    MetadataTimeout,
    TrackerNotRegistered,
    Replaced,
    Dead,
};

struct CatalogEntry {
    std::string infoHash;
    std::string title;
    std::string magnetUri;
    std::string trackerUrl;
    std::string posterUrl;
    std::vector<std::string> screenshots;
    std::string healthReason;
    /* Pre-resolved bencoded info dictionary (RF_ACCESS_PLAN П2.1), decoded
       from the catalog's base64 "info_dict" and SHA-1-verified against the
       magnet hash at parse time. Empty when the catalog carries none. */
    std::vector<uint8_t> infoDict;
    uint64_t topicId = 0;
    uint64_t size = 0;
    int64_t publishedAt = 0;
    int64_t sourceUpdatedAt = 0;
    int64_t catalogGeneratedAt = 0;
    int64_t lastCheckedAt = 0;
    uint32_t forumId = 0;
    uint32_t trackerId = 0;
    uint32_t peerCount = 0;
    CatalogHealth health = CatalogHealth::Unknown;
    bool metadataOk = false;

    bool isHiddenByDefault() const {
        return health == CatalogHealth::Dead ||
               health == CatalogHealth::Replaced ||
               health == CatalogHealth::TrackerNotRegistered;
    }
};

class CatalogService {
public:
    explicit CatalogService(std::string rootPath,
                            std::string bundledPath =
                                "romfs:/catalog/catalog.json");

    bool load(std::string& error);
    bool refresh(std::string& error);

    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const std::string& sourceLabel() const { return sourceLabel_; }
    const std::string& rootPath() const { return rootPath_; }

    static bool parseJson(const std::string& json,
                          std::vector<CatalogEntry>& entries,
                          std::string& error);

    // True when `url` is on the trusted-host allowlist for catalog bytes
    // (GitHub release download or a configured mirror, RF_ACCESS_PLAN П3.1).
    // Every network source is gated on this before a byte is fetched.
    static bool isTrustedSource(const std::string& url);

private:
    bool loadFile(const std::string& path, const std::string& label,
                  std::string& error);
    // RF_ACCESS_PLAN П3.1: refresh sources, tried in order by refresh().
    bool refreshFromGitHubRelease(std::string& error);
    bool refreshFromMirror(const std::string& url, const std::string& label,
                           std::string& error);
    // Shared verify/parse/cache/adopt step; signature is nullptr when the
    // source carried none (RF_ACCESS_PLAN П3.2).
    bool commitCatalog(const std::string& body, const std::string* signature,
                       const std::string& label, std::string& error);

    std::string rootPath_;
    std::string catalogRoot_;
    std::string cachePath_;
    std::string bundledPath_;
    std::vector<CatalogEntry> entries_;
    std::string sourceLabel_;
};

} // namespace pipensx
