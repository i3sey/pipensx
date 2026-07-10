#pragma once

#include <cstdint>
#include <string>

namespace pipensx {

enum class CatalogFilter {
    All,
    Games,
};

enum class StreamSelection {
    AllFiles,
    PackagesOnly,
};

// Where stream installs commit content (PERF_PLAN 7.4). SystemMemory targets
// eMMC/NAND, whose write path is typically faster than SD.
enum class InstallLocation {
    SdCard,
    SystemMemory,
};

struct AppSettingsData {
    CatalogFilter catalogFilter = CatalogFilter::Games;
    bool refreshCatalogOnLaunch = false;
    uint64_t lastCatalogRefreshMs = 0;
    uint64_t lastMetadataRefreshMs = 0;
    StreamSelection streamSelection = StreamSelection::AllFiles;
    InstallLocation installLocation = InstallLocation::SdCard;
    bool showCompletedDownloads = true;
    bool extendedTelemetry = false;
    bool checkForUpdatesOnLaunch = true;
    // First-run disclaimer: the catalog is a third-party RuTracker dump. Shown
    // once, then this is set so later launches skip it.
    bool catalogDisclaimerAcknowledged = false;

    bool operator==(const AppSettingsData& other) const;
    bool operator!=(const AppSettingsData& other) const {
        return !(*this == other);
    }
};

bool dailyRefreshDue(uint64_t nowMs, uint64_t lastRefreshMs);

class AppSettings {
public:
    explicit AppSettings(std::string path,
                         std::string legacyTelemetryPath = {});

    bool load(std::string& error);
    bool update(const AppSettingsData& values, std::string& error);
    bool reset(std::string& error);

    const AppSettingsData& get() const { return values_; }
    uint64_t generation() const { return generation_; }
    const std::string& path() const { return path_; }

private:
    bool write(const AppSettingsData& values, std::string& error) const;

    std::string path_;
    std::string legacyTelemetryPath_;
    AppSettingsData values_;
    uint64_t generation_ = 0;
};

} // namespace pipensx
