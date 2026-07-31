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
    // UI language: "auto" follows the console's system language, otherwise a
    // borealis locale directory name. Read before Application::init() to set
    // Platform::APP_LOCALE_DEFAULT; borealis loads translations once, so a
    // change only takes effect on the next launch.
    std::string language = "auto";
    CatalogFilter catalogFilter = CatalogFilter::Games;
    bool refreshCatalogOnLaunch = false;
    uint64_t lastCatalogRefreshMs = 0;
    uint64_t lastMetadataRefreshMs = 0;
    uint64_t lastModsRefreshMs = 0;
    StreamSelection streamSelection = StreamSelection::AllFiles;
    InstallLocation installLocation = InstallLocation::SdCard;
    bool showCompletedDownloads = true;
    bool extendedTelemetry = false;
    bool checkForUpdatesOnLaunch = true;
    // First-run disclaimer: the catalog is a third-party RuTracker dump. Shown
    // once, then this is set so later launches skip it.
    bool catalogDisclaimerAcknowledged = false;
    // Web companion LAN server (plain HTTP, port 8080). The PIN gates
    // mutating endpoints; empty = no auth, otherwise 4-8 digits (enforced at
    // parse time so a hand-edited settings.json cannot smuggle odd values).
    bool webServerEnabled = true;
    std::string webServerPin;
    // How many torrents download at once. The default 1 is the serial
    // queue: nothing shares the link, so a single transfer runs at the speed
    // the swarm can actually give it. Raising it splits bandwidth, RAM budget
    // and SD throughput between tasks — opt-in, not something a stock install
    // does behind the user's back. Hand-edited values are clamped to the
    // supported range at parse time.
    uint32_t maxActiveDownloads = 1;

    bool operator==(const AppSettingsData& other) const;
    bool operator!=(const AppSettingsData& other) const {
        return !(*this == other);
    }
};

// Supported values for AppSettingsData::language, in the order the Settings
// selector lists them. Anything else is rejected at parse time, so a hand-edited
// settings.json cannot leave the app pointing at a locale we do not ship.
inline constexpr const char* kLanguageValues[] = {"auto", "en-US", "ru"};

bool isSupportedLanguage(const std::string& value);

// True for a valid web PIN: empty (auth off) or 4-8 ASCII digits.
bool isValidWebPin(const std::string& value);

// The supported range for AppSettingsData::maxActiveDownloads lives in
// download_manager.hpp: it is the engine's limit, and settings only validate
// against it. Include that header where you need clampMaxActiveDownloads.

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
