#pragma once

#include "app_settings.hpp"
#include "download_manager.hpp"

#include <string>

namespace pipensx {

// Whitelist JSON for the web companion Settings tab. Secrets are flags, not
// values. Used by GET /api/settings and as the PATCH response body.
std::string companionSettingsJson(const AppSettingsData& values);

// Partial update of the companion whitelist. Unknown keys fail. Empty string
// on torboxApiKey / realdebridApiKey clears. Omitted keys are left alone.
bool applyCompanionSettingsPatch(AppSettingsData& values,
                                 const std::string& jsonBody,
                                 std::string& error);

// Runtime side effects SettingsView already applies after persist. Catalog
// URL/filter only live in AppSettings — refresh stays a console action.
inline void applyCompanionSettingsRuntime(const AppSettingsData& values,
                                          DownloadManager& manager) {
    manager.setMaxActiveDownloads(values.maxActiveDownloads);
    manager.setInstallTarget(values.installLocation ==
                                     InstallLocation::SystemMemory
                                 ? install::InstallStorageTarget::Nand
                                 : install::InstallStorageTarget::SdCard);
    manager.setTorboxApiKey(values.torboxApiKey);
    manager.setTorrserverUrl(values.torrserverUrl);
    manager.setRealdebridApiKey(values.realdebridApiKey);
    applyProxySetting(values.proxyUrl);
}

}  // namespace pipensx
