#include "retro_catalog.hpp"

#include "catalog_urls.hpp"
#include "core/client_identity.h"
#include "curl_https.hpp"
#include "magnet_resolver.hpp"
#include <algorithm>
#include <borealis/extern/nlohmann/json.hpp>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {
using Json = nlohmann::json;

constexpr size_t metadataLimit = 24 * 1024 * 1024;
constexpr size_t cacheLimit = 32 * 1024 * 1024;
constexpr size_t maxReleaseRows = 25000;

bool text(const Json& row, const char* key, std::string& out, size_t limit) {
    const auto it = row.find(key);
    if (it == row.end() || it->is_null()) return true;
    if (!it->is_string()) return false;
    out = it->get<std::string>();
    return out.size() <= limit && out.find('\0') == std::string::npos;
}

bool imageAddress(const std::string& value) {
    const size_t start = value.rfind("https://", 0) == 0 ? 8 :
                         value.rfind("http://", 0) == 0 ? 7 : 0;
    if (!start || value.size() <= start) return false;
    const auto end = value.find('/', start);
    const auto host = value.substr(start, end == std::string::npos ? end : end - start);
    return !host.empty() && host.find_first_of("@?#\\") == std::string::npos &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}

bool imageUrl(const std::string& value, const std::string& base, std::string& out) {
    if (value.empty()) return true;
    if (imageAddress(value)) { out = value; return true; }
    if (!imageAddress(base) || value.front() == '/' ||
        value.find_first_of("\\:%?#") != std::string::npos) return false;
    size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    out = base + (base.back() == '/' ? "" : "/") + value;
    return imageAddress(out);
}

bool parseRetroRows(const std::string& platform, const Json& root, const RetroSource& source,
                    std::vector<RetroRelease>& releases, std::string& error) {
    error.clear();
    auto fail = [&](const char* reason) { error = reason; return false; };
    if (!findRetroConsole(platform))
        return fail("Unsupported retro platform.");
    if (!root.is_array() || root.empty() || root.size() > maxReleaseRows)
        return fail("Retro catalog must be a nonempty array of releases.");
    std::vector<RetroRelease> parsed;
    parsed.reserve(root.size());
    std::set<std::string> topics;
    for (const auto& row : root) {
        if (!row.is_object()) return fail("Retro catalog contains an invalid release.");
        RetroRelease release;
        release.platform = platform;
        std::string cover;
        if (!text(row, "topic_id", release.topicId, 20) || release.topicId.empty() ||
            release.topicId.front() == '0' ||
            release.topicId.find_first_not_of("0123456789") != std::string::npos ||
            !topics.insert(release.topicId).second)
            return fail("Retro catalog contains an invalid or duplicate topic ID.");
        if (!text(row, "title", release.title, 2048) ||
            release.title.find_first_not_of(" \r\n\t") == std::string::npos ||
            !text(row, "magnet", release.magnetUri, 4096))
            return fail("Retro release has an invalid title or magnet.");
        MagnetSpec magnet;
        if (!MagnetResolver::parse(release.magnetUri, magnet, error)) return false;
        release.infoHash = magnet.infoHashHex;
        if (!text(row, "title_id", release.serial, 128) ||
            !text(row, "content_type", release.contentType, 128) ||
            !text(row, "description", release.description, 512 * 1024) ||
            !text(row, "size", release.size, 128) ||
            !text(row, "year", release.year, 4096) ||
            !text(row, "genre", release.genre, 4096) ||
            !text(row, "developer", release.developer, 4096) ||
            !text(row, "publisher", release.publisher, 4096) ||
            !text(row, "interface_lang", release.interfaceLang, 4096) ||
            !text(row, "voice_lang", release.voiceLang, 4096) ||
            !text(row, "cover", cover, 2048) ||
            !imageUrl(cover, source.imageBaseUrl, release.coverUrl))
            return fail("Retro release contains invalid metadata or an image path.");
        if (row.contains("is_romset")) {
            if (!row["is_romset"].is_boolean()) return fail("Retro ROM-set flag must be boolean.");
            release.romset = row["is_romset"].get<bool>();
        }
        if (row.contains("screenshots")) {
            const auto& images = row["screenshots"];
            if (!images.is_array() || images.size() > 64) return fail("Invalid retro screenshots.");
            for (const auto& image : images) {
                std::string url;
                if (!image.is_string() || image.get_ref<const std::string&>().size() > 2048 ||
                    !imageUrl(image.get_ref<const std::string&>(), source.imageBaseUrl, url) || url.empty())
                    return fail("Invalid retro screenshot URL.");
                release.screenshots.push_back(std::move(url));
            }
        }
        parsed.push_back(std::move(release));
    }
    releases = std::move(parsed);
    return true;
}

bool parseStatsJson(const Json& stats, std::map<std::string, RetroStats>& out) {
    if (!stats.is_object() || stats.empty() || stats.size() > 100000) return false;
    std::map<std::string, RetroStats> parsed;
    for (auto it = stats.begin(); it != stats.end(); ++it) {
        if (it.key().empty() || it.key().size() > 20 || it.key().front() == '0' ||
            it.key().find_first_not_of("0123456789") != std::string::npos || !it.value().is_object())
            return false;
        RetroStats entry;
        uint64_t* fields[] = {&entry.seeds, &entry.leeches, &entry.downloads};
        size_t index = 0;
        for (const char* key : {"seeds", "leeches", "downloads"}) {
            const auto value = it.value().find(key);
            if (value == it.value().end() || !value->is_number_unsigned()) return false;
            *fields[index++] = value->get<uint64_t>();
        }
        parsed.emplace(it.key(), entry);
    }
    out = std::move(parsed);
    return true;
}

bool parseEmulatorsJson(const Json& emulators, std::vector<RetroEmulatorPackage>& out) {
    if (!emulators.is_array() || emulators.empty() || emulators.size() > 256) return false;
    std::vector<RetroEmulatorPackage> parsed;
    std::set<std::string> ids;
    for (const auto& row : emulators) {
        if (!row.is_object() || !row.contains("supported_console_ids") ||
            !row["supported_console_ids"].is_array() || row["supported_console_ids"].empty())
            return false;
        RetroEmulatorPackage package;
        for (const auto& platform : row["supported_console_ids"]) {
            if (!platform.is_string()) return false;
            const std::string cid = platform.get<std::string>();
            if (cid.empty() || cid.size() > 64) return false;
            package.supportedConsoleIds.push_back(cid);
        }
        if (!text(row, "id", package.id, 80) || package.id.empty() || !ids.insert(package.id).second ||
            !text(row, "name", package.name, 256) || package.name.empty() ||
            !text(row, "author", package.author, 256) ||
            !text(row, "description", package.description, 4096) ||
            !text(row, "category", package.category, 64) ||
            !text(row, "version", package.version, 128) || package.version.empty() ||
            !text(row, "download_url", package.downloadUrl, 2048) ||
            package.downloadUrl.rfind("https://", 0) != 0 || !imageAddress(package.downloadUrl) ||
            !text(row, "filename", package.filename, 256) || package.filename.empty() ||
            package.filename == "." || package.filename == ".." ||
            package.filename.find_first_of("/\\:") != std::string::npos ||
            !text(row, "install_path", package.installPath, 512) ||
            !text(row, "extract_dir", package.extractDir, 512) ||
            !row.contains("file_size") || !row["file_size"].is_number_unsigned() ||
            !row.contains("is_archive") || !row["is_archive"].is_boolean())
            return false;
        package.bytes = row["file_size"].get<uint64_t>();
        if (!package.bytes || package.bytes > 512 * 1024 * 1024) return false;
        package.archive = row["is_archive"].get<bool>();
        parsed.push_back(std::move(package));
    }
    out = std::move(parsed);
    return !out.empty();
}

struct RetroTransfer {
    const RetroCatalog::Sink& sink;
    const std::atomic<bool>& cancelled;
};

size_t retroReceive(char* data, size_t size, size_t count, void* opaque) {
    auto& transfer = *static_cast<RetroTransfer*>(opaque);
    if (transfer.cancelled || !transfer.sink(data, size * count)) return 0;
    return size * count;
}

int retroProgress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<RetroTransfer*>(opaque)->cancelled ? 1 : 0;
}

bool retroFetch(const std::string& url, const RetroCatalog::Sink& sink,
                const std::atomic<bool>& cancelled, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = "Unable to initialize retro HTTP client."; return false; }
    RetroTransfer transfer{sink, cancelled};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, PIPENSX_USER_AGENT);
    curlPinHttpsOnly(curl);
    curlApplyTrustedSsl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, retroReceive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, retroProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result == CURLE_OK && status == 200 && !cancelled) return true;
    error = cancelled ? "Cancelled." : "Retro HTTP " + std::to_string(status) + ": " + curl_easy_strerror(result);
    return false;
}

const RetroSource* findKnownSource(const std::string& dataBaseUrl) {
    static const std::vector<RetroSource> sources = retroCatalogSources("");
    for (const auto& s : sources) {
        if (s.dataBaseUrl == dataBaseUrl)
            return &s;
    }
    return nullptr;
}

bool readRawFile(const std::string& path, std::string& body) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > static_cast<std::streamoff>(cacheLimit))
        return false;
    body.assign(static_cast<size_t>(input.tellg()), '\0');
    input.seekg(0);
    return static_cast<bool>(input.read(body.data(), body.size()));
}

bool writeSnapshot(const std::string& path, const std::string& body) {
    FILE* file = std::fopen((path + ".tmp").c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    if (std::fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (ok && std::rename((path + ".tmp").c_str(), path.c_str()) != 0) {
        struct stat existing{};
        ok = lstat(path.c_str(), &existing) == 0 && S_ISREG(existing.st_mode) &&
             unlink(path.c_str()) == 0 && std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
    }
    if (!ok) std::remove((path + ".tmp").c_str());
    return ok;
}

bool ensureDir(const std::string& path) {
    struct stat st{};
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool decodeLegacySnapshot(const Json& bundle, RetroCatalogSnapshot& snapshot, std::string& error) {
    auto fail = [&] { error = "Invalid retro metadata snapshot."; return false; };
    if (!bundle.is_object() || bundle.value("schema", Json()) != 1 ||
        !bundle.contains("source") || !bundle["source"].is_string()) return fail();
    const auto* source = findKnownSource(bundle["source"].get<std::string>());
    if (!source) return fail();
    RetroCatalogSnapshot parsed;
    parsed.platform = "gba";
    parsed.source = *source;
    for (const char* key : {"games", "stats", "emulators"})
        if (!bundle.contains(key) || !bundle[key].is_string() ||
            bundle[key].get_ref<const std::string&>().size() > metadataLimit) return fail();
    if (!parseRetroCatalog("gba", bundle["games"].get_ref<const std::string&>(), *source, parsed.releases, error))
        return false;
    const auto stats = Json::parse(bundle["stats"].get_ref<const std::string&>(), nullptr, false);
    const auto emulators = Json::parse(bundle["emulators"].get_ref<const std::string&>(), nullptr, false);
    if (!parseStatsJson(stats, parsed.stats) || !parseEmulatorsJson(emulators, parsed.emulators))
        return fail();
    snapshot = std::move(parsed);
    error.clear();
    return true;
}

bool decodeSharedV2(const Json& bundle, std::map<std::string, RetroStats>& statsOut,
                    std::vector<RetroEmulatorPackage>& emuOut, RetroSource& sourceOut) {
    if (!bundle.is_object() || bundle.value("schema", Json()) != 2 ||
        !bundle.contains("source") || !bundle["source"].is_string() ||
        !bundle.contains("stats") || !bundle.contains("emulators"))
        return false;
    const auto* source = findKnownSource(bundle["source"].get<std::string>());
    if (!source) return false;
    std::map<std::string, RetroStats> parsedStats;
    std::vector<RetroEmulatorPackage> parsedEmus;
    if (!parseStatsJson(bundle["stats"], parsedStats) ||
        !parseEmulatorsJson(bundle["emulators"], parsedEmus))
        return false;
    statsOut = std::move(parsedStats);
    emuOut = std::move(parsedEmus);
    sourceOut = *source;
    return true;
}

bool decodeConsoleV2(const std::string& platform, const Json& bundle,
                     std::vector<RetroRelease>& releasesOut, RetroSource& sourceOut,
                     std::string& error) {
    if (!bundle.is_object() || bundle.value("schema", Json()) != 2 ||
        bundle.value("platform", std::string()) != platform ||
        !bundle.contains("source") || !bundle["source"].is_string() ||
        !bundle.contains("games")) {
        error = "Invalid cached retro console snapshot.";
        return false;
    }
    const auto* source = findKnownSource(bundle["source"].get<std::string>());
    if (!source) {
        error = "Unknown retro catalog source.";
        return false;
    }
    if (!parseRetroRows(platform, bundle["games"], *source, releasesOut, error))
        return false;
    sourceOut = *source;
    return true;
}

bool readSharedFiles(const std::string& root, std::map<std::string, RetroStats>& stats,
                     std::vector<RetroEmulatorPackage>& emulators, RetroSource& source,
                     std::string* rawBodyOut = nullptr) {
    for (const char* file : {"/shared.json", "/shared.json.previous"}) {
        std::string body;
        if (!readRawFile(root + file, body)) continue;
        const Json parsed = Json::parse(body, nullptr, false);
        if (decodeSharedV2(parsed, stats, emulators, source)) {
            if (rawBodyOut) *rawBodyOut = std::move(body);
            return true;
        }
    }
    return false;
}

bool readConsoleFiles(const std::string& root, const std::string& platform,
                      std::vector<RetroRelease>& releases, RetroSource& source,
                      std::string& error, std::string* rawBodyOut = nullptr) {
    for (const char* suffix : {".json", ".json.previous"}) {
        std::string body;
        if (!readRawFile(root + "/consoles/" + platform + suffix, body)) continue;
        const Json parsed = Json::parse(body, nullptr, false);
        if (decodeConsoleV2(platform, parsed, releases, source, error)) {
            if (rawBodyOut) *rawBodyOut = std::move(body);
            return true;
        }
    }
    return false;
}

bool saveWithPrevious(const std::string& path, const std::string& newBody,
                      const std::function<bool(const std::string&)>& validator,
                      std::string& error, const char* preserveErr, const char* saveErr) {
    std::string existingBody;
    if (readRawFile(path, existingBody) && validator(existingBody)) {
        if (!writeSnapshot(path + ".previous", existingBody)) {
            error = preserveErr;
            return false;
        }
    }
    if (newBody.size() > cacheLimit) {
        error = "Retro cache exceeds its size limit.";
        return false;
    }
    if (!writeSnapshot(path, newBody)) {
        error = saveErr;
        return false;
    }
    return true;
}

std::string buildConsoleCacheBody(const std::string& platform, const RetroSource& source,
                                  const std::string& rawGamesBody) {
    size_t start = 0;
    while (start < rawGamesBody.size() &&
           (rawGamesBody[start] == ' ' || rawGamesBody[start] == '\t' ||
            rawGamesBody[start] == '\r' || rawGamesBody[start] == '\n'))
        ++start;
    size_t end = rawGamesBody.size();
    while (end > start &&
           (rawGamesBody[end - 1] == ' ' || rawGamesBody[end - 1] == '\t' ||
            rawGamesBody[end - 1] == '\r' || rawGamesBody[end - 1] == '\n'))
        --end;
    std::string out;
    out.reserve((end - start) + 128);
    out += "{\"schema\":2,\"platform\":";
    out += Json(platform).dump();
    out += ",\"source\":";
    out += Json(source.dataBaseUrl).dump();
    out += ",\"games\":";
    out.append(rawGamesBody, start, end - start);
    out += "}";
    return out;
}

std::string buildSharedCacheBody(const RetroSource& source, const std::string& rawStatsBody,
                                 const std::string& rawEmulatorsBody) {
    auto trimmed = [](const std::string& s) -> std::string {
        size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };
    std::string out;
    out.reserve(rawStatsBody.size() + rawEmulatorsBody.size() + 160);
    out += "{\"schema\":2,\"source\":";
    out += Json(source.dataBaseUrl).dump();
    out += ",\"updated_at\":";
    out += std::to_string(static_cast<long long>(std::time(nullptr)));
    out += ",\"stats\":";
    out += trimmed(rawStatsBody);
    out += ",\"emulators\":";
    out += trimmed(rawEmulatorsBody);
    out += "}";
    return out;
}

} // namespace

std::vector<RetroSource> retroCatalogSources(const std::string& preferredSource) {
    std::vector<RetroSource> sources = {
        {std::string(kCatalogGitLabBaseUrl) + "retro/data/", kCatalogGitLabBaseUrl},
        {std::string(kCatalogGitVerseBaseUrl) + "retro/data/", kCatalogGitVerseBaseUrl},
        {"https://raw.githubusercontent.com/Langegen/console-games/main/data/",
         "https://raw.githubusercontent.com/Langegen/console-games/main/"},
    };
    const auto preferred = std::find_if(sources.begin(), sources.end(), [&](const RetroSource& source) {
        return !preferredSource.empty() && preferredSource.rfind(source.imageBaseUrl, 0) == 0;
    });
    if (preferred != sources.end()) std::rotate(sources.begin(), preferred, preferred + 1);
    return sources;
}

std::vector<RetroSource> gbaCatalogSources(const std::string& preferredSource) {
    return retroCatalogSources(preferredSource);
}

bool parseRetroCatalog(const std::string& platform, const std::string& body,
                       const RetroSource& source, std::vector<RetroRelease>& releases,
                       std::string& error) {
    error.clear();
    if (body.size() > metadataLimit) {
        error = "Retro catalog exceeds the metadata limit.";
        return false;
    }
    const auto root = Json::parse(body, nullptr, false);
    return parseRetroRows(platform, root, source, releases, error);
}

bool parseGbaCatalog(const std::string& body, const RetroSource& source,
                     std::vector<RetroRelease>& releases, std::string& error) {
    return parseRetroCatalog("gba", body, source, releases, error);
}

uint64_t parseRetroSizeBytes(const std::string& text) {
    if (text.empty()) return 0;
    char* end = nullptr;
    const double val = std::strtod(text.c_str(), &end);
    if (!end || end == text.c_str() || val <= 0.0) return 0;
    std::string unit;
    for (const char* p = end; *p; ++p) {
        if (!std::isspace(static_cast<unsigned char>(*p)))
            unit.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    double mult = 1.0;
    if (unit == "kb" || unit == "kib" || unit == "k" || unit == "кб") mult = 1024.0;
    else if (unit == "mb" || unit == "mib" || unit == "m" || unit == "мб") mult = 1024.0 * 1024.0;
    else if (unit == "gb" || unit == "gib" || unit == "g" || unit == "гб") mult = 1024.0 * 1024.0 * 1024.0;
    else if (unit == "tb" || unit == "tib" || unit == "t" || unit == "тб") mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return static_cast<uint64_t>(val * mult);
}

RetroCatalog::RetroCatalog(std::string appRoot, Fetcher fetcher)
    : root_(std::move(appRoot) + "/retro"), fetcher_(fetcher ? std::move(fetcher) : retroFetch) {}

bool RetroCatalog::loadShared(std::map<std::string, RetroStats>& stats,
                              std::vector<RetroEmulatorPackage>& emulators,
                              RetroSource& source, std::string& error) const {
    std::lock_guard<std::mutex> lock(*mutex_);
    if (readSharedFiles(root_, stats, emulators, source)) {
        error.clear();
        return true;
    }
    error = "No valid cached retro shared metadata.";
    return false;
}

bool RetroCatalog::load(RetroCatalogSnapshot& snapshot, std::string& error) const {
    return load("gba", snapshot, error);
}

bool RetroCatalog::load(const std::string& platform, RetroCatalogSnapshot& snapshot,
                        std::string& error) const {
    std::lock_guard<std::mutex> lock(*mutex_);
    if (!findRetroConsole(platform)) {
        error = "Unsupported retro platform.";
        return false;
    }
    RetroCatalogSnapshot parsed;
    parsed.platform = platform;
    RetroSource consoleSource, sharedSource;
    std::string consoleError;
    const bool hasConsole = readConsoleFiles(root_, platform, parsed.releases, consoleSource, consoleError);
    const bool hasShared = readSharedFiles(root_, parsed.stats, parsed.emulators, sharedSource);
    if (hasConsole && hasShared) {
        parsed.source = consoleSource;
        snapshot = std::move(parsed);
        error.clear();
        return true;
    }
    if (platform == "gba") {
        for (const char* legacy : {"/catalog.json", "/previous.json"}) {
            std::string body;
            if (!readRawFile(root_ + legacy, body)) continue;
            const Json bundle = Json::parse(body, nullptr, false);
            if (decodeLegacySnapshot(bundle, parsed, error)) {
                snapshot = std::move(parsed);
                error.clear();
                return true;
            }
        }
    }
    error = !consoleError.empty() ? consoleError : "No valid cached retro catalog.";
    return false;
}

bool RetroCatalog::refresh(RetroCatalogSnapshot& snapshot, const std::string& preferredSource,
                           const std::atomic<bool>& cancelled, std::string& error) const {
    return refresh("gba", snapshot, preferredSource, true, cancelled, error);
}

bool RetroCatalog::refresh(const std::string& platform, RetroCatalogSnapshot& snapshot,
                           const std::string& preferredSource, bool forceShared,
                           const std::atomic<bool>& cancelled, std::string& error) const {
    std::lock_guard<std::mutex> lock(*mutex_);
    const RetroConsoleDef* console = findRetroConsole(platform);
    if (!console) {
        error = "Unsupported retro platform.";
        return false;
    }

    auto fetchBody = [&](const std::string& url, std::string& body) -> bool {
        if (cancelled) { error = "Cancelled."; return false; }
        body.clear();
        const auto sink = [&](const char* data, size_t size) {
            if (cancelled || size > metadataLimit - body.size()) return false;
            body.append(data, size);
            return true;
        };
        return fetcher_(url, sink, cancelled, error);
    };

    if (!forceShared) {
        std::map<std::string, RetroStats> cachedStats;
        std::vector<RetroEmulatorPackage> cachedEmus;
        RetroSource sharedSource;
        if (readSharedFiles(root_, cachedStats, cachedEmus, sharedSource)) {
            const auto sources = retroCatalogSources(preferredSource);
            const bool matchesPreferred = preferredSource.empty() ||
                (!sources.empty() && sources[0].dataBaseUrl == sharedSource.dataBaseUrl);
            if (matchesPreferred) {
                std::string gamesBody;
                std::vector<RetroRelease> releases;
                if (fetchBody(sharedSource.dataBaseUrl + console->jsonFile, gamesBody) &&
                    !cancelled &&
                    parseRetroCatalog(platform, gamesBody, sharedSource, releases, error)) {
                    if (!ensureDir(root_) || !ensureDir(root_ + "/consoles")) {
                        error = "Unable to create retro cache directory.";
                        return false;
                    }
                    const std::string consoleCache = buildConsoleCacheBody(platform, sharedSource, gamesBody);
                    if (!saveWithPrevious(root_ + "/consoles/" + platform + ".json", consoleCache,
                            [&](const std::string& b) {
                                std::vector<RetroRelease> dummy;
                                RetroSource ds;
                                std::string de;
                                return decodeConsoleV2(platform, Json::parse(b, nullptr, false), dummy, ds, de);
                            },
                            error, "Unable to preserve previous retro console cache.",
                            "Unable to save retro console cache."))
                        return false;
                    snapshot.platform = platform;
                    snapshot.releases = std::move(releases);
                    snapshot.stats = std::move(cachedStats);
                    snapshot.emulators = std::move(cachedEmus);
                    snapshot.source = sharedSource;
                    error.clear();
                    return true;
                }
                if (cancelled) {
                    error = "Cancelled.";
                    return false;
                }
            }
        }
    }

    for (const auto& source : retroCatalogSources(preferredSource)) {
        std::string gamesBody, statsBody, emusBody;
        if (!fetchBody(source.dataBaseUrl + console->jsonFile, gamesBody) ||
            !fetchBody(source.dataBaseUrl + "console_games_stats.json", statsBody) ||
            !fetchBody(source.dataBaseUrl + "emulators.json", emusBody)) {
            if (cancelled) { error = "Cancelled."; return false; }
            continue;
        }
        if (cancelled) { error = "Cancelled."; return false; }

        RetroCatalogSnapshot parsed;
        parsed.platform = platform;
        parsed.source = source;
        if (!parseRetroCatalog(platform, gamesBody, source, parsed.releases, error)) continue;
        const Json statsJson = Json::parse(statsBody, nullptr, false);
        const Json emusJson = Json::parse(emusBody, nullptr, false);
        if (!parseStatsJson(statsJson, parsed.stats) ||
            !parseEmulatorsJson(emusJson, parsed.emulators)) {
            error = "Invalid retro metadata snapshot.";
            continue;
        }

        if (!ensureDir(root_) || !ensureDir(root_ + "/consoles")) {
            error = "Unable to create retro cache directory.";
            return false;
        }
        const std::string sharedCache = buildSharedCacheBody(source, statsBody, emusBody);
        const std::string consoleCache = buildConsoleCacheBody(platform, source, gamesBody);

        if (!saveWithPrevious(root_ + "/shared.json", sharedCache,
                [](const std::string& b) {
                    std::map<std::string, RetroStats> ds;
                    std::vector<RetroEmulatorPackage> de;
                    RetroSource s;
                    return decodeSharedV2(Json::parse(b, nullptr, false), ds, de, s);
                },
                error, "Unable to preserve previous retro shared cache.",
                "Unable to save retro shared cache."))
            return false;

        if (!saveWithPrevious(root_ + "/consoles/" + platform + ".json", consoleCache,
                [&](const std::string& b) {
                    std::vector<RetroRelease> dr;
                    RetroSource ds;
                    std::string de;
                    return decodeConsoleV2(platform, Json::parse(b, nullptr, false), dr, ds, de);
                },
                error, "Unable to preserve previous retro console cache.",
                "Unable to save retro console cache."))
            return false;

        snapshot = std::move(parsed);
        error.clear();
        return true;
    }
    if (error.empty()) error = "All retro catalog sources failed.";
    return false;
}

std::future<RetroCatalogResult> RetroCatalog::refreshAsync(const std::string& preferredSource,
    std::shared_ptr<std::atomic<bool>> cancelled) const {
    return refreshAsync("gba", preferredSource, true, std::move(cancelled));
}

std::future<RetroCatalogResult> RetroCatalog::refreshAsync(const std::string& platform,
    const std::string& preferredSource, bool forceShared,
    std::shared_ptr<std::atomic<bool>> cancelled) const {
    if (!cancelled) cancelled = std::make_shared<std::atomic<bool>>(false);
    return std::async(std::launch::async, [catalog = *this, platform, preferredSource, forceShared, cancelled] {
        RetroCatalogResult result;
        std::string cacheError;
        result.cached = catalog.load(platform, result.snapshot, cacheError);
        result.refreshed = catalog.refresh(platform, result.snapshot, preferredSource, forceShared,
                                           *cancelled, result.error);
        return result;
    });
}

} // namespace pipensx
