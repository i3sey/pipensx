#include "torrent_metainfo_fetch.hpp"

#include "curl_https.hpp"
#include "magnet_resolver.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <unistd.h>

extern "C" {
#include "core/metainfo.h"
#include "core/sha1.h"
#include "core/util.h"
}

namespace pipensx {
namespace {

constexpr size_t kMaxTorrentBytes = 2 * 1024 * 1024;

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    const size_t nbytes = size * nmemb;
    if (out->size() + nbytes > kMaxTorrentBytes)
        return 0;
    out->insert(out->end(), reinterpret_cast<uint8_t*>(ptr),
                reinterpret_cast<uint8_t*>(ptr) + nbytes);
    return nbytes;
}

bool writeTorrentAtomic(const std::string& path,
                        const std::vector<uint8_t>& torrent,
                        std::string& error) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create the torrent file.";
            return false;
        }
        output.write(reinterpret_cast<const char*>(torrent.data()),
                     static_cast<std::streamsize>(torrent.size()));
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write the torrent file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        error = "Unable to replace the torrent file.";
        return false;
    }
    return true;
}

std::string upperHex(std::string hex) {
    for (char& c : hex)
        c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(c)));
    return hex;
}

bool parseHexHash(const std::string& hex, uint8_t out[20],
                  std::string& error) {
    if (hex.size() != 40) {
        error = "Invalid torrent info hash.";
        return false;
    }
    for (size_t i = 0; i < 20; ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[i * 2]);
        const int lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            error = "Invalid torrent info hash.";
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool defaultHttpGet(const std::string& url, std::vector<uint8_t>& body,
                    long& httpStatus, std::string& error) {
    body.clear();
    httpStatus = 0;
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curlPinHttpsOnly(curl);
    curlApplyTrustedSsl(curl);
    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    else
        error = std::string("Torrent cache request failed: ") +
                curl_easy_strerror(result);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool debridFilesReady(const DebridInfo& info) {
    return !info.files.empty() &&
           info.phase != DebridInfo::Phase::Creating &&
           info.phase != DebridInfo::Phase::Failed;
}

bool pollUntilFiles(DebridProvider& provider, const std::string& id,
                    std::atomic<bool>& cancelled,
                    std::chrono::steady_clock::time_point deadline,
                    DebridInfo& info, std::string& error) {
    do {
        std::string fetchError;
        if (!provider.fetchInfo(id, info, fetchError)) {
            error = std::move(fetchError);
        } else {
            error.clear();
            if (info.phase == DebridInfo::Phase::Failed)
                return false;
            if (debridFilesReady(info))
                return true;
        }
        for (int i = 0; i < 8 && !cancelled.load() &&
                        std::chrono::steady_clock::now() < deadline;
             ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    } while (!cancelled.load() &&
             std::chrono::steady_clock::now() < deadline);
    if (error.empty())
        error = "Unable to resolve torrent metadata.";
    return false;
}

} // namespace

std::string itorrentsUrlForHash(const std::string& infoHashHex) {
    return "https://itorrents.org/torrent/" + upperHex(infoHashHex) +
           ".torrent";
}

bool torrentBodyMatchesInfoHash(const std::vector<uint8_t>& body,
                                const std::string& infoHashHex,
                                std::string& error) {
    uint8_t expected[20];
    if (!parseHexHash(infoHashHex, expected, error))
        return false;
    if (body.empty() || body.front() != 'd') {
        error = "Torrent cache returned a non-torrent body.";
        return false;
    }
    metainfo_t parsed;
    if (!metainfo_parse(body.data(), body.size(), &parsed)) {
        error = "Torrent cache returned an unsupported torrent.";
        return false;
    }
    const bool match =
        std::memcmp(parsed.info_hash, expected, 20) == 0;
    metainfo_free(&parsed);
    if (!match) {
        error = "Torrent cache body does not match the info hash.";
        return false;
    }
    return true;
}

bool writeTorrentFromInfoDict(const std::string& magnetUri,
                              const std::vector<uint8_t>& infoDict,
                              const std::string& outPath,
                              std::string& error) {
    MagnetSpec spec;
    if (!MagnetResolver::parse(magnetUri, spec, error))
        return false;
    std::vector<uint8_t> torrent;
    if (!MagnetResolver::buildTorrent(spec, infoDict, torrent, error))
        return false;
    return writeTorrentAtomic(outPath, torrent, error);
}

bool fetchTorrentByInfoHash(const std::string& infoHashHex,
                            const std::string& outPath,
                            std::atomic<bool>& cancelled,
                            std::string& error,
                            TorrentHttpGet* transport) {
    if (cancelled.load()) {
        error = "Cancelled.";
        return false;
    }
    uint8_t ignored[20];
    if (!parseHexHash(infoHashHex, ignored, error))
        return false;

    const std::string url = itorrentsUrlForHash(infoHashHex);
    log_msg("[debrid-meta] fetching %s\n", url.c_str());
    std::vector<uint8_t> body;
    long status = 0;
    std::string transportError;
    const bool ok =
        transport ? (*transport)(url, body, status, transportError)
                  : defaultHttpGet(url, body, status, transportError);
    if (cancelled.load()) {
        error = "Cancelled.";
        return false;
    }
    if (!ok) {
        error = transportError.empty() ? "Torrent cache request failed."
                                       : transportError;
        return false;
    }
    if (status != 200) {
        error = "Torrent cache HTTP " + std::to_string(status) + ".";
        return false;
    }
    if (!torrentBodyMatchesInfoHash(body, infoHashHex, error))
        return false;
    return writeTorrentAtomic(outPath, body, error);
}

bool ensureTorrentFileForDebrid(const std::string& magnetUri,
                                const std::string& infoHashHex,
                                const std::vector<uint8_t>& infoDict,
                                const std::string& outPath,
                                std::atomic<bool>& cancelled,
                                std::string& error,
                                TorrentHttpGet* transport) {
    if (!infoDict.empty()) {
        if (writeTorrentFromInfoDict(magnetUri, infoDict, outPath, error)) {
            log_msg("[debrid-meta] built torrent from catalog info_dict\n");
            return true;
        }
        log_msg("[debrid-meta] info_dict build failed: %s\n", error.c_str());
    }
    return fetchTorrentByInfoHash(infoHashHex, outPath, cancelled, error,
                                  transport);
}

bool createDebridWithMetainfoFallback(
    DebridProvider& provider, const std::string& magnetUri,
    const std::string& infoHashHex, const std::vector<uint8_t>& infoDict,
    const std::string& tmpTorrentPath, std::atomic<bool>& cancelled,
    std::chrono::steady_clock::time_point deadline, std::string& debridId,
    DebridInfo& info, std::string& error,
    const std::function<void(DebridCreateStage)>& onStage) {
    debridId.clear();
    info = DebridInfo{};
    error.clear();

    auto cleanupTmp = [&]() { unlink(tmpTorrentPath.c_str()); };

    if (onStage)
        onStage(DebridCreateStage::SendingMagnet);
    bool ok = provider.createFromMagnet(magnetUri, debridId, error);
    if (ok && !cancelled.load()) {
        if (pollUntilFiles(provider, debridId, cancelled, deadline, info,
                           error)) {
            cleanupTmp();
            return true;
        }
    }
    if (!debridId.empty()) {
        std::string ignored;
        provider.remove(debridId, ignored);
        debridId.clear();
    }
    if (cancelled.load()) {
        error = "Cancelled.";
        cleanupTmp();
        return false;
    }

    // Fresh window for the file-upload path.
    const auto fileDeadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(60);

    if (onStage)
        onStage(DebridCreateStage::FetchingTorrent);
    if (!ensureTorrentFileForDebrid(magnetUri, infoHashHex, infoDict,
                                    tmpTorrentPath, cancelled, error)) {
        cleanupTmp();
        if (error.empty())
            error = "Unable to resolve torrent metadata.";
        return false;
    }
    if (cancelled.load()) {
        error = "Cancelled.";
        cleanupTmp();
        return false;
    }

    if (onStage)
        onStage(DebridCreateStage::UploadingTorrent);
    ok = provider.createFromFile(tmpTorrentPath, debridId, error);
    cleanupTmp();
    if (!ok || cancelled.load()) {
        if (!debridId.empty()) {
            std::string ignored;
            provider.remove(debridId, ignored);
            debridId.clear();
        }
        if (cancelled.load())
            error = "Cancelled.";
        return false;
    }
    if (!pollUntilFiles(provider, debridId, cancelled, fileDeadline, info,
                        error)) {
        std::string ignored;
        provider.remove(debridId, ignored);
        debridId.clear();
        return false;
    }
    return true;
}

} // namespace pipensx
