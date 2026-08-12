#include "app/app_settings.hpp"
#include "app/download_manager.hpp"  // clampMaxActiveDownloads

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <unistd.h>

using pipensx::AppSettings;
using pipensx::AppSettingsData;
using pipensx::CatalogFilter;
using pipensx::DebridProviderKind;
using pipensx::StreamSelection;
using pipensx::InstallLocation;
using pipensx::dailyRefreshDue;
using pipensx::isLocalToday;
using pipensx::isValidProxyUrl;
using pipensx::isValidCatalogSourceUrl;
using pipensx::effectiveCatalogSourceUrl;
using pipensx::applyProxySetting;

namespace {

const char* SettingsPath = "/tmp/pipensx-settings-test.json";
const char* LegacyPath = "/tmp/pipensx-settings-test.enabled";

void cleanup() {
    unlink(SettingsPath);
    unlink("/tmp/pipensx-settings-test.json.tmp");
    unlink(LegacyPath);
}

void testMissingFileUsesSafeDefaults() {
    cleanup();
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    const AppSettingsData& values = settings.get();
    assert(values.catalogFilter == CatalogFilter::Games);
    assert(!values.refreshCatalogOnLaunch);
    assert(values.lastCatalogRefreshMs == 0);
    assert(values.lastCatalogRefreshWallSec == 0);
    assert(values.lastMetadataRefreshMs == 0);
    assert(values.lastModsRefreshMs == 0);
    assert(values.streamSelection == StreamSelection::AllFiles);
    assert(values.installLocation == InstallLocation::SdCard);
    assert(values.showCompletedDownloads);
    assert(!values.extendedTelemetry);
    assert(values.checkForUpdatesOnLaunch);
    assert(values.maxActiveDownloads == 1);
    // "auto" keeps the console's system language, so a Russian Switch gets a
    // Russian UI on first launch with no user action.
    assert(values.language == "auto");
    assert(!values.catalogDisclaimerAcknowledged);
    // Torrenting stays off until the user opts in on the first-run screen.
    assert(!values.torrentingEnabled);
    assert(values.torboxApiKey.empty());
    assert(values.torrserverUrl.empty());
    assert(values.debridProvider == DebridProviderKind::TorBox);
    assert(!values.firstRunCompleted);
    // Fresh install gets a random companion PIN so mutations are never open.
    assert(values.webServerPin.size() == 6);
    assert(pipensx::isValidWebPin(values.webServerPin));
    assert(!values.webServerPin.empty());
    assert(access(SettingsPath, F_OK) == 0);
}

void testUpdatePersistsEveryPublicSetting() {
    cleanup();
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    AppSettingsData changed = settings.get();
    changed.language = "ru";
    changed.catalogFilter = CatalogFilter::All;
    changed.refreshCatalogOnLaunch = true;
    changed.lastCatalogRefreshMs = 123456;
    changed.lastCatalogRefreshWallSec = 1700000000;
    changed.lastMetadataRefreshMs = 234567;
    changed.lastModsRefreshMs = 345678;
    changed.streamSelection = StreamSelection::PackagesOnly;
    changed.installLocation = InstallLocation::SystemMemory;
    changed.showCompletedDownloads = false;
    changed.extendedTelemetry = true;
    changed.checkForUpdatesOnLaunch = false;
    changed.webServerEnabled = false;
    changed.webServerPin = "12345678";
    changed.maxActiveDownloads = 3;
    changed.catalogDisclaimerAcknowledged = true;
    changed.torrentingEnabled = true;
    changed.torboxApiKey = "0a1b2c3d-4e5f-6789-abcd-ef0123456789";
    changed.torrserverUrl = "http://192.168.1.10:8090";
    changed.catalogSourceUrl =
        "https://cdn.example.com/repo/switch_games.json";
    changed.debridProvider = DebridProviderKind::TorrServer;
    changed.firstRunCompleted = true;
    assert(settings.update(changed, error));

    AppSettings restored(SettingsPath, LegacyPath);
    assert(restored.load(error));
    assert(restored.get() == changed);
}

void testOldSettingsJsonDefaultsRefreshTimes() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << "{"
               << "\"version\":1,"
               << "\"catalog_filter\":\"games\","
               << "\"refresh_catalog_on_launch\":true,"
               << "\"stream_selection\":\"all_files\","
               << "\"install_location\":\"sd_card\","
               << "\"show_completed_downloads\":true,"
               << "\"extended_telemetry\":false,"
               << "\"catalog_disclaimer_ack\":true"
               << "}";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().refreshCatalogOnLaunch);
    assert(settings.get().lastCatalogRefreshMs == 0);
    assert(settings.get().lastCatalogRefreshWallSec == 0);
    assert(settings.get().lastMetadataRefreshMs == 0);
    assert(settings.get().lastModsRefreshMs == 0);
    assert(settings.get().catalogDisclaimerAcknowledged);
    assert(settings.get().checkForUpdatesOnLaunch);
}

void testInvalidFileFailsClosedToDefaults() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << "{not-json";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(!settings.load(error));
    assert(!error.empty());
    assert(settings.get() == AppSettingsData{});
}

// A hand-edited settings.json must not leave the app pointing at a locale we
// do not ship: borealis would log a load failure and fall back per-key, which
// reads as a half-translated UI rather than an error.
void testUnknownLanguageIsRejected() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":1,"language":"klingon"})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(!settings.load(error));
    assert(!error.empty());
    assert(settings.get() == AppSettingsData{});

    for (const char* supported : pipensx::kLanguageValues)
        assert(pipensx::isSupportedLanguage(supported));
    assert(!pipensx::isSupportedLanguage("klingon"));
}

void testLegacyTelemetryFlagMigratesOnce() {
    cleanup();
    {
        std::ofstream output(LegacyPath);
        output << "enabled\n";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().extendedTelemetry);
    assert(!settings.get().webServerPin.empty());
    assert(access(SettingsPath, F_OK) == 0);
    assert(access(LegacyPath, F_OK) != 0);
}

// A hand-edited PIN that is not 4-8 digits is replaced with a generated PIN
// rather than leaving the companion open on the LAN.
void testInvalidWebPinIsCleared() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":1,"web_server_pin":"letters"})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().webServerPin.size() == 6);
    assert(pipensx::isValidWebPin(settings.get().webServerPin));
    assert(settings.get().webServerEnabled);

    assert(pipensx::isValidWebPin(""));
    assert(pipensx::isValidWebPin("1234"));
    assert(pipensx::isValidWebPin("12345678"));
    assert(!pipensx::isValidWebPin("123"));
    assert(!pipensx::isValidWebPin("123456789"));
    assert(!pipensx::isValidWebPin("12a4"));

    const std::string generated = pipensx::generateWebPin();
    assert(generated.size() == 6);
    assert(pipensx::isValidWebPin(generated));
}

// Clearing the PIN via update() regenerates one instead of storing empty.
void testUpdateEmptyPinRegenerates() {
    cleanup();
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    AppSettingsData values = settings.get();
    const std::string previous = values.webServerPin;
    values.webServerPin.clear();
    assert(settings.update(values, error));
    assert(settings.get().webServerPin.size() == 6);
    assert(pipensx::isValidWebPin(settings.get().webServerPin));
    (void)previous;
}

// A hand-edited count outside [1,4] degrades to the nearest supported value
// rather than failing the whole settings load.
void testMaxActiveDownloadsClamped() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":2,"max_active_downloads":99})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().maxActiveDownloads == pipensx::kMaxActiveDownloads);

    assert(pipensx::clampMaxActiveDownloads(0) == 1);
    assert(pipensx::clampMaxActiveDownloads(1) == 1);
    assert(pipensx::clampMaxActiveDownloads(4) == 4);
    assert(pipensx::clampMaxActiveDownloads(5) == 4);
    assert(pipensx::clampMaxActiveDownloads(UINT64_MAX) == 4);
}

// v1 -> v2: every stored download count goes back to the serial queue, and
// everything else in the file comes through untouched. Raising the count
// afterwards has to survive a restart — the update() that persists it also
// stamps the new version, which is what ends the reset.
void testVersionOneResetsActiveDownloads() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":1,"max_active_downloads":4,)"
               << R"("language":"ru","web_server_pin":"4242"})";
    }
    {
        AppSettings settings(SettingsPath, LegacyPath);
        std::string error;
        assert(settings.load(error));
        assert(settings.get().maxActiveDownloads == 1);
        assert(settings.get().language == "ru");
        assert(settings.get().webServerPin == "4242");
    }
    // update() stamps the new version, so a deliberate 4 now sticks.
    {
        AppSettings settings(SettingsPath, LegacyPath);
        std::string error;
        assert(settings.load(error));
        AppSettingsData values = settings.get();
        values.maxActiveDownloads = 4;
        assert(settings.update(values, error));
    }
    {
        AppSettings settings(SettingsPath, LegacyPath);
        std::string error;
        assert(settings.load(error));
        assert(settings.get().maxActiveDownloads == 4);
    }
}

// A file from a build newer than this one is not something we can safely
// reinterpret, so it fails closed instead of being silently downgraded.
void testFutureVersionIsRejected() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":99,"language":"ru"})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(!settings.load(error));
    assert(!error.empty());
}

void testDailyRefreshDue() {
    const uint64_t day = 24ULL * 60ULL * 60ULL * 1000ULL;
    assert(dailyRefreshDue(1000, 0));
    assert(!dailyRefreshDue(day + 999, 1000));
    assert(dailyRefreshDue(day + 1000, 1000));
    assert(dailyRefreshDue(999, 1000));
}

void testIsLocalToday() {
    assert(!isLocalToday(0));
    assert(!isLocalToday(-1));
    assert(isLocalToday(static_cast<int64_t>(time(nullptr))));
    assert(!isLocalToday(static_cast<int64_t>(time(nullptr)) - 48 * 60 * 60));
}

void testLegacyDebridKeysTolerated() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << "{\"version\":1,\"torrenting_enabled\":true,"
               << "\"torbox_api_key\":\"abc\"}";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().torrentingEnabled);
    assert(settings.get().torboxApiKey == "abc");
}

// v2 -> v3: the struct default flipped torrenting off when debrid landed. A
// file written before that has no say in the matter, so it must migrate to on
// rather than leave an existing install unable to download anything.
void testPreV3FileKeepsTorrentingOn() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << "{\"version\":2,\"web_server_enabled\":true}";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().torrentingEnabled);
}

void testVersionThreeHonoursTorrentingOff() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << "{\"version\":3,\"torrenting_enabled\":false}";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(!settings.get().torrentingEnabled);
}

void testProxyUrlValidation() {
    assert(isValidProxyUrl(""));
    assert(isValidProxyUrl("socks5://192.168.1.2:10808"));
    assert(isValidProxyUrl("socks5h://proxy.lan:1080"));
    assert(isValidProxyUrl("http://10.0.0.1:3128"));
    assert(isValidProxyUrl("https://proxy.example.com"));
    // No scheme, unsupported scheme, or anything past host:port.
    assert(!isValidProxyUrl("192.168.1.2:10808"));
    assert(!isValidProxyUrl("ftp://proxy:21"));
    assert(!isValidProxyUrl("socks5://proxy:1080/path"));
    assert(!isValidProxyUrl("socks5://user@proxy:1080"));
    assert(!isValidProxyUrl("socks5://proxy:0"));
    assert(!isValidProxyUrl("socks5://proxy:70000"));
    assert(!isValidProxyUrl("socks5://proxy:abc"));
    assert(!isValidProxyUrl("socks5://"));
    assert(!isValidProxyUrl("socks5://host with space:1080"));
}

// A hand-edited settings.json must not be able to smuggle a proxy the
// validator would reject through the parser.
void testInvalidProxyUrlIsCleared() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":3,"proxy_url":"ftp://nope:21"})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().proxyUrl.empty());
}

void testInvalidCatalogSourceUrlIsCleared() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":3,"catalog_source_url":"ftp://nope/catalog.json"})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().catalogSourceUrl.empty());
}

void testCatalogSourceUrlValidation() {
    assert(isValidCatalogSourceUrl(""));
    assert(isValidCatalogSourceUrl(
        "https://cdn.example.com/repo/catalog.json"));
    assert(!isValidCatalogSourceUrl(
        "http://cdn.example.com/repo/catalog.json"));
    assert(!isValidCatalogSourceUrl("https://"));
    assert(!isValidCatalogSourceUrl(
        "https://user:pass@cdn.example.com/repo/catalog.json"));
    assert(!isValidCatalogSourceUrl(std::string(513, 'a')));
    assert(effectiveCatalogSourceUrl("") ==
           "https://raw.githubusercontent.com/Langegen/switch-games/"
           "refs/heads/main/switch_games.json");
    assert(effectiveCatalogSourceUrl("https://cdn.example.com/x.json") ==
           "https://cdn.example.com/x.json");
}

void testProxySettingReachesEnvironment() {
    applyProxySetting("socks5://192.168.1.2:10808");
    const char* value = std::getenv("ALL_PROXY");
    assert(value && std::string(value) == "socks5://192.168.1.2:10808");
    const char* noProxy = std::getenv("NO_PROXY");
    assert(noProxy && std::string(noProxy).find("127.0.0.1") != std::string::npos);
    applyProxySetting("");
    assert(std::getenv("ALL_PROXY") == nullptr);
    assert(std::getenv("NO_PROXY") == nullptr);
}

} // namespace

int main() {
    testMissingFileUsesSafeDefaults();
    testUpdatePersistsEveryPublicSetting();
    testOldSettingsJsonDefaultsRefreshTimes();
    testInvalidFileFailsClosedToDefaults();
    testUnknownLanguageIsRejected();
    testLegacyTelemetryFlagMigratesOnce();
    testInvalidWebPinIsCleared();
    testUpdateEmptyPinRegenerates();
    testMaxActiveDownloadsClamped();
    testVersionOneResetsActiveDownloads();
    testFutureVersionIsRejected();
    testLegacyDebridKeysTolerated();
    testPreV3FileKeepsTorrentingOn();
    testVersionThreeHonoursTorrentingOff();
    testProxyUrlValidation();
    testInvalidProxyUrlIsCleared();
    testInvalidCatalogSourceUrlIsCleared();
    testCatalogSourceUrlValidation();
    testProxySettingReachesEnvironment();
    testDailyRefreshDue();
    testIsLocalToday();
    cleanup();
    std::puts("app settings tests passed");
    return 0;
}
