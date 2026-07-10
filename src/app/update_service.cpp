#include "update_service.hpp"

#include <curl/curl.h>
#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
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
constexpr int kFetchAttempts = 3;

enum class TransferKind {
    Metadata,
    Download,
};

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

int enlargeSocketBuffer(void*, curl_socket_t socket, curlsocktype purpose) {
    // Borealis boots the Switch socket service with tiny default buffers;
    // a larger receive window is what keeps the NRO download off the
    // kilobytes-per-second floor.
    if (purpose == CURLSOCKTYPE_IPCXN) {
        int size = 256 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size));
    }
    return CURL_SOCKOPT_OK;
}

int reportTransferProgress(void* opaque, curl_off_t downloadTotal,
                           curl_off_t downloadNow, curl_off_t, curl_off_t) {
    const auto* progress =
        static_cast<const UpdateService::ProgressCallback*>(opaque);
    if (progress && *progress && downloadTotal > 0)
        (*progress)(static_cast<uint64_t>(downloadNow),
                    static_cast<uint64_t>(downloadTotal));
    return 0;
}

bool configureCurl(CURL* curl, const std::string& url, TransferKind kind,
                   std::string& error) {
    if (!curl) {
        error = "Unable to initialize updater HTTP client.";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/" PIPENSX_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (kind == TransferKind::Download) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L * 60L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, enlargeSocketBuffer);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    return true;
}

bool fetchText(const std::string& url, size_t limit, std::string& body,
               std::string& error) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!configureCurl(curl, url, TransferKind::Metadata, error))
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
               std::string& error,
               const UpdateService::ProgressCallback* progress = nullptr) {
    FileWriter writer;
    writer.output.open(path, std::ios::binary | std::ios::trunc);
    writer.limit = limit;
    if (!writer.output) {
        error = "Unable to create update download.";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!configureCurl(curl, url, TransferKind::Download, error)) {
        unlink(path.c_str());
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                         reportTransferProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,
                         const_cast<UpdateService::ProgressCallback*>(progress));
    }
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

bool startsWith(const std::string& value, const char* prefix) {
    return value.compare(0, std::strlen(prefix), prefix) == 0;
}

bool retryableHttpError(const std::string& error) {
    const size_t marker = error.find("HTTP ");
    if (marker == std::string::npos)
        return false;
    try {
        const int status = std::stoi(error.substr(marker + 5));
        return status == 408 || status == 429 || status >= 500;
    } catch (...) {
        return false;
    }
}

bool retryableFetchError(const std::string& error) {
    return startsWith(error, "Update network error:") ||
           startsWith(error, "Update download failed:") ||
           retryableHttpError(error);
}

template <typename Fetch>
bool fetchWithRetry(Fetch fetch, std::string& error) {
    for (int attempt = 1; attempt <= kFetchAttempts; ++attempt) {
        error.clear();
        if (fetch())
            return true;
        const bool retryable = retryableFetchError(error);
        if (!retryable || attempt == kFetchAttempts) {
            if (retryable)
                error += " (after " + std::to_string(attempt) + " attempts).";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
    }
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

bool copyFileContents(const std::string& source, const std::string& destination,
                      std::string& error) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = std::strerror(errno);
        return false;
    }
    std::ofstream output(destination,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        error = std::strerror(errno);
        return false;
    }
    std::array<char, 64 * 1024> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0)
            output.write(buffer.data(), count);
        if (!output) {
            error = std::strerror(errno);
            return false;
        }
    }
    output.flush();
    if (input.bad() || !output) {
        error = std::strerror(errno);
        return false;
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
        assetFetcher_ = [this](const std::string& url, const std::string& path,
                               size_t limit, std::string& error) {
            return fetchFile(url, path, limit, error, &progress_);
        };
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
    if (!fetchWithRetry([&] {
            return metadataFetcher_(kLatestReleaseUrl, kMetadataLimit, body,
                                    result.error);
        }, result.error))
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
    if (!fetchWithRetry([&] {
            return metadataFetcher_(release.checksumUrl, kChecksumLimit,
                                    checksumText, error);
        }, error))
        return false;
    std::string expectedChecksum;
    if (!parseChecksum(checksumText, expectedChecksum)) {
        error = "Update checksum is invalid.";
        return false;
    }
    const std::string temporary = stagedPath();
    const std::string marker = temporary + ".sha256";
    const std::string helper = helperPath();
    unlink(temporary.c_str());
    unlink(marker.c_str());
    unlink(helper.c_str());
    if (!fetchWithRetry([&] {
            return assetFetcher_(release.nroUrl, temporary, kNroLimit, error);
        }, error))
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
    std::ofstream markerFile(marker, std::ios::binary | std::ios::trunc);
    markerFile << expectedChecksum << '\n';
    markerFile.flush();
    if (!markerFile) {
        unlink(marker.c_str());
        unlink(temporary.c_str());
        error = "Unable to save staged update checksum.";
        return false;
    }
    markerFile.close();
    std::string helperError;
    if (!copyFileContents(targetPath_, helper, helperError)) {
        unlink(helper.c_str());
        unlink(marker.c_str());
        unlink(temporary.c_str());
        error = "Unable to create update helper: " + helperError;
        return false;
    }
    return true;
}

bool UpdateService::finalizeStaged(std::string& error) const {
    const std::string temporary = stagedPath();
    const std::string marker = temporary + ".sha256";
    std::ifstream markerFile(marker, std::ios::binary);
    std::ostringstream markerText;
    markerText << markerFile.rdbuf();
    std::string expectedChecksum;
    if (!markerFile || !parseChecksum(markerText.str(), expectedChecksum)) {
        error = "Staged update checksum is missing or invalid.";
        return false;
    }
    std::string actualChecksum;
    if (!checksumFile(temporary, actualChecksum, error))
        return false;
    if (actualChecksum != expectedChecksum) {
        error = "Staged update checksum does not match.";
        return false;
    }

    const std::string backup = targetPath_ + ".previous";
    unlink(backup.c_str());
    bool haveBackup = false;
    if (access(targetPath_.c_str(), F_OK) == 0) {
        if (rename(targetPath_.c_str(), backup.c_str()) == 0) {
            haveBackup = true;
        } else {
            std::string backupError;
            if (!copyFileContents(targetPath_, backup, backupError)) {
                error = "Unable to back up current application: " +
                        backupError;
                return false;
            }
            haveBackup = true;
        }
    }

    std::string copyError;
    bool installed = copyFileContents(temporary, targetPath_, copyError);
    if (installed) {
        std::string installedChecksum;
        installed = checksumFile(targetPath_, installedChecksum, copyError) &&
                    installedChecksum == expectedChecksum;
        if (!installed && copyError.empty())
            copyError = "installed file checksum does not match";
    }
    if (!installed) {
        unlink(targetPath_.c_str());
        if (haveBackup) {
            if (rename(backup.c_str(), targetPath_.c_str()) != 0) {
                std::string ignored;
                copyFileContents(backup, targetPath_, ignored);
            }
        }
        error = "Unable to finalize staged update: " + copyError;
        return false;
    }

    if (haveBackup)
        unlink(backup.c_str());
    unlink(marker.c_str());
    unlink(temporary.c_str());
    return true;
}

bool UpdateService::stagedReady() const {
    const std::string temporary = stagedPath();
    std::ifstream markerFile(temporary + ".sha256", std::ios::binary);
    std::ostringstream markerText;
    markerText << markerFile.rdbuf();
    std::string expectedChecksum;
    if (!markerFile || !parseChecksum(markerText.str(), expectedChecksum))
        return false;
    std::string actualChecksum;
    std::string error;
    return checksumFile(temporary, actualChecksum, error) &&
           actualChecksum == expectedChecksum;
}

void UpdateService::discardStaged() const {
    const std::string temporary = stagedPath();
    unlink(temporary.c_str());
    unlink((temporary + ".sha256").c_str());
    unlink(helperPath().c_str());
}

bool UpdateService::isStagedLaunch(
        const std::vector<std::string>& arguments) const {
    const std::string helper = helperPath();
    for (const std::string& argument : arguments) {
        if (argument == "--finish-update" ||
            argument.find(helper) != std::string::npos)
            return true;
    }
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
