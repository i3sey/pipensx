#include "catalog_service.hpp"
#include "magnet_resolver.hpp"

extern "C" {
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <borealis/extern/nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr size_t kMaxCatalogBytes = 16 * 1024 * 1024;
constexpr size_t kMaxCatalogEntries = 20000;
constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/bqio/switch-dumps/releases/latest";

struct HttpBuffer {
    std::string data;
    bool overflow = false;
};

size_t writeHttp(void* bytes, size_t size, size_t count, void* user) {
    HttpBuffer* buffer = static_cast<HttpBuffer*>(user);
    size_t total = size * count;
    if (buffer->data.size() + total > kMaxCatalogBytes) {
        buffer->overflow = true;
        return 0;
    }
    buffer->data.append(static_cast<const char*>(bytes), total);
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

bool readFile(const std::string& path, std::string& data,
              std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Unable to open catalog file.";
        return false;
    }
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxCatalogBytes)) {
        error = "Catalog file is empty or too large.";
        return false;
    }
    data.resize(static_cast<size_t>(size));
    input.read(data.data(), size);
    if (!input) {
        error = "Unable to read catalog file.";
        return false;
    }
    return true;
}

bool httpGet(const std::string& url, std::string& body, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    HttpBuffer buffer;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeHttp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300 ||
        buffer.overflow) {
        if (buffer.overflow)
            error = "Catalog download exceeded the size limit.";
        else if (result != CURLE_OK)
            error = std::string("Catalog network error: ") +
                    curl_easy_strerror(result);
        else
            error = "Catalog server returned HTTP " + std::to_string(status) +
                    ".";
        return false;
    }
    body = std::move(buffer.data);
    return true;
}

bool writeAtomic(const std::string& path, const std::string& data,
                 std::string& error) {
    std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create catalog cache.";
            return false;
        }
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write catalog cache.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        error = "Unable to replace catalog cache.";
        return false;
    }
    return true;
}

CatalogHealth parseHealth(const nlohmann::json& item) {
    if (!item.contains("health") || !item["health"].is_string())
        return CatalogHealth::Unknown;
    std::string value = item["health"].get<std::string>();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (value == "ok")
        return CatalogHealth::Ok;
    if (value == "no_peers")
        return CatalogHealth::NoPeers;
    if (value == "metadata_timeout")
        return CatalogHealth::MetadataTimeout;
    if (value == "tracker_not_registered")
        return CatalogHealth::TrackerNotRegistered;
    if (value == "replaced")
        return CatalogHealth::Replaced;
    if (value == "dead")
        return CatalogHealth::Dead;
    return CatalogHealth::Unknown;
}

uint64_t readUnsigned(const nlohmann::json& item, const char* key) {
    if (!item.contains(key))
        return 0;
    if (item[key].is_number_unsigned())
        return item[key].get<uint64_t>();
    if (item[key].is_number_integer()) {
        int64_t value = item[key].get<int64_t>();
        if (value > 0)
            return static_cast<uint64_t>(value);
    }
    return 0;
}

int64_t readSigned(const nlohmann::json& item, const char* key) {
    if (!item.contains(key) || !item[key].is_number_integer())
        return 0;
    return item[key].get<int64_t>();
}

} // namespace

CatalogService::CatalogService(std::string rootPath, std::string bundledPath)
    : rootPath_(std::move(rootPath)),
      catalogRoot_(rootPath_ + "/catalog"),
      cachePath_(catalogRoot_ + "/catalog.json"),
      bundledPath_(std::move(bundledPath)) {
    makeDirectories(catalogRoot_);
}

bool CatalogService::parseJson(const std::string& json,
                               std::vector<CatalogEntry>& entries,
                               std::string& error) {
    entries.clear();
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        error = "Catalog JSON is not a valid array.";
        return false;
    }
    if (root.empty() || root.size() > kMaxCatalogEntries) {
        error = "Catalog contains an invalid number of entries.";
        return false;
    }

    std::set<std::string> hashes;
    entries.reserve(root.size());
    for (const auto& item : root) {
        if (!item.is_object() ||
            !item.contains("title") || !item["title"].is_string() ||
            !item.contains("magnetURI") || !item["magnetURI"].is_string())
            continue;
        CatalogEntry entry;
        entry.title = item["title"].get<std::string>();
        entry.magnetUri = item["magnetURI"].get<std::string>();
        if (entry.title.empty() || entry.title.size() > 1024 ||
            entry.magnetUri.size() > 2048)
            continue;
        MagnetSpec magnet;
        std::string magnetError;
        if (!MagnetResolver::parse(entry.magnetUri, magnet, magnetError))
            continue;
        entry.infoHash = magnet.infoHashHex;
        entry.trackerUrl = magnet.trackerUrl;
        if (item.contains("poster") && item["poster"].is_string())
            entry.posterUrl = item["poster"].get<std::string>();
        if (entry.posterUrl.size() > 2048)
            entry.posterUrl.clear();
        entry.size = readUnsigned(item, "size");
        entry.topicId = readUnsigned(item, "topic_id");
        entry.forumId = static_cast<uint32_t>(readUnsigned(item, "forum_id"));
        entry.trackerId =
            static_cast<uint32_t>(readUnsigned(item, "tracker_id"));
        entry.peerCount =
            static_cast<uint32_t>(readUnsigned(item, "peer_count"));
        entry.publishedAt = readSigned(item, "published_date");
        entry.sourceUpdatedAt = readSigned(item, "source_updated_at");
        entry.catalogGeneratedAt = readSigned(item, "catalog_generated_at");
        entry.lastCheckedAt = readSigned(item, "last_checked_at");
        entry.health = parseHealth(item);
        if (item.contains("metadata_ok") && item["metadata_ok"].is_boolean())
            entry.metadataOk = item["metadata_ok"].get<bool>();
        if (item.contains("failure_reason") &&
            item["failure_reason"].is_string()) {
            entry.healthReason = item["failure_reason"].get<std::string>();
            if (entry.healthReason.size() > 512)
                entry.healthReason.resize(512);
        }
        if (!hashes.insert(entry.infoHash).second)
            continue;
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        error = "Catalog does not contain usable RuTracker magnets.";
        return false;
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const CatalogEntry& left, const CatalogEntry& right) {
                         return left.publishedAt > right.publishedAt;
                     });
    return true;
}

bool CatalogService::loadFile(const std::string& path,
                              const std::string& label,
                              std::string& error) {
    std::string data;
    if (!readFile(path, data, error))
        return false;
    std::vector<CatalogEntry> parsed;
    if (!parseJson(data, parsed, error))
        return false;
    entries_ = std::move(parsed);
    sourceLabel_ = label;
    log_msg("[catalog] loaded %zu entries from %s\n", entries_.size(),
            path.c_str());
    return true;
}

bool CatalogService::load(std::string& error) {
    std::string cacheError;
    if (loadFile(cachePath_, "cached catalog", cacheError))
        return true;
    if (loadFile(bundledPath_, "bundled catalog", error))
        return true;
    if (!cacheError.empty())
        error = cacheError + " " + error;
    return false;
}

bool CatalogService::refresh(std::string& error) {
    std::string releaseBody;
    if (!httpGet(kLatestReleaseApi, releaseBody, error))
        return false;
    nlohmann::json release =
        nlohmann::json::parse(releaseBody, nullptr, false);
    if (release.is_discarded() || !release.is_object() ||
        !release.contains("assets") || !release["assets"].is_array()) {
        error = "GitHub returned an invalid release description.";
        return false;
    }
    std::string assetUrl;
    for (const auto& asset : release["assets"]) {
        if (!asset.is_object() ||
            !asset.contains("name") || !asset["name"].is_string() ||
            !asset.contains("browser_download_url") ||
            !asset["browser_download_url"].is_string())
            continue;
        std::string name = asset["name"].get<std::string>();
        if (name.size() >= 5 && name.substr(name.size() - 5) == ".json") {
            assetUrl = asset["browser_download_url"].get<std::string>();
            break;
        }
    }
    if (assetUrl.rfind(
            "https://github.com/bqio/switch-dumps/releases/download/", 0) != 0) {
        error = "Latest release does not contain a trusted catalog asset.";
        return false;
    }

    std::string catalogBody;
    if (!httpGet(assetUrl, catalogBody, error))
        return false;
    std::vector<CatalogEntry> parsed;
    if (!parseJson(catalogBody, parsed, error))
        return false;
    if (!writeAtomic(cachePath_, catalogBody, error))
        return false;
    entries_ = std::move(parsed);
    sourceLabel_ = "latest GitHub catalog";
    log_msg("[catalog] refreshed %zu entries\n", entries_.size());
    return true;
}

} // namespace pipensx
