#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "debrid_provider.hpp"

namespace pipensx {

// Optional injectable GET for tests. Returns false on transport failure;
// httpStatus/body are filled on success (including non-200 statuses).
using TorrentHttpGet = std::function<bool(const std::string& url,
                                          std::vector<uint8_t>& body,
                                          long& httpStatus,
                                          std::string& error)>;

enum class DebridCreateStage {
    SendingMagnet,
    FetchingTorrent,
    UploadingTorrent,
};

// itorrents.org path for a 40-char hex infohash (uppercased).
std::string itorrentsUrlForHash(const std::string& infoHashHex);

// True when body is a torrent whose info dict SHA-1 matches infoHashHex.
bool torrentBodyMatchesInfoHash(const std::vector<uint8_t>& body,
                                const std::string& infoHashHex,
                                std::string& error);

bool writeTorrentFromInfoDict(const std::string& magnetUri,
                              const std::vector<uint8_t>& infoDict,
                              const std::string& outPath,
                              std::string& error);

// HTTPS torrent-cache fetch (no BitTorrent). transport nullptr → real curl.
bool fetchTorrentByInfoHash(const std::string& infoHashHex,
                            const std::string& outPath,
                            std::atomic<bool>& cancelled,
                            std::string& error,
                            TorrentHttpGet* transport = nullptr);

// infoDict → buildTorrent when present; else HTTPS cache fetch.
bool ensureTorrentFileForDebrid(const std::string& magnetUri,
                                const std::string& infoHashHex,
                                const std::vector<uint8_t>& infoDict,
                                const std::string& outPath,
                                std::atomic<bool>& cancelled,
                                std::string& error,
                                TorrentHttpGet* transport = nullptr);

// createFromMagnet + poll; on failure, ensureTorrentFile + createFromFile.
// tmpTorrentPath is unlinked before return. onStage may be null.
bool createDebridWithMetainfoFallback(
    DebridProvider& provider, const std::string& magnetUri,
    const std::string& infoHashHex, const std::vector<uint8_t>& infoDict,
    const std::string& tmpTorrentPath, std::atomic<bool>& cancelled,
    std::chrono::steady_clock::time_point deadline, std::string& debridId,
    DebridInfo& info, std::string& error,
    const std::function<void(DebridCreateStage)>& onStage = {});

} // namespace pipensx
