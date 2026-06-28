#include "app/app_settings.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using pipensx::AppSettings;
using pipensx::AppSettingsData;
using pipensx::CatalogFilter;
using pipensx::PreferredAction;
using pipensx::StreamSelection;

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
    assert(values.catalogFilter == CatalogFilter::All);
    assert(!values.refreshCatalogOnLaunch);
    assert(values.preferredAction == PreferredAction::StreamInstall);
    assert(values.streamSelection == StreamSelection::AllFiles);
    assert(values.showCompletedDownloads);
    assert(!values.extendedTelemetry);
}

void testUpdatePersistsEveryPublicSetting() {
    cleanup();
    AppSettings settings(SettingsPath, LegacyPath);
    std::string error;
    assert(settings.load(error));
    AppSettingsData changed = settings.get();
    changed.catalogFilter = CatalogFilter::Games;
    changed.refreshCatalogOnLaunch = true;
    changed.preferredAction = PreferredAction::Download;
    changed.streamSelection = StreamSelection::PackagesOnly;
    changed.showCompletedDownloads = false;
    changed.extendedTelemetry = true;
    assert(settings.update(changed, error));

    AppSettings restored(SettingsPath, LegacyPath);
    assert(restored.load(error));
    assert(restored.get() == changed);
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

} // namespace

int main() {
    testMissingFileUsesSafeDefaults();
    testUpdatePersistsEveryPublicSetting();
    testInvalidFileFailsClosedToDefaults();
    testLegacyTelemetryFlagMigratesOnce();
    cleanup();
    std::puts("app settings tests passed");
    return 0;
}
