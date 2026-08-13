#include "storage_manager.hpp"

#include "install_space.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pipensx {
namespace {

constexpr const char* kInstallTemp = "/install-temp";

bool addBytes(uint64_t& target, uint64_t value) {
    if (value > UINT64_MAX - target) {
        target = UINT64_MAX;
        return false;
    }
    target += value;
    return true;
}

// Recursive file/dir size. Unlike directorySize, this never fails: a missing
// path or unreadable entry contributes 0 rather than short-circuiting the
// whole scan (a breakdown should degrade gracefully, not disappear).
void accumulateSize(const std::string& path, uint64_t& out) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        addBytes(out, static_cast<uint64_t>(st.st_size));
        return;
    }
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return;
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        accumulateSize(path + "/" + entry->d_name, out);
    }
    closedir(dir);
}

bool removeTree(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        if (!removeTree(path + "/" + entry->d_name))
            ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

bool isTempTorrentName(const std::string& name) {
    return name.rfind("_update_tmp_", 0) == 0 ||
           name.rfind("_catalog_tmp_", 0) == 0;
}

} // namespace

bool directorySize(const std::string& path, uint64_t& out) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return false;
    uint64_t size = 0;
    if (!S_ISDIR(st.st_mode)) {
        size = static_cast<uint64_t>(st.st_size);
    } else {
        DIR* dir = opendir(path.c_str());
        if (!dir)
            return false;
        bool ok = true;
        while (dirent* entry = readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0)
                continue;
            uint64_t childSize = 0;
            if (directorySize(path + "/" + entry->d_name, childSize))
                addBytes(size, childSize);
            else
                ok = false;
        }
        closedir(dir);
        if (!ok)
            return false;
    }
    out = size;
    return true;
}

StorageBreakdown scanStorageBreakdown(const std::string& rootPath) {
    StorageBreakdown result;
    accumulateSize(rootPath + "/downloads", result.downloadsBytes);
    accumulateSize(rootPath + "/torrents", result.torrentBytes);
    accumulateSize(rootPath + "/catalog/images", result.imageCacheBytes);
    accumulateSize(rootPath + "/catalog/metadata", result.metadataCacheBytes);
    accumulateSize(rootPath + "/installed-icons", result.iconsBytes);
    accumulateSize(rootPath + kInstallTemp, result.temporaryBytes);

    // Transient magnet-resolution torrents live directly under the root.
    DIR* dir = opendir(rootPath.c_str());
    if (dir) {
        while (dirent* entry = readdir(dir)) {
            if (isTempTorrentName(entry->d_name))
                accumulateSize(rootPath + "/" + entry->d_name,
                               result.temporaryBytes);
        }
        closedir(dir);
    }

    const StorageSpaceSnapshot storage = queryStorageSpace(rootPath);
    result.available = storage.available;
    result.totalBytes = storage.totalBytes;
    result.freeBytes = storage.freeBytes;
    if (storage.available && storage.totalBytes >= storage.freeBytes) {
        const uint64_t used = storage.totalBytes - storage.freeBytes;
        const uint64_t managed = result.downloadsBytes + result.torrentBytes +
                                 result.imageCacheBytes +
                                 result.metadataCacheBytes +
                                 result.temporaryBytes + result.iconsBytes;
        result.otherBytes = used > managed ? used - managed : 0;
    }
    return result;
}

bool clearTemporaryFiles(const std::string& rootPath, std::string& error,
                         uint64_t& recovered) {
    error.clear();
    recovered = 0;

    uint64_t tempBytes = 0;
    accumulateSize(rootPath + kInstallTemp, tempBytes);
    if (!removeTree(rootPath + kInstallTemp)) {
        error = "Unable to remove the install-temp directory.";
        return false;
    }
    addBytes(recovered, tempBytes);

    DIR* dir = opendir(rootPath.c_str());
    if (!dir) {
        error = "Unable to list application storage.";
        return false;
    }
    while (dirent* entry = readdir(dir)) {
        if (!isTempTorrentName(entry->d_name))
            continue;
        const std::string path = rootPath + "/" + entry->d_name;
        uint64_t size = 0;
        accumulateSize(path, size);
        if (unlink(path.c_str()) == 0)
            addBytes(recovered, size);
    }
    closedir(dir);
    return true;
}

bool clearOrphanTorrents(const std::string& torrentRoot,
                         const std::vector<std::string>& activeInfoHashes,
                         std::string& error, uint64_t& recovered) {
    error.clear();
    recovered = 0;
    DIR* dir = opendir(torrentRoot.c_str());
    if (!dir) {
        // No torrent directory yet: nothing orphaned.
        if (errno == ENOENT)
            return true;
        error = "Unable to list torrent metadata.";
        return false;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 8 || name.rfind(".torrent") != name.size() - 8)
            continue;
        const std::string hash = upperAscii(name.substr(0, name.size() - 8));
        bool active = false;
        for (const std::string& candidate : activeInfoHashes)
            if (upperAscii(candidate) == hash) {
                active = true;
                break;
            }
        if (active)
            continue;
        const std::string path = torrentRoot + "/" + name;
        uint64_t size = 0;
        accumulateSize(path, size);
        if (unlink(path.c_str()) == 0)
            addBytes(recovered, size);
    }
    closedir(dir);
    return true;
}

uint64_t orphanTorrentBytes(
    const std::string& torrentRoot,
    const std::vector<std::string>& activeInfoHashes) {
    uint64_t total = 0;
    DIR* dir = opendir(torrentRoot.c_str());
    if (!dir)
        return 0;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 8 || name.rfind(".torrent") != name.size() - 8)
            continue;
        const std::string hash = upperAscii(name.substr(0, name.size() - 8));
        bool active = false;
        for (const std::string& candidate : activeInfoHashes)
            if (upperAscii(candidate) == hash) {
                active = true;
                break;
            }
        if (active)
            continue;
        accumulateSize(torrentRoot + "/" + name, total);
    }
    closedir(dir);
    return total;
}

} // namespace pipensx
