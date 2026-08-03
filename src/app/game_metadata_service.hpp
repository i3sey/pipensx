#pragma once

#include "update_source.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pipensx {

// Ways a game can be played together, as published in the metadata index's
// optional "modes" array. Bit flags so a whole snapshot can be OR-ed into one
// mask (see GameMetadataService::availablePlayerModes).
enum PlayerMode : uint8_t {
    kPlayerModeSplit = 1 << 0,
    kPlayerModeCoop = 1 << 1,
    kPlayerModeLan = 1 << 2,
    kPlayerModeOnline = 1 << 3,
};

struct GameMetadata {
    std::string infoHash;
    std::string titleId;
    std::string match;
    std::string name;
    std::string intro;
    std::string description;
    std::string publisher;
    std::string releaseDate;
    std::string iconUrl;
    std::string bannerUrl;
    std::vector<std::string> screenshots;
    std::vector<std::string> categories;
    // Newest published version of the game's bundled update, in the same
    // display form as the installed title's NACP display_version ("1.2.3",
    // optional leading 'v'). Carried by the metadata index (titledb-derived);
    // empty when the index does not emit it yet — the game-update check then
    // reports "source unknown" for this title.
    std::string latestVersion;
    // eShop "No. of players": how many can play on one console. 0 = unknown.
    uint8_t players = 0;
    // PlayerMode bits. `hasModes` separates "the index carries no mode record
    // for this game" from "it does, and every mode is false" — only the former
    // falls back to `players` in catalogEntryMatchesPlayerFilter.
    uint8_t modes = 0;
    bool hasModes = false;
};

struct MetadataManifest {
    uint32_t schemaVersion = 0;
    std::string generatedAt;
    std::string langegenCommit;
    std::string titledbCommit;
    std::string indexUrl;
    std::string indexSha256;
    size_t indexBytes = 0;
    size_t entryCount = 0;
};

struct MetadataSnapshot {
    MetadataManifest manifest;
    std::vector<GameMetadata> items;
    std::string manifestJson;
    std::vector<uint8_t> indexData;
};

class GameMetadataService : public IUpdateMetadataSource {
public:
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };

    using ImageData = std::shared_ptr<const DecodedImage>;
    using ImageCallback = std::function<void(ImageData)>;

    // Decode size classes. The same source URL can be held twice: once shrunk
    // for grid/rail art, once near-native for the fullscreen viewer. The
    // memory cache keys on url+class; the on-disk byte cache stays per URL, so
    // the second class costs a decode, never a download.
    static constexpr int kImageDimCard = 360;
    static constexpr int kImageDimFull = 1280;

    using MetadataFetcher = std::function<bool(
        const std::string&, size_t, std::vector<uint8_t>&, std::string&)>;

    explicit GameMetadataService(std::string rootPath,
                                 std::string bundledPath =
                                     "romfs:/catalog/"
                                     "game_metadata_index.json.zst",
                                 std::string manifestUrl =
                                     "https://github.com/i3sey/"
                                     "pipensx-metadata/releases/latest/"
                                     "download/manifest.json",
                                 MetadataFetcher metadataFetcher = {});
    ~GameMetadataService();

    GameMetadataService(const GameMetadataService&) = delete;
    GameMetadataService& operator=(const GameMetadataService&) = delete;

    bool load(std::string& error);
    bool fetchLatest(MetadataSnapshot& snapshot, std::string& error) const;
    void adopt(MetadataSnapshot snapshot);
    const GameMetadata* findByInfoHash(const std::string& infoHash) const;
    // Appends every index entry matching titleId that carries a non-empty
    // latestVersion (the same entry set collectLatestVersions folds). The
    // caller chooses among bundles; returns false when the title has none.
    bool findByTitleId(const std::string& titleId,
                       std::vector<const GameMetadata*>& out) const;
    // IUpdateMetadataSource: candidate update versions for a title id.
    bool collectLatestVersions(const std::string& titleId,
                               std::vector<std::string>& out) const override;
    bool refreshDetails(const std::string& titleId, GameMetadata& metadata,
                        std::string& error) const;
    bool loadImage(const std::string& url, std::vector<uint8_t>& bytes,
                   std::string& error) const;
    void requestImage(const std::string& url, ImageCallback callback,
                      int maxDim = kImageDimCard) const;
    // UI_PLAN F6: synchronous memory-cache probe (bumps LRU recency).
    // Non-null result = decoded RGBA ready for a same-frame texture upload.
    ImageData cachedImage(const std::string& url,
                          int maxDim = kImageDimCard) const;
    // UI_PLAN F6: warm the memory cache without a callback; no-op when the
    // URL is cached, queued, in retry backoff, or empty.
    void prefetchImage(const std::string& url,
                       int maxDim = kImageDimCard) const;
    // UI_PLAN F6: invalidate decoded covers (catalog refresh); the disk
    // cache stays — clearImageCache() removes both.
    void dropMemoryImageCache() const;
    enum class ImageNetwork {
        Full,
        // Active transfer: covers keep loading, under a per-fetch receive cap
        // so they take a slice of the link instead of racing the swarm.
        Throttled,
        // Cache-only. Nothing uncached ever reaches the network — the golden
        // runner renders placeholders instead of fixture URLs.
        Off,
    };
    void setImageNetwork(ImageNetwork mode) const;
    bool clearImageCache(std::string& error) const;

    size_t size() const { return byHash_.size(); }
    const MetadataManifest& manifest() const { return manifest_; }

    // PlayerMode bits present anywhere in the loaded index. The catalogue
    // builds its player-filter menu from this, so an index that predates the
    // field (or a mode nobody in it supports) simply has no menu entry.
    uint8_t availablePlayerModes() const { return availableModes_; }
    // True when the filter has anything to work with at all: either a mode
    // flag, or a couch-multiplayer player count to fall back on.
    bool hasPlayerData() const {
        return availableModes_ != 0 || localPlayerCounts_;
    }

    static bool parseIndex(const std::string& json,
                           std::vector<GameMetadata>& items,
                           std::string& error);
    static bool prepareSnapshot(const std::string& manifestJson,
                                const std::string& indexJson,
                                MetadataSnapshot& snapshot,
                                std::string& error);
    static bool isTrustedSource(const std::string& url);
    static bool isTrustedRedirect(const std::string& url);

private:
    enum class ImageLoadResult {
        Loaded,
        Failed,
    };

    struct CachedImage {
        ImageData image;
        uint64_t access = 0;
    };

    // Queued decode: the URL says what to read, maxDim which size class to
    // produce. Both are needed to write the result under the right cache key.
    struct ImageJob {
        std::string url;
        int maxDim = kImageDimCard;
    };

    void imageWorkerMain() const;
    // Refresh availableModes_/localPlayerCounts_ from byHash_. Called from
    // every place that reassigns it (load, adopt).
    void recomputePlayerSummary();
    // Rebuild byTitleId_ (titleId → latestVersion strings) and
    // byTitleIdHashes_ (titleId → info hashes) from byHash_. Called from every
    // place that reassigns byHash_ (load, adopt).
    void rebuildTitleIdIndex();
    bool loadCachedSnapshot(MetadataSnapshot& snapshot,
                            std::string& error) const;
    ImageLoadResult loadImageInternal(const std::string& url,
                                      std::vector<uint8_t>& bytes,
                                      std::string& error) const;
    void cacheImageLocked(const std::string& key,
                          ImageData image) const;

    std::string rootPath_;
    std::string cacheRoot_;
    std::string imageRoot_;
    std::string bundledPath_;
    std::string manifestUrl_;
    MetadataFetcher metadataFetcher_;
    mutable std::mutex imageMutex_;
    mutable std::condition_variable imageReady_;
    mutable std::deque<ImageJob> imageQueue_;
    mutable std::unordered_map<std::string, std::vector<ImageCallback>>
        imageRequests_;
    mutable std::unordered_map<std::string, CachedImage> imageCache_;
    mutable std::unordered_map<std::string, uint64_t> imageRetryAfter_;
    mutable std::vector<std::thread> imageWorkers_;
    mutable size_t imageCacheBytes_ = 0;
    mutable uint64_t imageAccess_ = 0;
    mutable std::atomic<ImageNetwork> imageNetwork_{ImageNetwork::Full};
    mutable std::atomic<bool> stoppingRequested_{false};
    mutable bool stoppingImages_ = false;
    std::unordered_map<std::string, GameMetadata> byHash_;
    // titleId (upper-case hex) → non-empty latestVersion of every entry that
    // carries one. Built beside byHash_ (same UI-thread-only reassignment
    // rule) and consumed by collectLatestVersions().
    std::unordered_map<std::string, std::vector<std::string>> byTitleId_;
    // titleId (upper-case hex) → info hashes of the same entry set as
    // byTitleId_. Consumed by findByTitleId() for update downloads.
    std::unordered_map<std::string, std::vector<std::string>> byTitleIdHashes_;
    MetadataManifest manifest_;
    uint8_t availableModes_ = 0;
    bool localPlayerCounts_ = false;
};

} // namespace pipensx
