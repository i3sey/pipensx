#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pipensx {

struct TorrentPreview;
struct DownloadTask;

struct AutorunProfile {
    std::string title;
    std::string executable;
    std::string targetFolder;
    std::string status;
    std::string fps;
    std::string overclock;
    std::string translator;
    std::string renderer;
    std::string notes;
    std::string dxvkConf;
    std::string registry;

    bool valid() const {
        return !executable.empty() && !targetFolder.empty();
    }
};

inline std::string autorunCompatibilityBadge(const AutorunProfile& profile) {
    std::string icon = "🟢";
    if (profile.status == "playable_with_tweaks") {
        icon = "🟡";
    } else if (profile.status == "issues" || profile.status == "unplayable") {
        icon = "🔴";
    }
    if (!profile.fps.empty()) {
        return icon + " " + profile.fps + " FPS";
    }
    if (profile.status == "playable_with_tweaks") {
        return icon + " Tweaks";
    }
    if (profile.status == "issues" || profile.status == "unplayable") {
        return icon + " Issues";
    }
    return icon + " Playable";
}

enum class AutorunValidationResult {
    ValidPortable,
    InstallerDetected,
    DiskImageDetected,
    NoExecutable,
};

struct AutorunValidation {
    AutorunValidationResult result = AutorunValidationResult::ValidPortable;
    std::string mainExecutable;
    std::string warning;
    std::vector<std::string> suspiciousFiles;

    bool isPortable() const {
        return result == AutorunValidationResult::ValidPortable;
    }
};

struct AutorunReleaseInfo {
    std::string version;
    std::string archiveUrl;
    std::string checksumUrl;
    size_t size = 0;
};

struct AutorunUpdateCheck {
    bool ok = false;
    bool updateAvailable = false;
    std::string installedVersion;
    AutorunReleaseInfo release;
    std::string error;
};

namespace install {
class InstallBackend;
}

struct SysClkProfile {
    uint32_t cpu = 1785;
    uint32_t gpu = 768;
    uint32_t mem = 1900;

    bool operator==(const SysClkProfile& o) const {
        return cpu == o.cpu && gpu == o.gpu && mem == o.mem;
    }
};

// Default recommended DXVK config for AutoRun on Nintendo Switch.
inline constexpr const char* kDefaultDxvkConf =
    "# AutoRun Switch DXVK optimized configuration\n"
    "dxvk.enableAsync = true\n"
    "dxvk.numCompilerThreads = 4\n"
    "dxvk.maxChunkSize = 32\n"
    "d3d9.maxFrameLatency = 1\n"
    "dxgi.maxFrameLatency = 1\n";

// Path resolvers for AutoRun relative to the Switch SD root.
std::string autorunWineRoot(const std::string& sdRoot);
std::string autorunRuntimeNroPath(const std::string& sdRoot);
std::string autorunGamesPath(const std::string& sdRoot);
std::string autorunDesktopPath(const std::string& sdRoot);
std::string autorunDocumentsPath(const std::string& sdRoot);
std::string autorunRegistryPath(const std::string& sdRoot);
std::string autorunSystemRegistryPath(const std::string& sdRoot);
std::string autorunVersionPath(const std::string& sdRoot);
std::string autorunForwardersPath(const std::string& sdRoot);
std::string autorunForwarderConfigPath(const std::string& sdRoot, uint64_t titleId);
std::string autorunSysClkConfigPath(const std::string& sdRoot);

// Deterministic Title ID generation with homebrew prefix (0x0100BEEFxxxxxxxx).
uint64_t generateAutorunTitleId(const std::string& gameTitle);
std::string autorunTitleIdToString(uint64_t titleId);

// Forwarder INI configuration on SD (sdmc:/switch/wine/forwarders/<titleId>.ini).
bool createAutorunForwarderConfig(const std::string& sdRoot,
                                  uint64_t titleId,
                                  const std::string& gameTitle,
                                  const std::string& gameFolder,
                                  const std::string& executable,
                                  std::string& error);

bool readAutorunForwarderConfig(const std::string& sdRoot,
                                uint64_t titleId,
                                std::string& gameTitle,
                                std::string& gameFolder,
                                std::string& executable,
                                std::string& nroPath,
                                std::string& nroArgv,
                                std::string& error);

bool removeAutorunForwarderConfig(const std::string& sdRoot,
                                  uint64_t titleId,
                                  std::string& error);

// sys-clk integration (/config/sys-clk/config.ini).
bool isSysClkInstalled(const std::string& sdRoot);
bool parseOverclockProfile(const std::string& ocStr, SysClkProfile& out);
bool hasSysClkProfile(const std::string& sdRoot, uint64_t titleId);
bool applySysClkProfile(const std::string& sdRoot,
                        uint64_t titleId,
                        const SysClkProfile& profile,
                        std::string& error);
bool removeSysClkProfile(const std::string& sdRoot,
                         uint64_t titleId,
                         std::string& error);

// Stub-NSP forwarder and PFS0 generator.
std::vector<uint8_t> generateNacp(const std::string& title,
                                  const std::string& author = "AutoRun / pipensx",
                                  const std::string& version = "1.0.0");

std::vector<uint8_t> buildPfs0(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);

bool parsePfs0(const uint8_t* data, size_t size,
               std::vector<std::pair<std::string, std::vector<uint8_t>>>& outFiles);

std::vector<uint8_t> generateAutorunStubNsp(
    const std::string& title,
    uint64_t titleId,
    const std::vector<uint8_t>& iconJpeg = {});

bool installAutorunForwarder(const std::string& sdRoot,
                             install::InstallBackend& backend,
                             const std::string& gameTitle,
                             const std::string& gameFolder,
                             const std::string& executable,
                             const std::vector<uint8_t>& iconJpeg,
                             std::string& error);

// Checks if the AutoRun runtime NRO is present.
bool isAutorunInstalled(const std::string& sdRoot);

// Reads currently installed runtime version from version.json/version.txt.
std::string installedAutorunVersion(const std::string& sdRoot);

// Creates standard Wine-NX directory layout and initial user.reg / system.reg.
bool prepareAutorunEnvironment(const std::string& sdRoot, std::string& error);

// Pre-download validation: checks torrent file list for installers or disc images.
AutorunValidation validateAutorunRelease(
    const TorrentPreview& preview,
    const std::string& expectedExecutable = {});

// Validates a list of file paths (case-insensitive checks for setup.exe, .iso, etc.).
AutorunValidation validateAutorunFilePaths(
    const std::vector<std::string>& filePaths,
    const std::string& expectedExecutable = {});

// Idempotently merges a Wine registry block into user.reg.
bool mergeUserRegistry(const std::string& userRegPath,
                       const std::string& registryTweak,
                       std::string& error);

// Generates a Windows Shell Link (.lnk) binary payload.
std::vector<uint8_t> generateShellLink(
    const std::string& targetWindowsPath,
    const std::string& workingDirWindowsPath);

// Creates a .lnk shortcut file on the Wine desktop.
bool createWineDesktopShortcut(const std::string& desktopPath,
                               const std::string& shortcutName,
                               const std::string& targetFolder,
                               const std::string& executable,
                               std::string& error);

// Post-deploy hook: writes dxvk.conf, merges user.reg, creates desktop shortcut.
bool deployAutorunGame(const std::string& sdRoot,
                       const std::string& gameFolder,
                       const std::string& gameTitle,
                       const AutorunProfile& profile,
                       std::string& error);

// Task-level persistence of Autorun profiles.
bool saveAutorunProfile(const std::string& appRoot,
                        const std::string& taskId,
                        const AutorunProfile& profile,
                        std::string& error);

bool loadAutorunProfile(const std::string& appRoot,
                        const std::string& taskId,
                        AutorunProfile& profile,
                        std::string& error);

void removeAutorunProfile(const std::string& appRoot,
                          const std::string& taskId);

// Extracts a runtime ZIP archive into the destination directory.
bool extractRuntimeZip(
    const std::string& archivePath,
    const std::string& destinationDir,
    const std::atomic<bool>& cancelled,
    const std::function<void(uint64_t, uint64_t)>& progress,
    std::string& error);

class AutorunService {
public:
    using MetadataFetcher = std::function<bool(
        const std::string&, size_t, std::string&, std::string&)>;
    using AssetFetcher = std::function<bool(
        const std::string&, const std::string&, size_t, std::string&)>;
    using ProgressCallback = std::function<void(uint64_t, uint64_t)>;

    explicit AutorunService(std::string sdRoot = "sdmc:",
                            MetadataFetcher metadataFetcher = {},
                            AssetFetcher assetFetcher = {});
    ~AutorunService();

    AutorunService(const AutorunService&) = delete;
    AutorunService& operator=(const AutorunService&) = delete;

    const std::string& sdRoot() const { return sdRoot_; }
    void setSdRoot(std::string sdRoot) { sdRoot_ = std::move(sdRoot); }

    bool isInstalled() const { return isAutorunInstalled(sdRoot_); }
    std::string installedVersion() const { return installedAutorunVersion(sdRoot_); }

    bool bootstrapEnvironment(std::string& error) const {
        return prepareAutorunEnvironment(sdRoot_, error);
    }

    AutorunUpdateCheck checkForUpdates(
        const std::string& releaseUrl = {}) const;

    bool installRuntimeArchive(
        const std::string& archivePath,
        const std::string& releaseVersion,
        std::string& error,
        const std::atomic<bool>& cancelled = false,
        const ProgressCallback& progress = {}) const;

    bool downloadAndInstallRuntime(
        const AutorunReleaseInfo& release,
        std::string& error,
        const std::atomic<bool>& cancelled = false,
        const ProgressCallback& progress = {}) const;

    bool deployGame(
        const std::string& gameFolder,
        const std::string& gameTitle,
        const AutorunProfile& profile,
        std::string& error) const {
        return deployAutorunGame(sdRoot_, gameFolder, gameTitle, profile, error);
    }

    bool deployTask(
        const DownloadTask& task,
        const AutorunProfile& profile,
        std::string& error) const;

    // Phase 5: Forwarder & Title ID
    uint64_t generateTitleId(const std::string& title) const {
        return generateAutorunTitleId(title);
    }
    std::string titleIdString(uint64_t titleId) const {
        return autorunTitleIdToString(titleId);
    }
    bool createForwarderConfig(uint64_t titleId,
                               const std::string& gameTitle,
                               const std::string& gameFolder,
                               const std::string& executable,
                               std::string& error) const {
        return createAutorunForwarderConfig(sdRoot_, titleId, gameTitle,
                                           gameFolder, executable, error);
    }
    bool removeForwarderConfig(uint64_t titleId, std::string& error) const {
        return removeAutorunForwarderConfig(sdRoot_, titleId, error);
    }

    // Phase 5: sys-clk
    bool isSysClkAvailable() const {
        return isSysClkInstalled(sdRoot_);
    }
    bool hasSysClk(uint64_t titleId) const {
        return hasSysClkProfile(sdRoot_, titleId);
    }
    bool applySysClk(uint64_t titleId, const SysClkProfile& profile, std::string& error) const {
        return applySysClkProfile(sdRoot_, titleId, profile, error);
    }
    bool applySysClk(uint64_t titleId, const std::string& ocStr, std::string& error) const {
        SysClkProfile profile;
        parseOverclockProfile(ocStr, profile);
        return applySysClkProfile(sdRoot_, titleId, profile, error);
    }
    bool removeSysClk(uint64_t titleId, std::string& error) const {
        return removeSysClkProfile(sdRoot_, titleId, error);
    }

    // Phase 5: Stub-NSP & Forwarder Installation
    std::vector<uint8_t> generateStubNsp(
        const std::string& gameTitle,
        uint64_t titleId,
        const std::vector<uint8_t>& iconJpeg = {}) const {
        return generateAutorunStubNsp(gameTitle, titleId, iconJpeg);
    }

    bool installForwarder(
        install::InstallBackend& backend,
        const std::string& gameTitle,
        const std::string& gameFolder,
        const std::string& executable,
        const std::vector<uint8_t>& iconJpeg,
        std::string& error) const {
        return installAutorunForwarder(sdRoot_, backend, gameTitle,
                                      gameFolder, executable, iconJpeg, error);
    }

    static bool parseRelease(const std::string& json,
                             AutorunReleaseInfo& release,
                             std::string& error);

private:
    std::string sdRoot_;
    MetadataFetcher metadataFetcher_;
    AssetFetcher assetFetcher_;
};

} // namespace pipensx
