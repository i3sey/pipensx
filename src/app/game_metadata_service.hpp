#pragma once

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
};

class GameMetadataService {
public:
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };

    using ImageData = std::shared_ptr<const DecodedImage>;
    using ImageCallback = std::function<void(ImageData)>;

    explicit GameMetadataService(std::string rootPath,
                                 std::string bundledPath =
                                     "romfs:/catalog/game_metadata_index.json");
    ~GameMetadataService();

    GameMetadataService(const GameMetadataService&) = delete;
    GameMetadataService& operator=(const GameMetadataService&) = delete;

    bool load(std::string& error);
    const GameMetadata* findByInfoHash(const std::string& infoHash) const;
    bool refreshDetails(const std::string& titleId, GameMetadata& metadata,
                        std::string& error) const;
    bool loadImage(const std::string& url, std::vector<uint8_t>& bytes,
                   std::string& error) const;
    void requestImage(const std::string& url, ImageCallback callback) const;
    // UI_PLAN F6: synchronous memory-cache probe (bumps LRU recency).
    // Non-null result = decoded RGBA ready for a same-frame texture upload.
    ImageData cachedImage(const std::string& url) const;
    // UI_PLAN F6: warm the memory cache without a callback; no-op when the
    // URL is cached, queued, in retry backoff, or empty.
    void prefetchImage(const std::string& url) const;
    // UI_PLAN F6: invalidate decoded covers (catalog refresh); the disk
    // cache stays — clearImageCache() removes both.
    void dropMemoryImageCache() const;
    void setImageNetworkPaused(bool paused) const;
    bool clearImageCache(std::string& error) const;

    size_t size() const { return byHash_.size(); }

    static bool parseIndex(const std::string& json,
                           std::vector<GameMetadata>& items,
                           std::string& error);

private:
    enum class ImageLoadResult {
        Loaded,
        Deferred,
        Failed,
    };

    struct CachedImage {
        ImageData image;
        uint64_t access = 0;
    };

    void imageWorkerMain() const;
    ImageLoadResult loadImageInternal(const std::string& url,
                                      std::vector<uint8_t>& bytes,
                                      std::string& error) const;
    void cacheImageLocked(const std::string& url,
                          ImageData image) const;

    std::string rootPath_;
    std::string cacheRoot_;
    std::string imageRoot_;
    std::string bundledPath_;
    mutable std::mutex imageMutex_;
    mutable std::condition_variable imageReady_;
    mutable std::deque<std::string> imageQueue_;
    mutable std::unordered_map<std::string, std::vector<ImageCallback>>
        imageRequests_;
    mutable std::unordered_map<std::string, CachedImage> imageCache_;
    mutable std::unordered_map<std::string, uint64_t> imageRetryAfter_;
    mutable std::vector<std::thread> imageWorkers_;
    mutable size_t imageCacheBytes_ = 0;
    mutable uint64_t imageAccess_ = 0;
    mutable std::atomic<bool> imageNetworkPaused_{false};
    mutable bool stoppingImages_ = false;
    std::unordered_map<std::string, GameMetadata> byHash_;
};

} // namespace pipensx
