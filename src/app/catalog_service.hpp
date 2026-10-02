#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
    /* Inline catalogue metadata carried by the catalogue source. An optional
       metadata index is keyed by info-hash and may not match these entries,
       so the detail card falls back to these fields for its facts table,
       description and cover. Empty when absent. */
    std::string year;
    std::string genre;
    std::string developer;
    std::string publisher;
    std::string description;
    /* Catalogue fields kept from the first dumps: Nintendo title id when
       known, CFW/firmware note, and a free-text multiplayer summary
       (scrape text — not structured player modes). interface_lang /
       voice_lang are releaser notes; the detail card shows them only in the
       Russian locale. */
    std::string titleId;
    std::string performance;
    std::string multiplayer;
    std::string interfaceLang;
    std::string voiceLang;
    /* Structured pipensx-catalog v2 fields. Absent (empty/zero) in entries
       parsed from older snapshots; the detail card falls back to the
       free-text fields above when these are missing. */
    std::string packageType;
    std::string version;
    std::vector<std::string> languagesInterface;
    std::vector<std::string> languagesVoice;
    std::string languageNote;
    uint32_t playersMin = 0;
    uint32_t playersMax = 0;
    bool playersOnline = false;
    std::string performanceNote;
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

// Fully indexed catalogue built by a refresh worker. Publishing this on the
// UI thread only moves ready containers; it never scans the catalogue.
struct CatalogSnapshot {
    std::vector<CatalogEntry> entries;
    std::unordered_map<std::string, size_t> infoHashIndex;
    std::string sourceLabel;
    int64_t snapshotEpochSec = 0;
};

// Ownership displaced by a publish. UI callers hand this to a worker so the
// last references and the old hash table are not destroyed in a render frame.
struct RetiredCatalogSnapshot {
    std::shared_ptr<const std::vector<CatalogEntry>> entries;
    std::unordered_map<std::string, size_t> infoHashIndex;
};

// Built-in pipensx-catalog release channel used when catalogSourceUrl is
// empty: a tiny manifest fetched first, the ~20 MB catalog only when its
// SHA-256 changed (manifest-delta). A custom catalogSourceUrl stays a single
// JSON fetched without any manifest check.
std::string defaultCatalogSourceUrl();
std::string defaultCatalogManifestUrl();

// Verified manifest of the default release channel (schemaVersion 2:
// catalog.sha256/bytes/entries checked before adopt).
struct CatalogManifest {
    uint32_t schemaVersion = 0;
    std::string catalogSha256; // lowercase hex, 64 chars
    size_t catalogBytes = 0;
    size_t catalogEntries = 0;
};

class CatalogService {
public:
    explicit CatalogService(std::string rootPath,
                            std::string bundledPath = {});

    bool load(std::string& error);

    // Worker-thread safe startup path: read, parse and index the preferred
    // cache (or the bundled fallback) without touching the live UI snapshot.
    bool prepareInitialSnapshot(CatalogSnapshot& snapshot,
                                std::string& error) const;

    // Pool-thread safe: fetch, parse and persist without touching live state.
    // The CatalogSnapshot overload also builds the lookup index and is the
    // refresh-path API; the vector overload remains for startup/tests.
    bool fetchLatest(std::vector<CatalogEntry>& parsed, std::string& error,
                     const std::string& sourceUrl,
                     const std::atomic<bool>* cancelled = nullptr);
    bool fetchLatest(CatalogSnapshot& snapshot, std::string& error,
                     const std::string& sourceUrl,
                     const std::atomic<bool>* cancelled = nullptr);
    // UI-thread only: publish a freshly fetched batch. The CatalogSnapshot
    // overload only moves ready containers; the vector compatibility overload
    // builds its index synchronously and is not used by refresh UI paths.
    RetiredCatalogSnapshot adopt(std::vector<CatalogEntry> parsed,
                                 const std::string& sourceUrl = {});
    RetiredCatalogSnapshot adopt(CatalogSnapshot snapshot,
                                 const std::string& sourceUrl = {});

    const std::vector<CatalogEntry>& entries() const { return *entries_; }

    // Case-insensitive lookup by info-hash. Null when absent. Used by the
    // game-update flow: the metadata index is catalog∩titledb, so the entry
    // carries the magnet/info_dict for the hash the index alone cannot resolve.
    const CatalogEntry* findByInfoHash(const std::string& infoHash) const;

    // Immutable shared snapshot of the live catalogue. Observers on other
    // threads (the web companion) hold this instead of a ~10 MB deep copy;
    // adopt() publishes a fresh vector, so a held snapshot stays valid and
    // consistent for as long as the pointer is kept.
    std::shared_ptr<const std::vector<CatalogEntry>> sharedEntries() const {
        return entries_;
    }

    // Runs on the UI thread at the end of every adopt(), so observers that
    // keep their own reference to the catalogue (the web companion) stay
    // current no matter which refresh path adopted the batch.
    void setOnAdopt(
        std::function<void(std::shared_ptr<const std::vector<CatalogEntry>>)>
            cb) {
        onAdopt_ = std::move(cb);
    }
    const std::string& sourceLabel() const { return sourceLabel_; }
    const std::string& rootPath() const { return rootPath_; }

    // True when the live snapshot came from the on-disk cache of a previous
    // network fetch, not the bundled dump or a live refresh. Used to treat a
    // today's cache as already-fresh even if the wall stamp was never saved.
    bool snapshotFromCache() const;

    // Wall-clock seconds for when the live snapshot was written or loaded
    // (cache/bundled mtime, or refresh time after adopt). 0 when empty.
    int64_t snapshotEpochSec() const { return snapshotEpochSec_; }

    // Changes whenever a different live catalogue snapshot is published.
    // UI views use this to retain derived presentation data across navigation
    // without rescanning the unchanged catalogue.
    uint64_t generation() const { return generation_; }

    static bool parseJson(const std::string& json,
                          std::vector<CatalogEntry>& entries,
                          std::string& error,
                          const std::atomic<bool>* cancelled = nullptr);

    // True when `url` is allowed to serve catalog bytes for `sourceUrl`.
    // The built-in channel keeps a repo-prefix allowlist; a custom source
    // trusts only redirects within the source file's directory.
    static bool isTrustedSource(const std::string& url,
                                const std::string& sourceUrl);
    // True when `url` may serve the built-in channel after a redirect:
    // the catalog repo itself plus the GitHub release-asset hosts that
    // `releases/latest/download` redirects to.
    static bool isTrustedRedirect(const std::string& url);

    // Parse a catalog manifest body (pure, unit-tested). Rejects wrong
    // schema versions and out-of-limit sizes/counts.
    static bool parseManifest(const std::string& body,
                              CatalogManifest& manifest, std::string& error);
    // Check downloaded catalog bytes against a parsed manifest: exact size
    // and SHA-256. Entry-count agreement is checked after parseJson.
    static bool verifyCatalogBody(const std::string& body,
                                  const CatalogManifest& manifest,
                                  std::string& error);

private:
    bool loadFileSnapshot(const std::string& path, const std::string& label,
                          CatalogSnapshot& snapshot,
                          std::string& error) const;
    static void buildIndex(const std::vector<CatalogEntry>& entries,
                           std::unordered_map<std::string, size_t>& index);
    // Built-in release channel: manifest fetch, SHA-256 delta against the
    // cached manifest, verified catalog download. Never touches live state.
    bool fetchDefaultChannel(std::vector<CatalogEntry>& parsed,
                             std::string& error,
                             const std::atomic<bool>* cancelled);

    std::string rootPath_;
    std::string catalogRoot_;
    std::string cachePath_;
    std::string manifestPath_;
    std::string bundledPath_;
    // Never null — starts as an empty vector. Reassigned only on the UI
    // thread (see adopt()).
    std::shared_ptr<const std::vector<CatalogEntry>> entries_ =
        std::make_shared<const std::vector<CatalogEntry>>();
    std::unordered_map<std::string, size_t> infoHashIndex_;
    std::string sourceLabel_;
    int64_t snapshotEpochSec_ = 0;
    uint64_t generation_ = 0;
    std::function<void(std::shared_ptr<const std::vector<CatalogEntry>>)>
        onAdopt_;
};

} // namespace pipensx
