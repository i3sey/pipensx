#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipensx {

class AppSettings;
class CatalogService;
class DownloadManager;
class GameMetadataService;
class InstalledTitleService;
struct InstalledTitle;

enum class ArchiveSourceKind : uint8_t {
    None = 0,
    TaskHistory = 1,
    Catalog = 2,
};

enum class ArchiveBlockReason : uint8_t {
    None = 0,
    InvalidTitleId,
    AlreadyArchived,
    NotInstalled,
    HomebrewOrForwarder,
    ActiveInstallBusy,
    NoRestoreSource,
};

struct ArchiveRecipe {
    uint32_t schemaVersion = 1;
    std::string titleId;
    uint64_t applicationId = 0;
    std::string name;
    std::string publisher;
    std::string displayVersion;
    std::string installedVersion;
    ArchiveSourceKind sourceKind = ArchiveSourceKind::None;
    std::string infoHash;
    std::string magnetUri;
    std::string releaseTitle;
    bool hasTorrentFile = false;
    uint64_t freedBytes = 0;
    int64_t archivedAt = 0;
};

struct ArchiveInspection {
    std::string titleId;
    bool isArchived = false;
    bool canArchive = false;
    ArchiveBlockReason blockReason = ArchiveBlockReason::None;
    ArchiveSourceKind sourceKind = ArchiveSourceKind::None;
    uint64_t installedBytes = 0;
    uint64_t estimatedFreedBytes = 0;
    std::string resolvedInfoHash;
    std::string resolvedMagnetUri;
    std::string resolvedTitle;
    std::string resolvedTorrentPath;
    bool hasCatalogInfoDict = false;
};

struct ArchiveActionResult {
    std::string titleId;
    uint64_t removedBytes = 0;
    uint64_t stubBytes = 0;
    uint64_t freedBytes = 0;
    ArchiveRecipe recipe;
};

struct RestoreStartResult {
    bool queuedImmediately = false;
    bool reusedExistingTask = false;
    bool needsAsyncMagnet = false;
    std::string titleId;
    std::string taskId;
    std::string magnetUri;
    std::string infoHash;
    std::string torrentPath;
    std::string releaseTitle;
    std::vector<uint8_t> infoDict;
};

inline constexpr uint64_t kEstimatedArchiveStubBytes = 2ULL * 1024ULL * 1024ULL;

std::string normalizeArchiveTitleId(const std::string& titleId);
bool isArchivableBaseGameId(uint64_t applicationId);
std::string archiveBlockReasonCode(ArchiveBlockReason reason);

std::string archiveDirPath(const std::string& appRoot);
std::string archiveRecipePath(const std::string& appRoot,
                              const std::string& titleId);
std::string archiveBackupIconPath(const std::string& appRoot,
                                  const std::string& titleId);
std::string archiveBackupTorrentPath(const std::string& appRoot,
                                     const std::string& titleId);

bool saveArchiveRecipe(const std::string& appRoot,
                       const ArchiveRecipe& recipe,
                       std::string& error);
bool loadArchiveRecipe(const std::string& appRoot,
                       const std::string& titleId,
                       ArchiveRecipe& out,
                       std::string& error);
void removeArchiveArtifacts(const std::string& appRoot,
                            const std::string& titleId);
std::vector<ArchiveRecipe> listArchiveRecipes(const std::string& appRoot);

// Decode a 256x256 (or any resolution) image to RGB, dim subtly, and
// composite a bottom-right dark badge with a high-contrast cloud-download icon.
bool badgeArchiveIconJpeg(const uint8_t* srcJpeg, size_t srcSize,
                          std::vector<uint8_t>& outJpeg,
                          std::string& error);

class ArchiveService {
public:
    using IsPortTitleFn = std::function<bool(const std::string& titleId)>;
    using StubBuilderFn = std::function<bool(
        uint64_t titleId, const std::string& titleName,
        const std::string& publisher, const std::string& displayVersion,
        const std::string& nroPath, const std::string& args,
        const std::vector<uint8_t>& badgedIconJpeg,
        std::vector<uint8_t>& outNsp, std::string& error)>;

    ArchiveService(std::string appRoot,
                   InstalledTitleService* installed,
                   DownloadManager* downloads,
                   CatalogService* catalog,
                   GameMetadataService* metadata,
                   AppSettings* settings,
                   IsPortTitleFn isPortTitle = {},
                   std::string nroPath = "sdmc:/switch/pipensx/pipensx.nro",
                   StubBuilderFn stubBuilder = {});

    void setNroPath(std::string nroPath);
    const std::string& nroPath() const { return nroPath_; }

    ArchiveInspection inspectArchive(const std::string& titleId) const;
    std::unordered_map<std::string, ArchiveInspection> inspectLibrary(
        const std::vector<InstalledTitle>& titles) const;

    bool archiveTitle(const std::string& titleId,
                      ArchiveActionResult& result,
                      std::string& error);

    bool restoreTitle(const std::string& titleId,
                      std::atomic<bool>& cancelled,
                      RestoreStartResult& result,
                      std::string& error,
                      bool allowBlockingMagnetResolve = true);

    bool removeArchivedStub(const std::string& titleId,
                            std::string& error);

private:
    struct ResolvedSource {
        ArchiveSourceKind kind = ArchiveSourceKind::None;
        std::string infoHash;
        std::string magnetUri;
        std::string releaseTitle;
        std::string localTorrentPath;
        std::vector<uint8_t> infoDict;
    };

    ArchiveInspection inspectTitleWithSources(
        const std::string& upperId,
        uint64_t appId,
        const ResolvedSource* preResolved) const;
    bool resolveSource(const std::string& upperId,
                       uint64_t appId,
                       ResolvedSource& out) const;
    bool buildStubNsp(uint64_t titleId,
                      const std::string& name,
                      const std::string& publisher,
                      const std::string& displayVersion,
                      const std::string& upperId,
                      const std::vector<uint8_t>& badgedIcon,
                      std::vector<uint8_t>& outNsp,
                      std::string& error) const;

    std::string appRoot_;
    InstalledTitleService* installed_ = nullptr;
    DownloadManager* downloads_ = nullptr;
    CatalogService* catalog_ = nullptr;
    GameMetadataService* metadata_ = nullptr;
    AppSettings* settings_ = nullptr;
    IsPortTitleFn isPortTitle_;
    std::string nroPath_;
    StubBuilderFn stubBuilder_;
};

} // namespace pipensx
