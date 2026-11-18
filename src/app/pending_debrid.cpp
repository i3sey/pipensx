#include "pending_debrid.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace pipensx {
namespace {

using Json = nlohmann::json;

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

double clampProgress(double value) {
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

std::string serialize(const std::vector<PendingDebridEntry>& entries) {
    Json root = Json::object();
    root["version"] = 1;
    Json list = Json::array();
    for (const PendingDebridEntry& entry : entries) {
        Json item = Json::object();
        item["provider"] = pendingProviderKey(entry.provider);
        item["info_hash"] = entry.infoHash;
        item["debrid_id"] = entry.debridId;
        item["title"] = entry.title;
        item["progress"] = clampProgress(entry.progress);
        item["ready"] = entry.ready;
        item["created_at"] = entry.createdAt;
        item["updated_at"] = entry.updatedAt;
        list.push_back(std::move(item));
    }
    root["entries"] = std::move(list);
    return root.dump(2) + "\n";
}

bool parse(const std::string& text, std::vector<PendingDebridEntry>& entries,
           std::string& error) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Pending debrid file is not valid JSON.";
        return false;
    }
    if (!root.contains("entries"))
        return true;
    if (!root["entries"].is_array()) {
        error = "Pending debrid file is malformed: 'entries' must be a list.";
        return false;
    }
    for (const Json& item : root["entries"]) {
        if (!item.is_object())
            continue;
        if (!item.contains("info_hash") || !item["info_hash"].is_string() ||
            !item.contains("debrid_id") || !item["debrid_id"].is_string())
            continue;
        PendingDebridEntry parsed;
        parsed.infoHash = lowerAscii(item["info_hash"].get<std::string>());
        parsed.debridId = item["debrid_id"].get<std::string>();
        if (parsed.infoHash.empty() || parsed.debridId.empty())
            continue;
        if (item.contains("provider") && item["provider"].is_string()) {
            DebridProviderKind kind = DebridProviderKind::TorBox;
            if (!parsePendingProviderKey(item["provider"].get<std::string>(),
                                         kind))
                continue;
            parsed.provider = kind;
        }
        if (item.contains("title") && item["title"].is_string())
            parsed.title = item["title"].get<std::string>();
        if (item.contains("progress") && item["progress"].is_number())
            parsed.progress = clampProgress(item["progress"].get<double>());
        if (item.contains("ready") && item["ready"].is_boolean())
            parsed.ready = item["ready"].get<bool>();
        if (item.contains("created_at") && item["created_at"].is_number_integer())
            parsed.createdAt = item["created_at"].get<int64_t>();
        if (item.contains("updated_at") && item["updated_at"].is_number_integer())
            parsed.updatedAt = item["updated_at"].get<int64_t>();
        entries.push_back(std::move(parsed));
    }
    return true;
}

} // namespace

const char* pendingProviderKey(DebridProviderKind kind) {
    switch (kind) {
        case DebridProviderKind::TorBox: return "torbox";
        case DebridProviderKind::TorrServer: return "torrserver";
        case DebridProviderKind::RealDebrid: return "realdebrid";
        case DebridProviderKind::AllDebrid: return "alldebrid";
    }
    return "torbox";
}

bool parsePendingProviderKey(const std::string& text, DebridProviderKind& out) {
    const std::string key = lowerAscii(text);
    if (key == "torbox") {
        out = DebridProviderKind::TorBox;
        return true;
    }
    if (key == "torrserver") {
        out = DebridProviderKind::TorrServer;
        return true;
    }
    if (key == "realdebrid") {
        out = DebridProviderKind::RealDebrid;
        return true;
    }
    if (key == "alldebrid") {
        out = DebridProviderKind::AllDebrid;
        return true;
    }
    return false;
}

bool isDebridNotFoundError(const std::string& error) {
    const std::string lower = lowerAscii(error);
    return lower.find("not found") != std::string::npos ||
           lower.find("http 404") != std::string::npos ||
           lower.find("unknown_ressource") != std::string::npos ||
           lower.find("magnet_invalid_id") != std::string::npos ||
           lower.find("deleted") != std::string::npos ||
           lower.find("expired") != std::string::npos;
}

PendingDebridStore::PendingDebridStore(std::string rootPath)
    : path_(std::move(rootPath) + "/pending_debrid.json") {
    std::string ignored;
    load(ignored);
}

bool PendingDebridStore::load(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    error.clear();

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (errno != ENOENT) {
            error = std::string("Unable to open pending debrid list: ") +
                    std::strerror(errno);
            return false;
        }
        generation_.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "Unable to read pending debrid file.";
        return false;
    }
    std::vector<PendingDebridEntry> parsed;
    if (!parse(buffer.str(), parsed, error))
        return false;
    entries_ = std::move(parsed);
    generation_.fetch_add(1, std::memory_order_release);
    return true;
}

std::vector<PendingDebridEntry> PendingDebridStore::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::optional<PendingDebridEntry> PendingDebridStore::find(
    DebridProviderKind provider, const std::string& infoHash) const {
    const std::string needle = lowerAscii(infoHash);
    if (needle.empty())
        return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PendingDebridEntry& entry : entries_) {
        if (entry.provider == provider && entry.infoHash == needle)
            return entry;
    }
    return std::nullopt;
}

bool PendingDebridStore::upsert(const PendingDebridEntry& entry,
                                std::string& error) {
    error.clear();
    const std::string key = lowerAscii(entry.infoHash);
    if (key.empty() || entry.debridId.empty()) {
        error = "Missing info hash or debrid transfer id.";
        return false;
    }

    const int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingDebridEntry> next = entries_;

    auto found = std::find_if(next.begin(), next.end(),
                              [&](const PendingDebridEntry& existing) {
                                  return existing.provider == entry.provider &&
                                         existing.infoHash == key;
                              });
    PendingDebridEntry updated;
    if (found != next.end()) {
        updated = *found;
        updated.debridId = entry.debridId;
        if (!entry.title.empty())
            updated.title = entry.title;
        updated.progress = clampProgress(entry.progress);
        updated.ready = entry.ready;
        if (updated.createdAt <= 0)
            updated.createdAt = entry.createdAt > 0 ? entry.createdAt : nowSec;
        updated.updatedAt = entry.updatedAt > 0 ? entry.updatedAt : nowSec;
        *found = std::move(updated);
    } else {
        updated.infoHash = key;
        updated.debridId = entry.debridId;
        updated.provider = entry.provider;
        updated.title = entry.title;
        updated.progress = clampProgress(entry.progress);
        updated.ready = entry.ready;
        updated.createdAt = entry.createdAt > 0 ? entry.createdAt : nowSec;
        updated.updatedAt = entry.updatedAt > 0 ? entry.updatedAt : nowSec;
        while (next.size() >= kMaxEntries) {
            auto oldest = std::min_element(
                next.begin(), next.end(),
                [](const PendingDebridEntry& a, const PendingDebridEntry& b) {
                    const int64_t ta = a.updatedAt > 0 ? a.updatedAt : a.createdAt;
                    const int64_t tb = b.updatedAt > 0 ? b.updatedAt : b.createdAt;
                    return ta < tb;
                });
            next.erase(oldest);
        }
        next.push_back(std::move(updated));
    }

    if (!write(next, error))
        return false;
    entries_ = std::move(next);
    generation_.fetch_add(1, std::memory_order_release);
    return true;
}

bool PendingDebridStore::remove(DebridProviderKind provider,
                                const std::string& infoHash,
                                std::string& error) {
    error.clear();
    const std::string key = lowerAscii(infoHash);
    if (key.empty())
        return true;

    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(entries_.begin(), entries_.end(),
                              [&](const PendingDebridEntry& entry) {
                                  return entry.provider == provider &&
                                         entry.infoHash == key;
                              });
    if (found == entries_.end())
        return true;

    std::vector<PendingDebridEntry> next = entries_;
    next.erase(next.begin() + (found - entries_.begin()));
    if (!write(next, error))
        return false;
    entries_ = std::move(next);
    generation_.fetch_add(1, std::memory_order_release);
    return true;
}

std::vector<PendingDebridEntry> PendingDebridStore::clearAll(
    std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty())
        return {};
    std::vector<PendingDebridEntry> empty;
    if (!write(empty, error))
        return {};
    std::vector<PendingDebridEntry> removed = std::move(entries_);
    entries_.clear();
    generation_.fetch_add(1, std::memory_order_release);
    return removed;
}

std::vector<PendingDebridEntry> PendingDebridStore::pruneExpired(
    int64_t nowEpochSec, int64_t ttlSeconds) {
    if (nowEpochSec <= 0 || ttlSeconds <= 0)
        return {};

    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty())
        return {};

    std::vector<PendingDebridEntry> kept;
    std::vector<PendingDebridEntry> expired;
    kept.reserve(entries_.size());
    for (const PendingDebridEntry& entry : entries_) {
        const int64_t ref = entry.updatedAt > 0 ? entry.updatedAt : entry.createdAt;
        if (ref > 0 && nowEpochSec > ref &&
            (nowEpochSec - ref) >= ttlSeconds) {
            expired.push_back(entry);
        } else {
            kept.push_back(entry);
        }
    }
    if (expired.empty())
        return {};

    std::string ignored;
    if (!write(kept, ignored))
        return {};
    entries_ = std::move(kept);
    generation_.fetch_add(1, std::memory_order_release);
    return expired;
}

size_t PendingDebridStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool PendingDebridStore::write(const std::vector<PendingDebridEntry>& entries,
                               std::string& error) const {
    const std::string temporary = path_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create pending debrid file.";
            return false;
        }
        output << serialize(entries);
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write pending debrid file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path_.c_str()) == 0)
        return true;

    // FAT drivers on Switch may refuse to rename over an existing file.
    int renameError = errno;
    if ((unlink(path_.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path_.c_str()) == 0) {
        return true;
    }
    int finalError = errno;
    unlink(temporary.c_str());
    error = std::string("Unable to replace pending debrid file: ") +
            std::strerror(finalError ? finalError : renameError);
    return false;
}

} // namespace pipensx
