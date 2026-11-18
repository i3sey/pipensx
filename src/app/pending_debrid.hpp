#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "debrid_provider.hpp"

namespace pipensx {

struct PendingDebridEntry {
    std::string infoHash;  // lowercase hex
    std::string debridId;
    DebridProviderKind provider = DebridProviderKind::TorBox;
    std::string title;
    double progress = 0.0;  // 0..1
    bool ready = false;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

const char* pendingProviderKey(DebridProviderKind kind);
bool parsePendingProviderKey(const std::string& text, DebridProviderKind& out);
bool isDebridNotFoundError(const std::string& error);

// Persistent store of unfinished (or ready-to-continue) cloud debrid transfers
// kept in <rootPath>/pending_debrid.json so exiting preparation does not lose
// background caching progress.
class PendingDebridStore {
public:
    static constexpr int64_t kDefaultTtlSeconds = 7 * 24 * 3600;
    static constexpr size_t kMaxEntries = 200;

    explicit PendingDebridStore(std::string rootPath);

    // Missing file is an empty store, not an error.
    bool load(std::string& error);

    std::vector<PendingDebridEntry> list() const;
    std::optional<PendingDebridEntry> find(DebridProviderKind provider,
                                           const std::string& infoHash) const;

    bool upsert(const PendingDebridEntry& entry, std::string& error);
    bool remove(DebridProviderKind provider, const std::string& infoHash,
                std::string& error);
    std::vector<PendingDebridEntry> clearAll(std::string& error);
    std::vector<PendingDebridEntry> pruneExpired(
        int64_t nowEpochSec, int64_t ttlSeconds = kDefaultTtlSeconds);

    size_t size() const;
    uint64_t generation() const {
        return generation_.load(std::memory_order_acquire);
    }
    const std::string& path() const { return path_; }

private:
    bool write(const std::vector<PendingDebridEntry>& entries,
               std::string& error) const;

    std::string path_;
    mutable std::mutex mutex_;
    std::vector<PendingDebridEntry> entries_;
    std::atomic<uint64_t> generation_{0};
};

} // namespace pipensx
