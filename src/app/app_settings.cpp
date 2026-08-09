#include "app_settings.hpp"

#include "download_manager.hpp"  // clampMaxActiveDownloads

#include <borealis/extern/nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

using Json = nlohmann::json;

// Fill an empty PIN with a random 6-digit value. Returns true when a new PIN
// was written into `values` (caller should persist).
bool ensureWebPin(AppSettingsData& values) {
    if (!values.webServerPin.empty())
        return false;
    values.webServerPin = generateWebPin();
    return true;
}

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

// Schema version written by this build. Bumped when a stored value has to be
// reinterpreted rather than merely added: parseSettings migrates anything
// older forward, so a bump is always paired with a rule below.
constexpr int kSettingsVersion = 3;

bool parseSettings(const std::string& text, AppSettingsData& values,
                   std::string& error) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Settings file is not valid JSON.";
        return false;
    }
    // No version key at all predates versioning, so it is a v1 file.
    int fileVersion = 1;
    if (root.contains("version")) {
        if (!root["version"].is_number_integer() || root["version"] < 1 ||
            root["version"] > kSettingsVersion) {
            error = "Settings file has an unsupported version.";
            return false;
        }
        fileVersion = root["version"].get<int>();
    }

    std::string catalog = catalogFilterName(values.catalogFilter);
    std::string selection = streamSelectionName(values.streamSelection);
    std::string install = installLocationName(values.installLocation);
    if (!readString(root, "language", values.language, error) ||
        !readString(root, "catalog_filter", catalog, error) ||
        !readBool(root, "refresh_catalog_on_launch",
                  values.refreshCatalogOnLaunch, error) ||
        !readUnsigned(root, "last_catalog_refresh_ms",
                      values.lastCatalogRefreshMs, error) ||
        !readUnsigned(root, "last_metadata_refresh_ms",
                      values.lastMetadataRefreshMs, error) ||
        !readUnsigned(root, "last_mods_refresh_ms",
                      values.lastModsRefreshMs, error) ||
        !readString(root, "stream_selection", selection, error) ||
        !readString(root, "install_location", install, error) ||
        !readBool(root, "show_completed_downloads",
                  values.showCompletedDownloads, error) ||
        !readBool(root, "extended_telemetry", values.extendedTelemetry,
                  error) ||
        !readBool(root, "check_for_updates_on_launch",
                  values.checkForUpdatesOnLaunch, error) ||
        !readBool(root, "catalog_disclaimer_ack",
                  values.catalogDisclaimerAcknowledged, error) ||
        !readBool(root, "web_server_enabled", values.webServerEnabled,
                  error) ||
        !readString(root, "web_server_pin", values.webServerPin, error) ||
        !readBool(root, "torrenting_enabled", values.torrentingEnabled,
                  error) ||
        !readString(root, "torbox_api_key", values.torboxApiKey, error)) {
        return false;
    }
    uint64_t maxActive = values.maxActiveDownloads;
    if (!readUnsigned(root, "max_active_downloads", maxActive, error))
        return false;
    values.maxActiveDownloads = clampMaxActiveDownloads(maxActive);
    // v1 -> v2: concurrent downloads became opt-in. Nothing splits the link
    // between engines, so the count a v1 build wrote — its default 2 included
    // — is not a choice we can tell apart from one the user made. Everyone
    // lands back on the serial queue. The migration is idempotent and the
    // file is not rewritten here: the first update() from the UI stamps
    // kSettingsVersion, so a count the user raises afterwards sticks.
    if (fileVersion < 2)
        values.maxActiveDownloads = kMinActiveDownloads;
    if (!isValidWebPin(values.webServerPin))
        values.webServerPin.clear();
    // v3 keys: absent in an older file, so only read them when present and
    // otherwise leave the struct defaults in place.
    if (root.contains("torrserver_url")) {
        if (!readString(root, "torrserver_url", values.torrserverUrl, error))
            return false;
    }
    if (root.contains("debrid_provider")) {
        std::string provider = "torbox";
        if (!readString(root, "debrid_provider", provider, error))
            return false;
        // "realdebrid" was a provider we no longer ship; it lands on the
        // default rather than on a kind that cannot fetch anything.
        values.debridProvider = provider == "torrserver"
            ? DebridProviderKind::TorrServer
            : DebridProviderKind::TorBox;
    }
    if (root.contains("first_run_completed")) {
        if (!readBool(root, "first_run_completed",
                      values.firstRunCompleted, error))
            return false;
    }
    if (!readString(root, "proxy_url", values.proxyUrl, error))
        return false;
    if (!isValidProxyUrl(values.proxyUrl))
        values.proxyUrl.clear();
    // v2 -> v3: debrid arrived and the struct default flipped torrenting off,
    // but a pre-v3 file was written by a build where torrenting was the only
    // way to download anything. Migrate it back on rather than silently
    // stopping an install that already works.
    if (fileVersion < 3)
        values.torrentingEnabled = true;

    if (!isSupportedLanguage(values.language)) {
        error = "Setting 'language' has an unknown value.";
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
    root["version"] = kSettingsVersion;
    root["language"] = values.language;
    root["catalog_filter"] = catalogFilterName(values.catalogFilter);
    root["refresh_catalog_on_launch"] = values.refreshCatalogOnLaunch;
    root["last_catalog_refresh_ms"] = values.lastCatalogRefreshMs;
    root["last_metadata_refresh_ms"] = values.lastMetadataRefreshMs;
    root["last_mods_refresh_ms"] = values.lastModsRefreshMs;
    root["stream_selection"] = streamSelectionName(values.streamSelection);
    root["install_location"] = installLocationName(values.installLocation);
    root["show_completed_downloads"] = values.showCompletedDownloads;
    root["extended_telemetry"] = values.extendedTelemetry;
    root["check_for_updates_on_launch"] = values.checkForUpdatesOnLaunch;
    root["catalog_disclaimer_ack"] = values.catalogDisclaimerAcknowledged;
    root["web_server_enabled"] = values.webServerEnabled;
    root["web_server_pin"] = values.webServerPin;
    root["max_active_downloads"] = values.maxActiveDownloads;
    root["torrenting_enabled"] = values.torrentingEnabled;
    root["torbox_api_key"] = values.torboxApiKey;
    root["torrserver_url"] = values.torrserverUrl;
    root["debrid_provider"] =
        values.debridProvider == DebridProviderKind::TorrServer
            ? "torrserver" : "torbox";
    root["first_run_completed"] = values.firstRunCompleted;
    root["proxy_url"] = values.proxyUrl;
    return root.dump(2) + "\n";
}

} // namespace

bool isSupportedLanguage(const std::string& value) {
    for (const char* supported : kLanguageValues) {
        if (value == supported)
            return true;
    }
    return false;
}

bool isValidWebPin(const std::string& value) {
    if (value.empty())
        return true;
    if (value.size() < 4 || value.size() > 8)
        return false;
    for (char c : value) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

std::string generateWebPin() {
    uint8_t bytes[4];
    rand_bytes(bytes, sizeof(bytes));
    uint32_t value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                     ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    value %= 1000000u;
    char pin[7];
    std::snprintf(pin, sizeof(pin), "%06u", value);
    return pin;
}

bool isValidProxyUrl(const std::string& value) {
    if (value.empty())
        return true;
    static const char* kSchemes[] = {"http://", "https://", "socks4://",
                                     "socks5://", "socks5h://"};
    const char* rest = nullptr;
    for (const char* scheme : kSchemes) {
        size_t length = std::strlen(scheme);
        if (value.size() > length && value.compare(0, length, scheme) == 0) {
            rest = value.c_str() + length;
            break;
        }
    }
    if (!rest)
        return false;
    // host[:port] and nothing else — no path, no credentials, no spaces. The
    // string ends up in an environment variable, so keep it boring.
    std::string authority(rest);
    if (authority.find_first_of("/ \t@?#") != std::string::npos)
        return false;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5)
            return false;
        for (char c : port)
            if (c < '0' || c > '9')
                return false;
        if (std::stoi(port) == 0 || std::stoi(port) > 65535)
            return false;
        authority.resize(colon);
    }
    return !authority.empty();
}

void applyProxySetting(const std::string& proxyUrl) {
    if (proxyUrl.empty()) {
        unsetenv("ALL_PROXY");
        unsetenv("all_proxy");
        unsetenv("NO_PROXY");
        unsetenv("no_proxy");
        return;
    }
    // Both spellings: libcurl checks the lowercase name first and some
    // builds only consult one of them.
    setenv("ALL_PROXY", proxyUrl.c_str(), 1);
    setenv("all_proxy", proxyUrl.c_str(), 1);
    // ALL_PROXY is curl's catch-all and would also route plain-HTTP LAN
    // TorrServer calls. Keep loopback off the proxy; a TorrServer on a
    // private LAN address still needs the proxy left empty (or a proxy that
    // can reach that LAN).
    static const char* kNoProxy = "localhost,127.0.0.1,::1";
    setenv("NO_PROXY", kNoProxy, 1);
    setenv("no_proxy", kNoProxy, 1);
}

bool AppSettingsData::operator==(const AppSettingsData& other) const {
    return language == other.language &&
           catalogFilter == other.catalogFilter &&
           refreshCatalogOnLaunch == other.refreshCatalogOnLaunch &&
           lastCatalogRefreshMs == other.lastCatalogRefreshMs &&
           lastMetadataRefreshMs == other.lastMetadataRefreshMs &&
           lastModsRefreshMs == other.lastModsRefreshMs &&
           streamSelection == other.streamSelection &&
           installLocation == other.installLocation &&
           showCompletedDownloads == other.showCompletedDownloads &&
           extendedTelemetry == other.extendedTelemetry &&
           checkForUpdatesOnLaunch == other.checkForUpdatesOnLaunch &&
           catalogDisclaimerAcknowledged ==
               other.catalogDisclaimerAcknowledged &&
           webServerEnabled == other.webServerEnabled &&
           webServerPin == other.webServerPin &&
           maxActiveDownloads == other.maxActiveDownloads &&
           torrentingEnabled == other.torrentingEnabled &&
           torboxApiKey == other.torboxApiKey &&
           torrserverUrl == other.torrserverUrl &&
           debridProvider == other.debridProvider &&
           firstRunCompleted == other.firstRunCompleted &&
           proxyUrl == other.proxyUrl;
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
            ensureWebPin(migrated);
            if (!write(migrated, error))
                return false;
            values_ = migrated;
            unlink(legacyTelemetryPath_.c_str());
        } else {
            AppSettingsData fresh;
            ensureWebPin(fresh);
            if (!write(fresh, error))
                return false;
            values_ = fresh;
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
    if (ensureWebPin(parsed)) {
        // Persist the generated PIN when possible, but keep the parsed file
        // in memory either way — a full/RO SD must not discard the rest of
        // the settings the user already had.
        std::string writeError;
        if (!write(parsed, writeError)) {
            diagnostic_error("settings", "web_pin", "error=%s",
                             writeError.c_str());
        }
    }
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
    AppSettingsData next = values;
    if (!isValidWebPin(next.webServerPin)) {
        error = "Web companion PIN must be 4-8 digits.";
        return false;
    }
    ensureWebPin(next);
    if (!write(next, error))
        return false;
    values_ = next;
    ++generation_;
    return true;
}

bool AppSettings::reset(std::string& error) {
    return update(AppSettingsData{}, error);
}

} // namespace pipensx
