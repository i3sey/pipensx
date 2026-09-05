#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

// Space breakdown of the SD card, split into the directories pipensx manages
// and a computed "other" bucket (installed games, system, other apps). Every
// managed bucket is measured by walking the on-disk tree; a missing directory
// counts as 0 bytes rather than failing the whole scan.
struct StorageBreakdown {
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t downloadsBytes = 0;
    uint64_t torrentBytes = 0;
    uint64_t imageCacheBytes = 0;
    uint64_t metadataCacheBytes = 0;
    uint64_t temporaryBytes = 0;
    uint64_t iconsBytes = 0;
    uint64_t otherBytes = 0;
    bool available = false;
};

// Recursive size of a file or directory. Returns false (and leaves `out`
// unchanged) when the path cannot be stat'd or any child cannot be measured.
bool directorySize(const std::string& path, uint64_t& out);

// Snapshot of every pipensx-managed directory under `rootPath`, plus the
// storage total/free (via queryStorageSpace) and a computed "other" bucket.
// Never fails: a missing directory counts as 0 bytes and an unavailable
// storage query simply leaves `available` false.
StorageBreakdown scanStorageBreakdown(const std::string& rootPath);

// Removes transient files: the install-temp tree and the `_update_tmp_*` /
// `_catalog_tmp_*` torrents left behind by magnet resolution. Returns the
// number of bytes recovered in `recovered`; an empty/nonexistent target is
// success with 0 recovered.
bool clearTemporaryFiles(const std::string& rootPath, std::string& error,
                         uint64_t& recovered);

// Removes `.torrent` files in `torrentRoot` whose info hash (the file name
// minus the extension, case-insensitive) is not in `activeInfoHashes`.
// Orphaned metainfo left behind after a crash or an interrupted remove is
// safe to drop: the download data is keyed separately. Returns recovered
// bytes.
bool clearOrphanTorrents(const std::string& torrentRoot,
                         const std::vector<std::string>& activeInfoHashes,
                         std::string& error, uint64_t& recovered);

// Total bytes of the orphaned `.torrent` files clearOrphanTorrents would
// remove — the recoverable-space estimate for the cleanup action.
uint64_t orphanTorrentBytes(
    const std::string& torrentRoot,
    const std::vector<std::string>& activeInfoHashes);

// Total bytes of orphaned download-data entries directly under `downloadsRoot`
// (B1: `downloads/<name>-<hash8>` left behind by a crash, a power-off or an
// interrupted remove). `activeDataPaths` are the full task dataPaths from a
// DownloadManager snapshot; a direct child is kept when its full path — or,
// defensively, its basename — matches an active task. Anything else (stale
// task directories and stray files) counts as orphaned.
uint64_t orphanDownloadBytes(
    const std::string& downloadsRoot,
    const std::vector<std::string>& activeDataPaths);

// Removes the orphaned download-data entries orphanDownloadBytes would count.
// Never touches an active task directory. A missing downloads directory is
// success with 0 recovered. Returns recovered bytes in `recovered`.
bool clearOrphanDownloads(const std::string& downloadsRoot,
                          const std::vector<std::string>& activeDataPaths,
                          std::string& error, uint64_t& recovered);

} // namespace pipensx
