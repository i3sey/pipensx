#include "game_metadata_service.hpp"

extern "C" {
#include "../core/antizapret.h"
#include "../core/sha1.h"
#include "../core/util.h"
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <dirent.h>
#include <fstream>
#include <borealis/extern/nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <borealis/extern/nanovg/stb_image.h>
#pragma GCC diagnostic pop

namespace pipensx {
namespace {

constexpr size_t kMaxIndexBytes = 24 * 1024 * 1024;
constexpr size_t kMaxDetailsBytes = 256 * 1024;
constexpr size_t kMaxImageBytes = 3 * 1024 * 1024;
constexpr size_t kMaxImageCacheBytes = 24 * 1024 * 1024;
constexpr uint64_t kImageRetryDelayMs = 30 * 1000;
constexpr size_t kImageWorkerCount = 2;

std::atomic<bool> imageProxyLogged{false};
#ifdef __SWITCH__
std::atomic<bool> imageRelayLogged{false};
#endif
std::atomic<uint32_t> imageFailureLogs{0};

struct HttpBuffer {
    std::vector<uint8_t> data;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeBytes(void* bytes, size_t size, size_t count, void* user) {
    HttpBuffer* buffer = static_cast<HttpBuffer*>(user);
    size_t total = size * count;
    if (buffer->data.size() + total > buffer->limit) {
        buffer->overflow = true;
        return 0;
    }
    const uint8_t* begin = static_cast<const uint8_t*>(bytes);
    buffer->data.insert(buffer->data.end(), begin, begin + total);
    return total;
}

bool makeDirectories(const std::string& path) {
    char buffer[1024];
    if (path.empty() || path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    // Skip a "device:" mount prefix (e.g. "sdmc:/") — mkdir on the bare mount
    // root fails with a non-EEXIST errno on libnx and would abort the walk
    // before the real subdirectories get created.
    char* start = buffer + 1;
    char* colon = std::strchr(buffer, ':');
    if (colon && colon[1] == '/')
        start = colon + 2;
    for (char* cursor = start; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *cursor = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

bool readFile(const std::string& path, std::vector<uint8_t>& bytes,
              size_t maxBytes, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Unable to open metadata file.";
        return false;
    }
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0 || size > static_cast<std::streamoff>(maxBytes)) {
        error = "Metadata file is empty or too large.";
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) {
        error = "Unable to read metadata file.";
        return false;
    }
    return true;
}

bool writeAtomic(const std::string& path, const std::vector<uint8_t>& data,
                 std::string& error) {
    std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create metadata cache.";
            return false;
        }
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write metadata cache.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        error = "Unable to replace metadata cache.";
        return false;
    }
    return true;
}

bool httpGetOnce(const std::string& url, size_t limit,
                 const antizapret_route_t* route,
                 std::vector<uint8_t>& data, std::string& error) {
    data.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    HttpBuffer buffer;
    buffer.limit = limit;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    const bool relayRequest =
        url.find("weserv.nl/") != std::string::npos ||
        url.find("i0.wp.com/") != std::string::npos ||
        url.find("duckduckgo.com/") != std::string::npos;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, relayRequest ? 20L : 25L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    if (route)
        antizapret_apply_route(curl, route);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300 ||
        buffer.overflow) {
        if (buffer.overflow)
            error = "HTTP response exceeded size limit.";
        else if (result != CURLE_OK)
            error = std::string("HTTP error: ") + curl_easy_strerror(result);
        else
            error = "HTTP status " + std::to_string(status) + ".";
        return false;
    }
    data = std::move(buffer.data);
    return true;
}

bool blockedResponse(const std::vector<uint8_t>& data) {
    return !data.empty() && antizapret_response_looks_blocked(
        reinterpret_cast<const char*>(data.data()), data.size());
}

#ifdef __SWITCH__
bool isNintendoImageUrl(const std::string& url) {
    static const std::string prefix =
        "https://img-eshop.cdn.nintendo.net/";
    return url.compare(0, prefix.size(), prefix) == 0;
}

std::string percentEncode(const std::string& value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 15]);
        }
    }
    return encoded;
}

std::string weservRelayUrl(const std::string& sourceUrl, bool secure) {
    return std::string(secure ? "https://images.weserv.nl/?url="
                              : "http://images.weserv.nl/?url=") +
           percentEncode(sourceUrl) + "&output=jpg";
}

// Jetpack Photon (Automattic infra, not Cloudflare). Wants the source host
// and path with the scheme stripped, ?ssl=1 to fetch the origin over HTTPS.
std::string photonRelayUrl(const std::string& sourceUrl) {
    static const std::string https = "https://";
    std::string rest = sourceUrl.compare(0, https.size(), https) == 0
                           ? sourceUrl.substr(https.size())
                           : sourceUrl;
    return "https://i0.wp.com/" + rest + "?ssl=1";
}

// DuckDuckGo image proxy (DDG/Bing infra, not Cloudflare).
std::string ddgRelayUrl(const std::string& sourceUrl) {
    return "https://external-content.duckduckgo.com/iu/?u=" +
           percentEncode(sourceUrl);
}

bool isRelayHost(const std::string& url) {
    return url.find("weserv.nl/") != std::string::npos ||
           url.find("i0.wp.com/") != std::string::npos ||
           url.find("duckduckgo.com/") != std::string::npos;
}
#endif

bool httpGet(const std::string& url, size_t limit, std::vector<uint8_t>& data,
             std::string& error) {
    const bool target = antizapret_is_enabled() &&
                        antizapret_is_target_url(url.c_str());
#ifdef __SWITCH__
    const bool preferProxy = target;
#else
    const bool preferProxy = target && antizapret_proxy_preferred();
#endif

    auto attempt = [&](const std::string& requestUrl,
                       const antizapret_route_t* route) {
        std::string attemptError;
        if (!httpGetOnce(requestUrl, limit, route, data, attemptError)) {
            error = std::move(attemptError);
            return false;
        }
        if (target && blockedResponse(data)) {
            data.clear();
            error = "HTTP response appears to be blocked.";
            return false;
        }
        if (route && route->type != ANTIZAPRET_ROUTE_DIRECT) {
            antizapret_note_proxy_success();
            bool expected = false;
            if (imageProxyLogged.compare_exchange_strong(expected, true))
                log_msg("[metadata] image proxy active: %s\n",
                        antizapret_route_name(route));
        }
        return true;
    };

#ifdef __SWITCH__
    if (isNintendoImageUrl(url)) {
        // img-eshop is geo-blocked for RU and the antizapret proxy only
        // un-blocks RKN-listed hosts, so every direct/proxy path to it dead-
        // ends. Skip them entirely and fan out across relays on diverse infra
        // (so one ASN slips past the censor); whichever answers first wins.
        const std::string relays[] = {
            photonRelayUrl(url),          // Automattic
            ddgRelayUrl(url),             // DuckDuckGo / Bing
            weservRelayUrl(url, false),   // Cloudflare, http
            weservRelayUrl(url, true),    // Cloudflare, https
        };
        for (const std::string& relayUrl : relays) {
            if (attempt(relayUrl, nullptr)) {
                bool expected = false;
                if (imageRelayLogged.compare_exchange_strong(expected, true))
                    log_msg("[metadata] image relay active: %s\n",
                            relayUrl.c_str());
                return true;
            }
        }
        return false;
    }
#endif

    if (!preferProxy && attempt(url, nullptr))
        return true;

    if (target) {
        antizapret_route_t routes[ANTIZAPRET_MAX_ROUTES];
        size_t routeCount = antizapret_get_routes(routes,
                                                  ANTIZAPRET_MAX_ROUTES);
        for (size_t i = 0; i < routeCount; ++i) {
            if (routes[i].type == ANTIZAPRET_ROUTE_DIRECT ||
                !antizapret_route_supported(&routes[i]))
                continue;
            if (attempt(url, &routes[i]))
                return true;
        }
    }

    if (preferProxy && attempt(url, nullptr))
        return true;
    return false;
}

std::string hex20String(const uint8_t digest[20]) {
    static const char digits[] = "0123456789abcdef";
    std::string out(40, '0');
    for (size_t i = 0; i < 20; ++i) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    return out;
}

std::string cacheNameForUrl(const std::string& url) {
    uint8_t digest[20];
    sha1(url.data(), url.size(), digest);
    return hex20String(digest) + ".img";
}

uint64_t monotonicMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                      .time_since_epoch()).count());
}

std::string stringValue(const nlohmann::json& item, const char* key) {
    if (!item.contains(key) || !item[key].is_string())
        return "";
    return item[key].get<std::string>();
}

void parseMetadataObject(const nlohmann::json& item, GameMetadata& metadata) {
    metadata.titleId = stringValue(item, "titleId");
    if (metadata.titleId.empty())
        metadata.titleId = stringValue(item, "id");
    metadata.match = stringValue(item, "match");
    metadata.name = stringValue(item, "name");
    metadata.intro = stringValue(item, "intro");
    metadata.description = stringValue(item, "description");
    metadata.publisher = stringValue(item, "publisher");
    metadata.releaseDate = stringValue(item, "releaseDate");
    metadata.iconUrl = stringValue(item, "iconUrl");
    if (metadata.iconUrl.empty())
        metadata.iconUrl = stringValue(item, "icon");
    metadata.bannerUrl = stringValue(item, "bannerUrl");
    if (metadata.bannerUrl.empty())
        metadata.bannerUrl = stringValue(item, "banner");
    metadata.screenshots.clear();
    if (item.contains("screenshots") && item["screenshots"].is_array()) {
        for (const auto& value : item["screenshots"]) {
            if (value.is_string() && metadata.screenshots.size() < 4)
                metadata.screenshots.push_back(value.get<std::string>());
        }
    } else if (item.contains("screens") && item["screens"].is_array()) {
        for (const auto& value : item["screens"]) {
            if (value.is_string() && metadata.screenshots.size() < 4)
                metadata.screenshots.push_back(value.get<std::string>());
        }
    }
    metadata.categories.clear();
    if (item.contains("categories") && item["categories"].is_array()) {
        for (const auto& value : item["categories"]) {
            if (value.is_string() && metadata.categories.size() < 6)
                metadata.categories.push_back(value.get<std::string>());
        }
    } else if (item.contains("category") && item["category"].is_array()) {
        for (const auto& value : item["category"]) {
            if (value.is_string() && metadata.categories.size() < 6)
                metadata.categories.push_back(value.get<std::string>());
        }
    }
}

} // namespace

GameMetadataService::GameMetadataService(std::string rootPath,
                                         std::string bundledPath)
    : rootPath_(std::move(rootPath)),
      cacheRoot_(rootPath_ + "/catalog/metadata"),
      imageRoot_(rootPath_ + "/catalog/images"),
      bundledPath_(std::move(bundledPath)) {
    makeDirectories(cacheRoot_);
    makeDirectories(imageRoot_);
    imageWorkers_.reserve(kImageWorkerCount);
    for (size_t i = 0; i < kImageWorkerCount; ++i)
        imageWorkers_.emplace_back(&GameMetadataService::imageWorkerMain, this);
}

GameMetadataService::~GameMetadataService() {
    std::vector<ImageCallback> cancelled;
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        stoppingImages_ = true;
        imageQueue_.clear();
        for (auto& request : imageRequests_)
            for (auto& callback : request.second)
                cancelled.push_back(std::move(callback));
        imageRequests_.clear();
    }
    imageReady_.notify_all();
    for (auto& callback : cancelled)
        callback(nullptr);
    for (std::thread& worker : imageWorkers_)
        if (worker.joinable())
            worker.join();
}

bool GameMetadataService::parseIndex(const std::string& json,
                                     std::vector<GameMetadata>& items,
                                     std::string& error) {
    items.clear();
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        error = "Metadata index is not a JSON array.";
        return false;
    }
    for (const auto& item : root) {
        if (!item.is_object())
            continue;
        GameMetadata metadata;
        metadata.infoHash = stringValue(item, "infoHash");
        if (metadata.infoHash.size() != 40)
            continue;
        std::transform(metadata.infoHash.begin(), metadata.infoHash.end(),
                       metadata.infoHash.begin(), [](unsigned char c) {
                           return static_cast<char>(std::toupper(c));
                       });
        parseMetadataObject(item, metadata);
        if (metadata.titleId.empty() || metadata.name.empty())
            continue;
        items.push_back(std::move(metadata));
    }
    if (items.empty()) {
        error = "Metadata index contains no usable entries.";
        return false;
    }
    return true;
}

bool GameMetadataService::load(std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readFile(bundledPath_, bytes, kMaxIndexBytes, error))
        return false;
    std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<GameMetadata> items;
    if (!parseIndex(json, items, error))
        return false;
    byHash_.clear();
    byHash_.reserve(items.size());
    for (GameMetadata& item : items)
        byHash_[item.infoHash] = std::move(item);
    log_msg("[metadata] loaded %zu game matches from %s\n", byHash_.size(),
            bundledPath_.c_str());
    return true;
}

const GameMetadata*
GameMetadataService::findByInfoHash(const std::string& infoHash) const {
    std::string key = infoHash;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    auto it = byHash_.find(key);
    return it == byHash_.end() ? nullptr : &it->second;
}

bool GameMetadataService::refreshDetails(const std::string& titleId,
                                         GameMetadata& metadata,
                                         std::string& error) const {
    if (titleId.empty()) {
        error = "Game metadata has no Title ID.";
        return false;
    }
    std::string cachePath = cacheRoot_ + "/" + titleId + ".json";
    std::vector<uint8_t> bytes;
    if (!readFile(cachePath, bytes, kMaxDetailsBytes, error)) {
        error.clear();
        std::string url = "https://api.nlib.cc/nx/" + titleId +
                          "?lang=ru&fields=name,description,intro,publisher,"
                          "releaseDate,icon,banner,screens";
        if (!httpGet(url, kMaxDetailsBytes, bytes, error))
            return false;
        std::string writeError;
        writeAtomic(cachePath, bytes, writeError);
    }

    nlohmann::json root = nlohmann::json::parse(bytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Nlib metadata is not a valid object.";
        return false;
    }
    GameMetadata refreshed = metadata;
    parseMetadataObject(root, refreshed);
    if (refreshed.titleId.empty())
        refreshed.titleId = titleId;
    if (refreshed.name.empty())
        refreshed.name = metadata.name;
    metadata = std::move(refreshed);
    return true;
}

bool GameMetadataService::loadImage(const std::string& url,
                                    std::vector<uint8_t>& bytes,
                                    std::string& error) const {
    return loadImageInternal(url, bytes, error) == ImageLoadResult::Loaded;
}

GameMetadataService::ImageLoadResult GameMetadataService::loadImageInternal(
    const std::string& url, std::vector<uint8_t>& bytes,
    std::string& error) const {
    bytes.clear();
    if (url.empty()) {
        error = "No image URL.";
        return ImageLoadResult::Failed;
    }

    const bool localImage = url.compare(0, 5, "sdmc:") == 0 ||
                            (!url.empty() && url[0] == '/');
    if (localImage) {
        if (!readFile(url, bytes, kMaxImageBytes, error))
            return ImageLoadResult::Failed;
        int width = 0;
        int height = 0;
        int channels = 0;
        if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                   &width, &height, &channels)) {
            error = "Local image is not valid.";
            return ImageLoadResult::Failed;
        }
        return ImageLoadResult::Loaded;
    }

    std::string path = imageRoot_ + "/" + cacheNameForUrl(url);
    if (readFile(path, bytes, kMaxImageBytes, error)) {
        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info_from_memory(bytes.data(),
                                  static_cast<int>(bytes.size()),
                                  &width, &height, &channels))
            return ImageLoadResult::Loaded;
        unlink(path.c_str());
        bytes.clear();
        log_msg("[metadata] removed invalid image cache '%s'\n",
                path.c_str());
    }
    error.clear();
    if (imageNetworkPaused_.load(std::memory_order_relaxed)) {
        error = "Image network deferred during active transfer.";
        return ImageLoadResult::Deferred;
    }
    if (!httpGet(url, kMaxImageBytes, bytes, error))
        return ImageLoadResult::Failed;
    if (bytes.size() < 8) {
        error = "Downloaded image is too small.";
        return ImageLoadResult::Failed;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                               &width, &height, &channels)) {
        error = "Downloaded response is not an image.";
        return ImageLoadResult::Failed;
    }
    std::string writeError;
    if (!writeAtomic(path, bytes, writeError)) {
        static std::atomic<bool> cacheWriteLogged{false};
        bool expected = false;
        if (cacheWriteLogged.compare_exchange_strong(expected, true))
            log_msg("[metadata] image cache write failed '%s': %s\n",
                    path.c_str(), writeError.c_str());
    }
    return ImageLoadResult::Loaded;
}

bool GameMetadataService::clearImageCache(std::string& error) const {
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        imageCache_.clear();
        imageCacheBytes_ = 0;
        imageRetryAfter_.clear();
    }
    DIR* directory = opendir(imageRoot_.c_str());
    if (!directory) {
        if (errno == ENOENT) {
            error.clear();
            return true;
        }
        error = std::string("Unable to open artwork cache: ") +
                std::strerror(errno);
        return false;
    }
    bool ok = true;
    int failure = 0;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.')
            continue;
        std::string path = imageRoot_ + "/" + entry->d_name;
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            ok = false;
            failure = errno;
        }
    }
    closedir(directory);
    if (!ok) {
        error = std::string("Unable to clear artwork cache: ") +
                std::strerror(failure);
        return false;
    }
    error.clear();
    return true;
}

void GameMetadataService::requestImage(const std::string& url,
                                       ImageCallback callback) const {
    if (!callback)
        return;
    if (url.empty()) {
        callback(nullptr);
        return;
    }

    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        const uint64_t now = monotonicMilliseconds();
        auto retry = imageRetryAfter_.find(url);
        if (stoppingImages_ ||
            (retry != imageRetryAfter_.end() && retry->second > now)) {
            rejected = true;
        } else {
            if (retry != imageRetryAfter_.end())
                imageRetryAfter_.erase(retry);
            auto request = imageRequests_.find(url);
            if (request != imageRequests_.end()) {
                request->second.push_back(std::move(callback));
                return;
            }
            imageRequests_[url].push_back(std::move(callback));
            imageQueue_.push_back(url);
        }
    }
    if (rejected) {
        callback(nullptr);
        return;
    }
    imageReady_.notify_one();
}

void GameMetadataService::setImageNetworkPaused(bool paused) const {
    bool previous = imageNetworkPaused_.exchange(
        paused, std::memory_order_relaxed);
    if (previous && !paused)
        imageReady_.notify_all();
}

void GameMetadataService::cacheImageLocked(
    const std::string& url, ImageData image) const {
    if (!image)
        return;
    auto existing = imageCache_.find(url);
    if (existing != imageCache_.end()) {
        imageCacheBytes_ -= existing->second.image->pixels.size();
        imageCache_.erase(existing);
    }
    const size_t bytes = image->pixels.size();
    if (bytes > kMaxImageCacheBytes)
        return;

    while (imageCacheBytes_ + bytes > kMaxImageCacheBytes &&
           !imageCache_.empty()) {
        auto oldest = std::min_element(
            imageCache_.begin(), imageCache_.end(),
            [](const auto& left, const auto& right) {
                return left.second.access < right.second.access;
            });
        imageCacheBytes_ -= oldest->second.image->pixels.size();
        imageCache_.erase(oldest);
    }
    CachedImage cached;
    cached.image = std::move(image);
    cached.access = ++imageAccess_;
    imageCacheBytes_ += bytes;
    imageCache_[url] = std::move(cached);
}

void GameMetadataService::imageWorkerMain() const {
    while (true) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(imageMutex_);
            imageReady_.wait(lock, [this] {
                return stoppingImages_ || !imageQueue_.empty();
            });
            if (stoppingImages_)
                return;
            url = std::move(imageQueue_.front());
            imageQueue_.pop_front();
        }
        const uint64_t startedMs = monotonicMilliseconds();

        ImageData result;
        bool memoryCacheHit = false;
        {
            std::lock_guard<std::mutex> lock(imageMutex_);
            auto cached = imageCache_.find(url);
            if (cached != imageCache_.end()) {
                cached->second.access = ++imageAccess_;
                result = cached->second.image;
                memoryCacheHit = true;
            }
        }

        std::string error;
        bool deferred = false;
        if (!result) {
            std::vector<uint8_t> bytes;
            ImageLoadResult loadResult = loadImageInternal(url, bytes, error);
            deferred = loadResult == ImageLoadResult::Deferred;
            if (loadResult == ImageLoadResult::Loaded) {
                int width = 0;
                int height = 0;
                int channels = 0;
                stbi_uc* pixels = stbi_load_from_memory(
                    bytes.data(), static_cast<int>(bytes.size()),
                    &width, &height, &channels, 4);
                const uint64_t decodedBytes = width > 0 && height > 0
                    ? static_cast<uint64_t>(width) * height * 4 : 0;
                if (pixels && width <= 4096 && height <= 4096 &&
                    decodedBytes <= 64 * 1024 * 1024) {
                    auto decoded = std::make_shared<DecodedImage>();
                    decoded->width = width;
                    decoded->height = height;
                    decoded->pixels.assign(pixels, pixels + decodedBytes);
                    result = std::move(decoded);
                } else {
                    error = "Unable to decode cached image.";
                }
                if (pixels)
                    stbi_image_free(pixels);
            }
        }
        if (deferred) {
            telemetry_log("image", "-",
                          "event=deferred reason=active_transfer");
            std::unique_lock<std::mutex> lock(imageMutex_);
            imageQueue_.push_back(std::move(url));
            imageReady_.wait(lock, [this] {
                return stoppingImages_ ||
                       !imageNetworkPaused_.load(std::memory_order_relaxed);
            });
            if (stoppingImages_)
                return;
            continue;
        }
        bool loaded = static_cast<bool>(result);
        if (!loaded) {
            uint32_t logIndex = imageFailureLogs.fetch_add(1);
            if (logIndex < 8) {
                diagnostic_error("image", "load", "error=%s",
                                 error.empty() ? "unknown" : error.c_str());
            }
        }
        telemetry_log("image", "-",
                      "event=load cache=%s ok=%d duration_ms=%llu bytes=%zu",
                      memoryCacheHit ? "memory" : "source", loaded ? 1 : 0,
                      (unsigned long long)(monotonicMilliseconds() - startedMs),
                      result ? result->pixels.size() : 0);
        std::vector<ImageCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(imageMutex_);
            if (loaded && imageCache_.find(url) == imageCache_.end())
                cacheImageLocked(url, result);
            auto request = imageRequests_.find(url);
            if (request != imageRequests_.end()) {
                callbacks = std::move(request->second);
                imageRequests_.erase(request);
            }
            if (!loaded)
                imageRetryAfter_[url] = monotonicMilliseconds() +
                                        kImageRetryDelayMs;
        }
        for (auto& callback : callbacks)
            callback(result);
    }
}

} // namespace pipensx
