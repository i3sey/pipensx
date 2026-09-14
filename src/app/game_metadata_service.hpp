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
    // Newest published version of the game's bundled update as a decimal
    // title version ("131072") — the same unit carried by the [vN] tags in
    // release file names and by the installed Patch content meta; the update
    // check folds candidates numerically. Carried by the metadata index
    // (titledb-derived); empty when the index does not emit it yet — the
    // game-update check then reports "source unknown" for this title.
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

struct GameMetadataIndexSnapshot;

struct MetadataSnapshot {
    MetadataManifest manifest;
    // Input for manually assembled/startup snapshots. prepareSnapshot() moves
    // these rows into preparedIndex so a fetched snapshot has one owner.
    std::vector<GameMetadata> items;
    std::string manifestJson;
    std::vector<uint8_t> indexData;
    // Filled by prepareSnapshot()/fetchLatest() on their caller's worker.
    // adopt() only stamps the generation and swaps this ready index.
    std::shared_ptr<GameMetadataIndexSnapshot> preparedIndex;
};

// Immutable, shareable view of the joined metadata index. A catalogue
// browse worker keeps this object alive while GameMetadataService::adopt()
// publishes a newer generation on the UI thread.
struct GameMetadataIndexSnapshot {
    std::vector<GameMetadata> items;
    std::unordered_map<std::string, size_t> byInfoHash;
    uint64_t generation = 0;
    uint8_t availablePlayerModes = 0;
    bool hasLocalPlayerCounts = false;
    std::unordered_map<std::string, std::vector<std::string>> byTitleId;
    std::unordered_map<std::string, std::vector<size_t>> byTitleIdItems;

    const GameMetadata* findByInfoHash(const std::string& infoHash,
                                       const std::string& titleId = {}) const;
    bool hasPlayerData() const {
        return availablePlayerModes != 0 || hasLocalPlayerCounts;
    }
};

struct RetiredMetadataSnapshot {
    std::shared_ptr<const GameMetadataIndexSnapshot> index;
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
    using ImageRequestCurrent = std::function<bool()>;

    enum class ImagePriority : uint8_t {
        Prefetch,
        Visible,
        Current,
    };

    // Decode size classes. The memory cache keys on url+class; the on-disk
    // byte cache stays per URL, so a second class costs a decode, never a
    // download. Icon matches the 72px download-list slot. Grid matches the
    // 180px catalog slot (360px GPU uploads blow Horizon's ~4 MB mapping
    // slack). Card is detail/hero/list art. Full is the screenshot viewer.
    static constexpr int kImageDimIcon = 72;
    static constexpr int kImageDimGrid = 180;
    static constexpr int kImageDimCard = 360;
    static constexpr int kImageDimFull = 1280;

    using MetadataFetcher = std::function<bool(
        const std::string&, size_t, std::vector<uint8_t>&, std::string&,
        const std::atomic<bool>*)>;
    using ImageFetcher = MetadataFetcher;

    explicit GameMetadataService(std::string rootPath,
                                 std::string bundledPath =
                                     "romfs:/catalog/"
                                     "game_metadata_index.json.zst",
                                 std::string manifestUrl =
                                     "https://github.com/i3sey/"
                                     "pipensx-metadata/releases/latest/"
                                     "download/manifest.json",
                                 MetadataFetcher metadataFetcher = {},
                                 ImageFetcher imageFetcher = {});
    ~GameMetadataService();

    GameMetadataService(const GameMetadataService&) = delete;
    GameMetadataService& operator=(const GameMetadataService&) = delete;

    bool load(std::string& error);
    bool fetchLatest(MetadataSnapshot& snapshot, std::string& error,
                     const std::atomic<bool>* cancelled = nullptr) const;
    RetiredMetadataSnapshot adopt(MetadataSnapshot snapshot);
    const GameMetadata* findByInfoHash(
        const std::string& infoHash, const std::string& titleId = {}) const;
    // Appends every index entry matching titleId that carries a non-empty
    // latestVersion (the same entry set collectLatestVersions folds). The
    // caller chooses among bundles; returns false when the title has none.
    bool findByTitleId(const std::string& titleId,
                       std::vector<const GameMetadata*>& out) const;
    // B3: among same-title bundles, find the one carrying the update
    // check's found version. The Updates hub used the newest-published
    // bundle even when it was the base game, so "install update"
    // downloaded the whole game. Returns the first numeric match (entries
    // from findByTitleId arrive newest-version-first); nullptr when the
    // wanted version is empty/unparseable or no bundle carries it — the
    // caller keeps its legacy pick then.
    static const GameMetadata* preferVersionMatch(
        const std::vector<const GameMetadata*>& entries,
        const std::string& foundVersion);
    // IUpdateMetadataSource: candidate update versions for a title id.
    bool collectLatestVersions(const std::string& titleId,
                               std::vector<std::string>& out) const override;
    bool refreshDetails(const std::string& titleId, GameMetadata& metadata,
                        std::string& error) const;
    bool loadImage(const std::string& url, std::vector<uint8_t>& bytes,
                   std::string& error) const;
    // Jobs are scheduled Current -> Visible -> Prefetch. `current` lets the
    // service discard a recycled UI cell's superseded request before doing
    // I/O; callbacks are still completed with nullptr when discarded.
    void requestImage(const std::string& url, ImageCallback callback,
                      int maxDim = kImageDimCard,
                      ImagePriority priority = ImagePriority::Visible,
                      ImageRequestCurrent current = {}) const;
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
        // Cache-only. Nothing uncached ever reaches the network on PC.
        Off,
    };
    void setImageNetwork(ImageNetwork mode) const;
    bool clearImageCache(std::string& error) const;

    size_t size() const { return index_->byInfoHash.size(); }
    // Bumps on every adopt() (including the startup cache load). Catalog UI
    // polls this to pick up a refresh that finished after the view was built.
    uint64_t generation() const { return generation_; }
    std::shared_ptr<const GameMetadataIndexSnapshot> sharedIndex() const {
        return index_;
    }
    const MetadataManifest& manifest() const { return manifest_; }

    // PlayerMode bits present anywhere in the loaded index. The catalogue
    // builds its player-filter menu from this, so an index that predates the
    // field (or a mode nobody in it supports) simply has no menu entry.
    uint8_t availablePlayerModes() const {
        return index_->availablePlayerModes;
    }
    // True when the filter has anything to work with at all: either a mode
    // flag, or a couch-multiplayer player count to fall back on.
    bool hasPlayerData() const {
        return index_->hasPlayerData();
    }

    static bool parseIndex(const std::string& json,
                           std::vector<GameMetadata>& items,
                           std::string& error,
                           const std::atomic<bool>* cancelled = nullptr);
    static bool prepareSnapshot(const std::string& manifestJson,
                                const std::string& indexJson,
                                MetadataSnapshot& snapshot,
                                std::string& error,
                                const std::atomic<bool>* cancelled = nullptr);
    static bool isTrustedSource(const std::string& url);
    static bool isTrustedRedirect(const std::string& url);

private:
    enum class ImageLoadResult {
        Loaded,
        NeedsNetwork,
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
        uint64_t id = 0;
        uint64_t startedMs = 0;
        std::vector<uint8_t> bytes;
        std::string error;
        bool downloaded = false;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    struct ImageCallbackRegistration {
        ImageCallback callback;
        ImageRequestCurrent current;
    };

    struct ImageRequest {
        std::vector<ImageCallbackRegistration> callbacks;
        ImagePriority priority = ImagePriority::Prefetch;
        uint64_t id = 0;
        bool prefetch = false;
        bool inFlight = false;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    void imageLocalWorkerMain() const;
    void imageNetworkWorkerMain() const;
    static std::shared_ptr<GameMetadataIndexSnapshot> buildIndex(
        std::vector<GameMetadata> items);
    bool loadCachedSnapshot(MetadataSnapshot& snapshot,
                            std::string& error,
                            const std::atomic<bool>* cancelled = nullptr) const;
    ImageLoadResult loadImageInternal(const std::string& url,
                                      std::vector<uint8_t>& bytes,
                                      std::string& error) const;
    ImageLoadResult probeImageSource(const std::string& url,
                                     std::vector<uint8_t>& bytes,
                                     std::string& error) const;
    bool fetchImageNetwork(const std::string& url,
                           std::vector<uint8_t>& bytes,
                           std::string& error,
                           const std::atomic<bool>* cancelled = nullptr) const;
    bool popImageJobLocked(std::deque<ImageJob>& queue,
                           ImageJob& job) const;
    void eraseImageJobsLocked(const std::string& key, uint64_t id) const;
    void finishImageJob(const ImageJob& job, ImageData result,
                        const std::string& error, bool memoryCacheHit) const;
    void pruneImageQueueLocked(
        std::vector<ImageCallback>& rejected) const;
    void cacheImageLocked(const std::string& key,
                          ImageData image) const;

    std::string rootPath_;
    std::string cacheRoot_;
    std::string imageRoot_;
    std::string bundledPath_;
    std::string manifestUrl_;
    MetadataFetcher metadataFetcher_;
    ImageFetcher imageFetcher_;
    mutable std::mutex imageMutex_;
    mutable std::condition_variable imageReady_;
    mutable std::deque<ImageJob> imageLocalQueue_;
    mutable std::deque<ImageJob> imageNetworkQueue_;
    mutable std::unordered_map<std::string, ImageRequest>
        imageRequests_;
    mutable std::unordered_map<std::string, CachedImage> imageCache_;
    // Old decoded caches are moved here in O(1) and destroyed by an image
    // worker, never by the UI thread that publishes a metadata refresh.
    mutable std::deque<std::unordered_map<std::string, CachedImage>>
        retiredImageCaches_;
    mutable std::unordered_map<std::string, uint64_t> imageRetryAfter_;
    mutable std::thread imageLocalWorker_;
    mutable std::vector<std::thread> imageNetworkWorkers_;
    mutable size_t imageCacheBytes_ = 0;
    mutable uint64_t imageAccess_ = 0;
    mutable uint64_t imageRequestId_ = 0;
    mutable std::atomic<ImageNetwork> imageNetwork_{ImageNetwork::Full};
    mutable std::atomic<bool> stoppingRequested_{false};
    mutable bool stoppingImages_ = false;
    std::shared_ptr<const GameMetadataIndexSnapshot> index_ =
        std::make_shared<const GameMetadataIndexSnapshot>();
    MetadataManifest manifest_;
    uint64_t generation_ = 0;
};

} // namespace pipensx
