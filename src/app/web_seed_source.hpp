#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pipensx {

// Fetches whole torrent pieces from a BEP-19 web seed (HTTP "GetRight" URL)
// using HTTP Range requests, on background threads. The owner — the torrent
// tick thread — assigns pieces with enqueue() and drains finished ones with
// popCompleted(); this class never touches the torrent engine itself, so all
// piece-state reads/writes stay on the tick thread and no locking crosses into
// core. Verification of the fetched bytes is the engine's job
// (torrent_submit_web_piece re-checks the SHA-1), so a hostile or broken web
// seed can only waste bandwidth, never corrupt the download.
//
// Backoff and source-disablement: when a piece fetch fails, the piece enters
// exponential backoff (2^n * baseBackoffSec, capped at maxBackoffSec).
// After maxConsecutiveFailures consecutive failures the source is marked dead
// and excluded from scheduling; it revives on any successful piece or after
// deadReviveSec seconds.
class WebSeedSource {
public:
    struct BackoffPolicy {
        int maxConsecutiveFailures = 10;
        int baseBackoffSec = 2;
        int maxBackoffSec = 300;
        int deadReviveSec = 120;
    };

    struct Completed {
        uint32_t piece = 0;
        std::vector<uint8_t> data;
        bool ok = false;
    };

    // baseUrl: a url-list entry. name: torrent name, appended when the URL is
    // directory-style (ends with '/'). pieceLength/totalLength/numPieces come
    // from the metainfo. Single-file torrents only (caller enforces).
    WebSeedSource(const std::string& baseUrl, const std::string& name,
                  uint64_t pieceLength, uint64_t totalLength,
                  uint32_t numPieces, unsigned threads = 4);
    WebSeedSource(const std::string& baseUrl, const std::string& name,
                  uint64_t pieceLength, uint64_t totalLength,
                  uint32_t numPieces, unsigned threads,
                  BackoffPolicy backoffPolicy);
    ~WebSeedSource();

    WebSeedSource(const WebSeedSource&) = delete;
    WebSeedSource& operator=(const WebSeedSource&) = delete;

    // Assign a piece to fetch.  Returns false when the piece is already
    // queued/in-flight, in backoff, or the source is dead.
    bool enqueue(uint32_t piece);
    // Pieces assigned but not yet drained (queued + fetching + ready).
    size_t inFlight() const;
    // Pop one finished piece (success or failure). False if none ready.
    bool popCompleted(Completed& out);

    uint32_t numPieces() const { return numPieces_; }

    // True when the source is in its dead-cooldown interval.
    bool isDead() const;

private:
    struct BackoffEntry {
        std::chrono::steady_clock::time_point retryAfter;
        int attempts = 0;
    };

    void workerMain();
    bool fetchPiece(uint32_t piece, std::vector<uint8_t>& out);
    uint64_t pieceLen(uint32_t piece) const;
    void recordFailure(uint32_t piece);
    void recordSuccess(uint32_t piece);

    std::string url_;
    uint64_t pieceLength_;
    uint64_t totalLength_;
    uint32_t numPieces_;
    BackoffPolicy backoffPolicy_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<uint32_t> pending_;
    std::unordered_set<uint32_t> assigned_;
    std::deque<Completed> completed_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};

    std::unordered_map<uint32_t, BackoffEntry> backoff_;
    int consecutiveFailures_ = 0;
    bool dead_ = false;
    std::chrono::steady_clock::time_point deadUntil_;
};

} // namespace pipensx
