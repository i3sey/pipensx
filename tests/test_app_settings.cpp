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
    assert(values.streamSelection == StreamSelection::AllFiles);
    assert(values.installLocation == InstallLocation::SdCard);
    assert(values.showCompletedDownloads);
    assert(!values.extendedTelemetry);
    assert(values.checkForUpdatesOnLaunch);
}

void testUpdatePersistsEveryPublicSetting() {
    cleanup();
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    AppSettingsData changed = settings.get();
    changed.catalogFilter = CatalogFilter::All;
    changed.refreshCatalogOnLaunch = true;
    changed.lastCatalogRefreshMs = 123456;
    changed.lastMetadataRefreshMs = 234567;
    changed.streamSelection = StreamSelection::PackagesOnly;
    changed.installLocation = InstallLocation::SystemMemory;
    changed.showCompletedDownloads = false;
    changed.extendedTelemetry = true;
    changed.checkForUpdatesOnLaunch = false;
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
    testLegacyTelemetryFlagMigratesOnce();
    testDailyRefreshDue();
    cleanup();
    std::puts("app settings tests passed");
    return 0;
}
