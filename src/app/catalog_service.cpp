#include "catalog_service.hpp"
#include "magnet_resolver.hpp"

extern "C" {
#include "../core/antizapret.h"
#include "../core/catalog_sig.h"
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
constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/bqio/switch-dumps/releases/latest";
constexpr const char* kTrustedReleasePrefix =
    "https://github.com/bqio/switch-dumps/releases/download/";

/* ed25519 public key that signs the network catalog (RF_ACCESS_PLAN П3.2).
   All-zero = signing not provisioned yet: the network refresh runs unverified,
   preserving the pre-signing behaviour. Replace with the real 32-byte key
   (tools/sign_catalog.py --gen-key emits both halves) to enforce a valid
   detached signature on every refresh; a missing or invalid signature then
   aborts the refresh so a MITM cannot strip it. Cached and bundled catalogs
   are local — trusted at build time or already verified when cached — and are
   not re-checked here. */
constexpr uint8_t kCatalogPublicKey[32] = {0};

bool catalogSigningEnabled() {
    for (uint8_t byte : kCatalogPublicKey)
        if (byte)
            return true;
    return false;
}

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

/* Verify a detached base64 ed25519 signature (the ".sig" release asset) over
   the raw catalog bytes. Tolerates surrounding whitespace/newlines in the
   signature file. */
bool verifyCatalogSignature(const std::string& catalog,
                            const std::string& signatureText,
                            std::string& error) {
    std::string trimmed;
    trimmed.reserve(signatureText.size());
    for (char c : signatureText)
        if (!std::isspace(static_cast<unsigned char>(c)))
            trimmed.push_back(c);
    std::vector<uint8_t> signature;
    if (!decodeBase64(trimmed, signature) || signature.size() != 64) {
        error = "Catalog signature is malformed.";
        return false;
    }
    const uint8_t* body = reinterpret_cast<const uint8_t*>(catalog.data());
    if (!catalog_sig_verify(kCatalogPublicKey, body, catalog.size(),
                            signature.data())) {
        error = "Catalog signature does not match the trusted key.";
        return false;
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

bool httpGetOnce(const std::string& url, const antizapret_route_t* route,
                 std::string& body, std::string& error) {
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
    if (route)
        antizapret_apply_route(curl, route);
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

bool httpGet(const std::string& url, std::string& body, std::string& error) {
    body.clear();
    const bool target = antizapret_is_enabled() &&
                        antizapret_is_target_url(url.c_str());
#ifdef __SWITCH__
    const bool preferProxy = target;
#else
    const bool preferProxy = target && antizapret_proxy_preferred();
#endif

    auto attempt = [&](const antizapret_route_t* route) {
        std::string attemptBody;
        std::string attemptError;
        if (!httpGetOnce(url, route, attemptBody, attemptError)) {
            error = std::move(attemptError);
            return false;
        }
        if (target && antizapret_response_looks_blocked(
                          attemptBody.data(), attemptBody.size())) {
            error = "Catalog response appears to be blocked.";
            return false;
        }
        body = std::move(attemptBody);
        error.clear();
        if (route && route->type != ANTIZAPRET_ROUTE_DIRECT) {
            antizapret_note_proxy_success();
            log_msg("[catalog] proxy active: %s\n",
                    antizapret_route_name(route));
        }
        return true;
    };

    if (!preferProxy && attempt(nullptr))
        return true;

    if (target) {
        antizapret_route_t routes[ANTIZAPRET_MAX_ROUTES];
        size_t routeCount = antizapret_get_routes(routes,
                                                  ANTIZAPRET_MAX_ROUTES);
        for (size_t i = 0; i < routeCount; ++i) {
            if (routes[i].type == ANTIZAPRET_ROUTE_DIRECT ||
                !antizapret_route_supported(&routes[i]))
                continue;
            if (attempt(&routes[i]))
                return true;
        }
    }

    if (preferProxy && attempt(nullptr))
        return true;
    return false;
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
        if (item.contains("screenshots") && item["screenshots"].is_array()) {
            for (const auto& value : item["screenshots"]) {
                if (!value.is_string() || entry.screenshots.size() >= 6)
                    continue;
                std::string url = value.get<std::string>();
                if (!url.empty() && url.size() <= 2048)
                    entry.screenshots.push_back(std::move(url));
            }
        }
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
    std::string signatureUrl;
    for (const auto& asset : release["assets"]) {
        if (!asset.is_object() ||
            !asset.contains("name") || !asset["name"].is_string() ||
            !asset.contains("browser_download_url") ||
            !asset["browser_download_url"].is_string())
            continue;
        std::string name = asset["name"].get<std::string>();
        std::string url = asset["browser_download_url"].get<std::string>();
        auto endsWith = [&](const char* suffix) {
            size_t n = std::strlen(suffix);
            return name.size() >= n &&
                   name.compare(name.size() - n, n, suffix) == 0;
        };
        // A signature asset ends in ".sig", never ".json", so the two never
        // collide. Take the first of each kind.
        if (assetUrl.empty() && endsWith(".json"))
            assetUrl = url;
        if (signatureUrl.empty() && endsWith(".sig"))
            signatureUrl = url;
    }
    if (assetUrl.rfind(kTrustedReleasePrefix, 0) != 0) {
        error = "Latest release does not contain a trusted catalog asset.";
        return false;
    }

    std::string catalogBody;
    if (!httpGet(assetUrl, catalogBody, error))
        return false;

    // Signature enforcement (RF_ACCESS_PLAN П3.2): only once a real public key
    // is baked in. Fail closed — a stripped or forged signature aborts the
    // refresh before the catalog is parsed or cached.
    if (catalogSigningEnabled()) {
        if (signatureUrl.rfind(kTrustedReleasePrefix, 0) != 0) {
            error = "The signed catalog release is missing its signature.";
            return false;
        }
        std::string signatureBody;
        if (!httpGet(signatureUrl, signatureBody, error))
            return false;
        if (!verifyCatalogSignature(catalogBody, signatureBody, error))
            return false;
        log_msg("[catalog] signature verified\n");
    }

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
