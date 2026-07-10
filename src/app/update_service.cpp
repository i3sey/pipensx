#include "update_service.hpp"

#include <curl/curl.h>
#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "core/sha256.h"
}

namespace pipensx {
namespace {

#ifndef PIPENSX_VERSION
#define PIPENSX_VERSION "0.0.0"
#endif

constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/i3sey/pipensx/releases/latest";
constexpr const char* kReleaseAssetPrefix =
    "https://github.com/i3sey/pipensx/releases/download/";
constexpr size_t kMetadataLimit = 512 * 1024;
constexpr size_t kChecksumLimit = 1024;
constexpr size_t kNroLimit = 64 * 1024 * 1024;

struct HttpBuffer {
    std::string data;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeString(char* bytes, size_t size, size_t count, void* opaque) {
    auto* buffer = static_cast<HttpBuffer*>(opaque);
    const size_t received = size * count;
    if (received > buffer->limit - std::min(buffer->limit, buffer->data.size())) {
        buffer->overflow = true;
        return 0;
    }
    buffer->data.append(bytes, received);
    return received;
}

struct FileWriter {
    std::ofstream output;
    size_t written = 0;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeFile(char* bytes, size_t size, size_t count, void* opaque) {
    auto* writer = static_cast<FileWriter*>(opaque);
    const size_t received = size * count;
    if (received > writer->limit - std::min(writer->limit, writer->written)) {
        writer->overflow = true;
        return 0;
    }
    writer->output.write(bytes, static_cast<std::streamsize>(received));
    if (!writer->output.good())
        return 0;
    writer->written += received;
    return received;
}

bool configureCurl(CURL* curl, const std::string& url, std::string& error) {
    if (!curl) {
        error = "Unable to initialize updater HTTP client.";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/" PIPENSX_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    return true;
}

bool fetchText(const std::string& url, size_t limit, std::string& body,
               std::string& error) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!configureCurl(curl, url, error))
        return false;
    HttpBuffer buffer;
    buffer.limit = limit;
    curl_slist* headers = curl_slist_append(nullptr,
        "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (buffer.overflow)
        error = "Update response exceeded its size limit.";
    else if (result != CURLE_OK)
        error = std::string("Update network error: ") + curl_easy_strerror(result);
    else if (status < 200 || status >= 300)
        error = "Update server returned HTTP " + std::to_string(status) + ".";
    else {
        body = std::move(buffer.data);
        return true;
    }
    return false;
}

bool fetchFile(const std::string& url, const std::string& path, size_t limit,
               std::string& error) {
    FileWriter writer;
    writer.output.open(path, std::ios::binary | std::ios::trunc);
    writer.limit = limit;
    if (!writer.output) {
        error = "Unable to create update download.";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!configureCurl(curl, url, error)) {
        unlink(path.c_str());
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    writer.output.close();
    if (writer.overflow)
        error = "Update download exceeded its size limit.";
    else if (result != CURLE_OK)
        error = std::string("Update download failed: ") + curl_easy_strerror(result);
    else if (status < 200 || status >= 300)
        error = "Update download returned HTTP " + std::to_string(status) + ".";
    else
        return true;
    unlink(path.c_str());
    return false;
}

bool trustedAssetUrl(const std::string& url) {
    return url.compare(0, std::strlen(kReleaseAssetPrefix),
                       kReleaseAssetPrefix) == 0;
}

bool parseVersion(const std::string& text, std::array<uint64_t, 3>& version) {
    std::string value = text;
    if (!value.empty() && value.front() == 'v')
        value.erase(value.begin());
    size_t start = 0;
    for (size_t index = 0; index < version.size(); ++index) {
        size_t end = value.find('.', start);
        if ((index + 1 == version.size()) != (end == std::string::npos))
            return false;
        const std::string part = value.substr(start, end - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(),
            [](unsigned char c) { return std::isdigit(c); }))
            return false;
        try {
            version[index] = std::stoull(part);
        } catch (...) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool parseChecksum(const std::string& text, std::string& checksum) {
    std::istringstream input(text);
    std::string token;
    input >> token;
    if (token.size() != 64 || !std::all_of(token.begin(), token.end(),
        [](unsigned char c) { return std::isxdigit(c); }))
        return false;
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    checksum = std::move(token);
    return true;
}

bool checksumFile(const std::string& path, std::string& checksum,
                  std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Unable to read downloaded update.";
        return false;
    }
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kNroLimit)) {
        error = "Downloaded update is empty or too large.";
        return false;
    }
    input.seekg(0);
    std::vector<unsigned char> data(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), size);
    if (!input) {
        error = "Unable to read downloaded update.";
        return false;
    }
    unsigned char digest[32];
    sha256(data.data(), data.size(), digest);
    static const char digits[] = "0123456789abcdef";
    checksum.clear();
    checksum.reserve(64);
    for (unsigned char byte : digest) {
        checksum.push_back(digits[byte >> 4]);
        checksum.push_back(digits[byte & 15]);
    }
    return true;
}

} // namespace

UpdateService::UpdateService(std::string targetPath,
                             MetadataFetcher metadataFetcher,
                             AssetFetcher assetFetcher)
    : targetPath_(std::move(targetPath)),
      metadataFetcher_(std::move(metadataFetcher)),
      assetFetcher_(std::move(assetFetcher)) {
    if (!metadataFetcher_)
        metadataFetcher_ = fetchText;
    if (!assetFetcher_)
        assetFetcher_ = fetchFile;
}

UpdateService::~UpdateService() {
    for (std::thread& worker : workers_)
        if (worker.joinable())
            worker.join();
}

bool UpdateService::isNewerVersion(const std::string& candidate,
                                   const std::string& current) {
    std::array<uint64_t, 3> candidateParts{};
    std::array<uint64_t, 3> currentParts{};
    return parseVersion(candidate, candidateParts) &&
           parseVersion(current, currentParts) && candidateParts > currentParts;
}

bool UpdateService::parseRelease(const std::string& json, ReleaseInfo& release,
                                 std::string& error) {
    release = {};
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "GitHub returned an invalid release.";
        return false;
    }
    if (root.value("draft", true) || root.value("prerelease", true)) {
        error = "GitHub latest release is not published and stable.";
        return false;
    }
    release.version = root.value("tag_name", "");
    if (!isNewerVersion(release.version, "0.0.0")) {
        error = "GitHub release has an invalid version tag.";
        return false;
    }
    if (!root.contains("assets") || !root["assets"].is_array()) {
        error = "GitHub release has no assets.";
        return false;
    }
    for (const auto& asset : root["assets"]) {
        if (!asset.is_object())
            continue;
        const std::string name = asset.value("name", "");
        const std::string url = asset.value("browser_download_url", "");
        if (!trustedAssetUrl(url))
            continue;
        if (name == "pipensx.nro")
            release.nroUrl = url;
        else if (name == "pipensx.nro.sha256")
            release.checksumUrl = url;
    }
    if (release.nroUrl.empty() || release.checksumUrl.empty()) {
        error = "GitHub release must include pipensx.nro and pipensx.nro.sha256.";
        return false;
    }
    return true;
}

UpdateCheckResult UpdateService::check() const {
    UpdateCheckResult result;
    std::string body;
    if (!metadataFetcher_(kLatestReleaseUrl, kMetadataLimit, body, result.error))
        return result;
    if (!parseRelease(body, result.release, result.error))
        return result;
    result.ok = true;
    result.updateAvailable = isNewerVersion(result.release.version,
                                            PIPENSX_VERSION);
    return result;
}

bool UpdateService::install(const ReleaseInfo& release, std::string& error) const {
    if (!trustedAssetUrl(release.nroUrl) ||
        !trustedAssetUrl(release.checksumUrl)) {
        error = "Update asset URL is not trusted.";
        return false;
    }
    std::string checksumText;
    if (!metadataFetcher_(release.checksumUrl, kChecksumLimit, checksumText,
                          error))
        return false;
    std::string expectedChecksum;
    if (!parseChecksum(checksumText, expectedChecksum)) {
        error = "Update checksum is invalid.";
        return false;
    }
    const std::string temporary = targetPath_ + ".update";
    unlink(temporary.c_str());
    if (!assetFetcher_(release.nroUrl, temporary, kNroLimit, error))
        return false;
    std::string actualChecksum;
    if (!checksumFile(temporary, actualChecksum, error)) {
        unlink(temporary.c_str());
        return false;
    }
    if (actualChecksum != expectedChecksum) {
        unlink(temporary.c_str());
        error = "Update checksum does not match GitHub release.";
        return false;
    }
    if (rename(temporary.c_str(), targetPath_.c_str()) == 0)
        return true;
    const int replaceError = errno;
    const std::string backup = targetPath_ + ".previous";
    unlink(backup.c_str());
    if (rename(targetPath_.c_str(), backup.c_str()) == 0) {
        if (rename(temporary.c_str(), targetPath_.c_str()) == 0) {
            unlink(backup.c_str());
            return true;
        }
        const int installError = errno;
        rename(backup.c_str(), targetPath_.c_str());
        unlink(temporary.c_str());
        error = std::string("Unable to install update: ") +
                std::strerror(installError);
        return false;
    }
    unlink(temporary.c_str());
    error = std::string("Unable to install update: ") +
            std::strerror(replaceError);
    return false;
}

void UpdateService::checkAsync(CheckCallback callback) {
    workers_.emplace_back([this, callback = std::move(callback)]() mutable {
        callback(check());
    });
}

void UpdateService::installAsync(ReleaseInfo release, InstallCallback callback) {
    workers_.emplace_back([this, release = std::move(release),
                           callback = std::move(callback)]() mutable {
        std::string error;
        const bool installed = install(release, error);
        callback(installed, std::move(error));
    });
}

} // namespace pipensx
