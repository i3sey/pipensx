#include "homebrew_catalog.hpp"

#include "catalog_urls.hpp"
#include "remote_archive.hpp"
#include "curl_https.hpp"
#include "core/sha256.h"
#include "core/client_identity.h"
#include <borealis/extern/nlohmann/json.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace pipensx {
namespace {
using Json = nlohmann::json;
constexpr uint64_t kMetadataLimit = 2 * 1024 * 1024;
constexpr uint64_t kDownloadLimit = 200 * 1024 * 1024;
constexpr uint64_t kExtractLimit = 512 * 1024 * 1024;

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

bool safePath(const std::string& path) {
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/') return false;
    size_t begin = 0;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        const std::string part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        for (unsigned char c : part)
            if (c < 32 || c == 127 || std::strchr("\\:*?\"<>|", c)) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool safeName(const std::string& name) {
    return !name.empty() && name.size() <= 80 && name != "." && name != ".." &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}

bool destinationAllowed(const std::string& path) {
    const std::string key = lower(path);
    return safePath(path) && (key.rfind("switch/", 0) == 0 || key.rfind("config/", 0) == 0 ||
        key.rfind("atmosphere/", 0) == 0) && key.rfind("switch/pipensx/", 0) != 0 &&
        key != "switch/pipensx.nro";
}

bool https(const std::string& url) {
    if (url.rfind("https://", 0) != 0 || url.size() <= 8) return false;
    const size_t end = url.find('/', 8);
    const std::string host = url.substr(8, end == std::string::npos ? end : end - 8);
    return !host.empty() && host.find_first_of("@?#\\ \r\n\t") == std::string::npos &&
        std::none_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}

bool directories(const std::string& path) {
    for (size_t end = 1; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != '/') continue;
        const std::string part = path.substr(0, end);
        if (part.empty() || part.back() == ':') continue;
        struct stat st{};
        if (lstat(part.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
        } else if (errno != ENOENT || mkdir(part.c_str(), 0777) != 0) return false;
    }
    return true;
}

std::string parent(const std::string& path) { return path.substr(0, path.find_last_of('/')); }

bool readFile(const std::string& path, std::string& body) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file && errno == ENOENT) file = fopen((path + ".previous").c_str(), "rb");
    if (!file) return false;
    body.clear();
    std::array<char, 8192> buffer;
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), file))) {
        if (body.size() + n > kMetadataLimit) { fclose(file); return false; }
        body.append(buffer.data(), n);
    }
    const bool ok = !ferror(file);
    fclose(file);
    return ok;
}

bool writeFile(const std::string& path, const std::string& body) {
    if (!directories(parent(path))) return false;
    FILE* file = fopen((path + ".tmp").c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(body.data(), 1, body.size(), file) == body.size();
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok && rename((path + ".tmp").c_str(), path.c_str()) != 0) {
        struct stat existing{};
        if (lstat(path.c_str(), &existing) != 0 || !S_ISREG(existing.st_mode)) {
            ok = false;
        } else {
            const std::string backup = path + ".previous";
            if ((unlink(backup.c_str()) != 0 && errno != ENOENT) ||
                rename(path.c_str(), backup.c_str()) != 0) {
                ok = false;
            } else if (rename((path + ".tmp").c_str(), path.c_str()) != 0) {
                rename(backup.c_str(), path.c_str());
                ok = false;
            }
        }
    }
    if (ok) unlink((path + ".previous").c_str());
    if (!ok) unlink((path + ".tmp").c_str());
    return ok;
}

void removeTree(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path.c_str()); return; }
    DIR* directory = opendir(path.c_str());
    if (!directory) return;
    while (auto* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") removeTree(path + "/" + name);
    }
    closedir(directory);
    rmdir(path.c_str());
}

struct Transfer {
    const HomebrewCatalog::Sink& sink;
    const std::atomic<bool>& cancelled;
    const HomebrewCatalog::Progress& progress;
    uint64_t limit, received = 0;
};

size_t receive(char* data, size_t size, size_t count, void* opaque) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    const size_t bytes = size * count;
    if (transfer.cancelled || bytes > transfer.limit - transfer.received ||
        !transfer.sink(reinterpret_cast<uint8_t*>(data), bytes)) return 0;
    transfer.received += bytes;
    return bytes;
}

int report(void* opaque, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (transfer.cancelled) return 1;
    if (transfer.progress) transfer.progress(received > 0 ? received : 0, total > 0 ? total : transfer.limit);
    return 0;
}

bool fetch(const std::string& url, uint64_t limit, const HomebrewCatalog::Sink& sink,
           const std::atomic<bool>& cancelled, const HomebrewCatalog::Progress& progress,
           std::string& error) {
    if (!https(url)) return fail(error, "Homebrew download requires HTTPS.");
    CURL* curl = curl_easy_init();
    if (!curl) return fail(error, "Unable to initialize homebrew HTTP client.");
    Transfer transfer{sink, cancelled, progress, limit};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, PIPENSX_USER_AGENT);
    curlPinHttpsOnly(curl);
    curlApplyTrustedSsl(curl);
    CurlSocketTuning socketTuning;
    curlTuneDownloadSocket(curl, &socketTuning);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1200L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, report);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (cancelled) return fail(error, "Cancelled.");
    if (result != CURLE_OK || status != 200)
        return fail(error, "Homebrew HTTP " + std::to_string(status) + ": " + curl_easy_strerror(result));
    return true;
}

std::string field(const Json& value, const char* key) {
    auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}
}

HomebrewCatalog::HomebrewCatalog(std::string appRoot, std::string sdRoot, Fetcher fetcher)
    : root_(std::move(appRoot) + "/homebrew"), sdRoot_(std::move(sdRoot)),
      fetcher_(fetcher ? std::move(fetcher) : fetch) {}

bool HomebrewCatalog::parse(const std::string& body, const std::string& language,
                           const std::string& baseUrl, std::vector<HomebrewPackage>& packages,
                           std::string& error) {
    packages.clear();
    if (body.empty() || body.size() > kMetadataLimit) return fail(error, "Homebrew catalog exceeds its size limit.");
    const auto root = Json::parse(body, nullptr, false);
    if (!root.is_object() || !root.contains("format") || root["format"] != 1 ||
        !root.contains("packages") || !root["packages"].is_array() || root["packages"].size() > 2000)
        return fail(error, "Homebrew catalog has an invalid format.");
    std::set<std::string> hidden, names;
    if (root.contains("hidden") && root["hidden"].is_array())
        for (const auto& name : root["hidden"]) if (name.is_string()) hidden.insert(lower(name.get<std::string>()));
    for (const auto& row : root["packages"]) {
        if (!row.is_object()) return fail(error, "Invalid homebrew package.");
        HomebrewPackage package;
        package.name = field(row, "name");
        if (hidden.count(lower(package.name))) continue;
        package.title = field(row, "title");
        package.author = field(row, "author");
        package.version = field(row, "version");
        package.category = field(row, "category");
        package.description = field(row, "description");
        if (row.contains("description_i18n") && row["description_i18n"].is_object()) {
            std::string key = language == "auto" ? "en" : language;
            if (key == "en-US") key = "en";
            const std::string localized = field(row["description_i18n"], key.c_str());
            if (!localized.empty()) package.description = localized;
        }
        package.changelog = field(row, "changelog");
        package.downloadUrl = field(row, "download_url");
        package.downloadType = field(row, "download_type");
        package.sha256 = lower(field(row, "sha256"));
        package.iconUrl = field(row, "icon_url");
        if (package.iconUrl.rfind("homebrew/icons/", 0) == 0 && safePath(package.iconUrl))
            package.iconUrl = baseUrl + package.iconUrl;
        if (!package.iconUrl.empty() && !https(package.iconUrl)) package.iconUrl.clear();
        if (!safeName(package.name) || !names.insert(lower(package.name)).second || package.title.empty() ||
            package.version.empty() || !https(package.downloadUrl) ||
            (package.downloadType != "file" && package.downloadType != "zip") ||
            package.sha256.size() != 64 || package.sha256.find_first_not_of("0123456789abcdef") != std::string::npos ||
            !row.contains("filesize") || !row["filesize"].is_number_unsigned() ||
            !row.contains("extracted") || !row["extracted"].is_number_unsigned() ||
            !row.contains("files") || !row["files"].is_array() || row["files"].empty() || row["files"].size() > 4096)
            return fail(error, "Invalid homebrew package: " + package.name);
        package.bytes = row["filesize"].get<uint64_t>();
        package.extractedBytes = row["extracted"].get<uint64_t>();
        if (!package.bytes || package.bytes > kDownloadLimit || !package.extractedBytes || package.extractedBytes > kExtractLimit)
            return fail(error, "Homebrew package exceeds its size limit.");
        std::set<std::string> paths;
        bool overlay = false;
        for (const auto& item : row["files"]) {
            if (!item.is_string()) return fail(error, "Invalid homebrew file list.");
            const std::string value = item.get<std::string>();
            if (value.rfind("U: ", 0) != 0 && value.rfind("G: ", 0) != 0)
                return fail(error, "Unknown homebrew file policy.");
            HomebrewFile file{value.substr(3), value[0] == 'G'};
            const std::string key = lower(file.path);
            if (!destinationAllowed(file.path) || !paths.insert(key).second)
                return fail(error, "Unsafe homebrew file path.");
            if (key.rfind("atmosphere/", 0) == 0) package.systemModule = true;
            if (key.rfind("switch/.overlays/", 0) == 0) overlay = true;
            package.files.push_back(std::move(file));
        }
        if (overlay && root.contains("overlay_dependencies") && root["overlay_dependencies"].is_array())
            for (const auto& dependency : root["overlay_dependencies"])
                if (dependency.is_string() && dependency != package.name)
                    package.dependencies.push_back(dependency.get<std::string>());
        if (package.downloadType == "file" && (package.files.size() != 1 || package.bytes != package.extractedBytes))
            return fail(error, "Invalid standalone homebrew payload.");
        packages.push_back(std::move(package));
    }
    if (packages.empty()) return fail(error, "Homebrew catalog is empty.");
    std::sort(packages.begin(), packages.end(), [](const auto& a, const auto& b) { return lower(a.title) < lower(b.title); });
    error.clear();
    return true;
}

void HomebrewCatalog::attachInstalled(std::vector<HomebrewPackage>& packages) const {
    for (auto& package : packages) {
        std::string body;
        if (!readFile(root_ + "/installed/" + package.name + ".json", body)) continue;
        const auto receipt = Json::parse(body, nullptr, false);
        bool present = true;
        for (const auto& file : package.files) {
            struct stat st{};
            if (lstat((sdRoot_ + "/" + file.path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                present = false;
                break;
            }
        }
        if (present && receipt.is_object()) package.installedVersion = field(receipt, "version");
    }
}

bool HomebrewCatalog::load(std::vector<HomebrewPackage>& packages, const std::string& language,
                          std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recover(error)) return false;
    std::string body;
    if (!readFile(root_ + "/catalog.json", body)) return fail(error, "No cached homebrew catalog.");
    if (!parse(body, language, kCatalogGitLabBaseUrl, packages, error)) return false;
    attachInstalled(packages);
    return true;
}

bool HomebrewCatalog::refresh(std::vector<HomebrewPackage>& packages, const std::string& language,
                             const std::string& sourceUrl, const std::atomic<bool>& cancelled,
                             std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& url : catalogAuxiliaryUrls("homebrew.json", sourceUrl)) {
        if (cancelled) return fail(error, "Cancelled.");
        std::string body;
        auto sink = [&](const uint8_t* data, size_t size) {
            if (size > kMetadataLimit - body.size()) return false;
            body.append(reinterpret_cast<const char*>(data), size);
            return true;
        };
        std::vector<HomebrewPackage> parsed;
        if (!fetcher_(url, kMetadataLimit, sink, cancelled, {}, error) ||
            !parse(body, language, url.substr(0, url.size() - std::strlen("homebrew.json")), parsed, error)) continue;
        if (cancelled) return fail(error, "Cancelled.");
        Json cached = Json::parse(body);
        const std::string base = url.substr(0, url.size() - std::strlen("homebrew.json"));
        for (auto& row : cached["packages"]) {
            const std::string icon = field(row, "icon_url");
            if (icon.rfind("homebrew/icons/", 0) == 0 && safePath(icon)) row["icon_url"] = base + icon;
        }
        if (!writeFile(root_ + "/catalog.json", cached.dump())) return fail(error, "Unable to save homebrew catalog.");
        attachInstalled(parsed);
        packages = std::move(parsed);
        error.clear();
        return true;
    }
    return false;
}

bool HomebrewCatalog::recover(std::string& error) const {
    std::string body;
    const std::string pending = root_ + "/pending";
    if (!readFile(pending + "/journal.json", body)) {
        struct stat st{};
        if (lstat((pending + "/journal.json").c_str(), &st) == 0 ||
            lstat((pending + "/journal.json.previous").c_str(), &st) == 0)
            return fail(error, "Unable to read interrupted homebrew installation.");
        removeTree(pending);
        return true;
    }
    const auto journal = Json::parse(body, nullptr, false);
    if (!journal.is_object() || !journal.contains("files") || !journal["files"].is_array() ||
        !journal.contains("committed") || !journal["committed"].is_boolean())
        return fail(error, "Interrupted homebrew installation requires recovery.");
    if (!journal["committed"].get<bool>()) {
        for (size_t i = 0; i < journal["files"].size(); ++i) {
            const auto& file = journal["files"][i];
            const std::string path = field(file, "path");
            if (!destinationAllowed(path) || !file.contains("existed") || !file["existed"].is_boolean())
                return fail(error, "Invalid homebrew recovery path.");
            const std::string target = sdRoot_ + "/" + path;
            const std::string backup = pending + "/backup-" + std::to_string(i);
            struct stat st{};
            if (!directories(parent(target))) return fail(error, "Unsafe homebrew recovery destination.");
            if (lstat(backup.c_str(), &st) == 0) {
                if ((unlink(target.c_str()) != 0 && errno != ENOENT) ||
                    rename(backup.c_str(), target.c_str()) != 0)
                    return fail(error, "Unable to restore homebrew file: " + path);
            } else if (!file["existed"].get<bool>() && unlink(target.c_str()) != 0 && errno != ENOENT)
                return fail(error, "Unable to remove interrupted homebrew file: " + path);
        }
    }
    if (journal["committed"].get<bool>()) {
        const std::string name = field(journal, "name");
        if (!safeName(name)) return fail(error, "Invalid homebrew recovery package.");
        const Json receipt = {{"version", field(journal, "version")}, {"sha256", field(journal, "sha256")}};
        if (!writeFile(root_ + "/installed/" + name + ".json", receipt.dump()))
            return fail(error, "Homebrew installed, but its version could not be recorded.");
    }
    removeTree(pending);
    return true;
}

bool HomebrewCatalog::install(const HomebrewPackage& package, const std::atomic<bool>& cancelled,
                             const Progress& progress, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recover(error)) return false;
    if (cancelled) return fail(error, "Cancelled.");
    if (!safeName(package.name) || package.files.empty() || !package.bytes || package.bytes > kDownloadLimit ||
        package.extractedBytes > kExtractLimit) return fail(error, "Invalid homebrew installation request.");
    const std::string pending = root_ + "/pending";
    if (!directories(pending)) return fail(error, "Unable to create homebrew staging directory.");
    struct Cleanup { std::string path; ~Cleanup() {
        if (access((path + "/journal.json").c_str(), F_OK) != 0 &&
            access((path + "/journal.json.previous").c_str(), F_OK) != 0) removeTree(path);
    }} cleanup{pending};
    struct statvfs storage{};
    if (statvfs(sdRoot_.c_str(), &storage) != 0 ||
        uint64_t(storage.f_bavail) * storage.f_frsize < package.bytes + package.extractedBytes + 1024 * 1024)
        return fail(error, "Insufficient free space for homebrew installation.");
    const std::string payload = pending + "/payload";
    FILE* output = fopen(payload.c_str(), "wb");
    if (!output) return fail(error, "Unable to create homebrew download.");
    sha256_ctx_t digest;
    sha256_init(&digest);
    uint64_t received = 0;
    auto sink = [&](const uint8_t* data, size_t size) {
        if (cancelled || size > package.bytes - received || fwrite(data, 1, size, output) != size) return false;
        sha256_update(&digest, data, size);
        received += size;
        return true;
    };
    bool ok = fetcher_(package.downloadUrl, package.bytes, sink, cancelled, progress, error);
    if (fclose(output) != 0) ok = false;
    if (cancelled) return fail(error, "Cancelled.");
    if (!ok) return fail(error, error.empty() ? "Unable to download homebrew package." : error);
    uint8_t hash[32];
    sha256_final(&digest, hash);
    std::string actual;
    for (uint8_t byte : hash) { char hex[3]; std::snprintf(hex, sizeof(hex), "%02x", byte); actual += hex; }
    if (received != package.bytes || actual != package.sha256) return fail(error, "Homebrew size or SHA-256 mismatch.");

    RangeFetcher local = [&](const std::string&, uint64_t offset, uint64_t end,
            const Sink& consumer, const std::function<bool()>& stopped, std::string& e) {
        FILE* input = fopen(payload.c_str(), "rb");
        if (!input) return fail(e, "Unable to open homebrew ZIP.");
        bool success = offset <= end && end <= package.bytes && fseeko(input, offset, SEEK_SET) == 0;
        std::array<uint8_t, 32768> buffer;
        while (success && offset < end) {
            const size_t bytes = std::min<uint64_t>(buffer.size(), end - offset);
            success = !stopped() && fread(buffer.data(), 1, bytes, input) == bytes && consumer(buffer.data(), bytes);
            offset += bytes;
        }
        fclose(input);
        return success || fail(e, "Unable to read homebrew ZIP.");
    };
    RemoteArchive archive("local", package.bytes, local);
    auto stopped = [&] { return cancelled.load(); };
    if (package.downloadType == "zip" && !archive.open(stopped, error)) return false;
    std::vector<size_t> members;
    uint64_t extracted = 0;
    std::set<std::string> destinations;
    Json journal = {{"committed", false}, {"files", Json::array()},
                    {"name", package.name}, {"version", package.version}, {"sha256", package.sha256}};
    std::vector<std::string> archivePaths;
    if (package.downloadType == "zip") {
        for (const auto& member : archive.entries()) {
            std::string path = member.path;
            std::replace(path.begin(), path.end(), '\\', '/');
            if (!path.empty() && path.back() != '/' && !safePath(path))
                return fail(error, "Unsafe homebrew ZIP member path.");
            archivePaths.push_back(std::move(path));
        }
        std::string prefix;
        for (int level = 0; level < 2; ++level) {
            std::set<std::string> roots;
            bool nested = true;
            for (const auto& path : archivePaths) {
                if (path.empty() || path.back() == '/' || path.rfind("__MACOSX/", 0) == 0) continue;
                const std::string relative = path.substr(prefix.size());
                const size_t slash = relative.find('/');
                if (slash == std::string::npos) { nested = false; break; }
                roots.insert(relative.substr(0, slash));
            }
            if (!nested || roots.size() != 1) break;
            const std::string top = lower(*roots.begin());
            const std::set<std::string> sdFolders = {"switch", "atmosphere", "config", "bootloader",
                "themes", "roms", "tico", "emummc", "retroarch", "payloads", "nintendo", "sxos", "ftpd", "lakka"};
            if (sdFolders.count(top)) break;
            prefix += *roots.begin() + "/";
        }
        for (auto& path : archivePaths) {
            if (path.rfind(prefix, 0) == 0) path.erase(0, prefix.size());
            else path.clear();
        }
    }
    for (size_t i = 0; i < package.files.size(); ++i) {
        const auto& file = package.files[i];
        const std::string key = lower(file.path);
        if (!destinationAllowed(file.path) || !destinations.insert(key).second)
            return fail(error, "Unsafe homebrew destination.");
        const std::string target = sdRoot_ + "/" + file.path;
        if (!directories(parent(target))) return fail(error, "Unsafe or unavailable homebrew destination.");
        struct stat st{};
        const bool existed = lstat(target.c_str(), &st) == 0;
        if (existed && !S_ISREG(st.st_mode)) return fail(error, "Homebrew destination is not a regular file.");
        journal["files"].push_back({{"path", file.path}, {"existed", existed}, {"preserve", file.preserve && existed}});
        if (package.downloadType == "zip") {
            size_t found = archive.entries().size();
            for (size_t j = 0; j < archive.entries().size(); ++j) {
                if (archivePaths[j] != file.path) continue;
                if (found != archive.entries().size()) return fail(error, "Duplicate homebrew ZIP member.");
                found = j;
            }
            if (found == archive.entries().size() || archive.entries()[found].symlink ||
                archive.entries()[found].bytes > package.extractedBytes - extracted || !archive.prepare(found, stopped, error))
                return fail(error, "Missing, unsafe or oversized homebrew ZIP member: " + file.path);
            extracted += archive.entries()[found].bytes;
            members.push_back(found);
        }
    }
    for (size_t i = 0; i < package.files.size(); ++i) {
        if (cancelled) return fail(error, "Cancelled.");
        if (journal["files"][i]["preserve"].get<bool>()) continue;
        const std::string staged = pending + "/new-" + std::to_string(i);
        if (package.downloadType == "file") {
            if (package.files.size() != 1 || rename(payload.c_str(), staged.c_str()) != 0)
                return fail(error, "Unable to stage homebrew file.");
        } else if (package.downloadType == "zip") {
            FILE* file = fopen(staged.c_str(), "wb");
            if (!file) return fail(error, "Unable to stage homebrew ZIP member.");
            const bool unpacked = archive.read(members[i], 0, archive.entries()[members[i]].bytes,
                [&](const uint8_t* data, size_t size) { return fwrite(data, 1, size, file) == size; }, stopped, error);
            const bool closed = fclose(file) == 0;
            if (!unpacked || !closed) return fail(error, "Unable to extract homebrew ZIP member: " + package.files[i].path);
        } else return fail(error, "Unsupported homebrew download type.");
        FILE* stagedFile = fopen(staged.c_str(), "rb+");
        if (!stagedFile) return fail(error, "Unable to reopen staged homebrew file.");
        const bool synced = fsync(fileno(stagedFile)) == 0;
        const bool closed = fclose(stagedFile) == 0;
        if (!synced || !closed) return fail(error, "Unable to flush staged homebrew file.");
    }
    if (cancelled) return fail(error, "Cancelled.");
    if (!writeFile(pending + "/journal.json", journal.dump())) return fail(error, "Unable to save homebrew recovery journal.");
    for (size_t i = 0; i < package.files.size(); ++i) {
        if (journal["files"][i]["preserve"].get<bool>()) continue;
        const std::string target = sdRoot_ + "/" + package.files[i].path;
        if (cancelled || (journal["files"][i]["existed"].get<bool>() &&
            rename(target.c_str(), (pending + "/backup-" + std::to_string(i)).c_str()) != 0) ||
            rename((pending + "/new-" + std::to_string(i)).c_str(), target.c_str()) != 0) {
            std::string recoveryError;
            if (!recover(recoveryError)) return fail(error, recoveryError);
            return fail(error, cancelled ? "Cancelled." : "Unable to commit homebrew files.");
        }
    }
    journal["committed"] = true;
    if (!writeFile(pending + "/journal.json", journal.dump())) {
        std::string recoveryError;
        if (!recover(recoveryError)) return fail(error, recoveryError);
        return fail(error, "Unable to commit homebrew recovery journal.");
    }
    if (!recover(error)) return false;
    error.clear();
    return true;
}

namespace {

namespace fs = std::filesystem;

constexpr size_t kNoMatch = std::numeric_limits<size_t>::max();
constexpr size_t kAmbiguous = std::numeric_limits<size_t>::max() - 1;

struct NroMetadata {
    std::string title;
    std::string author;
    std::string version;
    uint64_t iconOffset = 0;
    uint64_t iconSize = 0;
};

struct DiscoveredNro {
    fs::path fullPath;
    std::string relPath;
    std::string stem;
    std::string filename;
    NroMetadata meta;
};

struct CatalogIndex {
    std::unordered_map<std::string, size_t> byExactPath;
    std::unordered_map<std::string, size_t> byTitleAndAuthor;
    std::unordered_map<std::string, size_t> byTitle;
    std::unordered_map<std::string, size_t> byNroBasename;
};

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    return static_cast<uint64_t>(readLe32(p)) |
           (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

std::string trimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string extractBoundedString(const uint8_t* data, size_t maxLen) {
    const void* nul = std::memchr(data, '\0', maxLen);
    const size_t len = nul ? static_cast<size_t>(static_cast<const uint8_t*>(nul) - data) : maxLen;
    return trimAscii(std::string(reinterpret_cast<const char*>(data), len));
}

bool readAt(std::ifstream& in, uint64_t offset, uint8_t* dst, size_t len, uint64_t fileSize) {
    if (offset > fileSize || len > fileSize - offset) {
        return false;
    }
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        return false;
    }
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
    return in.good() && static_cast<size_t>(in.gcount()) == len;
}

NroMetadata parseNroMetadata(const fs::path& path) {
    NroMetadata meta;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return meta;
    }

    in.seekg(0, std::ios::end);
    const std::streampos endPos = in.tellg();
    if (endPos < 0) {
        return meta;
    }
    const uint64_t fileSize = static_cast<uint64_t>(endPos);

    constexpr size_t kNroHeaderMin = 0x20;
    std::array<uint8_t, kNroHeaderMin> nroHdr{};
    if (!readAt(in, 0, nroHdr.data(), nroHdr.size(), fileSize)) {
        return meta;
    }
    if (nroHdr[0x10] != 'N' || nroHdr[0x11] != 'R' || nroHdr[0x12] != 'O' || nroHdr[0x13] != '0') {
        return meta;
    }

    const uint64_t imageSize = readLe32(nroHdr.data() + 0x18);
    if (imageSize < kNroHeaderMin || imageSize > fileSize) {
        return meta;
    }

    constexpr size_t kAsetHeaderSize = 0x38;
    std::array<uint8_t, kAsetHeaderSize> asetHdr{};
    if (!readAt(in, imageSize, asetHdr.data(), asetHdr.size(), fileSize)) {
        return meta;
    }
    if (asetHdr[0] != 'A' || asetHdr[1] != 'S' || asetHdr[2] != 'E' || asetHdr[3] != 'T') {
        return meta;
    }

    constexpr uint64_t kMaxNroIconBytes = 2 * 1024 * 1024;
    const uint64_t iconRel = readLe64(asetHdr.data() + 0x08);
    const uint64_t iconSize = readLe64(asetHdr.data() + 0x10);
    if (iconSize >= 8 && iconSize <= kMaxNroIconBytes &&
        iconRel >= kAsetHeaderSize &&
        iconRel <= fileSize - imageSize &&
        iconSize <= fileSize - (imageSize + iconRel)) {
        meta.iconOffset = imageSize + iconRel;
        meta.iconSize = iconSize;
    }

    const uint64_t nacpRel = readLe64(asetHdr.data() + 0x18);
    const uint64_t nacpSize = readLe64(asetHdr.data() + 0x20);
    if (nacpSize == 0 || nacpRel > fileSize - imageSize) {
        return meta;
    }
    const uint64_t nacpOffset = imageSize + nacpRel;
    if (nacpSize > fileSize - nacpOffset) {
        return meta;
    }

    constexpr size_t kSlotCount = 16;
    constexpr size_t kSlotSize = 0x300;
    constexpr size_t kNameLen = 0x200;
    constexpr size_t kAuthorLen = 0x100;
    std::array<uint8_t, kSlotSize> slot{};

    for (size_t i = 0; i < kSlotCount && (meta.title.empty() || meta.author.empty()); ++i) {
        const uint64_t slotRel = static_cast<uint64_t>(i) * kSlotSize;
        if (slotRel >= nacpSize) {
            break;
        }
        const size_t avail = static_cast<size_t>(std::min<uint64_t>(kSlotSize, nacpSize - slotRel));
        if (!readAt(in, nacpOffset + slotRel, slot.data(), avail, fileSize)) {
            break;
        }
        if (meta.title.empty()) {
            meta.title = extractBoundedString(slot.data(), std::min(avail, kNameLen));
        }
        if (meta.author.empty() && avail > kNameLen) {
            meta.author = extractBoundedString(slot.data() + kNameLen,
                                               std::min(avail - kNameLen, kAuthorLen));
        }
    }

    constexpr uint64_t kDisplayVersionOffset = 0x3060;
    constexpr size_t kDisplayVersionLen = 16;
    if (kDisplayVersionOffset < nacpSize) {
        std::array<uint8_t, kDisplayVersionLen> verBuf{};
        const size_t verAvail = static_cast<size_t>(
            std::min<uint64_t>(kDisplayVersionLen, nacpSize - kDisplayVersionOffset));
        if (readAt(in, nacpOffset + kDisplayVersionOffset, verBuf.data(), verAvail, fileSize)) {
            meta.version = extractBoundedString(verBuf.data(), verAvail);
        }
    }

    return meta;
}

std::string normalizeRelPathDisplay(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    bool prevSlash = false;
    for (char ch : raw) {
        const char c = (ch == '\\') ? '/' : ch;
        if (c == '/') {
            if (!out.empty() && !prevSlash) {
                out.push_back('/');
            }
            prevSlash = true;
        } else {
            out.push_back(c);
            prevSlash = false;
        }
    }
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/') {
        out.erase(0, 2);
        while (!out.empty() && out[0] == '/') {
            out.erase(0, 1);
        }
    }
    return out;
}

std::string normalizeRelPathKey(const std::string& raw) {
    return lower(normalizeRelPathDisplay(raw));
}

std::string normalizeToken(const std::string& raw) {
    const std::string lowered = lower(raw);
    std::string out;
    out.reserve(lowered.size());
    for (unsigned char c : lowered) {
        if ((c & 0x80u) != 0 || std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool isDistinctiveToken(const std::string& normToken) {
    if (normToken.size() < 3) {
        return false;
    }
    static const std::unordered_set<std::string> kGeneric = {
        "main", "app", "switch", "homebrew", "boot", "loader", "menu",
        "index", "default", "nro", "bin", "release", "debug", "test",
        "game", "tool", "launcher", "installer", "updater", "demo", "sample"
    };
    return kGeneric.find(normToken) == kGeneric.end();
}

void insertUnique(std::unordered_map<std::string, size_t>& map, const std::string& key, size_t idx) {
    if (key.empty()) {
        return;
    }
    auto [it, inserted] = map.emplace(key, idx);
    if (!inserted && it->second != idx) {
        it->second = kAmbiguous;
    }
}

CatalogIndex buildCatalogIndex(const std::vector<HomebrewPackage>& catalog) {
    CatalogIndex index;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const HomebrewPackage& pkg = catalog[i];
        const std::string normAuthor = normalizeToken(pkg.author);

        for (const std::string& label : {pkg.title, pkg.name}) {
            const std::string normLabel = normalizeToken(label);
            if (normLabel.empty()) {
                continue;
            }
            insertUnique(index.byTitle, normLabel, i);
            if (!normAuthor.empty()) {
                insertUnique(index.byTitleAndAuthor, normLabel + '\0' + normAuthor, i);
            }
        }

        for (const HomebrewFile& file : pkg.files) {
            const std::string normPath = normalizeRelPathKey(file.path);
            if (normPath.empty()) {
                continue;
            }
            insertUnique(index.byExactPath, normPath, i);

            const fs::path fp(normPath);
            if (lower(fp.extension().string()) == ".nro") {
                const std::string normStem = normalizeToken(fp.stem().string());
                if (isDistinctiveToken(normStem)) {
                    insertUnique(index.byNroBasename, normStem, i);
                }
            }
        }
    }
    return index;
}

size_t matchCatalog(const CatalogIndex& index, const DiscoveredNro& disc) {
    if (auto it = index.byExactPath.find(normalizeRelPathKey(disc.relPath));
        it != index.byExactPath.end()) {
        return it->second == kAmbiguous ? kNoMatch : it->second;
    }

    const std::string normTitle = normalizeToken(disc.meta.title);
    const std::string normAuthor = normalizeToken(disc.meta.author);
    if (!normTitle.empty()) {
        if (!normAuthor.empty()) {
            if (auto it = index.byTitleAndAuthor.find(normTitle + '\0' + normAuthor);
                it != index.byTitleAndAuthor.end()) {
                return it->second == kAmbiguous ? kNoMatch : it->second;
            }
        }
        if (isDistinctiveToken(normTitle)) {
            if (auto it = index.byTitle.find(normTitle); it != index.byTitle.end()) {
                return it->second == kAmbiguous ? kNoMatch : it->second;
            }
        }
    }

    const std::string normStem = normalizeToken(disc.stem);
    if (isDistinctiveToken(normStem)) {
        if (auto it = index.byNroBasename.find(normStem); it != index.byNroBasename.end()) {
            return it->second == kAmbiguous ? kNoMatch : it->second;
        }
    }

    return kNoMatch;
}

bool enumerateNros(const fs::path& sdRoot, std::vector<DiscoveredNro>& out, std::string& error) {
    const fs::path switchRoot = sdRoot / "switch";
    std::error_code ec;
    const fs::file_status rootStatus = fs::symlink_status(switchRoot, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return true;
        }
        error = "Failed to inspect " + switchRoot.string() + ": " + ec.message();
        return false;
    }
    if (fs::is_symlink(rootStatus) || !fs::is_directory(rootStatus)) {
        return true;
    }

    std::vector<fs::path> pending;
    pending.push_back(switchRoot);
    bool isRoot = true;

    while (!pending.empty()) {
        const fs::path dir = std::move(pending.back());
        pending.pop_back();

        std::vector<fs::path> subdirs;
        {
            fs::directory_iterator it(dir, ec);
            if (ec) {
                if (isRoot) {
                    error = "Failed to read " + dir.string() + ": " + ec.message();
                    return false;
                }
                ec.clear();
                continue;
            }
            isRoot = false;

            const fs::directory_iterator end;
            while (it != end) {
                const fs::path entryPath = it->path();
                std::error_code stEc;
                const fs::file_status st = fs::symlink_status(entryPath, stEc);
                if (!stEc && !fs::is_symlink(st)) {
                    if (fs::is_directory(st)) {
                        subdirs.push_back(entryPath);
                    } else if (fs::is_regular_file(st) &&
                               lower(entryPath.extension().string()) == ".nro") {
                        const std::string relSub = entryPath.lexically_relative(switchRoot).generic_string();
                        if (!relSub.empty()) {
                            DiscoveredNro item;
                            item.fullPath = entryPath;
                            item.relPath = normalizeRelPathDisplay("switch/" + relSub);
                            item.stem = entryPath.stem().string();
                            item.filename = entryPath.filename().string();
                            out.push_back(std::move(item));
                        }
                    }
                }

                std::error_code incEc;
                it.increment(incEc);
                if (incEc) {
                    break;
                }
            }
        }

        for (auto& sub : subdirs) {
            pending.push_back(std::move(sub));
        }
    }

    return true;
}

bool isImageHeader(const uint8_t* data, size_t size) {
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return true;
    }
    return size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
           data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
}

std::string nroIconCachePath(const std::string& iconDir, const std::string& relPath) {
    const std::string key = lower(relPath);
    uint8_t digest[32]{};
    sha256(key.data(), key.size(), digest);
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(32);
    for (size_t i = 0; i < 16; ++i) {
        hex.push_back(kHex[digest[i] >> 4]);
        hex.push_back(kHex[digest[i] & 0x0F]);
    }
    return iconDir + "/" + hex + ".jpg";
}

bool writeBinaryAtomic(const std::string& path, const uint8_t* data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE* file = fopen(tmp.c_str(), "wb");
    if (!file) {
        return false;
    }
    bool ok = fwrite(data, 1, size, file) == size;
    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) {
        if ((unlink(path.c_str()) != 0 && errno != ENOENT) ||
            rename(tmp.c_str(), path.c_str()) != 0) {
            ok = false;
        }
    }
    if (!ok) {
        unlink(tmp.c_str());
    }
    return ok;
}

std::string extractNroIconToCache(const DiscoveredNro& disc, const std::string& iconDir) {
    if (iconDir.empty() || disc.relPath.empty()) {
        return {};
    }
    const std::string iconPath = nroIconCachePath(iconDir, disc.relPath);
    if (disc.meta.iconSize == 0) {
        unlink(iconPath.c_str());
        return {};
    }

    struct stat nroSt{};
    struct stat iconSt{};
    if (stat(iconPath.c_str(), &iconSt) == 0 && S_ISREG(iconSt.st_mode) &&
        iconSt.st_size == static_cast<off_t>(disc.meta.iconSize) &&
        stat(disc.fullPath.c_str(), &nroSt) == 0 &&
        iconSt.st_mtime >= nroSt.st_mtime) {
        return iconPath;
    }

    std::ifstream in(disc.fullPath, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streampos endPos = in.tellg();
    if (endPos < 0) {
        return {};
    }
    const uint64_t fileSize = static_cast<uint64_t>(endPos);
    if (disc.meta.iconSize > std::numeric_limits<size_t>::max()) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(disc.meta.iconSize));
    if (!readAt(in, disc.meta.iconOffset, bytes.data(), bytes.size(), fileSize) ||
        !isImageHeader(bytes.data(), bytes.size())) {
        unlink(iconPath.c_str());
        return {};
    }
    if (!directories(iconDir) || !writeBinaryAtomic(iconPath, bytes.data(), bytes.size())) {
        return {};
    }
    return iconPath;
}

bool parseHomebrewVersion(const std::string& raw,
                          std::vector<uint64_t>& parts,
                          bool& prerelease) {
    parts.clear();
    prerelease = false;
    std::string s = trimAscii(raw);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
        s.erase(s.begin());
    }
    if (s.empty()) {
        return false;
    }
    const size_t sep = s.find_first_of("-+");
    if (sep != std::string::npos) {
        prerelease = (s[sep] == '-');
        s.resize(sep);
    }
    if (s.empty()) {
        return false;
    }
    size_t start = 0;
    while (start <= s.size()) {
        const size_t dot = s.find('.', start);
        const std::string token = s.substr(start, dot == std::string::npos ? dot : dot - start);
        if (token.empty() || !std::all_of(token.begin(), token.end(),
                                          [](unsigned char c) { return std::isdigit(c); })) {
            return false;
        }
        uint64_t value = 0;
        for (unsigned char c : token) {
            const uint64_t digit = static_cast<uint64_t>(c - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
        parts.push_back(value);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return !parts.empty();
}

} // namespace

std::string homebrewMonogram(const std::string& title) {
    size_t offset = 0;
    while (offset < title.size() &&
           std::isspace(static_cast<unsigned char>(title[offset]))) {
        ++offset;
    }
    if (offset == title.size()) {
        return "?";
    }
    const unsigned char lead = static_cast<unsigned char>(title[offset]);
    if (lead < 0x80) {
        return std::string(1, static_cast<char>(std::toupper(lead)));
    }
    if ((lead & 0xE0) == 0xC0 && offset + 1 < title.size()) {
        const unsigned char b1 = static_cast<unsigned char>(title[offset + 1]);
        if ((b1 & 0xC0) != 0x80) {
            return "?";
        }
        uint32_t cp = (static_cast<uint32_t>(lead & 0x1F) << 6) | (b1 & 0x3F);
        if ((cp >= 0x00E0 && cp <= 0x00F6) || (cp >= 0x00F8 && cp <= 0x00FE) ||
            (cp >= 0x0430 && cp <= 0x044F)) {
            cp -= 0x20;
        } else if (cp == 0x0451 || cp == 0x0454 || cp == 0x0456 ||
                   cp == 0x0457 || cp == 0x045E) {
            cp -= 0x50;
        }
        return std::string{static_cast<char>(0xC0 | (cp >> 6)),
                           static_cast<char>(0x80 | (cp & 0x3F))};
    }
    if ((lead & 0xF0) == 0xE0 && offset + 2 < title.size() &&
        (static_cast<unsigned char>(title[offset + 1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(title[offset + 2]) & 0xC0) == 0x80) {
        return title.substr(offset, 3);
    }
    if ((lead & 0xF8) == 0xF0 && offset + 3 < title.size() &&
        (static_cast<unsigned char>(title[offset + 1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(title[offset + 2]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(title[offset + 3]) & 0xC0) == 0x80) {
        return title.substr(offset, 4);
    }
    return "?";
}

bool isHomebrewUpdateAvailable(const std::string& installedVersion,
                               const std::string& latestVersion) {
    std::vector<uint64_t> installedParts;
    std::vector<uint64_t> latestParts;
    bool installedPre = false;
    bool latestPre = false;
    if (!parseHomebrewVersion(installedVersion, installedParts, installedPre) ||
        !parseHomebrewVersion(latestVersion, latestParts, latestPre)) {
        return false;
    }
    const size_t n = std::max(installedParts.size(), latestParts.size());
    installedParts.resize(n, 0);
    latestParts.resize(n, 0);
    if (latestParts != installedParts) {
        return latestParts > installedParts;
    }
    return installedPre && !latestPre;
}

bool HomebrewCatalog::loadInstalled(std::vector<HomebrewPackage>& packages,
                                    const std::string& language,
                                    std::string& error) const {
    packages.clear();
    error.clear();

    std::vector<HomebrewPackage> catalog;
    std::string loadError;
    const bool hasCatalog = load(catalog, language, loadError);

    std::vector<DiscoveredNro> discovered;
    if (!enumerateNros(fs::path(sdRoot_), discovered, error)) {
        return false;
    }

    std::sort(discovered.begin(), discovered.end(),
              [](const DiscoveredNro& a, const DiscoveredNro& b) {
                  const std::string la = lower(a.relPath);
                  const std::string lb = lower(b.relPath);
                  return la != lb ? la < lb : a.relPath < b.relPath;
              });

    const CatalogIndex index = hasCatalog ? buildCatalogIndex(catalog) : CatalogIndex{};
    std::vector<bool> catalogMatched(catalog.size(), false);
    const std::string nroIconDir = root_ + "/nro-icons";

    packages.reserve(discovered.size());
    for (DiscoveredNro& disc : discovered) {
        disc.meta = parseNroMetadata(disc.fullPath);

        const size_t matchIdx = hasCatalog ? matchCatalog(index, disc) : kNoMatch;
        if (matchIdx != kNoMatch) {
            catalogMatched[matchIdx] = true;
            HomebrewPackage pkg = catalog[matchIdx];
            pkg.installedPath = disc.relPath;
            if (!disc.meta.version.empty()) {
                pkg.installedVersion = disc.meta.version;
            }
            if (pkg.iconUrl.empty()) {
                pkg.iconUrl = extractNroIconToCache(disc, nroIconDir);
            }
            packages.push_back(std::move(pkg));
        } else {
            HomebrewPackage pkg;
            pkg.name = disc.relPath;
            pkg.title = !disc.meta.title.empty()
                            ? disc.meta.title
                            : (!disc.stem.empty() ? disc.stem : disc.filename);
            pkg.author = disc.meta.author;
            pkg.version = disc.meta.version;
            pkg.installedVersion = disc.meta.version;
            pkg.installedPath = disc.relPath;
            pkg.iconUrl = extractNroIconToCache(disc, nroIconDir);
            packages.push_back(std::move(pkg));
        }
    }

    if (hasCatalog) {
        for (size_t i = 0; i < catalog.size(); ++i) {
            if (!catalog[i].installedVersion.empty() && !catalogMatched[i]) {
                packages.push_back(catalog[i]);
            }
        }
    }

    return true;
}

namespace {

bool isHomebrewSaveOrConfig(const std::string& relPath) {
    const std::string low = lower(relPath);
    size_t pos = 0;
    while (pos < low.size()) {
        const size_t slash = low.find('/', pos);
        const std::string seg = low.substr(pos, slash == std::string::npos ? slash : slash - pos);
        if (slash != std::string::npos) {
            if (seg == "save" || seg == "saves" || seg == "savedata" ||
                seg == "config" || seg == "configs" || seg == "userdata" || seg == "profiles") {
                return true;
            }
            pos = slash + 1;
        } else {
            break;
        }
    }
    const size_t slash = low.find_last_of('/');
    const size_t dot = low.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string ext = low.substr(dot);
        for (const char* keep : {".sav", ".srm", ".eep", ".fla", ".sra", ".save", ".state",
                                 ".cfg", ".ini", ".json", ".toml", ".yaml", ".yml"}) {
            if (ext == keep) return true;
        }
    }
    return false;
}

void pruneEmptyHomebrewParents(const std::string& sdRoot, const std::string& fullFilePath) {
    auto isProtectedDir = [&](const std::string& dir) {
        if (dir.size() <= sdRoot.size() || dir.rfind(sdRoot, 0) != 0) return true;
        std::string rel = dir.substr(sdRoot.size());
        while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        const std::string low = lower(rel);
        return low.empty() || low == "switch" || low == "config" ||
               low == "atmosphere" || low == "atmosphere/contents";
    };
    std::string cur = fullFilePath;
    while (true) {
        const size_t slash = cur.find_last_of('/');
        if (slash == std::string::npos) break;
        cur.resize(slash);
        if (isProtectedDir(cur)) break;
        if (rmdir(cur.c_str()) != 0) break;
    }
}

bool removeTreePreservingHomebrewSaves(const std::string& fullDir, const std::string& relDir,
                                       bool& keptAny, size_t& removedFiles, unsigned depth = 0) {
    if (depth > 16) return false;
    DIR* d = opendir(fullDir.c_str());
    if (!d) return errno == ENOENT;
    bool ok = true;
    bool childKept = false;
    while (auto* ent = readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        const std::string childFull = fullDir + "/" + name;
        const std::string childRel = relDir.empty() ? name : (relDir + "/" + name);
        struct stat st{};
        if (lstat(childFull.c_str(), &st) != 0) {
            if (errno != ENOENT) ok = false;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            childKept = true;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            const std::string lowName = lower(name);
            if (lowName == "save" || lowName == "saves" || lowName == "savedata" ||
                lowName == "config" || lowName == "configs" || lowName == "userdata" ||
                lowName == "profiles") {
                childKept = true;
                continue;
            }
            bool subKept = false;
            if (!removeTreePreservingHomebrewSaves(childFull, childRel, subKept, removedFiles, depth + 1))
                ok = false;
            if (subKept) childKept = true;
        } else if (S_ISREG(st.st_mode)) {
            if (isHomebrewSaveOrConfig(childRel)) {
                childKept = true;
                continue;
            }
            if (unlink(childFull.c_str()) == 0) {
                ++removedFiles;
            } else if (errno != ENOENT) {
                ok = false;
            }
        }
    }
    closedir(d);
    if (childKept) {
        keptAny = true;
        return ok;
    }
    if (rmdir(fullDir.c_str()) != 0 && errno != ENOENT && errno != ENOTEMPTY)
        ok = false;
    return ok;
}

} // namespace

bool HomebrewCatalog::uninstall(const HomebrewPackage& package, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();

    const std::string lowName = lower(package.name);
    const std::string lowPath = lower(package.installedPath);
    if (lowName == "pipensx" || lowPath == "switch/pipensx.nro" ||
        lowPath.rfind("switch/pipensx/", 0) == 0) {
        return fail(error, "Cannot uninstall pipensx from inside itself.");
    }

    size_t removedFiles = 0;
    bool hadFailure = false;

    auto unlinkRel = [&](const std::string& rel) {
        if (!destinationAllowed(rel) || isHomebrewSaveOrConfig(rel))
            return;
        const std::string full = sdRoot_ + "/" + rel;
        struct stat st{};
        if (lstat(full.c_str(), &st) != 0) {
            if (errno != ENOENT) hadFailure = true;
            return;
        }
        if (S_ISREG(st.st_mode)) {
            if (unlink(full.c_str()) == 0) {
                ++removedFiles;
                pruneEmptyHomebrewParents(sdRoot_, full);
            } else if (errno != ENOENT) {
                hadFailure = true;
            }
        }
    };

    if (!package.files.empty()) {
        for (const auto& file : package.files) {
            if (file.preserve) continue;
            unlinkRel(file.path);
        }
        if (!package.installedPath.empty())
            unlinkRel(package.installedPath);
    } else if (!package.installedPath.empty()) {
        if (!destinationAllowed(package.installedPath) ||
            lowPath.rfind("switch/", 0) != 0 ||
            lowPath.rfind("switch/.overlays/", 0) == 0 ||
            lowPath.rfind("switch/.packages/", 0) == 0) {
            return fail(error, "Unsupported homebrew path for removal.");
        }
        const std::string afterSwitch = package.installedPath.substr(7);
        const size_t slash = afterSwitch.find('/');
        if (slash == std::string::npos) {
            unlinkRel(package.installedPath);
        } else {
            const std::string appDir = afterSwitch.substr(0, slash);
            const std::string relDir = "switch/" + appDir;
            const std::string fullDir = sdRoot_ + "/" + relDir;
            bool keptAny = false;
            if (!removeTreePreservingHomebrewSaves(fullDir, relDir, keptAny, removedFiles))
                hadFailure = true;
        }
    } else {
        return fail(error, "No installed homebrew files to remove.");
    }

    if (safeName(package.name)) {
        const std::string receipt = root_ + "/installed/" + package.name + ".json";
        unlink(receipt.c_str());
        unlink((receipt + ".previous").c_str());
    }

    if (hadFailure)
        return fail(error, "Some homebrew files could not be removed.");
    return true;
}

} // namespace pipensx
