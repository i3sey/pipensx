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

// User-supplied proxy for the connectivity ladder (Phase W). Off = no manual
// proxy configured.
enum class ProxyType {
    Off,
    Http,
    Socks5,
};

// Winning route recorded by the first-run connectivity wizard (Phase W2), so
// later launches take it silently.
enum class ConnectivityMethod {
    Direct,
    Proxy,
    Antizapret,
    Mirror,
};

struct AppSettingsData {
    CatalogFilter catalogFilter = CatalogFilter::All;
    bool refreshCatalogOnLaunch = false;
    StreamSelection streamSelection = StreamSelection::AllFiles;
    InstallLocation installLocation = InstallLocation::SdCard;
    bool showCompletedDownloads = true;
    bool extendedTelemetry = false;
    // Route RuTracker traffic through antizapret proxies when direct access is
    // blocked (DPI). Default on preserves current unconditional behavior.
    bool useAntizapret = true;
    // Phase W connectivity ladder. Defaults preserve current behavior: no
    // manual proxy, default host, wizard not yet run, direct route.
    std::string manualProxyUrl;
    ProxyType manualProxyType = ProxyType::Off;
    std::string rutrackerHost;  // empty = rutracker.org
    bool connectivitySetupDone = false;
    ConnectivityMethod connectivityMethod = ConnectivityMethod::Direct;

    bool operator==(const AppSettingsData& other) const;
    bool operator!=(const AppSettingsData& other) const {
        return !(*this == other);
    }
};

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
