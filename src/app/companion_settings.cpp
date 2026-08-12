#include "companion_settings.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <string>
#include <unordered_set>

namespace pipensx {
namespace {

using Json = nlohmann::json;

const std::unordered_set<std::string> kAllowedKeys = {
    "maxActiveDownloads", "streamSelection",   "installLocation",
    "debridProvider",     "torboxApiKey",      "realdebridApiKey",
    "torrserverUrl",      "proxyUrl",          "catalogSourceUrl",
    "catalogFilter",      "refreshCatalogOnLaunch",
};

const char* streamSelectionJson(StreamSelection value) {
    return value == StreamSelection::PackagesOnly ? "packagesOnly" : "allFiles";
}

const char* installLocationJson(InstallLocation value) {
    return value == InstallLocation::SystemMemory ? "systemMemory" : "sdCard";
}

const char* catalogFilterJson(CatalogFilter value) {
    return value == CatalogFilter::Games ? "games" : "all";
}

const char* debridProviderJson(DebridProviderKind value) {
    switch (value) {
        case DebridProviderKind::TorrServer:
            return "torrserver";
        case DebridProviderKind::RealDebrid:
            return "realdebrid";
        case DebridProviderKind::TorBox:
            return "torbox";
    }
    return "torbox";
}

}  // namespace

std::string companionSettingsJson(const AppSettingsData& values) {
    Json j;
    j["maxActiveDownloads"] = values.maxActiveDownloads;
    j["streamSelection"] = streamSelectionJson(values.streamSelection);
    j["installLocation"] = installLocationJson(values.installLocation);
    j["debridProvider"] = debridProviderJson(values.debridProvider);
    j["torboxConfigured"] = !values.torboxApiKey.empty();
    j["realdebridConfigured"] = !values.realdebridApiKey.empty();
    j["torrserverUrl"] = values.torrserverUrl;
    j["proxyUrl"] = values.proxyUrl;
    j["catalogSourceUrl"] = values.catalogSourceUrl;
    j["catalogFilter"] = catalogFilterJson(values.catalogFilter);
    j["refreshCatalogOnLaunch"] = values.refreshCatalogOnLaunch;
    return j.dump();
}

bool applyCompanionSettingsPatch(AppSettingsData& values,
                                 const std::string& jsonBody,
                                 std::string& error) {
    Json root = Json::parse(jsonBody, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "invalid JSON body";
        return false;
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!kAllowedKeys.count(it.key())) {
            error = "unknown setting '" + it.key() + "'";
            return false;
        }
    }

    if (root.contains("maxActiveDownloads")) {
        if (!root["maxActiveDownloads"].is_number_unsigned()) {
            error = "maxActiveDownloads must be an unsigned number";
            return false;
        }
        values.maxActiveDownloads = clampMaxActiveDownloads(
            root["maxActiveDownloads"].get<uint64_t>());
    }
    if (root.contains("streamSelection")) {
        if (!root["streamSelection"].is_string()) {
            error = "streamSelection must be a string";
            return false;
        }
        const std::string v = root["streamSelection"].get<std::string>();
        if (v == "allFiles")
            values.streamSelection = StreamSelection::AllFiles;
        else if (v == "packagesOnly")
            values.streamSelection = StreamSelection::PackagesOnly;
        else {
            error = "streamSelection has an unknown value";
            return false;
        }
    }
    if (root.contains("installLocation")) {
        if (!root["installLocation"].is_string()) {
            error = "installLocation must be a string";
            return false;
        }
        const std::string v = root["installLocation"].get<std::string>();
        if (v == "sdCard")
            values.installLocation = InstallLocation::SdCard;
        else if (v == "systemMemory")
            values.installLocation = InstallLocation::SystemMemory;
        else {
            error = "installLocation has an unknown value";
            return false;
        }
    }
    if (root.contains("debridProvider")) {
        if (!root["debridProvider"].is_string()) {
            error = "debridProvider must be a string";
            return false;
        }
        const std::string v = root["debridProvider"].get<std::string>();
        if (v == "torbox")
            values.debridProvider = DebridProviderKind::TorBox;
        else if (v == "torrserver")
            values.debridProvider = DebridProviderKind::TorrServer;
        else if (v == "realdebrid")
            values.debridProvider = DebridProviderKind::RealDebrid;
        else {
            error = "debridProvider has an unknown value";
            return false;
        }
    }
    if (root.contains("torboxApiKey")) {
        if (!root["torboxApiKey"].is_string()) {
            error = "torboxApiKey must be a string";
            return false;
        }
        values.torboxApiKey = root["torboxApiKey"].get<std::string>();
    }
    if (root.contains("realdebridApiKey")) {
        if (!root["realdebridApiKey"].is_string()) {
            error = "realdebridApiKey must be a string";
            return false;
        }
        values.realdebridApiKey = root["realdebridApiKey"].get<std::string>();
    }
    if (root.contains("torrserverUrl")) {
        if (!root["torrserverUrl"].is_string()) {
            error = "torrserverUrl must be a string";
            return false;
        }
        values.torrserverUrl = root["torrserverUrl"].get<std::string>();
    }
    if (root.contains("proxyUrl")) {
        if (!root["proxyUrl"].is_string()) {
            error = "proxyUrl must be a string";
            return false;
        }
        values.proxyUrl = root["proxyUrl"].get<std::string>();
        if (!isValidProxyUrl(values.proxyUrl)) {
            error = "proxyUrl is not a valid proxy URL";
            return false;
        }
    }
    if (root.contains("catalogSourceUrl")) {
        if (!root["catalogSourceUrl"].is_string()) {
            error = "catalogSourceUrl must be a string";
            return false;
        }
        values.catalogSourceUrl = root["catalogSourceUrl"].get<std::string>();
        if (!isValidCatalogSourceUrl(values.catalogSourceUrl)) {
            error = "catalogSourceUrl is not a valid HTTPS catalog URL";
            return false;
        }
    }
    if (root.contains("catalogFilter")) {
        if (!root["catalogFilter"].is_string()) {
            error = "catalogFilter must be a string";
            return false;
        }
        const std::string v = root["catalogFilter"].get<std::string>();
        if (v == "all")
            values.catalogFilter = CatalogFilter::All;
        else if (v == "games")
            values.catalogFilter = CatalogFilter::Games;
        else {
            error = "catalogFilter has an unknown value";
            return false;
        }
    }
    if (root.contains("refreshCatalogOnLaunch")) {
        if (!root["refreshCatalogOnLaunch"].is_boolean()) {
            error = "refreshCatalogOnLaunch must be true or false";
            return false;
        }
        values.refreshCatalogOnLaunch =
            root["refreshCatalogOnLaunch"].get<bool>();
    }
    return true;
}

}  // namespace pipensx
