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
    /* Inline catalogue metadata carried by the Langegen switch_games.json
       source. An optional metadata index is keyed by info-hash and may not
       match these entries, so the detail card falls back to these fields for
       its facts table, description and cover. Empty when absent. */
    std::string year;
    std::string genre;
    std::string developer;
    std::string publisher;
    std::string description;
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
                            std::string bundledPath = {});

    bool load(std::string& error);

    // Pool-thread safe: fetch the latest catalogue from the trusted source,
    // parse it, and persist the on-disk cache. Fills `parsed` on success and
    // never touches entries_, so it may run on a worker thread. The caller
    // adopts the parsed batch on the UI thread via adopt().
    bool fetchLatest(std::vector<CatalogEntry>& parsed, std::string& error);
    // UI-thread only: adopt a freshly fetched batch as the live catalogue.
    // entries() is read unsynchronised by the render thread every frame, so
    // entries_ may only be reassigned here — never from a fetch worker.
    void adopt(std::vector<CatalogEntry> parsed);

    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const std::string& sourceLabel() const { return sourceLabel_; }
    const std::string& rootPath() const { return rootPath_; }

    static bool parseJson(const std::string& json,
                          std::vector<CatalogEntry>& entries,
                          std::string& error);

    // True when `url` is on the trusted-host allowlist for catalog bytes.
    // Every network source is gated on this before a byte is fetched.
    static bool isTrustedSource(const std::string& url);

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
