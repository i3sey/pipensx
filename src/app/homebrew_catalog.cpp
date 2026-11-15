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
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1200L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
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

} // namespace pipensx
