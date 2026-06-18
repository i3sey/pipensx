#include "game_metadata_service.hpp"

extern "C" {
#include "../core/sha1.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <borealis/extern/nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr size_t kMaxIndexBytes = 24 * 1024 * 1024;
constexpr size_t kMaxDetailsBytes = 256 * 1024;
constexpr size_t kMaxImageBytes = 3 * 1024 * 1024;

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
    for (char* cursor = buffer + 1; *cursor; ++cursor) {
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

bool httpGet(const std::string& url, size_t limit, std::vector<uint8_t>& data,
             std::string& error) {
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
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
    bytes.clear();
    if (url.empty()) {
        error = "No image URL.";
        return false;
    }
    std::string path = imageRoot_ + "/" + cacheNameForUrl(url);
    if (readFile(path, bytes, kMaxImageBytes, error))
        return true;
    error.clear();
    if (!httpGet(url, kMaxImageBytes, bytes, error))
        return false;
    if (bytes.size() < 8) {
        error = "Downloaded image is too small.";
        return false;
    }
    std::string writeError;
    writeAtomic(path, bytes, writeError);
    return true;
}

} // namespace pipensx
