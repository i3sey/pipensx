#include "magnet_resolver.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/dht.h"
#include "../core/metainfo.h"
#include "../core/net.h"
#include "../core/sha1.h"
#include "../core/tracker.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace pipensx {
namespace {

int trackerCancelled(void* user) {
    return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
}

// The ut_metadata id we advertise in our extension handshake. Peers address
// extended messages back to us using this id (BEP 10), regardless of the id
// the peer advertised for its own ut_metadata implementation.
constexpr uint8_t kLocalUtMetadataId = 1;

constexpr size_t kMetadataPieceSize = 16 * 1024;
constexpr size_t kMetadataLimit = 8 * 1024 * 1024;
constexpr uint64_t kOverallTimeoutMs = 90 * 1000;
constexpr int kIoTimeoutMs = 4000;
constexpr uint32_t kMaxPeersPerTracker = 64;
constexpr uint32_t kMaxMergedPeers = 192;
constexpr uint32_t kMaxConcurrentPeers = 12;
constexpr uint32_t kRequestPipeline = 8;
constexpr uint64_t kDhtSearchTimeoutMs = 25 * 1000;
constexpr uint32_t kDhtTargetPeers = 32;
constexpr int kDhtPollIntervalMs = 250;
constexpr uint16_t kDhtPort = 6881;

// PEX amplification (RF_ACCESS_PLAN П1.2): when the tracker/DHT phase yields a
// thin peer list, ask each of the few peers we have for their swarm view.
constexpr uint32_t kPexThinThreshold = 3;
constexpr uint8_t kLocalUtPexId = 2;
constexpr int kPexPeerTimeoutMs = 5000;

bool hexNibble(char c, uint8_t& value) {
    if (c >= '0' && c <= '9')
        value = static_cast<uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
        value = static_cast<uint8_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
        value = static_cast<uint8_t>(c - 'A' + 10);
    else
        return false;
    return true;
}

std::string urlDecode(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            uint8_t hi = 0, lo = 0;
            if (hexNibble(input[i + 1], hi) && hexNibble(input[i + 2], lo)) {
                result.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        result.push_back(input[i] == '+' ? ' ' : input[i]);
    }
    return result;
}

bool allowedTracker(const std::string& url) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0)
        return false;
    size_t hostEnd = url.find('/', prefix.size());
    std::string host = url.substr(prefix.size(), hostEnd - prefix.size());
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host == "bt.t-ru.org" || host == "bt2.t-ru.org" ||
           host == "bt3.t-ru.org" || host == "bt4.t-ru.org";
}

std::vector<std::string> rutrackerTrackerCandidates(
        const std::string& original) {
    std::vector<std::string> urls;
    auto add = [&](const std::string& url) {
        if (!allowedTracker(url))
            return;
        if (std::find(urls.begin(), urls.end(), url) == urls.end())
            urls.push_back(url);
    };
    add(original);
    add("http://bt.t-ru.org/ann?magnet");
    add("http://bt2.t-ru.org/ann?magnet");
    add("http://bt3.t-ru.org/ann?magnet");
    add("http://bt4.t-ru.org/ann?magnet");
    return urls;
}

bool containsCaseInsensitive(const char* text, const char* needle) {
    if (!text || !needle || !*needle)
        return false;
    std::string haystack(text);
    std::string query(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(query.begin(), query.end(), query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(query) != std::string::npos;
}

bool containsPeer(const std::vector<uint8_t>& peers, const uint8_t* compact) {
    for (size_t offset = 0; offset + 6 <= peers.size(); offset += 6) {
        if (std::memcmp(peers.data() + offset, compact, 6) == 0)
            return true;
    }
    return false;
}

void appendUniquePeers(std::vector<uint8_t>& peers,
                       const uint8_t* compact,
                       uint32_t count) {
    for (uint32_t i = 0; i < count && peers.size() / 6 < kMaxMergedPeers; ++i) {
        const uint8_t* item = compact + i * 6;
        if (containsPeer(peers, item))
            continue;
        peers.insert(peers.end(), item, item + 6);
    }
}

bool waitFd(socket_t fd, short events, int timeoutMs) {
    pollfd item{fd, events, 0};
    int result;
    do {
        result = poll(&item, 1, timeoutMs);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (item.revents & events) != 0;
}

/* Compact IPv4 peers discovered by the short-lived DHT search that runs in
   parallel with the tracker announces (RF_ACCESS_PLAN П1.1). With RuTracker
   blocked, DHT is the only peer source, so tracker failures alone no longer
   abort the resolve. */
struct DhtSearch {
    std::mutex mutex;
    std::vector<uint8_t> peers;
};

void dhtPeerFound(void* user, uint32_t ipBe, uint16_t portBe) {
    DhtSearch* search = static_cast<DhtSearch*>(user);
    uint8_t compact[6];
    std::memcpy(compact, &ipBe, 4);
    std::memcpy(compact + 4, &portBe, 2);
    std::lock_guard<std::mutex> lock(search->mutex);
    appendUniquePeers(search->peers, compact, 1);
}

uint32_t dhtPeerCount(DhtSearch& search) {
    std::lock_guard<std::mutex> lock(search.mutex);
    return static_cast<uint32_t>(search.peers.size() / 6);
}

bool writeTorrentAtomic(const std::string& path,
                        const std::vector<uint8_t>& torrent,
                        std::string& error) {
    std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create the resolved torrent file.";
            return false;
        }
        output.write(reinterpret_cast<const char*>(torrent.data()),
                     static_cast<std::streamsize>(torrent.size()));
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write the resolved torrent file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        error = "Unable to replace the resolved torrent file.";
        return false;
    }
    return true;
}

void runDhtSearch(const uint8_t infoHash[20],
                  std::atomic<bool>& cancelled,
                  std::atomic<bool>& stop,
                  DhtSearch& search) {
    if (cancelled || stop)
        return;
    uint8_t nodeId[20];
    rand_bytes(nodeId, 20);
    /* Fails when a running download already owns the DHT singleton or the
       UDP port; the resolve then proceeds on trackers alone. */
    dht_engine_t* engine = dht_engine_create(kDhtPort, nodeId);
    if (!engine) {
        log_msg("[magnet] dht unavailable, resolving without it\n");
        return;
    }
    dht_engine_bootstrap(engine);
    dht_engine_search(engine, infoHash, dhtPeerFound, &search);
    uint64_t deadline = now_ms() + kDhtSearchTimeoutMs;
    while (!cancelled && !stop && now_ms() < deadline &&
           dhtPeerCount(search) < kDhtTargetPeers) {
        waitFd(dht_engine_fd(engine), POLLIN, kDhtPollIntervalMs);
        dht_engine_tick(engine);
    }
    dht_engine_destroy(engine);
}

bool sendAll(socket_t fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        if (!waitFd(fd, POLLOUT, kIoTimeoutMs))
            return false;
        ssize_t count = send(fd, data + sent, size - sent, 0);
        if (count <= 0)
            return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool recvAll(socket_t fd, uint8_t* data, size_t size, int timeoutMs) {
    size_t received = 0;
    while (received < size) {
        if (!waitFd(fd, POLLIN, timeoutMs))
            return false;
        ssize_t count = recv(fd, data + received, size - received, 0);
        if (count <= 0)
            return false;
        received += static_cast<size_t>(count);
    }
    return true;
}

bool sendFrame(socket_t fd, const std::vector<uint8_t>& payload) {
    uint32_t size = static_cast<uint32_t>(payload.size());
    uint8_t header[4] = {
        static_cast<uint8_t>(size >> 24),
        static_cast<uint8_t>(size >> 16),
        static_cast<uint8_t>(size >> 8),
        static_cast<uint8_t>(size),
    };
    return sendAll(fd, header, sizeof(header)) &&
           sendAll(fd, payload.data(), payload.size());
}

bool recvFrame(socket_t fd, std::vector<uint8_t>& payload) {
    uint8_t header[4];
    if (!recvAll(fd, header, sizeof(header), kIoTimeoutMs))
        return false;
    uint32_t size = (static_cast<uint32_t>(header[0]) << 24) |
                    (static_cast<uint32_t>(header[1]) << 16) |
                    (static_cast<uint32_t>(header[2]) << 8) |
                    static_cast<uint32_t>(header[3]);
    if (size > kMetadataPieceSize + 4096)
        return false;
    payload.resize(size);
    return size == 0 || recvAll(fd, payload.data(), size, kIoTimeoutMs);
}

bool connectPeer(const uint8_t* compact, const uint8_t infoHash[20],
                 const uint8_t peerId[20], socket_t& fd) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    std::memcpy(&address.sin_addr.s_addr, compact, 4);
    std::memcpy(&address.sin_port, compact + 4, 2);
    fd = net_tcp_connect(&address);
    if (fd == INVALID_SOCK || !waitFd(fd, POLLOUT, kIoTimeoutMs))
        return false;
    int socketError = 0;
    socklen_t errorSize = sizeof(socketError);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0 ||
        socketError != 0)
        return false;

    uint8_t handshake[68]{};
    handshake[0] = 19;
    std::memcpy(handshake + 1, "BitTorrent protocol", 19);
    handshake[25] = 0x10;
    std::memcpy(handshake + 28, infoHash, 20);
    std::memcpy(handshake + 48, peerId, 20);
    if (!sendAll(fd, handshake, sizeof(handshake)))
        return false;

    uint8_t response[68];
    if (!recvAll(fd, response, sizeof(response), kIoTimeoutMs) ||
        response[0] != 19 ||
        std::memcmp(response + 1, "BitTorrent protocol", 19) != 0 ||
        std::memcmp(response + 28, infoHash, 20) != 0 ||
        (response[25] & 0x10) == 0)
        return false;
    net_set_tcp_receive_buffer(fd);
    return true;
}

bool negotiateMetadata(socket_t fd, uint8_t& peerExtension,
                       size_t& metadataSize) {
    static const char handshake[] = "d1:md11:ut_metadatai1eee";
    std::vector<uint8_t> request{20, 0};
    request.insert(request.end(), handshake, handshake + sizeof(handshake) - 1);
    if (!sendFrame(fd, request))
        return false;

    for (int message = 0; message < 32; ++message) {
        std::vector<uint8_t> frame;
        if (!recvFrame(fd, frame))
            return false;
        if (frame.size() < 3 || frame[0] != 20 || frame[1] != 0)
            continue;
        const char* begin = reinterpret_cast<const char*>(frame.data() + 2);
        const char* end = reinterpret_cast<const char*>(frame.data() + frame.size());
        be_node_t root;
        const char* cursor = begin;
        if (!be_decode(&cursor, end, &root) || root.type != BE_DICT)
            return false;
        be_node_t map;
        be_node_t extension;
        be_node_t size;
        if (!be_dict_get(root.buf, root.buf + root.raw_len, "m", 1, &map) ||
            map.type != BE_DICT ||
            !be_dict_get(map.buf, map.buf + map.raw_len, "ut_metadata", 11,
                         &extension) ||
            extension.type != BE_INT || extension.ival <= 0 ||
            extension.ival > 255 ||
            !be_dict_get(root.buf, root.buf + root.raw_len, "metadata_size", 13,
                         &size) ||
            size.type != BE_INT || size.ival <= 0 ||
            size.ival > static_cast<int64_t>(kMetadataLimit))
            return false;
        peerExtension = static_cast<uint8_t>(extension.ival);
        metadataSize = static_cast<size_t>(size.ival);
        return true;
    }
    return false;
}

bool sendMetadataRequest(socket_t fd, uint8_t extension, uint32_t piece) {
    std::string body = "d8:msg_typei0e5:piecei" + std::to_string(piece) + "ee";
    std::vector<uint8_t> frame{20, extension};
    frame.insert(frame.end(), body.begin(), body.end());
    return sendFrame(fd, frame);
}

bool receiveMetadataPiece(socket_t fd,
                          uint32_t& piece, const uint8_t*& data,
                          size_t& dataSize, std::vector<uint8_t>& frame) {
    for (int message = 0; message < 32; ++message) {
        if (!recvFrame(fd, frame)) {
            log_msg("[magnet] peer frame receive timed out\n");
            return false;
        }
        // Extended messages the peer sends back to us carry our advertised
        // ut_metadata id, not the peer's (BEP 10).
        if (frame.size() < 3 || frame[0] != 20 ||
            frame[1] != kLocalUtMetadataId)
            continue;
        const char* begin = reinterpret_cast<const char*>(frame.data() + 2);
        const char* end = reinterpret_cast<const char*>(frame.data() + frame.size());
        const char* cursor = begin;
        be_node_t header;
        if (!be_decode(&cursor, end, &header) || header.type != BE_DICT) {
            log_msg("[magnet] invalid metadata message header\n");
            return false;
        }
        be_node_t type;
        be_node_t index;
        if (!be_dict_get(header.buf, header.buf + header.raw_len, "msg_type", 8,
                         &type) ||
            !be_dict_get(header.buf, header.buf + header.raw_len, "piece", 5,
                         &index) ||
            type.type != BE_INT || index.type != BE_INT || index.ival < 0) {
            log_msg("[magnet] incomplete metadata message header\n");
            return false;
        }
        if (type.ival == 2) {
            log_msg("[magnet] peer rejected metadata piece %lld\n",
                    (long long)index.ival);
            return false;
        }
        if (type.ival != 1)
            continue;
        piece = static_cast<uint32_t>(index.ival);
        data = reinterpret_cast<const uint8_t*>(cursor);
        dataSize = static_cast<size_t>(end - cursor);
        return true;
    }
    return false;
}

bool fetchMetadataFromPeer(const uint8_t* compact,
                           const MagnetSpec& spec,
                           const uint8_t peerId[20],
                           uint32_t peerIndex,
                           uint32_t peerCount,
                           uint64_t deadline,
                           std::atomic<bool>& cancelled,
                           std::atomic<bool>& stopWorkers,
                           const MagnetResolver::ProgressCallback& progress,
                           std::vector<uint8_t>& metadata,
                           bool& handshakeVerified) {
    handshakeVerified = false;
    if (cancelled || stopWorkers || now_ms() >= deadline)
        return false;
    if (progress)
        progress({MagnetProgress::Stage::Connecting, 0, 0, peerIndex + 1,
                  peerCount});

    socket_t fd = INVALID_SOCK;
    if (!connectPeer(compact, spec.infoHash, peerId, fd)) {
        log_msg("[magnet] peer %u/%u connect or handshake failed\n",
                peerIndex + 1, peerCount);
        net_close(fd);
        return false;
    }
    log_msg("[magnet] peer %u/%u BitTorrent handshake ok\n",
            peerIndex + 1, peerCount);
    handshakeVerified = true;

    uint8_t extension = 0;
    size_t metadataSize = 0;
    if (!negotiateMetadata(fd, extension, metadataSize)) {
        log_msg("[magnet] peer %u/%u has no usable ut_metadata\n",
                peerIndex + 1, peerCount);
        net_close(fd);
        return false;
    }
    log_msg("[magnet] peer %u/%u ut_metadata=%u size=%zu\n",
            peerIndex + 1, peerCount, extension, metadataSize);

    std::vector<uint8_t> local(metadataSize);
    std::vector<uint8_t> received(
        (metadataSize + kMetadataPieceSize - 1) / kMetadataPieceSize, 0);
    std::vector<uint8_t> requested(received.size(), 0);
    uint32_t completed = 0;
    uint32_t inFlight = 0;

    while (!cancelled && !stopWorkers && now_ms() < deadline &&
           completed < received.size()) {
        for (uint32_t piece = 0;
             piece < received.size() && inFlight < kRequestPipeline; ++piece) {
            if (received[piece] || requested[piece])
                continue;
            if (!sendMetadataRequest(fd, extension, piece)) {
                net_close(fd);
                return false;
            }
            requested[piece] = 1;
            ++inFlight;
        }

        uint32_t piece = 0;
        const uint8_t* bytes = nullptr;
        size_t byteCount = 0;
        std::vector<uint8_t> frame;
        if (!receiveMetadataPiece(fd, piece, bytes, byteCount, frame)) {
            log_msg("[magnet] peer %u/%u metadata receive failed\n",
                    peerIndex + 1, peerCount);
            net_close(fd);
            return false;
        }
        if (piece >= received.size())
            continue;
        if (requested[piece] && inFlight)
            --inFlight;
        requested[piece] = 0;
        size_t offset = static_cast<size_t>(piece) * kMetadataPieceSize;
        size_t expected = std::min(kMetadataPieceSize,
                                   metadataSize - offset);
        if (byteCount != expected) {
            log_msg("[magnet] peer %u/%u wrong metadata piece size %zu/%zu\n",
                    peerIndex + 1, peerCount, byteCount, expected);
            net_close(fd);
            return false;
        }
        if (!received[piece]) {
            std::memcpy(local.data() + offset, bytes, byteCount);
            received[piece] = 1;
            ++completed;
            if (progress)
                progress({MagnetProgress::Stage::FetchingMetadata, completed,
                          static_cast<uint32_t>(received.size()),
                          peerIndex + 1, peerCount});
        }
    }

    net_close(fd);
    if (completed != received.size())
        return false;
    metadata = std::move(local);
    return true;
}

// Advertise both ut_metadata and ut_pex so the peer will push its swarm view
// to us on our advertised ut_pex id (kLocalUtPexId). BEP10 handshake.
bool sendPexHandshake(socket_t fd) {
    static const char handshake[] =
        "d1:md11:ut_metadatai1e6:ut_pexi2eee";
    std::vector<uint8_t> request{20, 0};
    request.insert(request.end(), handshake, handshake + sizeof(handshake) - 1);
    return sendFrame(fd, request);
}

// Connect to one thin-list peer, advertise ut_pex, and harvest the compact
// peers it pushes in any ut_pex "added" field (RF_ACCESS_PLAN П1.2). Best
// effort: peers emit PEX on their own cadence, so an empty result is normal.
// Merges into `out` (never the live `peers` vector — appending there while its
// data() is being iterated would dangle).
void harvestPexFromPeer(const uint8_t* compact, const MagnetSpec& spec,
                        const uint8_t peerId[20],
                        std::atomic<bool>& cancelled,
                        std::vector<uint8_t>& out) {
    socket_t fd = INVALID_SOCK;
    if (!connectPeer(compact, spec.infoHash, peerId, fd)) {
        net_close(fd);
        return;
    }
    if (!sendPexHandshake(fd)) {
        net_close(fd);
        return;
    }
    uint64_t deadline = now_ms() + kPexPeerTimeoutMs;
    while (!cancelled && now_ms() < deadline) {
        std::vector<uint8_t> frame;
        if (!recvFrame(fd, frame))
            break;
        // Extended messages the peer sends to us carry our advertised id, not
        // the peer's (BEP 10). Skip the peer's extension handshake (id 0) and
        // anything that is not our ut_pex channel.
        if (frame.size() < 3 || frame[0] != 20 || frame[1] != kLocalUtPexId)
            continue;
        const char* begin = reinterpret_cast<const char*>(frame.data() + 2);
        const char* end =
            reinterpret_cast<const char*>(frame.data() + frame.size());
        be_node_t root;
        const char* cursor = begin;
        if (!be_decode(&cursor, end, &root) || root.type != BE_DICT)
            break;
        be_node_t added;
        if (be_dict_get(root.buf, root.buf + root.raw_len, "added", 5, &added) &&
            added.type == BE_STR && added.slen >= 6) {
            appendUniquePeers(out,
                              reinterpret_cast<const uint8_t*>(added.sval),
                              static_cast<uint32_t>(added.slen / 6));
        }
    }
    net_close(fd);
}

std::string bencodeString(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

} // namespace

bool MagnetResolver::parse(const std::string& uri, MagnetSpec& spec,
                           std::string& error) {
    spec = {};
    if (uri.rfind("magnet:?", 0) != 0) {
        error = "Catalog entry has an invalid magnet URI.";
        return false;
    }
    bool haveHash = false;
    size_t start = 8;
    while (start <= uri.size()) {
        size_t end = uri.find('&', start);
        std::string pair = uri.substr(start, end - start);
        size_t equals = pair.find('=');
        std::string key = pair.substr(0, equals);
        std::string value = equals == std::string::npos
                          ? "" : urlDecode(pair.substr(equals + 1));
        if (key == "xt" && value.rfind("urn:btih:", 0) == 0) {
            std::string hash = value.substr(9);
            if (hash.size() != 40 || haveHash) {
                error = "Only one hexadecimal BitTorrent v1 hash is supported.";
                return false;
            }
            for (size_t i = 0; i < 20; ++i) {
                uint8_t hi = 0, lo = 0;
                if (!hexNibble(hash[i * 2], hi) ||
                    !hexNibble(hash[i * 2 + 1], lo)) {
                    error = "The magnet info hash is not hexadecimal.";
                    return false;
                }
                spec.infoHash[i] = static_cast<uint8_t>((hi << 4) | lo);
            }
            std::transform(hash.begin(), hash.end(), hash.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::toupper(c));
                           });
            spec.infoHashHex = hash;
            haveHash = true;
        } else if (key == "tr" && spec.trackerUrl.empty() &&
                   allowedTracker(value)) {
            spec.trackerUrl = value;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (!haveHash) {
        error = "The magnet does not contain a BitTorrent v1 info hash.";
        return false;
    }
    if (spec.trackerUrl.empty()) {
        error = "The magnet does not contain a supported RuTracker tracker.";
        return false;
    }
    return true;
}

bool MagnetResolver::buildTorrent(const MagnetSpec& spec,
                                  const std::vector<uint8_t>& info,
                                  std::vector<uint8_t>& torrent,
                                  std::string& error) {
    if (info.empty() || info.size() > kMetadataLimit) {
        error = "Peer metadata is empty or too large.";
        return false;
    }
    const char* cursor = reinterpret_cast<const char*>(info.data());
    const char* end = cursor + info.size();
    be_node_t root;
    if (!be_decode(&cursor, end, &root) || root.type != BE_DICT ||
        cursor != end) {
        error = "Peer metadata is not a valid info dictionary.";
        return false;
    }
    uint8_t digest[20];
    sha1(info.data(), info.size(), digest);
    if (std::memcmp(digest, spec.infoHash, 20) != 0) {
        error = "Peer metadata does not match the magnet info hash.";
        return false;
    }

    /* Bake every known RuTracker mirror into the torrent as an announce-list,
       not just the single tr= carried by the magnet (PERF_PLAN 1.6). Without
       this the resolved torrent had trackers=1 and the client only announced
       to one mirror, starving the swarm. metainfo_parse flattens every
       announce-list tier into mi->trackers[] and announces to all of them.
       Dict keys stay sorted: announce < announce-list < info. */
    std::vector<std::string> trackers =
        rutrackerTrackerCandidates(spec.trackerUrl);
    std::string announceList = "13:announce-listl";
    for (const std::string& tracker : trackers)
        announceList += "l" + bencodeString(tracker) + "e";
    announceList += "e";
    std::string prefix = "d8:announce" + bencodeString(spec.trackerUrl) +
                         announceList + "4:info";
    torrent.assign(prefix.begin(), prefix.end());
    torrent.insert(torrent.end(), info.begin(), info.end());
    torrent.push_back('e');

    metainfo_t parsed;
    if (!metainfo_parse(torrent.data(), torrent.size(), &parsed)) {
        error = "Resolved metadata is not a supported safe torrent.";
        return false;
    }
    bool hashMatches = std::memcmp(parsed.info_hash, spec.infoHash, 20) == 0;
    metainfo_free(&parsed);
    if (!hashMatches) {
        error = "Generated torrent failed info-hash validation.";
        return false;
    }
    return true;
}

bool MagnetResolver::resolveToFile(const std::string& uri,
                                   const std::string& path,
                                   std::atomic<bool>& cancelled,
                                   const ProgressCallback& progress,
                                   std::string& error,
                                   std::vector<uint8_t>* verifiedPeers,
                                   const std::vector<uint8_t>* presetInfo)
    const {
    if (verifiedPeers)
        verifiedPeers->clear();
    MagnetSpec spec;
    if (!parse(uri, spec, error))
        return false;

    /* Pre-resolved info dictionary from the catalog (RF_ACCESS_PLAN П2.1):
       CI resolves the magnet outside RF and embeds the dictionary, so the
       client skips the tracker→peer→ut_metadata phase entirely. buildTorrent
       still SHA-1-checks it against the magnet hash, so a tampered
       dictionary falls through to the normal network resolve. */
    if (presetInfo && !presetInfo->empty()) {
        if (progress)
            progress({MagnetProgress::Stage::Validating, 1, 1});
        std::vector<uint8_t> torrent;
        std::string presetError;
        if (buildTorrent(spec, *presetInfo, torrent, presetError)) {
            log_msg("[magnet] resolved from catalog info_dict (%zu bytes)\n",
                    presetInfo->size());
            return writeTorrentAtomic(path, torrent, error);
        }
        log_msg("[magnet] catalog info_dict rejected: %s\n",
                presetError.c_str());
    }

    uint8_t peerId[20];
    std::memcpy(peerId, "-PN0001-", 8);
    rand_bytes(peerId + 8, 12);

    if (progress)
        progress({MagnetProgress::Stage::FindingPeers});
    DhtSearch dhtSearch;
    std::atomic<bool> dhtStop{false};
    std::thread dhtThread([&spec, &cancelled, &dhtStop, &dhtSearch] {
        runDhtSearch(spec.infoHash, cancelled, dhtStop, dhtSearch);
    });
    std::vector<uint8_t> peers;
    std::string firstFailure;
    bool sawTrackerFailure = false;
    bool sawNotRegistered = false;
    auto trackers = rutrackerTrackerCandidates(spec.trackerUrl);
    for (const std::string& tracker : trackers) {
        if (cancelled)
            break;
        uint8_t batch[kMaxPeersPerTracker * 6];
        tracker_announce_result_t result;
        uint32_t count = tracker_announce_url_ex_cancel(
            tracker.c_str(), spec.infoHash, peerId, 6881, 0, 0,
            batch, kMaxPeersPerTracker, &result,
            trackerCancelled, &cancelled);
        if (cancelled)
            break;
        if (result.tracker_failure) {
            sawTrackerFailure = true;
            if (firstFailure.empty())
                firstFailure = result.failure_reason;
            if (containsCaseInsensitive(result.failure_reason,
                                        "not registered"))
                sawNotRegistered = true;
            log_msg("[magnet] tracker %s rejected hash: %s\n",
                    tracker.c_str(), result.failure_reason);
            if (sawNotRegistered)
                break;
        }
        appendUniquePeers(peers, batch, count);
        if (count)
            log_msg("[magnet] tracker %s added %u peers, total=%u\n",
                    tracker.c_str(), count,
                    static_cast<unsigned>(peers.size() / 6));
        if (peers.size() / 6 >= kMaxMergedPeers)
            break;
    }
    /* When RuTracker answered — with peers or an authoritative "not
       registered" — there is nothing to wait for. When the trackers were
       unreachable (blocked in RF), the DHT search keeps running until its
       own deadline as the fallback peer source. */
    if (!peers.empty() || sawNotRegistered)
        dhtStop.store(true);
    dhtThread.join();
    {
        std::lock_guard<std::mutex> lock(dhtSearch.mutex);
        uint32_t dhtCount = static_cast<uint32_t>(dhtSearch.peers.size() / 6);
        if (dhtCount) {
            appendUniquePeers(peers, dhtSearch.peers.data(), dhtCount);
            log_msg("[magnet] dht added %u peers, total=%u\n",
                    dhtCount, static_cast<unsigned>(peers.size() / 6));
        }
    }
    uint32_t peerCount = static_cast<uint32_t>(peers.size() / 6);
    if (!peerCount) {
        if (sawNotRegistered) {
            error = "RuTracker says this torrent is not registered anymore. "
                    "The catalog entry is stale.";
        } else if (sawTrackerFailure && !firstFailure.empty()) {
            error = "RuTracker rejected this torrent: " + firstFailure;
        } else {
            error = "RuTracker trackers and the DHT returned no usable peers.";
        }
        return false;
    }

    /* PEX amplification (RF_ACCESS_PLAN П1.2): a blocked-tracker/DHT-only
       resolve can surface just 1–3 peers. Before committing the metadata-fetch
       workers, ask those few peers for their swarm view and grow the set. PEX
       cannot bootstrap from nothing, but it cheaply multiplies a thin result.
       Harvest into a scratch vector — appending to `peers` while iterating
       peers.data() would dangle on reallocation. */
    if (peerCount <= kPexThinThreshold && !cancelled) {
        std::vector<uint8_t> pexPeers;
        for (uint32_t i = 0; i < peerCount && !cancelled; ++i)
            harvestPexFromPeer(peers.data() + i * 6, spec, peerId, cancelled,
                               pexPeers);
        uint32_t before = static_cast<uint32_t>(peers.size() / 6);
        if (!pexPeers.empty())
            appendUniquePeers(peers, pexPeers.data(),
                              static_cast<uint32_t>(pexPeers.size() / 6));
        uint32_t added = static_cast<uint32_t>(peers.size() / 6) - before;
        if (added)
            log_msg("[magnet] pex added %u peers, total=%u\n", added,
                    static_cast<unsigned>(peers.size() / 6));
        peerCount = static_cast<uint32_t>(peers.size() / 6);
    }

    uint64_t deadline = now_ms() + kOverallTimeoutMs;
    std::mutex mutex;
    uint32_t nextPeer = 0;
    bool resolved = false;
    std::atomic<bool> stopWorkers{false};
    std::vector<uint8_t> metadata;
    std::vector<uint8_t> verifiedEndpoints;
    std::vector<std::thread> workers;
    uint32_t workerCount = std::min(peerCount, kMaxConcurrentPeers);
    for (uint32_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&] {
            while (!cancelled && !stopWorkers && now_ms() < deadline) {
                uint32_t peerIndex = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (resolved || nextPeer >= peerCount)
                        return;
                    peerIndex = nextPeer++;
                }

                std::vector<uint8_t> candidate;
                bool handshakeVerified = false;
                bool fetched = fetchMetadataFromPeer(
                    peers.data() + peerIndex * 6, spec, peerId, peerIndex,
                    peerCount, deadline, cancelled, stopWorkers, progress,
                    candidate, handshakeVerified);
                if (handshakeVerified) {
                    std::lock_guard<std::mutex> lock(mutex);
                    appendUniquePeers(verifiedEndpoints,
                                      peers.data() + peerIndex * 6, 1);
                }
                if (!fetched)
                    continue;

                std::vector<uint8_t> torrentProbe;
                std::string probeError;
                if (!buildTorrent(spec, candidate, torrentProbe, probeError)) {
                    log_msg("[magnet] peer %u/%u metadata rejected: %s\n",
                            peerIndex + 1, peerCount, probeError.c_str());
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!resolved) {
                        metadata = std::move(candidate);
                        resolved = true;
                        stopWorkers.store(true);
                    }
                }
                return;
            }
        });
    }
    for (std::thread& worker : workers)
        worker.join();

    if (cancelled) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!resolved) {
            error = "Metadata resolution was cancelled.";
            return false;
        }
    }
    if (metadata.empty()) {
        error = "Peers were found, but none returned torrent metadata. "
                "Try this catalog item again later.";
        return false;
    }
    if (progress)
        progress({MagnetProgress::Stage::Validating, 1, 1});
    std::vector<uint8_t> torrent;
    if (!buildTorrent(spec, metadata, torrent, error))
        return false;
    if (!writeTorrentAtomic(path, torrent, error))
        return false;
    if (verifiedPeers)
        *verifiedPeers = std::move(verifiedEndpoints);
    return true;
}

} // namespace pipensx
