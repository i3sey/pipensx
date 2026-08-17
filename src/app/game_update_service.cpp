#include "game_update_service.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

constexpr int kStateVersion = 1;

// Strict decimal parse ("131072"); rejects signs, whitespace and overflow.
bool parseDecimal(const std::string& text, uint64_t& out) {
    if (text.empty())
        return false;
    uint64_t value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parseState(const std::string& name, GameUpdateState& out) {
    if (name == "update_available") {
        out = GameUpdateState::UpdateAvailable;
        return true;
    }
    if (name == "up_to_date") {
        out = GameUpdateState::UpToDate;
        return true;
    }
    if (name == "source_unknown") {
        out = GameUpdateState::SourceUnknown;
        return true;
    }
    if (name == "check_error") {
        out = GameUpdateState::CheckError;
        return true;
    }
    if (name == "not_checked") {
        out = GameUpdateState::NotChecked;
        return true;
    }
    return false;
}

} // namespace

const char* GameUpdateService::stateName(GameUpdateState state) {
    switch (state) {
    case GameUpdateState::NotChecked:
        return "not_checked";
    case GameUpdateState::Checking:
        return "checking";
    case GameUpdateState::UpToDate:
        return "up_to_date";
    case GameUpdateState::UpdateAvailable:
        return "update_available";
    case GameUpdateState::SourceUnknown:
        return "source_unknown";
    case GameUpdateState::CheckError:
        return "check_error";
    }
    return "not_checked";
}

GameUpdateService::GameUpdateService(const IUpdateMetadataSource* source,
                                     std::string statePath)
    : source_(source), statePath_(std::move(statePath)) {}

GameUpdateResult GameUpdateService::compute(
    const std::string& titleId, const std::string& currentVersion) const {
    GameUpdateResult result;
    result.currentVersion = currentVersion;

    std::vector<std::string> candidates;
    if (!source_ || !source_->collectLatestVersions(titleId, candidates)) {
        // No entry at all for this title: no update source (req #7).
        result.state = GameUpdateState::SourceUnknown;
        return result;
    }

    // Versions are decimal title versions ("0", "131072"): the installed
    // side comes from the Patch content meta, the candidate side from the
    // [vN] tags in the source release file names. Max-fold across
    // bundles/regions (Q6); a non-numeric candidate is a source data bug and
    // is reported as CheckError, not silently skipped.
    std::string best;
    uint64_t bestValue = 0;
    std::string firstNonEmpty;
    bool anyParseable = false;
    for (const std::string& candidate : candidates) {
        if (candidate.empty())
            continue;
        if (firstNonEmpty.empty())
            firstNonEmpty = candidate;
        uint64_t value = 0;
        if (!parseDecimal(candidate, value))
            continue;
        if (!anyParseable || value > bestValue) {
            best = candidate;
            bestValue = value;
            anyParseable = true;
        }
    }
    if (firstNonEmpty.empty()) {
        // Entries exist but none carries a version yet (forward-compat: the
        // index has not started emitting latestVersion).
        result.state = GameUpdateState::SourceUnknown;
        return result;
    }
    if (!anyParseable) {
        result.state = GameUpdateState::CheckError;
        result.error = "Update source version is not numeric: " +
                       firstNonEmpty + ".";
        return result;
    }
    result.foundVersion = best;

    if (currentVersion.empty()) {
        result.state = GameUpdateState::CheckError;
        result.error = "Installed title reports no version.";
        return result;
    }
    uint64_t currentValue = 0;
    if (!parseDecimal(currentVersion, currentValue)) {
        result.state = GameUpdateState::CheckError;
        result.error = "Installed title version is not numeric: " +
                       currentVersion + ".";
        return result;
    }
    result.state = bestValue > currentValue
                       ? GameUpdateState::UpdateAvailable
                       : GameUpdateState::UpToDate;
    return result;
}

GameUpdateResult GameUpdateService::checkOne(
    const std::string& titleId, const std::string& currentVersion,
    std::string& saveError) {
    saveError.clear();
    GameUpdateResult previous;
    auto it = results_.find(titleId);
    if (it != results_.end())
        previous = it->second;
    if (checking_)
        return previous;
    checking_ = true;
    GameUpdateResult result = compute(titleId, currentVersion);
    result.checkedAt = now_ms();
    results_[titleId] = result;
    ++resultsGeneration_;
    checking_ = false;
    save(saveError);
    return result;
}

void GameUpdateService::checkAll(const std::vector<InstalledTitle>& titles,
                                 uint64_t installedGeneration,
                                 uint64_t metadataRefreshMs,
                                 std::string& saveError) {
    saveError.clear();
    if (checking_)
        return;
    checking_ = true;
    const uint64_t checkedAt = now_ms();
    GameUpdateResults next;
    size_t updates = 0;
    for (const InstalledTitle& title : titles) {
        GameUpdateResult result = compute(title.titleId, title.version);
        result.checkedAt = checkedAt;
        if (result.state == GameUpdateState::UpdateAvailable)
            ++updates;
        next[title.titleId] = std::move(result);
    }
    results_ = std::move(next);
    ++resultsGeneration_;
    installedGeneration_ = installedGeneration;
    metadataRefreshMs_ = metadataRefreshMs;
    lastCheckedAt_ = checkedAt;
    checking_ = false;
    save(saveError);
    telemetry_log("game_updates", "check_all",
                  "event=check_all count=%zu updates=%zu duration_ms=%llu",
                  titles.size(), updates,
                  static_cast<unsigned long long>(now_ms() - checkedAt));
}

const GameUpdateResult* GameUpdateService::find(
    const std::string& titleId) const {
    auto it = results_.find(titleId);
    return it == results_.end() ? nullptr : &it->second;
}

bool GameUpdateService::isIgnored(const std::string& titleId) const {
    return ignored_.count(titleId) != 0;
}

void GameUpdateService::setIgnored(const std::string& titleId, bool ignored,
                                   std::string& saveError) {
    saveError.clear();
    if (ignored)
        ignored_.insert(titleId);
    else
        ignored_.erase(titleId);
    save(saveError);
}

size_t GameUpdateService::availableCount(
    const std::vector<InstalledTitle>& titles) const {
    size_t count = 0;
    for (const InstalledTitle& title : titles) {
        if (ignored_.count(title.titleId))
            continue;
        auto it = results_.find(title.titleId);
        if (it != results_.end() &&
            it->second.state == GameUpdateState::UpdateAvailable)
            ++count;
    }
    return count;
}

bool GameUpdateService::load(std::string& error) {
    error.clear();
    results_.clear();
    ignored_.clear();
    installedGeneration_ = 0;
    metadataRefreshMs_ = 0;
    lastCheckedAt_ = 0;
    resultsGeneration_ = 0;

    std::ifstream input(statePath_, std::ios::binary);
    if (!input) {
        if (errno != ENOENT) {
            error = std::string("Unable to open game-update state: ") +
                    std::strerror(errno);
            return false;
        }
        return true;
    }
    std::string data((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    nlohmann::json root = nlohmann::json::parse(data, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        root.value("version", 0) != kStateVersion) {
        error = "Game-update state file is not valid.";
        return false;
    }
    installedGeneration_ = root.value("installed_generation", uint64_t{0});
    metadataRefreshMs_ = root.value("metadata_refresh_ms", uint64_t{0});
    lastCheckedAt_ = root.value("last_checked_at", uint64_t{0});
    if (root.contains("results") && root["results"].is_array()) {
        for (const auto& item : root["results"]) {
            if (!item.is_object())
                continue;
            const std::string titleId = item.value("title_id", "");
            if (titleId.empty())
                continue;
            GameUpdateResult result;
            if (!parseState(item.value("state", ""), result.state))
                continue;
            result.currentVersion = item.value("current_version", "");
            result.foundVersion = item.value("found_version", "");
            result.error = item.value("error", "");
            result.checkedAt = item.value("checked_at", uint64_t{0});
            results_[titleId] = std::move(result);
        }
    }
    if (root.contains("ignored_title_ids") &&
        root["ignored_title_ids"].is_array()) {
        for (const auto& item : root["ignored_title_ids"]) {
            if (!item.is_string())
                continue;
            const std::string titleId = item.get<std::string>();
            if (!titleId.empty())
                ignored_.insert(titleId);
        }
    }
    return true;
}

bool GameUpdateService::save(std::string& error) const {
    error.clear();
    nlohmann::json root = nlohmann::json::object();
    root["version"] = kStateVersion;
    root["installed_generation"] = installedGeneration_;
    root["metadata_refresh_ms"] = metadataRefreshMs_;
    root["last_checked_at"] = lastCheckedAt_;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& entry : results_) {
        nlohmann::json item = nlohmann::json::object();
        item["title_id"] = entry.first;
        item["state"] = stateName(entry.second.state);
        item["current_version"] = entry.second.currentVersion;
        item["found_version"] = entry.second.foundVersion;
        item["error"] = entry.second.error;
        item["checked_at"] = entry.second.checkedAt;
        list.push_back(std::move(item));
    }
    root["results"] = std::move(list);
    nlohmann::json ignored = nlohmann::json::array();
    for (const std::string& titleId : ignored_)
        ignored.push_back(titleId);
    root["ignored_title_ids"] = std::move(ignored);

    std::string temporary = statePath_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create game-update state file.";
            return false;
        }
        output << root.dump(2) << "\n";
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write game-update state file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), statePath_.c_str()) == 0)
        return true;

    // Same fallback as AppSettings::write: some FAT drivers refuse to rename
    // over an existing file.
    int renameError = errno;
    if ((unlink(statePath_.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), statePath_.c_str()) == 0) {
        return true;
    }
    int finalError = errno;
    unlink(temporary.c_str());
    error = std::string("Unable to replace game-update state file: ") +
            std::strerror(finalError ? finalError : renameError);
    return false;
}

bool GameUpdateService::stale(uint64_t installedGeneration,
                              uint64_t metadataRefreshMs) const {
    if (lastCheckedAt_ == 0)
        return false;
    return installedGeneration_ != installedGeneration ||
           metadataRefreshMs_ != metadataRefreshMs;
}

} // namespace pipensx
