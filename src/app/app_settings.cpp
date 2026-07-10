#include "app_settings.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

using Json = nlohmann::json;

const char* catalogFilterName(CatalogFilter value) {
    return value == CatalogFilter::Games ? "games" : "all";
}

const char* streamSelectionName(StreamSelection value) {
    return value == StreamSelection::PackagesOnly ? "packages_only" : "all_files";
}

const char* installLocationName(InstallLocation value) {
    return value == InstallLocation::SystemMemory ? "system_memory" : "sd_card";
}

bool readString(const Json& root, const char* key, std::string& value,
                std::string& error) {
    if (!root.contains(key))
        return true;
    if (!root[key].is_string()) {
        error = std::string("Setting '") + key + "' must be a string.";
        return false;
    }
    value = root[key].get<std::string>();
    return true;
}

bool readBool(const Json& root, const char* key, bool& value,
              std::string& error) {
    if (!root.contains(key))
        return true;
    if (!root[key].is_boolean()) {
        error = std::string("Setting '") + key + "' must be true or false.";
        return false;
    }
    value = root[key].get<bool>();
    return true;
}

bool readUnsigned(const Json& root, const char* key, uint64_t& value,
                  std::string& error) {
    if (!root.contains(key))
        return true;
    if (!root[key].is_number_unsigned()) {
        error = std::string("Setting '") + key +
                "' must be an unsigned number.";
        return false;
    }
    value = root[key].get<uint64_t>();
    return true;
}

bool parseSettings(const std::string& text, AppSettingsData& values,
                   std::string& error) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Settings file is not valid JSON.";
        return false;
    }
    if (root.contains("version") &&
        (!root["version"].is_number_integer() || root["version"] != 1)) {
        error = "Settings file has an unsupported version.";
        return false;
    }

    std::string catalog = catalogFilterName(values.catalogFilter);
    std::string selection = streamSelectionName(values.streamSelection);
    std::string install = installLocationName(values.installLocation);
    if (!readString(root, "catalog_filter", catalog, error) ||
        !readBool(root, "refresh_catalog_on_launch",
                  values.refreshCatalogOnLaunch, error) ||
        !readUnsigned(root, "last_catalog_refresh_ms",
                      values.lastCatalogRefreshMs, error) ||
        !readUnsigned(root, "last_metadata_refresh_ms",
                      values.lastMetadataRefreshMs, error) ||
        !readString(root, "stream_selection", selection, error) ||
        !readString(root, "install_location", install, error) ||
        !readBool(root, "show_completed_downloads",
                  values.showCompletedDownloads, error) ||
        !readBool(root, "extended_telemetry", values.extendedTelemetry,
                  error) ||
        !readBool(root, "check_for_updates_on_launch",
                  values.checkForUpdatesOnLaunch, error) ||
        !readBool(root, "catalog_disclaimer_ack",
                  values.catalogDisclaimerAcknowledged, error)) {
        return false;
    }

    if (catalog == "all")
        values.catalogFilter = CatalogFilter::All;
    else if (catalog == "games")
        values.catalogFilter = CatalogFilter::Games;
    else {
        error = "Setting 'catalog_filter' has an unknown value.";
        return false;
    }
    if (selection == "all_files")
        values.streamSelection = StreamSelection::AllFiles;
    else if (selection == "packages_only")
        values.streamSelection = StreamSelection::PackagesOnly;
    else {
        error = "Setting 'stream_selection' has an unknown value.";
        return false;
    }
    if (install == "sd_card")
        values.installLocation = InstallLocation::SdCard;
    else if (install == "system_memory")
        values.installLocation = InstallLocation::SystemMemory;
    else {
        error = "Setting 'install_location' has an unknown value.";
        return false;
    }
    return true;
}

std::string serializeSettings(const AppSettingsData& values) {
    Json root;
    root["version"] = 1;
    root["catalog_filter"] = catalogFilterName(values.catalogFilter);
    root["refresh_catalog_on_launch"] = values.refreshCatalogOnLaunch;
    root["last_catalog_refresh_ms"] = values.lastCatalogRefreshMs;
    root["last_metadata_refresh_ms"] = values.lastMetadataRefreshMs;
    root["stream_selection"] = streamSelectionName(values.streamSelection);
    root["install_location"] = installLocationName(values.installLocation);
    root["show_completed_downloads"] = values.showCompletedDownloads;
    root["extended_telemetry"] = values.extendedTelemetry;
    root["check_for_updates_on_launch"] = values.checkForUpdatesOnLaunch;
    root["catalog_disclaimer_ack"] = values.catalogDisclaimerAcknowledged;
    return root.dump(2) + "\n";
}

} // namespace

bool AppSettingsData::operator==(const AppSettingsData& other) const {
    return catalogFilter == other.catalogFilter &&
           refreshCatalogOnLaunch == other.refreshCatalogOnLaunch &&
           lastCatalogRefreshMs == other.lastCatalogRefreshMs &&
           lastMetadataRefreshMs == other.lastMetadataRefreshMs &&
           streamSelection == other.streamSelection &&
           installLocation == other.installLocation &&
           showCompletedDownloads == other.showCompletedDownloads &&
           extendedTelemetry == other.extendedTelemetry &&
           checkForUpdatesOnLaunch == other.checkForUpdatesOnLaunch &&
           catalogDisclaimerAcknowledged == other.catalogDisclaimerAcknowledged;
}

bool dailyRefreshDue(uint64_t nowMs, uint64_t lastRefreshMs) {
    constexpr uint64_t kDayMs = 24ULL * 60ULL * 60ULL * 1000ULL;
    return lastRefreshMs == 0 || nowMs < lastRefreshMs ||
           nowMs - lastRefreshMs >= kDayMs;
}

AppSettings::AppSettings(std::string path, std::string legacyTelemetryPath)
    : path_(std::move(path)),
      legacyTelemetryPath_(std::move(legacyTelemetryPath)) {}

bool AppSettings::load(std::string& error) {
    values_ = AppSettingsData{};
    error.clear();

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (errno != ENOENT) {
            error = std::string("Unable to open settings: ") +
                    std::strerror(errno);
            return false;
        }
        if (!legacyTelemetryPath_.empty() &&
            access(legacyTelemetryPath_.c_str(), F_OK) == 0) {
            AppSettingsData migrated;
            migrated.extendedTelemetry = true;
            if (!write(migrated, error))
                return false;
            values_ = migrated;
            unlink(legacyTelemetryPath_.c_str());
        }
        ++generation_;
        return true;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "Unable to read settings file.";
        return false;
    }
    AppSettingsData parsed;
    if (!parseSettings(buffer.str(), parsed, error))
        return false;
    values_ = parsed;
    ++generation_;
    return true;
}

bool AppSettings::write(const AppSettingsData& values,
                        std::string& error) const {
    std::string temporary = path_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create settings file.";
            return false;
        }
        output << serializeSettings(values);
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write settings file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path_.c_str()) == 0)
        return true;

    int renameError = errno;
    if ((unlink(path_.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path_.c_str()) == 0) {
        return true;
    }
    int finalError = errno;
    unlink(temporary.c_str());
    error = std::string("Unable to replace settings file: ") +
            std::strerror(finalError ? finalError : renameError);
    return false;
}

bool AppSettings::update(const AppSettingsData& values, std::string& error) {
    if (!write(values, error))
        return false;
    values_ = values;
    ++generation_;
    return true;
}

bool AppSettings::reset(std::string& error) {
    return update(AppSettingsData{}, error);
}

} // namespace pipensx
