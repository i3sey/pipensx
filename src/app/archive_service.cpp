#include "archive_service.hpp"

#include "add_release.hpp"
#include "app_settings.hpp"
#include "autorun_service.hpp"
#include "catalog_section.hpp"
#include "catalog_service.hpp"
#include "download_manager.hpp"
#include "game_metadata_service.hpp"
#include "game_update_install.hpp"
#include "install/install_backend.hpp"
#include "installed_title_service.hpp"
#include "magnet_resolver.hpp"
#include "nx_file_types.hpp"
#include "task_files.hpp"
#include "torrent_metainfo_fetch.hpp"

extern "C" {
#include "../core/util.h"
}

#ifdef __SWITCH__
#include <switch.h>
#include "../../vendor/autorun-forwarder/builder.h"
#endif

#include <borealis/extern/nlohmann/json.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <borealis/extern/nanovg/stb_image.h>
#include "../../vendor/borealis/library/lib/extern/glfw/deps/stb_image_write.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace pipensx {
namespace {

using nlohmann::json;

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool makeDirs(const std::string& path) {
    if (path.empty() || path.size() >= 1024)
        return false;
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());
    char* start = buf + 1;
    if (char* colon = std::strchr(buf, ':')) {
        if (colon[1] == '/')
            start = colon + 2;
    }
    for (char* p = start; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

bool writeBytesAtomic(const std::string& path,
                      const void* data,
                      size_t size,
                      std::string& error) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !makeDirs(path.substr(0, slash))) {
        error = "Unable to create destination directory.";
        return false;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Unable to open temporary output file.";
            return false;
        }
        if (size > 0)
            out.write(static_cast<const char*>(data),
                      static_cast<std::streamsize>(size));
        out.flush();
        if (!out.good()) {
            unlink(tmp.c_str());
            error = "Failed while writing file.";
            return false;
        }
    }
    if (rename(tmp.c_str(), path.c_str()) == 0)
        return true;
    if ((unlink(path.c_str()) == 0 || errno == ENOENT) &&
        rename(tmp.c_str(), path.c_str()) == 0) {
        return true;
    }
    unlink(tmp.c_str());
    error = "Unable to commit file atomically.";
    return false;
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long end = std::ftell(f);
    if (end <= 0 || end > 16L * 1024L * 1024L ||
        std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(end));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

bool copyFileToAtomic(const std::string& src,
                      const std::string& dst,
                      std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(src, bytes)) {
        error = "Unable to read source file.";
        return false;
    }
    return writeBytesAtomic(dst, bytes.data(), bytes.size(), error);
}

bool pathHasExactBaseId(const std::string& path, const std::string& upperBaseId) {
    const std::string upperPath = upperAscii(path);
    return upperPath.find(upperBaseId) != std::string::npos;
}

bool pathLooksExcludedRelease(const std::string& titleLower) {
    return titleLower.find("update only") != std::string::npos ||
           titleLower.find("только обновлен") != std::string::npos ||
           titleLower.find("dlc only") != std::string::npos ||
           titleLower.find("только dlc") != std::string::npos ||
           titleLower.find("cheats") != std::string::npos;
}

int scoreCatalogCandidate(const CatalogEntry& entry,
                          const GameMetadata* meta,
                          const std::string& upperId,
                          const std::string& appRoot) {
    int score = 0;
    if (entry.titles.size() <= 1)
        score += 1000;
    else
        score -= 200; // Multi-game pack as fallback only.

    if (upperAscii(entry.titleId) == upperId)
        score += 400;
    else if (meta && upperAscii(meta->titleId) == upperId)
        score += 250;

    if (!entry.infoDict.empty())
        score += 300;

    struct stat st {};
    const std::string cachedTorrent =
        appRoot + "/torrents/" + lowerAscii(entry.infoHash) + ".torrent";
    if (!entry.infoHash.empty() && stat(cachedTorrent.c_str(), &st) == 0)
        score += 200;

    if (entry.health == CatalogHealth::Ok)
        score += 120;
    if (entry.hasPeerCount && entry.peerCount > 0)
        score += static_cast<int>(std::min<uint32_t>(entry.peerCount, 100));
    if (entry.packageType == "nsp" || entry.packageType == "nsz")
        score += 40;
    return score;
}

bool entryMatchesTitle(const CatalogEntry& entry,
                       const GameMetadata* meta,
                       const std::string& upperId) {
    auto eqBase = [&](const std::string& raw) {
        uint64_t parsed = 0;
        if (!InstalledTitleService::parseTitleId(raw, parsed))
            return false;
        return InstalledTitleService::formatTitleId(
                   InstalledTitleService::nxBaseApplicationId(parsed)) == upperId;
    };
    if (eqBase(entry.titleId))
        return true;
    for (const auto& t : entry.titles) {
        if (eqBase(t.titleId))
            return true;
    }
    return meta && eqBase(meta->titleId);
}

bool isEligibleBaseCatalogEntry(const CatalogEntry& entry,
                                 const GameMetadata* meta) {
    if (entry.isAutorun() || entry.isHiddenByDefault())
        return false;
    if (entry.infoHash.empty() && entry.magnetUri.empty())
        return false;
    if (entry.type == "port" || entry.packageType == "update" ||
        entry.packageType == "dlc" || entry.packageType == "mod" ||
        entry.packageType == "cheats") {
        return false;
    }
    const std::string metaTid = meta ? meta->titleId : std::string();
    if (!catalogTitleIsGame(entry.title, entry.titleId, metaTid,
                            entry.packageType)) {
        return false;
    }
    const std::string lowerTitle = lowerAscii(entry.title);
    if (pathLooksExcludedRelease(lowerTitle))
        return false;
    return true;
}

bool matchTaskSourceForTitle(const DownloadTask& task,
                             const std::string& upperId,
                             const std::string& appRoot,
                             const CatalogEntry* catMatch,
                             GameMetadataService* metadata,
                             ArchiveSourceKind& kindOut,
                             std::string& infoHashOut,
                             std::string& magnetOut,
                             std::string& releaseTitleOut,
                             std::string& torrentPathOut,
                             std::vector<uint8_t>& infoDictOut) {
    if (task.mode == TransferMode::PortInstall ||
        task.mode == TransferMode::AutoRunInstall) {
        return false;
    }
    bool matchesBase = false;
    TaskFileManifest manifest;
    std::string ignoreErr;
    const bool hasManifest =
        loadTaskFileManifest(appRoot, task.id, manifest, ignoreErr);
    if (hasManifest) {
        for (const auto& f : manifest.files) {
            if (f.package &&
                (pathHasExactBaseId(f.logicalPath, upperId) ||
                 pathHasExactBaseId(f.localPath, upperId))) {
                matchesBase = true;
                break;
            }
        }
    }

    struct stat st {};
    std::string torrentPath;
    if (!task.metainfoPath.empty() &&
        stat(task.metainfoPath.c_str(), &st) == 0 && st.st_size > 0) {
        torrentPath = task.metainfoPath;
    } else {
        const std::string fallback =
            appRoot + "/torrents/" + lowerAscii(task.id) + ".torrent";
        if (stat(fallback.c_str(), &st) == 0 && st.st_size > 0)
            torrentPath = fallback;
    }

    if (!matchesBase && !hasManifest && !torrentPath.empty()) {
        TorrentPreview preview;
        if (DownloadManager::previewTorrent(torrentPath, preview, ignoreErr)) {
            for (const auto& f : preview.files) {
                if (f.package && pathHasExactBaseId(f.path, upperId)) {
                    matchesBase = true;
                    break;
                }
            }
        }
    }
    if (!matchesBase && catMatch) {
        const GameMetadata* meta = metadata
            ? metadata->findByInfoHash(catMatch->infoHash, upperId)
            : nullptr;
        if (isEligibleBaseCatalogEntry(*catMatch, meta) &&
            entryMatchesTitle(*catMatch, meta, upperId)) {
            matchesBase = true;
        }
    }
    if (!matchesBase)
        return false;

    kindOut = ArchiveSourceKind::TaskHistory;
    infoHashOut = lowerAscii(task.id);
    torrentPathOut = torrentPath;
    releaseTitleOut = task.name;
    magnetOut = updateMagnetFor(infoHashOut, catMatch);
    if (catMatch)
        infoDictOut = catMatch->infoDict;
    return true;
}

const char* sourceKindToString(ArchiveSourceKind kind) {
    switch (kind) {
    case ArchiveSourceKind::TaskHistory: return "task";
    case ArchiveSourceKind::Catalog: return "catalog";
    case ArchiveSourceKind::None: default: return "none";
    }
}

ArchiveSourceKind sourceKindFromString(const std::string& s) {
    if (s == "task")
        return ArchiveSourceKind::TaskHistory;
    if (s == "catalog")
        return ArchiveSourceKind::Catalog;
    return ArchiveSourceKind::None;
}

struct SystemCleanupGuard {
    DownloadManager* dm = nullptr;
    bool held = false;
    ~SystemCleanupGuard() {
        if (dm && held)
            dm->endSystemCleanup();
    }
    bool acquire(DownloadManager* target, std::string& error) {
        if (!target)
            return true;
        if (!target->beginSystemCleanup(error))
            return false;
        dm = target;
        held = true;
        return true;
    }
};

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float smoothstep(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

float sdCircle(float px, float py, float cx, float cy, float r) {
    const float dx = px - cx;
    const float dy = py - cy;
    return std::sqrt(dx * dx + dy * dy) - r;
}

float sdRoundedBox(float px, float py, float cx, float cy,
                   float halfW, float halfH, float r) {
    const float qx = std::fabs(px - cx) - halfW + r;
    const float qy = std::fabs(py - cy) - halfH + r;
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

float sdTriangle(float px, float py,
                 float x0, float y0,
                 float x1, float y1,
                 float x2, float y2) {
    auto dot2 = [](float ax, float ay) { return ax * ax + ay * ay; };
    auto cross = [](float ax, float ay, float bx, float by) {
        return ax * by - ay * bx;
    };
    const float e0x = x1 - x0, e0y = y1 - y0;
    const float e1x = x2 - x1, e1y = y2 - y1;
    const float e2x = x0 - x2, e2y = y0 - y2;
    const float v0x = px - x0, v0y = py - y0;
    const float v1x = px - x1, v1y = py - y1;
    const float v2x = px - x2, v2y = py - y2;
    const float t0 = clamp01((v0x * e0x + v0y * e0y) / dot2(e0x, e0y));
    const float t1 = clamp01((v1x * e1x + v1y * e1y) / dot2(e1x, e1y));
    const float t2 = clamp01((v2x * e2x + v2y * e2y) / dot2(e2x, e2y));
    const float pq0x = v0x - e0x * t0, pq0y = v0y - e0y * t0;
    const float pq1x = v1x - e1x * t1, pq1y = v1y - e1y * t1;
    const float pq2x = v2x - e2x * t2, pq2y = v2y - e2y * t2;
    const float s = cross(e0x, e0y, e2x, e2y) >= 0.0f ? 1.0f : -1.0f;
    const float minD = std::min({dot2(pq0x, pq0y), dot2(pq1x, pq1y), dot2(pq2x, pq2y)});
    const float minS = std::min({s * cross(v0x, v0y, e0x, e0y),
                                 s * cross(v1x, v1y, e1x, e1y),
                                 s * cross(v2x, v2y, e2x, e2y)});
    return -std::sqrt(minD) * (minS > 0.0f ? 1.0f : -1.0f);
}

void blendOver(uint8_t* dst, float r, float g, float b, float alpha) {
    const float a = clamp01(alpha);
    const float inv = 1.0f - a;
    dst[0] = static_cast<uint8_t>(std::lround(dst[0] * inv + r * a));
    dst[1] = static_cast<uint8_t>(std::lround(dst[1] * inv + g * a));
    dst[2] = static_cast<uint8_t>(std::lround(dst[2] * inv + b * a));
}

void jpegWriteCallback(void* ctx, void* data, int size) {
    if (!ctx || !data || size <= 0)
        return;
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

std::string normalizeArchiveTitleId(const std::string& titleId) {
    uint64_t parsed = 0;
    if (!InstalledTitleService::parseTitleId(titleId, parsed))
        return {};
    return InstalledTitleService::formatTitleId(
        InstalledTitleService::nxBaseApplicationId(parsed));
}

bool isArchivableBaseGameId(uint64_t applicationId) {
    if ((applicationId & 0xFF00000000000000ULL) != 0x0100000000000000ULL)
        return false;
    if ((applicationId & 0x1FFFULL) != 0)
        return false;
    if ((applicationId & 0xFFFFFFFF00000000ULL) == 0x0100BEEF00000000ULL)
        return false;
    return true;
}

std::string archiveBlockReasonCode(ArchiveBlockReason reason) {
    switch (reason) {
    case ArchiveBlockReason::None: return {};
    case ArchiveBlockReason::InvalidTitleId: return "invalid_id";
    case ArchiveBlockReason::AlreadyArchived: return "already_archived";
    case ArchiveBlockReason::NotInstalled: return "not_installed";
    case ArchiveBlockReason::HomebrewOrForwarder: return "homebrew_or_forwarder";
    case ArchiveBlockReason::ActiveInstallBusy: return "install_busy";
    case ArchiveBlockReason::NoRestoreSource: return "no_source";
    }
    return "unknown";
}

std::string archiveDirPath(const std::string& appRoot) {
    std::string root = appRoot;
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    return root + "/archives";
}

std::string archiveRecipePath(const std::string& appRoot,
                              const std::string& titleId) {
    return archiveDirPath(appRoot) + "/" + upperAscii(titleId) + ".json";
}

std::string archiveBackupIconPath(const std::string& appRoot,
                                  const std::string& titleId) {
    return archiveDirPath(appRoot) + "/" + upperAscii(titleId) + ".jpg";
}

std::string archiveBackupTorrentPath(const std::string& appRoot,
                                     const std::string& titleId) {
    return archiveDirPath(appRoot) + "/" + upperAscii(titleId) + ".torrent";
}

bool saveArchiveRecipe(const std::string& appRoot,
                       const ArchiveRecipe& recipe,
                       std::string& error) {
    error.clear();
    if (recipe.titleId.empty()) {
        error = "Archive recipe title ID is empty.";
        return false;
    }
    json j;
    j["schemaVersion"] = recipe.schemaVersion;
    j["titleId"] = upperAscii(recipe.titleId);
    j["applicationId"] = InstalledTitleService::formatTitleId(recipe.applicationId);
    j["name"] = recipe.name;
    j["publisher"] = recipe.publisher;
    j["displayVersion"] = recipe.displayVersion;
    j["installedVersion"] = recipe.installedVersion;
    j["sourceKind"] = sourceKindToString(recipe.sourceKind);
    j["infoHash"] = lowerAscii(recipe.infoHash);
    j["magnetUri"] = recipe.magnetUri;
    j["releaseTitle"] = recipe.releaseTitle;
    j["hasTorrentFile"] = recipe.hasTorrentFile;
    j["freedBytes"] = recipe.freedBytes;
    j["archivedAt"] = recipe.archivedAt;
    const std::string text = j.dump(2) + "\n";
    return writeBytesAtomic(archiveRecipePath(appRoot, recipe.titleId),
                            text.data(), text.size(), error);
}

bool loadArchiveRecipe(const std::string& appRoot,
                       const std::string& titleId,
                       ArchiveRecipe& out,
                       std::string& error) {
    out = ArchiveRecipe {};
    error.clear();
    const std::string upper = upperAscii(titleId);
    std::vector<uint8_t> raw;
    if (!readFileBytes(archiveRecipePath(appRoot, upper), raw)) {
        error = "Archive manifest not found.";
        return false;
    }
    const json parsed = json::parse(raw.begin(), raw.end(), nullptr, false);
    if (!parsed.is_object()) {
        error = "Archive manifest is malformed.";
        return false;
    }
    out.schemaVersion = parsed.value("schemaVersion", 1U);
    out.titleId = upperAscii(parsed.value("titleId", upper));
    uint64_t parsedAppId = 0;
    if (InstalledTitleService::parseTitleId(out.titleId, parsedAppId))
        out.applicationId = parsedAppId;
    out.name = parsed.value("name", std::string());
    out.publisher = parsed.value("publisher", std::string());
    out.displayVersion = parsed.value("displayVersion", std::string());
    out.installedVersion = parsed.value("installedVersion", std::string());
    out.sourceKind = sourceKindFromString(parsed.value("sourceKind", std::string()));
    out.infoHash = lowerAscii(parsed.value("infoHash", std::string()));
    out.magnetUri = parsed.value("magnetUri", std::string());
    out.releaseTitle = parsed.value("releaseTitle", std::string());
    out.hasTorrentFile = parsed.value("hasTorrentFile", false);
    out.freedBytes = parsed.value("freedBytes", uint64_t{0});
    out.archivedAt = parsed.value("archivedAt", int64_t{0});
    if (out.titleId.empty()) {
        error = "Archive manifest lacks a valid title ID.";
        return false;
    }
    return true;
}

void removeArchiveArtifacts(const std::string& appRoot,
                            const std::string& titleId) {
    if (titleId.empty())
        return;
    unlink(archiveRecipePath(appRoot, titleId).c_str());
    unlink(archiveBackupIconPath(appRoot, titleId).c_str());
    unlink(archiveBackupTorrentPath(appRoot, titleId).c_str());
}

std::vector<ArchiveRecipe> listArchiveRecipes(const std::string& appRoot) {
    std::vector<ArchiveRecipe> result;
    const std::string dirPath = archiveDirPath(appRoot);
    DIR* dir = opendir(dirPath.c_str());
    if (!dir)
        return result;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() != 21 || name.substr(16) != ".json")
            continue;
        ArchiveRecipe recipe;
        std::string ignoreErr;
        if (loadArchiveRecipe(appRoot, name.substr(0, 16), recipe, ignoreErr))
            result.push_back(std::move(recipe));
    }
    closedir(dir);
    std::sort(result.begin(), result.end(),
              [](const ArchiveRecipe& a, const ArchiveRecipe& b) {
                  return a.titleId < b.titleId;
              });
    return result;
}

bool badgeArchiveIconJpeg(const uint8_t* srcJpeg, size_t srcSize,
                          std::vector<uint8_t>& outJpeg,
                          std::string& error) {
    outJpeg.clear();
    error.clear();
    if (!srcJpeg || srcSize < 8) {
        error = "Source icon is empty.";
        return false;
    }
    int srcW = 0, srcH = 0, srcComp = 0;
    std::unique_ptr<uint8_t, void (*)(void*)> decoded(
        stbi_load_from_memory(srcJpeg, static_cast<int>(srcSize),
                              &srcW, &srcH, &srcComp, 3),
        stbi_image_free);
    if (!decoded || srcW <= 0 || srcH <= 0) {
        error = "Unable to decode source icon.";
        return false;
    }

    constexpr int kDim = 256;
    std::vector<uint8_t> canvas(static_cast<size_t>(kDim * kDim * 3));
    const uint8_t* src = decoded.get();
    if (srcW == kDim && srcH == kDim) {
        for (size_t i = 0; i < canvas.size(); ++i)
            canvas[i] = static_cast<uint8_t>((src[i] * 224U + 127U) / 255U);
    } else {
        for (int y = 0; y < kDim; ++y) {
            const float sy = (y + 0.5f) * static_cast<float>(srcH) / kDim - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, srcH - 1);
            const int y1 = std::min(y0 + 1, srcH - 1);
            const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
            for (int x = 0; x < kDim; ++x) {
                const float sx = (x + 0.5f) * static_cast<float>(srcW) / kDim - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, srcW - 1);
                const int x1 = std::min(x0 + 1, srcW - 1);
                const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
                const size_t i00 = static_cast<size_t>((y0 * srcW + x0) * 3);
                const size_t i10 = static_cast<size_t>((y0 * srcW + x1) * 3);
                const size_t i01 = static_cast<size_t>((y1 * srcW + x0) * 3);
                const size_t i11 = static_cast<size_t>((y1 * srcW + x1) * 3);
                const size_t dst = static_cast<size_t>((y * kDim + x) * 3);
                for (int c = 0; c < 3; ++c) {
                    const float top = src[i00 + c] * (1.0f - fx) + src[i10 + c] * fx;
                    const float bot = src[i01 + c] * (1.0f - fx) + src[i11 + c] * fx;
                    const float val = top * (1.0f - fy) + bot * fy;
                    canvas[dst + c] = static_cast<uint8_t>(
                        std::lround(val * (224.0f / 255.0f)));
                }
            }
        }
    }

    // Composite bottom-right rounded dark pill (56x56 at (188, 188)) +
    // high-contrast #F5F7FA cloud-with-downward-arrow SDF glyph.
    for (int y = 178; y < 252; ++y) {
        for (int x = 178; x < 252; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            uint8_t* dst = &canvas[static_cast<size_t>((y * kDim + x) * 3)];

            const float dShadow = sdRoundedBox(px, py - 1.5f, 216.0f, 216.0f,
                                               28.5f, 28.5f, 14.5f);
            const float shadowAlpha = 0.28f * (1.0f - smoothstep(-1.0f, 3.0f, dShadow));
            if (shadowAlpha > 0.001f)
                blendOver(dst, 0.0f, 0.0f, 0.0f, shadowAlpha);

            const float dPill = sdRoundedBox(px, py, 216.0f, 216.0f,
                                             28.0f, 28.0f, 14.0f);
            const float pillMask = 1.0f - smoothstep(-0.75f, 0.75f, dPill);
            if (pillMask > 0.001f)
                blendOver(dst, 26.0f, 29.0f, 34.0f, 0.88f * pillMask);

            const float borderMask = 1.0f - smoothstep(0.0f, 1.3f, std::fabs(dPill));
            if (borderMask > 0.001f)
                blendOver(dst, 255.0f, 255.0f, 255.0f, 0.24f * borderMask);

            const float dCloud = std::min({
                sdCircle(px, py, 216.0f, 207.5f, 8.5f),
                sdCircle(px, py, 206.0f, 213.5f, 6.5f),
                sdCircle(px, py, 226.0f, 213.5f, 6.5f),
                sdRoundedBox(px, py, 216.0f, 215.0f, 10.0f, 5.0f, 0.0f),
            });
            const float dShaft = sdRoundedBox(px, py, 216.0f, 218.0f,
                                              1.9f, 6.0f, 0.8f);
            const float dHead = sdTriangle(px, py,
                                           216.0f, 232.0f,
                                           209.5f, 222.5f,
                                           222.5f, 222.5f);
            const float dArrow = std::min(dShaft, dHead);
            const float dCloudCut = std::max(dCloud, -(dArrow - 2.0f));
            const float dGlyph = std::min(dCloudCut, dArrow);
            const float glyphAlpha = 1.0f - smoothstep(-0.75f, 0.75f, dGlyph);
            if (glyphAlpha > 0.001f)
                blendOver(dst, 245.0f, 247.0f, 250.0f, glyphAlpha);
        }
    }

    for (int quality : {90, 80, 68}) {
        outJpeg.clear();
        if (!stbi_write_jpg_to_func(jpegWriteCallback, &outJpeg,
                                    kDim, kDim, 3, canvas.data(), quality)) {
            error = "Unable to encode archive icon JPEG.";
            return false;
        }
        if (outJpeg.size() <= 120 * 1024)
            return true;
    }
    return true;
}

ArchiveService::ArchiveService(std::string appRoot,
                               InstalledTitleService* installed,
                               DownloadManager* downloads,
                               CatalogService* catalog,
                               GameMetadataService* metadata,
                               AppSettings* settings,
                               IsPortTitleFn isPortTitle,
                               std::string nroPath,
                               StubBuilderFn stubBuilder)
    : appRoot_(std::move(appRoot)),
      installed_(installed),
      downloads_(downloads),
      catalog_(catalog),
      metadata_(metadata),
      settings_(settings),
      isPortTitle_(std::move(isPortTitle)),
      nroPath_(nroPath.empty() ? "sdmc:/switch/pipensx/pipensx.nro"
                               : std::move(nroPath)),
      stubBuilder_(std::move(stubBuilder)) {}

void ArchiveService::setNroPath(std::string nroPath) {
    if (!nroPath.empty())
        nroPath_ = std::move(nroPath);
}

bool ArchiveService::resolveSource(const std::string& upperId,
                                   uint64_t appId,
                                   ResolvedSource& out) const {
    out = ResolvedSource {};
    (void)appId;

    auto findCatalogByHash = [&](const std::string& infoHashLower) -> const CatalogEntry* {
        if (!catalog_ || infoHashLower.empty())
            return nullptr;
        auto entries = catalog_->sharedEntries();
        if (!entries)
            return nullptr;
        for (const auto& e : *entries) {
            if (lowerAscii(e.infoHash) == infoHashLower)
                return &e;
        }
        return nullptr;
    };

    // 1. Check DownloadManager tasks (newest first).
    if (downloads_) {
        const auto tasks = downloads_->snapshot();
        for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
            const CatalogEntry* catMatch = findCatalogByHash(lowerAscii(it->id));
            if (matchTaskSourceForTitle(
                    *it, upperId, appRoot_, catMatch, metadata_,
                    out.kind, out.infoHash, out.magnetUri,
                    out.releaseTitle, out.localTorrentPath, out.infoDict)) {
                return true;
            }
        }
    }

    // 2. Check CatalogService + GameMetadataService.
    if (catalog_) {
        auto entries = catalog_->sharedEntries();
        if (entries) {
            const CatalogEntry* best = nullptr;
            int bestScore = -10000;
            for (const auto& entry : *entries) {
                const GameMetadata* meta = metadata_
                    ? metadata_->findByInfoHash(entry.infoHash, upperId)
                    : nullptr;
                if (!entryMatchesTitle(entry, meta, upperId) ||
                    !isEligibleBaseCatalogEntry(entry, meta)) {
                    continue;
                }
                const int score = scoreCatalogCandidate(entry, meta, upperId, appRoot_);
                if (!best || score > bestScore) {
                    best = &entry;
                    bestScore = score;
                }
            }
            if (best) {
                out.kind = ArchiveSourceKind::Catalog;
                out.infoHash = lowerAscii(best->infoHash);
                out.magnetUri = best->magnetUri.empty()
                    ? updateMagnetFor(out.infoHash, best)
                    : best->magnetUri;
                out.releaseTitle = best->title;
                out.infoDict = best->infoDict;
                struct stat st {};
                const std::string cachedTorrent =
                    appRoot_ + "/torrents/" + out.infoHash + ".torrent";
                if (!out.infoHash.empty() && stat(cachedTorrent.c_str(), &st) == 0 &&
                    st.st_size > 0) {
                    out.localTorrentPath = cachedTorrent;
                }
                return true;
            }
        }
    }

    return false;
}

bool ArchiveService::buildStubNsp(uint64_t titleId,
                                  const std::string& name,
                                  const std::string& publisher,
                                  const std::string& displayVersion,
                                  const std::string& upperId,
                                  const std::vector<uint8_t>& badgedIcon,
                                  std::vector<uint8_t>& outNsp,
                                  std::string& error) const {
    outNsp.clear();
    error.clear();
    const std::string args = nroPath_ + " --restore-archive " + upperId;
    if (stubBuilder_) {
        return stubBuilder_(titleId, name, publisher, displayVersion,
                            nroPath_, args, badgedIcon, outNsp, error);
    }
#ifdef __SWITCH__
    std::vector<uint8_t> loaderMain;
    std::vector<uint8_t> loaderNpdm;
    if (!readFileBytes("romfs:/autorun-loader.nso", loaderMain) ||
        !readFileBytes("romfs:/autorun-loader.npdm", loaderNpdm)) {
        error = "Bundled forwarder loader assets are missing.";
        return false;
    }
    void* rawOut = nullptr;
    size_t rawSize = 0;
    const uint32_t rc = pipensxBuildArchiveForwarder(
        titleId, name.c_str(), publisher.c_str(), displayVersion.c_str(),
        nroPath_.c_str(), args.c_str(),
        loaderMain.data(), loaderMain.size(),
        loaderNpdm.data(), loaderNpdm.size(),
        badgedIcon.data(), badgedIcon.size(),
        &rawOut, &rawSize);
    if (rc != 0 || !rawOut || rawSize == 0) {
        if (rawOut)
            std::free(rawOut);
        char text[128];
        std::snprintf(text, sizeof(text),
                      "Unable to build archive stub package (0x%08x).", rc);
        error = text;
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(rawOut);
    outNsp.assign(bytes, bytes + rawSize);
    std::free(rawOut);
    return true;
#else
    (void)titleId;
    (void)name;
    (void)publisher;
    (void)displayVersion;
    (void)badgedIcon;
    error = "Stub forwarder builder requires Switch hardware.";
    return false;
#endif
}

ArchiveInspection ArchiveService::inspectTitleWithSources(
    const std::string& upper,
    uint64_t appId,
    const ResolvedSource* preResolved) const {
    ArchiveInspection info;
    if (upper.empty()) {
        info.blockReason = ArchiveBlockReason::InvalidTitleId;
        return info;
    }
    info.titleId = upper;

    if (!isArchivableBaseGameId(appId)) {
        info.blockReason = ArchiveBlockReason::HomebrewOrForwarder;
        return info;
    }

    if (installed_ && installed_->isArchived(upper)) {
        info.isArchived = true;
        info.blockReason = ArchiveBlockReason::AlreadyArchived;
        install::TitleStorageFootprint footprint;
        std::string ignoreErr;
        if (install::inspectInstalledTitleFootprint(appRoot_, appId, footprint, ignoreErr) &&
            footprint.found) {
            info.installedBytes = footprint.totalBytes;
        }
        ArchiveRecipe recipe;
        if (loadArchiveRecipe(appRoot_, upper, recipe, ignoreErr)) {
            info.sourceKind = recipe.sourceKind;
            info.resolvedInfoHash = recipe.infoHash;
            info.resolvedMagnetUri = recipe.magnetUri;
            info.resolvedTitle = recipe.releaseTitle;
            if (info.installedBytes == 0)
                info.installedBytes = kEstimatedArchiveStubBytes;
            info.estimatedFreedBytes = recipe.freedBytes;
        }
        return info;
    }

    if (installed_ && !installed_->contains(upper)) {
        info.blockReason = ArchiveBlockReason::NotInstalled;
        return info;
    }

    if (isPortTitle_ && isPortTitle_(upper)) {
        info.blockReason = ArchiveBlockReason::HomebrewOrForwarder;
        return info;
    }

    install::TitleStorageFootprint footprint;
    std::string ignoreErr;
    if (install::inspectInstalledTitleFootprint(appRoot_, appId, footprint, ignoreErr) &&
        footprint.found) {
        info.installedBytes = footprint.totalBytes;
        info.estimatedFreedBytes = footprint.totalBytes > kEstimatedArchiveStubBytes
            ? footprint.totalBytes - kEstimatedArchiveStubBytes
            : footprint.totalBytes;
    }

    if (downloads_ && downloads_->hasActiveInstallation()) {
        info.blockReason = ArchiveBlockReason::ActiveInstallBusy;
        return info;
    }

    ResolvedSource fallback;
    const ResolvedSource* src = preResolved;
    if (!src) {
        if (resolveSource(upper, appId, fallback))
            src = &fallback;
    }
    if (!src || src->kind == ArchiveSourceKind::None) {
        info.blockReason = ArchiveBlockReason::NoRestoreSource;
        return info;
    }

    info.canArchive = true;
    info.blockReason = ArchiveBlockReason::None;
    info.sourceKind = src->kind;
    info.resolvedInfoHash = src->infoHash;
    info.resolvedMagnetUri = src->magnetUri;
    info.resolvedTitle = src->releaseTitle;
    info.resolvedTorrentPath = src->localTorrentPath;
    info.hasCatalogInfoDict = !src->infoDict.empty();
    return info;
}

ArchiveInspection ArchiveService::inspectArchive(const std::string& titleId) const {
    const std::string upper = normalizeArchiveTitleId(titleId);
    uint64_t appId = 0;
    if (!upper.empty())
        InstalledTitleService::parseTitleId(upper, appId);
    return inspectTitleWithSources(upper, appId, nullptr);
}

std::unordered_map<std::string, ArchiveInspection>
ArchiveService::inspectLibrary(const std::vector<InstalledTitle>& titles) const {
    std::unordered_map<std::string, ArchiveInspection> out;
    out.reserve(titles.size());

    std::unordered_map<std::string, const CatalogEntry*> catalogByHash;
    struct RankedEntry {
        const CatalogEntry* entry = nullptr;
        int score = -10000;
    };
    std::unordered_map<std::string, RankedEntry> bestCatalogByTitle;

    std::shared_ptr<const std::vector<CatalogEntry>> entries;
    if (catalog_)
        entries = catalog_->sharedEntries();
    if (entries) {
        catalogByHash.reserve(entries->size());
        for (const auto& entry : *entries) {
            if (!entry.infoHash.empty())
                catalogByHash.emplace(lowerAscii(entry.infoHash), &entry);
            const GameMetadata* meta = metadata_
                ? metadata_->findByInfoHash(entry.infoHash)
                : nullptr;
            if (!isEligibleBaseCatalogEntry(entry, meta))
                continue;

            auto considerId = [&](const std::string& rawId) {
                const std::string upper = normalizeArchiveTitleId(rawId);
                if (upper.empty())
                    return;
                const int score = scoreCatalogCandidate(entry, meta, upper, appRoot_);
                auto it = bestCatalogByTitle.find(upper);
                if (it == bestCatalogByTitle.end() || score > it->second.score)
                    bestCatalogByTitle[upper] = RankedEntry{&entry, score};
            };
            if (!entry.titleId.empty())
                considerId(entry.titleId);
            for (const auto& t : entry.titles)
                considerId(t.titleId);
            if (meta && !meta->titleId.empty())
                considerId(meta->titleId);
        }
    }

    const std::vector<DownloadTask> tasks = downloads_
        ? downloads_->snapshot() : std::vector<DownloadTask>{};

    for (const auto& title : titles) {
        const std::string upper = normalizeArchiveTitleId(title.titleId);
        uint64_t appId = title.applicationId;
        if (appId == 0 && !upper.empty())
            InstalledTitleService::parseTitleId(upper, appId);
        if (upper.empty())
            continue;

        ResolvedSource src;
        if (installed_ && !installed_->isArchived(upper) &&
            isArchivableBaseGameId(appId)) {
            for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
                const CatalogEntry* catMatch = nullptr;
                auto hashIt = catalogByHash.find(lowerAscii(it->id));
                if (hashIt != catalogByHash.end())
                    catMatch = hashIt->second;
                if (matchTaskSourceForTitle(
                        *it, upper, appRoot_, catMatch, metadata_,
                        src.kind, src.infoHash, src.magnetUri,
                        src.releaseTitle, src.localTorrentPath, src.infoDict)) {
                    break;
                }
            }
            if (src.kind == ArchiveSourceKind::None) {
                auto catIt = bestCatalogByTitle.find(upper);
                if (catIt != bestCatalogByTitle.end() && catIt->second.entry) {
                    const CatalogEntry* best = catIt->second.entry;
                    src.kind = ArchiveSourceKind::Catalog;
                    src.infoHash = lowerAscii(best->infoHash);
                    src.magnetUri = best->magnetUri.empty()
                        ? updateMagnetFor(src.infoHash, best)
                        : best->magnetUri;
                    src.releaseTitle = best->title;
                    src.infoDict = best->infoDict;
                    struct stat st {};
                    const std::string cachedTorrent =
                        appRoot_ + "/torrents/" + src.infoHash + ".torrent";
                    if (!src.infoHash.empty() &&
                        stat(cachedTorrent.c_str(), &st) == 0 && st.st_size > 0) {
                        src.localTorrentPath = cachedTorrent;
                    }
                }
            }
        }

        out[upper] = inspectTitleWithSources(upper, appId, &src);
    }
    return out;
}

bool ArchiveService::archiveTitle(const std::string& titleId,
                                  ArchiveActionResult& result,
                                  std::string& error) {
    result = ArchiveActionResult {};
    error.clear();

    const ArchiveInspection inspect = inspectArchive(titleId);
    if (!inspect.canArchive) {
        switch (inspect.blockReason) {
        case ArchiveBlockReason::AlreadyArchived:
            error = "Game is already archived.";
            break;
        case ArchiveBlockReason::HomebrewOrForwarder:
            error = "Only Switch base games can be archived.";
            break;
        case ArchiveBlockReason::ActiveInstallBusy:
            error = "Wait for active installations to finish first.";
            break;
        case ArchiveBlockReason::NoRestoreSource:
            error = "No catalog or task source found for this game.";
            break;
        case ArchiveBlockReason::NotInstalled:
            error = "Title is not installed.";
            break;
        default:
            error = "Invalid title ID.";
            break;
        }
        return false;
    }

    const std::string upper = inspect.titleId;
    uint64_t appId = 0;
    InstalledTitleService::parseTitleId(upper, appId);

    InstalledTitle metaTitle;
    if (installed_) {
        for (const auto& t : installed_->titles()) {
            if (upperAscii(t.titleId) == upper) {
                metaTitle = t;
                break;
            }
        }
    }
    if (metaTitle.name.empty())
        metaTitle.name = !inspect.resolvedTitle.empty() ? inspect.resolvedTitle : upper;

    ResolvedSource src;
    if (!resolveSource(upper, appId, src)) {
        error = "No catalog or task source found for this game.";
        return false;
    }

    std::vector<uint8_t> rawIcon;
    if (!installed_ || !installed_->readControlIcon(upper, rawIcon, error)) {
        if (error.empty())
            error = "Application icon is unavailable.";
        return false;
    }
    std::vector<uint8_t> badgedIcon;
    if (!badgeArchiveIconJpeg(rawIcon.data(), rawIcon.size(), badgedIcon, error))
        return false;

    std::vector<uint8_t> stubNsp;
    if (!buildStubNsp(appId, metaTitle.name, metaTitle.publisher,
                      metaTitle.displayVersion, upper, badgedIcon,
                      stubNsp, error)) {
        return false;
    }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> pfsFiles;
    if (!parsePfs0(stubNsp.data(), stubNsp.size(), pfsFiles) || pfsFiles.empty()) {
        error = "Generated archive forwarder package is invalid.";
        return false;
    }

    SystemCleanupGuard cleanupGuard;
    if (!cleanupGuard.acquire(downloads_, error))
        return false;

    if (!writeBytesAtomic(archiveBackupIconPath(appRoot_, upper),
                          rawIcon.data(), rawIcon.size(), error)) {
        return false;
    }

    bool hasTorrentBackup = false;
    const std::string torrentDst = archiveBackupTorrentPath(appRoot_, upper);
    if (!src.localTorrentPath.empty()) {
        std::string copyErr;
        hasTorrentBackup = copyFileToAtomic(src.localTorrentPath, torrentDst, copyErr);
    } else if (!src.infoDict.empty()) {
        const std::string magnet = src.magnetUri.empty()
            ? updateMagnetFor(src.infoHash, nullptr)
            : src.magnetUri;
        std::string writeErr;
        const std::string tmp = torrentDst + ".tmp";
        if (writeTorrentFromInfoDict(magnet, src.infoDict, tmp, writeErr) &&
            rename(tmp.c_str(), torrentDst.c_str()) == 0) {
            hasTorrentBackup = true;
        } else {
            unlink(tmp.c_str());
        }
    }

    ArchiveRecipe recipe;
    recipe.schemaVersion = 1;
    recipe.titleId = upper;
    recipe.applicationId = appId;
    recipe.name = metaTitle.name;
    recipe.publisher = metaTitle.publisher;
    recipe.displayVersion = metaTitle.displayVersion;
    recipe.installedVersion = metaTitle.version;
    recipe.sourceKind = src.kind;
    recipe.infoHash = src.infoHash;
    recipe.magnetUri = src.magnetUri;
    recipe.releaseTitle = src.releaseTitle;
    recipe.hasTorrentFile = hasTorrentBackup;
    recipe.freedBytes = inspect.estimatedFreedBytes;
    recipe.archivedAt = static_cast<int64_t>(std::time(nullptr));
    if (!saveArchiveRecipe(appRoot_, recipe, error)) {
        removeArchiveArtifacts(appRoot_, upper);
        return false;
    }

    uint64_t removedBytes = 0;
    if (!install::removeTitleContentPreservingSaves(
            appRoot_, appId, &removedBytes, error)) {
        return false;
    }

    auto backend = install::createInstallBackend(
        appRoot_, install::InstallStorageTarget::SdCard);
    if (!backend) {
        error = "Unable to create installation backend.";
        return false;
    }
    const std::string taskId = "archive-stub-" + upper;
    const std::string packageName = metaTitle.name + " [" + upper + "].nsp";
    if (!backend->beginPackage(taskId, packageName)) {
        error = backend->error();
        return false;
    }
    for (const auto& entry : pfsFiles) {
        if (!backend->beginFile(entry.first, entry.second.size()) ||
            (!entry.second.empty() &&
             !backend->writeFile(entry.second.data(), entry.second.size())) ||
            !backend->endFile()) {
            error = backend->error();
            backend->rollbackPackage();
            return false;
        }
    }
    bool alreadyInstalled = false;
    if (!backend->commitPackage(alreadyInstalled)) {
        error = backend->error();
        backend->rollbackPackage();
        return false;
    }

    const std::string archIconCache =
        appRoot_ + "/installed-icons/" + upper + "-arch.jpg";
    std::string ignoreErr;
    writeBytesAtomic(archIconCache, badgedIcon.data(), badgedIcon.size(), ignoreErr);
    if (installed_) {
        unlink((appRoot_ + "/installed-icons/" + upper + ".jpg").c_str());
    }

    result.titleId = upper;
    result.removedBytes = removedBytes;
    result.stubBytes = stubNsp.size();
    result.freedBytes = removedBytes > stubNsp.size()
        ? removedBytes - stubNsp.size()
        : removedBytes;
    recipe.freedBytes = result.freedBytes;
    saveArchiveRecipe(appRoot_, recipe, ignoreErr);
    result.recipe = std::move(recipe);

    if (installed_) {
        std::string refreshErr;
        installed_->refresh(refreshErr);
    }

    log_msg("[archive] archived %s freed=%llu stub=%zu\n",
            upper.c_str(),
            static_cast<unsigned long long>(result.freedBytes),
            stubNsp.size());
    return true;
}

bool ArchiveService::restoreTitle(const std::string& titleId,
                                  std::atomic<bool>& cancelled,
                                  RestoreStartResult& result,
                                  std::string& error,
                                  bool allowBlockingMagnetResolve) {
    result = RestoreStartResult {};
    error.clear();

    const std::string upper = normalizeArchiveTitleId(titleId);
    if (upper.empty()) {
        error = "Invalid title ID.";
        return false;
    }
    result.titleId = upper;

    if (!downloads_) {
        error = "Download manager is unavailable.";
        return false;
    }

    ArchiveRecipe recipe;
    std::string ignoreErr;
    bool hasRecipe = loadArchiveRecipe(appRoot_, upper, recipe, ignoreErr);

    ResolvedSource liveSrc;
    uint64_t appId = 0;
    InstalledTitleService::parseTitleId(upper, appId);
    const bool hasLiveSrc = resolveSource(upper, appId, liveSrc);

    if (!hasRecipe) {
        if (!hasLiveSrc) {
            error = "No restore manifest or catalog source found for this game.";
            return false;
        }
        recipe.titleId = upper;
        recipe.applicationId = appId;
        recipe.name = liveSrc.releaseTitle;
        recipe.sourceKind = liveSrc.kind;
        recipe.infoHash = liveSrc.infoHash;
        recipe.magnetUri = liveSrc.magnetUri;
        recipe.releaseTitle = liveSrc.releaseTitle;
    } else if (hasLiveSrc) {
        if (recipe.magnetUri.empty())
            recipe.magnetUri = liveSrc.magnetUri;
        if (recipe.infoHash.empty())
            recipe.infoHash = liveSrc.infoHash;
    }

    const std::string torrentFile = archiveBackupTorrentPath(appRoot_, upper);
    struct stat st {};
    const bool hasBackupTorrent =
        stat(torrentFile.c_str(), &st) == 0 && st.st_size > 0;

    if (!recipe.infoHash.empty()) {
        const std::string wantId = lowerAscii(recipe.infoHash);
        for (const auto& t : downloads_->snapshot()) {
            if (lowerAscii(t.id) != wantId)
                continue;
            if (t.status == DownloadStatus::Queued ||
                t.status == DownloadStatus::Checking ||
                t.status == DownloadStatus::Fetching ||
                t.status == DownloadStatus::Downloading ||
                t.status == DownloadStatus::Verifying ||
                t.status == DownloadStatus::Installing ||
                t.status == DownloadStatus::Committing) {
                std::string moveErr;
                downloads_->moveToFront(t.id, moveErr);
                result.queuedImmediately = true;
                result.reusedExistingTask = true;
                result.taskId = t.id;
                return true;
            }
            if (t.status == DownloadStatus::Paused ||
                t.status == DownloadStatus::Error) {
                if (downloads_->resume(t.id, error)) {
                    result.queuedImmediately = true;
                    result.reusedExistingTask = true;
                    result.taskId = t.id;
                    return true;
                }
                std::string rmErr;
                downloads_->remove(t.id, t.status == DownloadStatus::Error, rmErr);
                break;
            }
            if (t.status == DownloadStatus::Completed &&
                t.mode == TransferMode::DownloadOnly) {
                if (downloads_->installDownloaded(t.id, error)) {
                    result.queuedImmediately = true;
                    result.reusedExistingTask = true;
                    result.taskId = t.id;
                    return true;
                }
            }
            std::string rmErr;
            if (!downloads_->remove(t.id, false, rmErr)) {
                error = rmErr.empty()
                    ? "Unable to replace previous download record for restore."
                    : rmErr;
                return false;
            }
            break;
        }
    }

    AddRequest req;
    req.title = !recipe.releaseTitle.empty() ? recipe.releaseTitle : recipe.name;
    req.titleId = upper;
    req.infoHashHex = recipe.infoHash;
    req.magnetUri = !recipe.magnetUri.empty()
        ? recipe.magnetUri
        : updateMagnetFor(recipe.infoHash, nullptr);
    req.infoDict = liveSrc.infoDict;
    req.requestedMode = TransferMode::StreamInstall;
    req.selection = settings_ ? settings_->get().streamSelection
                              : StreamSelection::AllFiles;
    req.oneTap.titleInstalled = false;
    req.oneTap.titleId = upper;
    if (installed_)
        req.oneTap.installedDlcIds = installed_->dlcTitleIds();

    ReleaseAdder adder(*downloads_);
    const AddInputKind inputKind =
        hasBackupTorrent ? AddInputKind::TorrentFile : AddInputKind::Catalog;
    req.input = inputKind;
    const AddSourceChoice choice = adder.choose(inputKind);
    if (!choice.accepted) {
        error = choice.refusal.empty()
            ? "No active download source is configured."
            : choice.refusal;
        return false;
    }

    auto finishOutcome = [&](const AddOutcome& outcome) {
        if (!outcome.ok) {
            error = outcome.error.empty()
                ? "Unable to start archive restore download."
                : outcome.error;
            return false;
        }
        result.queuedImmediately = true;
        result.taskId = outcome.taskId;
        return true;
    };

    if (!choice.direct) {
        if (!req.magnetUri.empty() || !req.infoHashHex.empty()) {
            const AddOutcome viaMagnet = adder.addViaDebrid(req, cancelled);
            if (viaMagnet.ok || !hasBackupTorrent)
                return finishOutcome(viaMagnet);
        }
        if (hasBackupTorrent)
            return finishOutcome(adder.addTorrentFile(req, torrentFile, cancelled));
        error = "Archive manifest contains neither magnet URI nor torrent file.";
        return false;
    }

    if (hasBackupTorrent)
        return finishOutcome(adder.addTorrentFile(req, torrentFile, cancelled));

    if (!req.infoDict.empty() && !req.magnetUri.empty()) {
        makeDirs(archiveDirPath(appRoot_));
        const std::string tmp =
            archiveDirPath(appRoot_) + "/restore-" + upper + ".torrent";
        if (writeTorrentFromInfoDict(req.magnetUri, req.infoDict, tmp, error)) {
            const AddOutcome outcome = adder.importResolvedTorrent(req, tmp, {});
            unlink(tmp.c_str());
            return finishOutcome(outcome);
        }
    }

    if (req.magnetUri.empty() && req.infoHashHex.empty()) {
        error = "Archive restore source is unavailable.";
        return false;
    }
    if (!allowBlockingMagnetResolve) {
        result.needsAsyncMagnet = true;
        result.magnetUri = req.magnetUri;
        result.infoHash = req.infoHashHex;
        result.releaseTitle = req.title;
        result.infoDict = req.infoDict;
        return true;
    }

    makeDirs(archiveDirPath(appRoot_));
    const std::string tmp =
        archiveDirPath(appRoot_) + "/restore-" + upper + ".torrent";
    MagnetResolver resolver;
    std::vector<uint8_t> initialPeers;
    if (!resolver.resolveToFile(
            req.magnetUri, tmp, cancelled, {}, error, &initialPeers,
            req.infoDict.empty() ? nullptr : &req.infoDict)) {
        unlink(tmp.c_str());
        if (error.empty())
            error = "Unable to resolve restore torrent.";
        return false;
    }
    const AddOutcome outcome =
        adder.importResolvedTorrent(req, tmp, initialPeers);
    unlink(tmp.c_str());
    return finishOutcome(outcome);
}

bool ArchiveService::removeArchivedStub(const std::string& titleId,
                                        std::string& error) {
    error.clear();
    const std::string upper = normalizeArchiveTitleId(titleId);
    if (upper.empty()) {
        error = "Invalid title ID.";
        return false;
    }
    uint64_t appId = 0;
    if (!InstalledTitleService::parseTitleId(upper, appId) ||
        !isArchivableBaseGameId(appId)) {
        error = "Invalid application ID.";
        return false;
    }

    SystemCleanupGuard guard;
    if (!guard.acquire(downloads_, error))
        return false;

    if (!install::removeTitleContentPreservingSaves(
            appRoot_, appId, nullptr, error)) {
        return false;
    }
    removeArchiveArtifacts(appRoot_, upper);
    if (installed_) {
        installed_->invalidateCachedIcon(upper);
        std::string refreshErr;
        installed_->refresh(refreshErr);
    }
    return true;
}

} // namespace pipensx
