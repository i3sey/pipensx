#include "catalog_service.hpp"
#include "magnet_resolver.hpp"

extern "C" {
#include "../core/sha1.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
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
constexpr size_t kMaxInfoDictBytes = 8 * 1024 * 1024;

/* The live catalogue source: the Langegen switch-games repo publishes a single
   switch_games.json on GitHub's raw host. Fetched on demand by the refresh
   button and (when the catalogue is empty or the launch toggle is on) in the
   background at startup. Must satisfy isTrustedSource(). */
constexpr const char* kCatalogSourceUrl =
    "https://raw.githubusercontent.com/Langegen/switch-games/"
    "refs/heads/main/switch_games.json";

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decodeBase64(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.empty() || text.size() % 4 != 0)
        return false;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        int values[4];
        int padding = 0;
        for (size_t j = 0; j < 4; ++j) {
            char c = text[i + j];
            if (c == '=' && i + 4 == text.size() && j >= 2) {
                values[j] = 0;
                ++padding;
                continue;
            }
            if (padding || (values[j] = base64Value(c)) < 0)
                return false;
        }
        uint32_t block = (static_cast<uint32_t>(values[0]) << 18) |
                         (static_cast<uint32_t>(values[1]) << 12) |
                         (static_cast<uint32_t>(values[2]) << 6) |
                         static_cast<uint32_t>(values[3]);
        out.push_back(static_cast<uint8_t>(block >> 16));
        if (padding < 2)
            out.push_back(static_cast<uint8_t>(block >> 8));
        if (padding < 1)
            out.push_back(static_cast<uint8_t>(block));
    }
    return true;
}

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
    body.clear();
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
    if (rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    // Switch fsdev/FatFs rename() fails when the target already exists (no
    // POSIX overwrite), so drop the old file first and retry.
    if ((unlink(path.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    unlink(temporary.c_str());
    error = "Unable to replace catalog cache.";
    return false;
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

// A plain string value, trimmed to `limit` bytes; empty when absent or not a
// string. Used for the Langegen inline metadata (year/genre/publisher/…).
std::string readString(const nlohmann::json& item, const char* key,
                       size_t limit) {
    if (!item.contains(key) || !item[key].is_string())
        return std::string();
    std::string value = item[key].get<std::string>();
    if (value.size() > limit)
        value.resize(limit);
    return value;
}

// Numeric id that the source may encode as a JSON number (bqio) or a decimal
// string (Langegen "topic_id":"6878751").
uint64_t readFlexibleUnsigned(const nlohmann::json& item, const char* key) {
    if (!item.contains(key))
        return 0;
    if (item[key].is_string()) {
        uint64_t value = 0;
        for (char c : item[key].get_ref<const std::string&>()) {
            if (c < '0' || c > '9')
                return value;  // stop at the first non-digit
            value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        return value;
    }
    return readUnsigned(item, key);
}

// A human size string such as "5.19 GB" / "512 MB" / "700 KiB" → bytes. RuTracker
// labels are binary (1 GB == 1024 MiB), so scale on 1024. Returns 0 when the
// value is unparseable, which the detail card renders as "Unknown".
uint64_t parseSizeToBytes(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    double value = 0.0;
    bool sawDigit = false;
    bool fraction = false;
    double scale = 0.1;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            sawDigit = true;
            if (fraction) {
                value += (c - '0') * scale;
                scale *= 0.1;
            } else {
                value = value * 10 + (c - '0');
            }
        } else if ((c == '.' || c == ',') && !fraction) {
            fraction = true;
        } else {
            break;
        }
    }
    if (!sawDigit)
        return 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    char unit = i < text.size() ? static_cast<char>(std::toupper(
                                      static_cast<unsigned char>(text[i])))
                                : 'B';
    double multiplier = 1.0;
    switch (unit) {
        case 'T': multiplier = 1024.0 * 1024 * 1024 * 1024; break;
        case 'G': multiplier = 1024.0 * 1024 * 1024; break;
        case 'M': multiplier = 1024.0 * 1024; break;
        case 'K': multiplier = 1024.0; break;
        default: multiplier = 1.0; break;  // bytes or unknown
    }
    double bytes = value * multiplier;
    if (bytes < 0.0)
        return 0;
    return static_cast<uint64_t>(bytes + 0.5);
}

// The catalogue size field: a JSON number of bytes (bqio) or a human string
// (Langegen "size":"5.19 GB").
uint64_t readFlexibleSize(const nlohmann::json& item, const char* key) {
    if (!item.contains(key))
        return 0;
    if (item[key].is_string())
        return parseSizeToBytes(item[key].get_ref<const std::string&>());
    return readUnsigned(item, key);
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
        // The Langegen source names the magnet "magnet" and the cover "cover";
        // the older bqio dumps used "magnetURI" and "poster". Accept either so
        // a cached bqio snapshot still parses.
        const char* magnetKey = item.contains("magnet") ? "magnet" : "magnetURI";
        if (!item.is_object() ||
            !item.contains("title") || !item["title"].is_string() ||
            !item.contains(magnetKey) || !item[magnetKey].is_string())
            continue;
        CatalogEntry entry;
        entry.title = item["title"].get<std::string>();
        entry.magnetUri = item[magnetKey].get<std::string>();
        if (entry.title.empty() || entry.title.size() > 1024 ||
            entry.magnetUri.size() > 2048)
            continue;
        MagnetSpec magnet;
        std::string magnetError;
        if (!MagnetResolver::parse(entry.magnetUri, magnet, magnetError))
            continue;
        entry.infoHash = magnet.infoHashHex;
        entry.trackerUrl = magnet.trackerUrl;
        entry.posterUrl = item.contains("cover")
                              ? readString(item, "cover", 2048)
                              : readString(item, "poster", 2048);
        if (item.contains("screenshots") && item["screenshots"].is_array()) {
            for (const auto& value : item["screenshots"]) {
                if (!value.is_string() || entry.screenshots.size() >= 6)
                    continue;
                std::string url = value.get<std::string>();
                if (!url.empty() && url.size() <= 2048)
                    entry.screenshots.push_back(std::move(url));
            }
        }
        entry.size = readFlexibleSize(item, "size");
        entry.topicId = readFlexibleUnsigned(item, "topic_id");
        // Langegen inline metadata: shown on the detail card, which never
        // matches these entries in the bundled game_metadata_index.
        entry.year = readString(item, "year", 32);
        entry.genre = readString(item, "genre", 256);
        entry.developer = readString(item, "developer", 256);
        entry.publisher = readString(item, "publisher", 256);
        entry.description = readString(item, "description", 4096);
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
        /* Pre-resolved info dictionary (RF_ACCESS_PLAN П2.1). A dictionary
           that fails to decode or does not hash to the magnet's btih is
           dropped here, so entry.infoDict non-empty always means verified;
           the entry itself stays usable through the network resolve. */
        if (item.contains("info_dict") && item["info_dict"].is_string()) {
            const std::string& encoded =
                item["info_dict"].get_ref<const std::string&>();
            if (encoded.size() <= kMaxInfoDictBytes / 3 * 4 + 4 &&
                decodeBase64(encoded, entry.infoDict)) {
                uint8_t digest[20];
                sha1(entry.infoDict.data(), entry.infoDict.size(), digest);
                if (entry.infoDict.size() > kMaxInfoDictBytes ||
                    std::memcmp(digest, magnet.infoHash, 20) != 0) {
                    log_msg("[catalog] info_dict for %s rejected\n",
                            entry.infoHash.c_str());
                    entry.infoDict.clear();
                }
            } else {
                entry.infoDict.clear();
            }
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
    if (!bundledPath_.empty()) {
        if (loadFile(bundledPath_, "bundled catalog", error))
            return true;
        if (!cacheError.empty())
            error = cacheError + " " + error;
        return false;
    }

    // A fresh public install intentionally has no bundled catalog. The UI
    // sees an empty list and starts the trusted live refresh in the background.
    entries_.clear();
    sourceLabel_.clear();
    error.clear();
    return true;
}

bool CatalogService::isTrustedSource(const std::string& url) {
    // Host (with path prefix) allowed to serve catalog bytes. Only the Langegen
    // switch-games repo on GitHub's raw host; every network fetch is gated on
    // this so a redirect or MITM to another host is refused before any parse.
    static const char* const kPrefixes[] = {
        "https://raw.githubusercontent.com/Langegen/switch-games/",
    };
    for (const char* prefix : kPrefixes)
        if (url.rfind(prefix, 0) == 0)
            return true;
    return false;
}

bool CatalogService::fetchLatest(std::vector<CatalogEntry>& parsed,
                                 std::string& error) {
    // Single source: the Langegen switch_games.json on GitHub's raw host. Runs
    // on a worker thread: network fetch + parse + cache write only, so it never
    // touches entries_. The cached catalogue in memory survives a failure —
    // the caller keeps showing it on error.
    parsed.clear();
    if (!isTrustedSource(kCatalogSourceUrl)) {
        error = "Catalog URL is not on the trusted host list.";
        return false;
    }
    std::string catalogBody;
    if (!httpGet(kCatalogSourceUrl, catalogBody, error))
        return false;
    if (!parseJson(catalogBody, parsed, error))
        return false;
    if (!writeAtomic(cachePath_, catalogBody, error))
        return false;
    return true;
}

void CatalogService::adopt(std::vector<CatalogEntry> parsed) {
    // UI thread only: entries() is read unsynchronised by the render thread, so
    // this swap must never happen on the fetch worker (data race → UAF).
    entries_ = std::move(parsed);
    sourceLabel_ = "Langegen switch-games";
    log_msg("[catalog] refreshed %zu entries from %s\n", entries_.size(),
            sourceLabel_.c_str());
}

} // namespace pipensx
