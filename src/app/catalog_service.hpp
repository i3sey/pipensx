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

private:
    bool loadFile(const std::string& path, const std::string& label,
                  std::string& error);

    std::string rootPath_;
    std::string catalogRoot_;
    std::string cachePath_;
    std::string bundledPath_;
    std::vector<CatalogEntry> entries_;
    std::string sourceLabel_;
};

} // namespace pipensx
