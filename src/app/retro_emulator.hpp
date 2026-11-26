#pragma once

#include "homebrew_catalog.hpp"
#include "retro_catalog.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace pipensx {

struct RetroEmulatorProfile {
    enum class LaunchMode {
        PositionalRom,
        NoRomOpenEmulator,
    };

    std::string id;
    std::string displayName;
    std::string author;
    bool isRetroArchCore = false;
    std::vector<std::string> supportedConsoles;
    std::string nroRelativePath;
    std::vector<std::string> alternateNroRelativePaths;
    std::string expectedArchiveNroName; // lowercase basename inside .7z/.zip
    std::string allowedUrlPrefix;
    RetroEmulatorPackage fallbackPackage;
    LaunchMode launchMode = LaunchMode::PositionalRom;
    bool applicationModeRequired = true;

    // Legacy compatibility fields for mgbaProfile() callers
    std::string platform;
    std::string extension;
};

struct RetroBiosStatus {
    bool required = false;
    bool optional = false;
    bool present = true;
    std::string expectedPath;
    std::string hintKey;
};

const std::vector<RetroEmulatorProfile>& retroEmulatorProfiles();
const RetroEmulatorProfile* findRetroEmulatorProfile(const std::string& id);
std::vector<const RetroEmulatorProfile*> emulatorsForConsole(const std::string& consoleId);
const RetroEmulatorProfile& mgbaProfile();

bool isCustomEmulatorPath(const std::string& value);
bool isValidEmulatorSelection(const std::string& consoleId, const std::string& selection);

RetroBiosStatus checkBiosStatus(const std::string& sdRoot, const std::string& consoleId,
                                const std::string& emulatorId, const std::string& systemDirectory = {});

// Vita3K-nx on-device installer helpers (handles NoNpDrm decryption, .vpk/.zip, and PSP2UPDAT.PUP).
std::string findPendingVitaFirmwarePup(const std::string& sdRoot);
std::string extractVitaTitleIdFromRomPath(const std::string& romPath);
bool isVitaTitleInstalledDecrypted(const std::string& sdRoot, const std::string& titleId);
std::string resolveVitaInstallTarget(const std::string& romPath);
bool prepareVita3kInstallLaunch(const std::string& sdRoot, const std::string& targetPath,
                                const std::string& returnNro, std::string& nroOut,
                                std::string& argsOut, std::string& error);

class RetroEmulatorInstaller {
public:
    using Fetcher = HomebrewCatalog::Fetcher;
    using Progress = HomebrewCatalog::Progress;
    explicit RetroEmulatorInstaller(std::string sdRoot, Fetcher fetcher = {});

    static bool supports(const RetroEmulatorPackage& package);
    static RetroEmulatorPackage resolvePackage(
        const std::string& emulatorId,
        const std::vector<RetroEmulatorPackage>& catalogPackages);

    bool detect(std::string& nroPath, std::string& error) const;
    bool detect(const std::string& emulatorId, std::string& nroPath, std::string& error) const;

    bool install(const RetroEmulatorPackage& package, const std::atomic<bool>& cancelled,
                 const Progress& progress, std::string& error, bool allowReplace = false) const;
    bool uninstall(const std::string& emulatorId, std::string& error) const;

    static bool launchArguments(const std::string& nroPath, const std::string& romPath,
                                std::string& arguments, std::string& error);
    static bool launchArguments(const std::string& emulatorId, const std::string& nroPath,
                                const std::string& romPath, std::string& arguments,
                                std::string& error);

private:
    std::string sdRoot_;
    Fetcher fetcher_;
    static std::mutex mutex_;
};

} // namespace pipensx
