#include "game_metadata_service.hpp"

extern "C" {
#include "../core/sha1.h"
#include "../core/sha256.h"
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
constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr size_t kMaxDetailsBytes = 256 * 1024;
constexpr size_t kMaxImageBytes = 3 * 1024 * 1024;
// UI_PLAN F6: decoded-RGBA budget for instant catalog re-entry. 96 MB sits
// mid-range of the 64-128 MB plan window; the LRU sweep in cacheImageLocked
// keeps the worst case bounded while scrolling the full catalog.
constexpr size_t kMaxImageCacheBytes = 96 * 1024 * 1024;
constexpr uint64_t kImageRetryDelayMs = 30 * 1000;
constexpr size_t kImageWorkerCount = 2;
constexpr const char* kDefaultMetadataIndexUrl =
    "https://github.com/i3sey/pipensx-metadata/releases/latest/download/"
    "game_metadata_index.json";

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
                 std::vector<uint8_t>& data, std::string& error,
                 bool followRedirects = true,
                 std::string* effectiveUrl = nullptr,
                 bool verifyTls = false) {
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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    const bool relayRequest =
        url.find("weserv.nl/") != std::string::npos ||
        url.find("i0.wp.com/") != std::string::npos ||
        url.find("duckduckgo.com/") != std::string::npos;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, relayRequest ? 20L : 25L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effectiveUrl)
        *effectiveUrl = effective ? std::string(effective) : std::string();
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

#endif

bool httpGet(const std::string& url, size_t limit, std::vector<uint8_t>& data,
             std::string& error) {
    auto attempt = [&](const std::string& requestUrl) {
        std::string attemptError;
        if (!httpGetOnce(requestUrl, limit, data, attemptError)) {
            error = std::move(attemptError);
            return false;
        }
        return true;
    };

#ifdef __SWITCH__
    if (isNintendoImageUrl(url)) {
        // img-eshop is geo-blocked for RU, so a direct fetch dead-ends there.
        // Fan out across image relays on diverse infra (so one ASN slips past
        // the censor); whichever answers first wins.
        const std::string relays[] = {
            photonRelayUrl(url),          // Automattic
            ddgRelayUrl(url),             // DuckDuckGo / Bing
            weservRelayUrl(url, false),   // Cloudflare, http
            weservRelayUrl(url, true),    // Cloudflare, https
        };
        for (const std::string& relayUrl : relays) {
            if (attempt(relayUrl)) {
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

    return attempt(url);
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

std::string hex32String(const uint8_t digest[32]) {
    static const char digits[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
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

// Covers render at 180px. Cache decoded source art near 2x display size so
// the working set survives shelf scrolling instead of thrashing the 96 MB LRU.
constexpr int kMaxCoverDim = 360;

void downscaleRgba(std::vector<uint8_t>& pixels, int& width, int& height) {
    const int longEdge = std::max(width, height);
    if (longEdge <= kMaxCoverDim || width <= 0 || height <= 0)
        return;
    const int factor = (longEdge + kMaxCoverDim - 1) / kMaxCoverDim;
    const int dw = width / factor;
    const int dh = height / factor;
    if (dw <= 0 || dh <= 0)
        return;
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    const uint32_t area = static_cast<uint32_t>(factor) * factor;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (int fy = 0; fy < factor; ++fy) {
                const uint8_t* row =
                    pixels.data() +
                    (static_cast<size_t>(y * factor + fy) * width +
                     static_cast<size_t>(x) * factor) * 4;
                for (int fx = 0; fx < factor; ++fx) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    a += row[3];
                    row += 4;
                }
            }
            uint8_t* dst = out.data() + (static_cast<size_t>(y) * dw + x) * 4;
            dst[0] = static_cast<uint8_t>(r / area);
            dst[1] = static_cast<uint8_t>(g / area);
            dst[2] = static_cast<uint8_t>(b / area);
            dst[3] = static_cast<uint8_t>(a / area);
        }
    }
    pixels = std::move(out);
    width = dw;
    height = dh;
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

std::string manifestIndexSha(const std::string& json) {
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        !root.contains("index") || !root["index"].is_object() ||
        !root["index"].contains("sha256") ||
        !root["index"]["sha256"].is_string())
        return {};
    std::string sha = root["index"]["sha256"].get<std::string>();
    std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (sha.size() != 64 ||
        !std::all_of(sha.begin(), sha.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        }))
        return {};
    return sha;
}

bool fetchTrustedMetadata(const std::string& url, size_t limit,
                          std::vector<uint8_t>& data, std::string& error) {
    std::string effectiveUrl;
    if (!httpGetOnce(url, limit, data, error, true, &effectiveUrl, true))
        return false;
    if (!GameMetadataService::isTrustedRedirect(effectiveUrl)) {
        data.clear();
        error = "Metadata download redirected to an untrusted host.";
        return false;
    }
    return true;
}

void pruneIndexCache(const std::string& root, const std::string& keepSha) {
    DIR* directory = opendir(root.c_str());
    if (!directory)
        return;
    while (dirent* entry = readdir(directory)) {
        std::string name = entry->d_name;
        if (name.size() != 69 || name.substr(64) != ".json" ||
            name.compare(0, 64, keepSha) == 0)
            continue;
        const bool isHash = std::all_of(
            name.begin(), name.begin() + 64, [](unsigned char c) {
                return std::isxdigit(c) != 0;
            });
        if (isHash)
            unlink((root + "/" + name).c_str());
    }
    closedir(directory);
}

} // namespace

GameMetadataService::GameMetadataService(std::string rootPath,
                                         std::string bundledPath,
                                         std::string manifestUrl,
                                         MetadataFetcher metadataFetcher)
    : rootPath_(std::move(rootPath)),
      cacheRoot_(rootPath_ + "/catalog/metadata"),
      imageRoot_(rootPath_ + "/catalog/images"),
      bundledPath_(std::move(bundledPath)),
      manifestUrl_(std::move(manifestUrl)),
      metadataFetcher_(std::move(metadataFetcher)) {
    if (!metadataFetcher_) {
        metadataFetcher_ = [](const std::string& url, size_t limit,
                              std::vector<uint8_t>& data,
                              std::string& error) {
            return fetchTrustedMetadata(url, limit, data, error);
        };
    }
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

bool GameMetadataService::isTrustedSource(const std::string& url) {
    static const char* const prefixes[] = {
        "https://raw.githubusercontent.com/i3sey/pipensx-metadata/",
        "https://github.com/i3sey/pipensx-metadata/releases/",
    };
    for (const char* prefix : prefixes)
        if (url.rfind(prefix, 0) == 0)
            return true;
    return false;
}

bool GameMetadataService::isTrustedRedirect(const std::string& url) {
    static const char* const prefixes[] = {
        "https://raw.githubusercontent.com/i3sey/pipensx-metadata/",
        "https://github.com/i3sey/pipensx-metadata/releases/",
        "https://release-assets.githubusercontent.com/",
        "https://objects.githubusercontent.com/",
    };
    for (const char* prefix : prefixes)
        if (url.rfind(prefix, 0) == 0)
            return true;
    return false;
}

bool GameMetadataService::prepareSnapshot(const std::string& manifestJson,
                                          const std::string& indexJson,
                                          MetadataSnapshot& snapshot,
                                          std::string& error) {
    snapshot = {};
    if (manifestJson.empty() || manifestJson.size() > kMaxManifestBytes) {
        error = "Metadata manifest is empty or too large.";
        return false;
    }
    nlohmann::json root =
        nlohmann::json::parse(manifestJson, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        !root.contains("schemaVersion") ||
        !root["schemaVersion"].is_number_unsigned() ||
        root["schemaVersion"].get<uint32_t>() != 1 ||
        !root.contains("index") || !root["index"].is_object()) {
        error = "Metadata manifest has an unsupported schema.";
        return false;
    }
    const nlohmann::json& index = root["index"];
    if (!index.contains("sha256") || !index["sha256"].is_string() ||
        !index.contains("bytes") || !index["bytes"].is_number_unsigned() ||
        !index.contains("entries") ||
        !index["entries"].is_number_unsigned()) {
        error = "Metadata manifest index fields are invalid.";
        return false;
    }

    MetadataManifest manifest;
    manifest.schemaVersion = 1;
    manifest.generatedAt = stringValue(root, "generatedAt");
    manifest.langegenCommit = stringValue(root, "langegenCommit");
    manifest.titledbCommit = stringValue(root, "titledbCommit");
    manifest.indexUrl = index.contains("url") && index["url"].is_string()
        ? index["url"].get<std::string>()
        : kDefaultMetadataIndexUrl;
    manifest.indexSha256 = index["sha256"].get<std::string>();
    manifest.indexBytes = index["bytes"].get<size_t>();
    manifest.entryCount = index["entries"].get<size_t>();
    std::transform(manifest.indexSha256.begin(), manifest.indexSha256.end(),
                   manifest.indexSha256.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (manifest.generatedAt.empty() || manifest.langegenCommit.empty() ||
        manifest.titledbCommit.empty() ||
        !isTrustedSource(manifest.indexUrl) ||
        manifest.indexBytes == 0 || manifest.indexBytes > kMaxIndexBytes ||
        manifest.indexBytes != indexJson.size() ||
        manifest.indexSha256.size() != 64 || manifest.entryCount == 0) {
        error = "Metadata manifest does not match the index.";
        return false;
    }
    if (!std::all_of(manifest.indexSha256.begin(),
                     manifest.indexSha256.end(), [](unsigned char c) {
                         return std::isxdigit(c) != 0;
                     })) {
        error = "Metadata index SHA-256 is invalid.";
        return false;
    }
    uint8_t digest[32];
    sha256(indexJson.data(), indexJson.size(), digest);
    if (hex32String(digest) != manifest.indexSha256) {
        error = "Metadata index SHA-256 does not match.";
        return false;
    }
    std::vector<GameMetadata> items;
    if (!parseIndex(indexJson, items, error))
        return false;
    if (items.size() != manifest.entryCount) {
        error = "Metadata manifest entry count does not match.";
        return false;
    }
    snapshot.manifest = std::move(manifest);
    snapshot.items = std::move(items);
    snapshot.manifestJson = manifestJson;
    snapshot.indexData.assign(indexJson.begin(), indexJson.end());
    error.clear();
    return true;
}

bool GameMetadataService::loadCachedSnapshot(MetadataSnapshot& snapshot,
                                             std::string& error) const {
    const std::string manifestPath = cacheRoot_ + "/manifest.json";
    std::vector<uint8_t> manifestBytes;
    if (!readFile(manifestPath, manifestBytes, kMaxManifestBytes, error))
        return false;
    const std::string manifestJson(
        reinterpret_cast<const char*>(manifestBytes.data()),
        manifestBytes.size());
    const std::string sha = manifestIndexSha(manifestJson);
    if (sha.empty()) {
        error = "Cached metadata manifest is invalid.";
        return false;
    }
    std::vector<uint8_t> indexBytes;
    if (!readFile(cacheRoot_ + "/" + sha + ".json", indexBytes,
                  kMaxIndexBytes, error))
        return false;
    const std::string indexJson(
        reinterpret_cast<const char*>(indexBytes.data()), indexBytes.size());
    return prepareSnapshot(manifestJson, indexJson, snapshot, error);
}

bool GameMetadataService::load(std::string& error) {
    byHash_.clear();
    manifest_ = {};

    std::string cacheError;
    MetadataSnapshot cached;
    if (loadCachedSnapshot(cached, cacheError)) {
        adopt(std::move(cached));
        error.clear();
        log_msg("[metadata] loaded %zu cached game matches\n",
                byHash_.size());
        return true;
    }
    if (!cacheError.empty())
        log_msg("[metadata] cached index ignored: %s\n", cacheError.c_str());

    if (bundledPath_.empty() || access(bundledPath_.c_str(), R_OK) != 0) {
        // Public builds intentionally omit the generated metadata index. Live
        // catalog entries carry the fields needed by the detail view.
        error.clear();
        log_msg("[metadata] optional index is not embedded\n");
        return true;
    }

    std::vector<uint8_t> bytes;
    if (!readFile(bundledPath_, bytes, kMaxIndexBytes, error))
        return false;
    std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<GameMetadata> items;
    if (!parseIndex(json, items, error))
        return false;
    byHash_.reserve(items.size());
    for (GameMetadata& item : items)
        byHash_[item.infoHash] = std::move(item);
    log_msg("[metadata] loaded %zu game matches from %s\n", byHash_.size(),
            bundledPath_.c_str());
    return true;
}

void GameMetadataService::adopt(MetadataSnapshot snapshot) {
    std::unordered_map<std::string, GameMetadata> next;
    next.reserve(snapshot.items.size());
    for (GameMetadata& item : snapshot.items)
        next[item.infoHash] = std::move(item);
    byHash_ = std::move(next);
    manifest_ = std::move(snapshot.manifest);
}

bool GameMetadataService::fetchLatest(MetadataSnapshot& snapshot,
                                      std::string& error) const {
    snapshot = {};
    auto useCachedAfterFailure = [&](const std::string& refreshError) {
        std::string cacheError;
        MetadataSnapshot cached;
        if (loadCachedSnapshot(cached, cacheError)) {
            snapshot = std::move(cached);
            error.clear();
            log_msg("[metadata] refresh failed (%s); using cached index\n",
                    refreshError.c_str());
            return true;
        }
        error = refreshError;
        if (!cacheError.empty())
            error += " Cached metadata: " + cacheError;
        return false;
    };
    if (!isTrustedSource(manifestUrl_)) {
        error = "Metadata manifest URL is not trusted.";
        return false;
    }
    std::vector<uint8_t> manifestBytes;
    if (!metadataFetcher_(manifestUrl_, kMaxManifestBytes, manifestBytes,
                          error))
        return useCachedAfterFailure(error);
    const std::string manifestJson(
        reinterpret_cast<const char*>(manifestBytes.data()),
        manifestBytes.size());
    nlohmann::json root =
        nlohmann::json::parse(manifestJson, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        !root.contains("index") || !root["index"].is_object() ||
        !root["index"].contains("bytes") ||
        !root["index"]["bytes"].is_number_unsigned()) {
        return useCachedAfterFailure(
            "Metadata manifest index fields are invalid.");
    }
    const nlohmann::json& index = root["index"];
    const std::string indexUrl =
        index.contains("url") && index["url"].is_string()
            ? index["url"].get<std::string>()
            : kDefaultMetadataIndexUrl;
    const size_t expectedBytes = index["bytes"].get<size_t>();
    if (!isTrustedSource(indexUrl) || expectedBytes == 0 ||
        expectedBytes > kMaxIndexBytes) {
        return useCachedAfterFailure(
            "Metadata index URL or size is not trusted.");
    }
    std::vector<uint8_t> indexBytes;
    if (!metadataFetcher_(indexUrl, kMaxIndexBytes, indexBytes, error))
        return useCachedAfterFailure(error);
    const std::string indexJson(
        reinterpret_cast<const char*>(indexBytes.data()), indexBytes.size());
    if (!prepareSnapshot(manifestJson, indexJson, snapshot, error))
        return useCachedAfterFailure(error);

    const std::string indexPath =
        cacheRoot_ + "/" + snapshot.manifest.indexSha256 + ".json";
    if (!writeAtomic(indexPath, snapshot.indexData, error))
        return useCachedAfterFailure(error);
    if (!writeAtomic(cacheRoot_ + "/manifest.json", manifestBytes, error))
        return useCachedAfterFailure(error);
    pruneIndexCache(cacheRoot_, snapshot.manifest.indexSha256);
    log_msg("[metadata] cached %zu game matches from %s\n",
            snapshot.items.size(), snapshot.manifest.generatedAt.c_str());
    error.clear();
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

GameMetadataService::ImageData
GameMetadataService::cachedImage(const std::string& url) const {
    if (url.empty())
        return nullptr;
    std::lock_guard<std::mutex> lock(imageMutex_);
    auto cached = imageCache_.find(url);
    if (cached == imageCache_.end())
        return nullptr;
    cached->second.access = ++imageAccess_;
    return cached->second.image;
}

void GameMetadataService::prefetchImage(const std::string& url) const {
    if (url.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        const uint64_t now = monotonicMilliseconds();
        auto retry = imageRetryAfter_.find(url);
        if (stoppingImages_ ||
            (retry != imageRetryAfter_.end() && retry->second > now))
            return;
        if (imageCache_.count(url) != 0 || imageRequests_.count(url) != 0)
            return;
        // Empty callback slot: later requestImage() calls for the same URL
        // coalesce onto this in-flight decode instead of re-queueing it.
        imageRequests_[url];
        imageQueue_.push_back(url);
    }
    imageReady_.notify_one();
}

void GameMetadataService::dropMemoryImageCache() const {
    std::lock_guard<std::mutex> lock(imageMutex_);
    imageCache_.clear();
    imageCacheBytes_ = 0;
    imageRetryAfter_.clear();
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
                    std::vector<uint8_t> rgba(pixels, pixels + decodedBytes);
                    downscaleRgba(rgba, width, height);
                    auto decoded = std::make_shared<DecodedImage>();
                    decoded->width = width;
                    decoded->height = height;
                    decoded->pixels = std::move(rgba);
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
