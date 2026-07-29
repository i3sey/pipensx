#include "app/app_settings.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using pipensx::AppSettings;
using pipensx::AppSettingsData;
using pipensx::CatalogFilter;
using pipensx::StreamSelection;
using pipensx::InstallLocation;
using pipensx::dailyRefreshDue;

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
    assert(access(SettingsPath, F_OK) == 0);
    assert(access(LegacyPath, F_OK) != 0);
}

// A hand-edited PIN that is not 4-8 digits silently degrades to "no PIN"
// rather than locking the user out of their own companion page.
void testInvalidWebPinIsCleared() {
    cleanup();
    {
        std::ofstream output(SettingsPath);
        output << R"({"version":1,"web_server_pin":"letters"})";
    }
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    assert(settings.get().webServerPin.empty());
    assert(settings.get().webServerEnabled);

    assert(pipensx::isValidWebPin(""));
    assert(pipensx::isValidWebPin("1234"));
    assert(pipensx::isValidWebPin("12345678"));
    assert(!pipensx::isValidWebPin("123"));
    assert(!pipensx::isValidWebPin("123456789"));
    assert(!pipensx::isValidWebPin("12a4"));
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

} // namespace

int main() {
    testMissingFileUsesSafeDefaults();
    testUpdatePersistsEveryPublicSetting();
    testOldSettingsJsonDefaultsRefreshTimes();
    testInvalidFileFailsClosedToDefaults();
    testUnknownLanguageIsRejected();
    testLegacyTelemetryFlagMigratesOnce();
    testInvalidWebPinIsCleared();
    testMaxActiveDownloadsClamped();
    testVersionOneResetsActiveDownloads();
    testFutureVersionIsRejected();
    testDailyRefreshDue();
    cleanup();
    std::puts("app settings tests passed");
    return 0;
}
