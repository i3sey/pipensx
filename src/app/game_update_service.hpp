#pragma once

#include "app/installed_title_service.hpp"
#include "app/update_source.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pipensx {

// Per-app state of the game-update check (check-only feature: it reports
// whether an update exists, it never downloads one).
enum class GameUpdateState {
    // Check has never run for this title (fresh install / first launch).
    NotChecked,
    // A check is in flight. Runtime only — never persisted.
    Checking,
    // Installed version is the newest the source knows.
    UpToDate,
    // The source knows a newer version.
    UpdateAvailable,
    // No update source for this title: the metadata index has no entry for
    // its title id, or the entries carry no version yet.
    SourceUnknown,
    // The check could not produce a verdict: the installed title reports no
    // version, or the candidate/current versions are not comparable.
    CheckError,
};

struct GameUpdateResult {
    GameUpdateState state = GameUpdateState::NotChecked;
    std::string currentVersion;
    std::string foundVersion;
    std::string error;
    uint64_t checkedAt = 0;
};

// Sorted by title id for deterministic rendering and persistence.
using GameUpdateResults = std::map<std::string, GameUpdateResult>;

// In-memory game-update check over the metadata index (catalog∩titledb).
// The check itself is synchronous and network-free: the candidate versions
// are already loaded with the metadata index. Results persist to a JSON file
// and survive relaunches; stale() reports when they no longer reflect the
// current index or installed set.
class GameUpdateService {
public:
    // `source` must outlive this service; null disables the check entirely
    // (every title reports SourceUnknown).
    explicit GameUpdateService(const IUpdateMetadataSource* source,
                               std::string statePath);

    GameUpdateService(const GameUpdateService&) = delete;
    GameUpdateService& operator=(const GameUpdateService&) = delete;

    // Synchronous, in-memory. `currentVersion` is the installed NACP
    // display_version. Stores the result and persists synchronously.
    GameUpdateResult checkOne(const std::string& titleId,
                              const std::string& currentVersion,
                              std::string& saveError);
    // Re-checks every installed title and replaces the whole result set.
    // Snapshot the caller's generations here so stale() can compare them.
    void checkAll(const std::vector<InstalledTitle>& titles,
                  uint64_t installedGeneration, uint64_t metadataRefreshMs,
                  std::string& saveError);

    const GameUpdateResults& results() const { return results_; }
    const GameUpdateResult* find(const std::string& titleId) const;

    bool load(std::string& error);
    bool save(std::string& error) const;

    // True when persisted results may no longer reflect reality: the
    // installed set/versions changed, or the metadata index (the source of
    // latestVersion) was refreshed since the last checkAll. False when
    // nothing was ever checked.
    bool stale(uint64_t installedGeneration, uint64_t metadataRefreshMs) const;
    uint64_t lastCheckedAt() const { return lastCheckedAt_; }

    // State name round-trips (persisted JSON strings).
    static const char* stateName(GameUpdateState state);

private:
    GameUpdateResult compute(const std::string& titleId,
                             const std::string& currentVersion) const;

    const IUpdateMetadataSource* source_;
    std::string statePath_;
    GameUpdateResults results_;
    uint64_t installedGeneration_ = 0;
    uint64_t metadataRefreshMs_ = 0;
    uint64_t lastCheckedAt_ = 0;
    bool checking_ = false;
};

} // namespace pipensx
