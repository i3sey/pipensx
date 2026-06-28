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

const char* preferredActionName(PreferredAction value) {
    return value == PreferredAction::Download ? "download" : "stream_install";
}

const char* streamSelectionName(StreamSelection value) {
    return value == StreamSelection::PackagesOnly ? "packages_only" : "all_files";
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
    std::string action = preferredActionName(values.preferredAction);
    std::string selection = streamSelectionName(values.streamSelection);
    if (!readString(root, "catalog_filter", catalog, error) ||
        !readBool(root, "refresh_catalog_on_launch",
                  values.refreshCatalogOnLaunch, error) ||
        !readString(root, "preferred_action", action, error) ||
        !readString(root, "stream_selection", selection, error) ||
        !readBool(root, "show_completed_downloads",
                  values.showCompletedDownloads, error) ||
        !readBool(root, "extended_telemetry", values.extendedTelemetry,
                  error)) {
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
    if (action == "stream_install")
        values.preferredAction = PreferredAction::StreamInstall;
    else if (action == "download")
        values.preferredAction = PreferredAction::Download;
    else {
        error = "Setting 'preferred_action' has an unknown value.";
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
    return true;
}

std::string serializeSettings(const AppSettingsData& values) {
    Json root;
    root["version"] = 1;
    root["catalog_filter"] = catalogFilterName(values.catalogFilter);
    root["refresh_catalog_on_launch"] = values.refreshCatalogOnLaunch;
    root["preferred_action"] = preferredActionName(values.preferredAction);
    root["stream_selection"] = streamSelectionName(values.streamSelection);
    root["show_completed_downloads"] = values.showCompletedDownloads;
    root["extended_telemetry"] = values.extendedTelemetry;
    return root.dump(2) + "\n";
}

} // namespace

bool AppSettingsData::operator==(const AppSettingsData& other) const {
    return catalogFilter == other.catalogFilter &&
           refreshCatalogOnLaunch == other.refreshCatalogOnLaunch &&
           preferredAction == other.preferredAction &&
           streamSelection == other.streamSelection &&
           showCompletedDownloads == other.showCompletedDownloads &&
           extendedTelemetry == other.extendedTelemetry;
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
