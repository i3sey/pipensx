#include "download_manager.hpp"
#include "task_files.hpp"
#include "request_gate.hpp"
#include "stream_budget_arbiter.hpp"
#include "stream_ram_budget.hpp"
#include "web_seed_source.hpp"
#include "torbox_provider.hpp"
#include "torrserver_provider.hpp"
#include "realdebrid_provider.hpp"
#include "debrid_transfer.hpp"
#include "package_coordinator.hpp"
#include "nx_file_types.hpp"
#include "../install/install_backend.hpp"
#include "../install/install_journal.hpp"
#include "../install/package_stream.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/metainfo.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <typeinfo>
#include <unistd.h>

namespace pipensx {
namespace {

// TCP listen port for runner slot 0; slot i listens on kBasePeerPort + i
// (51413-51416), so N=1 keeps the classic port. The shared DHT engine's UDP
// socket also lives on 51413 (different protocol, no clash) and the magnet
// resolver no longer binds a port of its own.
constexpr uint16_t kBasePeerPort = 51413;

uint8_t actionValue(FileAction action) {
    return static_cast<uint8_t>(action);
}

bool isValidFileAction(uint8_t value) {
    return value == actionValue(FileAction::Skip) ||
           value == actionValue(FileAction::Download) ||
           value == actionValue(FileAction::Install);
}

FileAction defaultActionFor(const TorrentPreview::File& file,
                            TransferMode mode) {
    return mode == TransferMode::StreamInstall && file.package
        ? FileAction::Install
        : FileAction::Download;
}

std::vector<uint8_t> actionsFromLegacySelection(
    const TorrentPreview& preview,
    TransferMode mode,
    const std::vector<uint8_t>& selectedFiles) {
    std::vector<uint8_t> actions;
    actions.reserve(preview.files.size());
    const bool useSelection = !selectedFiles.empty();
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const bool selected = !useSelection || selectedFiles[i] != 0;
        if (!selected) {
            actions.push_back(actionValue(FileAction::Skip));
        } else {
            actions.push_back(actionValue(defaultActionFor(preview.files[i],
                                                          mode)));
        }
    }
    return actions;
}

bool makeDirectories(const std::string& path) {
    if (path.empty())
        return false;
    char buffer[1024];
    if (path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* p = buffer + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

bool directoryEmpty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool empty = true;
    while (struct dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(dir);
    return empty;
}

// Remove stray resolve temp files left by a cancelled update-file chooser:
// brls::Dialog dismisses with B without invoking any callback, so the unlink
// on the "later" button is skipped. Startup-only — no resolve can be in
// flight while the manager is being constructed.
void sweepTempTorrents(const std::string& root, const char* prefix) {
    DIR* dir = opendir(root.c_str());
    if (!dir)
        return;
    const size_t prefixLen = std::strlen(prefix);
    while (struct dirent* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, prefix, prefixLen) == 0)
            ::unlink((root + "/" + entry->d_name).c_str());
    }
    closedir(dir);
}

bool copyFile(const std::string& source, const std::string& destination) {
    std::ifstream input(source, std::ios::binary);
    if (!input)
        return false;
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << input.rdbuf();
    output.flush();
    return input.good() || input.eof() ? output.good() : false;
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
        std::string child = path + "/" + entry->d_name;
        if (!removeTree(child))
            ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
}

std::string safeComponent(const std::string& name) {
    std::string result;
    result.reserve(std::min<size_t>(name.size(), 64));
    for (unsigned char c : name) {
        if (result.size() >= 64)
            break;
        if (std::isalnum(c) || c == '-' || c == '_')
            result.push_back(static_cast<char>(c));
        else if (c == ' ' || c == '.')
            result.push_back('_');
    }
    if (result.empty())
        result = "download";
    return result;
}

bool isManagedChild(const std::string& root, const std::string& path) {
    std::string prefix = root + "/";
    if (path.rfind(prefix, 0) != 0)
        return false;
    std::string child = path.substr(prefix.size());
    return !child.empty() && child.find('/') == std::string::npos &&
           child != "." && child != "..";
}

std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

std::string bint(uint64_t value) {
    return "i" + std::to_string(value) + "e";
}

bool dictionaryString(const be_node_t& dict, const char* key,
                      std::string& value) {
    be_node_t node;
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &node) ||
        node.type != BE_STR)
        return false;
    value.assign(node.sval, node.slen);
    return true;
}

bool dictionaryInteger(const be_node_t& dict, const char* key,
                       uint64_t& value) {
    be_node_t node;
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &node) ||
        node.type != BE_INT || node.ival < 0)
        return false;
    value = static_cast<uint64_t>(node.ival);
    return true;
}


void upgradeLegacySelection(DownloadTask& task) {
    if (task.fileSelection.empty())
        return;
    metainfo_t metainfo;
    if (!metainfo_load(task.metainfoPath.c_str(), &metainfo))
        return;
    if (task.fileSelection.size() == metainfo.num_files) {
        std::vector<uint8_t> actions;
        actions.reserve(task.fileSelection.size());
        for (uint32_t i = 0; i < metainfo.num_files; ++i) {
            if (task.fileSelection[i] == 0) {
                actions.push_back(actionValue(FileAction::Skip));
            } else if (task.mode == TransferMode::StreamInstall &&
                       isPackageName(metainfo.files[i].path)) {
                actions.push_back(actionValue(FileAction::Install));
            } else {
                actions.push_back(actionValue(FileAction::Download));
            }
        }
        task.fileSelection = std::move(actions);
    }
    metainfo_free(&metainfo);
}


DownloadStatus persistedStatus(const std::string& value) {
    if (value == "paused")
        return DownloadStatus::Paused;
    if (value == "completed")
        return DownloadStatus::Completed;
    if (value == "installed")
        return DownloadStatus::Installed;
    if (value == "error")
        return DownloadStatus::Error;
    return DownloadStatus::Queued;
}

std::string persistedStatus(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Paused: return "paused";
        case DownloadStatus::Completed: return "completed";
        case DownloadStatus::Installed: return "installed";
        case DownloadStatus::Error: return "error";
        default: return "queued";
    }
}

TransferMode persistedMode(const std::string& value) {
    return value == "install" ? TransferMode::StreamInstall
                              : TransferMode::DownloadOnly;
}

const char* persistedMode(TransferMode mode) {
    return mode == TransferMode::StreamInstall ? "install" : "download";
}

const char* persistedSource(TaskSource source) {
    return source == TaskSource::Debrid ? "debrid" : "torrent";
}

TaskSource persistedSource(const std::string& value) {
    return (value == "debrid" || value == "torbox")
           ? TaskSource::Debrid : TaskSource::Torrent;
}

} // namespace

void updateTaskDownloadProgress(DownloadTask& task, uint64_t completedBytes,
                                uint64_t progressAtMs) {
    // During startup/final verification the engine reports completed_bytes
    // growing from zero. Keep the high-water mark so a pause/resume does not
    // flash the UI back to 0% while pieces are re-found on disk.
    if ((task.status == DownloadStatus::Checking ||
         task.status == DownloadStatus::Verifying) &&
        completedBytes < task.completedBytes) {
        task.downloadProgressUpdatedAtMs = 0;
        return;
    }
    if (completedBytes < task.completedBytes) {
        task.downloadProgressUpdatedAtMs = 0;
    } else if (progressAtMs >= task.downloadProgressUpdatedAtMs) {
        task.downloadProgressUpdatedAtMs = progressAtMs;
    }
    task.completedBytes = completedBytes;
}

void updateTaskInstallProgress(DownloadTask& task, uint64_t installedBytes,
                               uint64_t installTotalBytes,
                               DownloadStatus status, uint64_t nowMs) {
    auto resetRate = [&task] {
        task.installSpeedBytesPerSecond = 0;
        task.installSpeedUpdatedAtMs = 0;
        task.installRateBaseBytes = 0;
        task.installRateBaseAtMs = 0;
    };
    const bool continuing = task.status == DownloadStatus::Installing &&
                            status == DownloadStatus::Installing &&
                            installTotalBytes > installedBytes &&
                            installedBytes >= task.installedBytes;
    if (!continuing)
        resetRate();

    task.status = status;
    task.installedBytes = installedBytes;
    task.installTotalBytes = installTotalBytes;

    if (status != DownloadStatus::Installing || !installTotalBytes ||
        installedBytes >= installTotalBytes) {
        resetRate();
        return;
    }

    if (!task.installRateBaseAtMs ||
        installedBytes < task.installRateBaseBytes) {
        task.installRateBaseBytes = installedBytes;
        task.installRateBaseAtMs = nowMs;
        return;
    }

    if (nowMs <= task.installRateBaseAtMs ||
        nowMs - task.installRateBaseAtMs < kInstallRateWindowMs ||
        installedBytes == task.installRateBaseBytes)
        return;

    const uint64_t elapsed = nowMs - task.installRateBaseAtMs;
    const uint64_t sample =
        (installedBytes - task.installRateBaseBytes) * 1000 / elapsed;
    if (sample) {
        if (task.installSpeedBytesPerSecond) {
            const uint64_t previous = task.installSpeedBytesPerSecond;
            task.installSpeedBytesPerSecond = sample >= previous
                ? previous + (sample - previous) * 3 / 10
                : previous - (previous - sample) * 3 / 10;
        } else {
            task.installSpeedBytesPerSecond = sample;
        }
        task.installSpeedUpdatedAtMs = nowMs;
    }
    task.installRateBaseBytes = installedBytes;
    task.installRateBaseAtMs = nowMs;
}

uint64_t currentInstallSpeed(const DownloadTask& task, uint64_t nowMs) {
    if (task.status != DownloadStatus::Installing ||
        !task.installSpeedBytesPerSecond || !task.installSpeedUpdatedAtMs ||
        nowMs < task.installSpeedUpdatedAtMs ||
        nowMs - task.installSpeedUpdatedAtMs > kProgressRateStaleMs)
        return 0;
    return task.installSpeedBytesPerSecond;
}

std::optional<uint64_t> taskEtaSeconds(const DownloadTask& task,
                                       uint64_t nowMs) {
    uint64_t completed = 0;
    uint64_t total = 0;
    uint64_t speed = 0;
    if (task.status == DownloadStatus::Downloading) {
        const auto wanted = downloadProgressBytes(task);
        completed = wanted.first;
        total = wanted.second;
        speed = task.speedBytesPerSecond;
        if (!task.downloadProgressUpdatedAtMs ||
            nowMs < task.downloadProgressUpdatedAtMs ||
            nowMs - task.downloadProgressUpdatedAtMs > kProgressRateStaleMs)
            return std::nullopt;
    } else if (task.status == DownloadStatus::Installing) {
        completed = task.installedBytes;
        total = task.installTotalBytes;
        speed = currentInstallSpeed(task, nowMs);
    } else {
        return std::nullopt;
    }
    if (!total || completed >= total || !speed)
        return std::nullopt;
    const uint64_t remaining = total - completed;
    return remaining / speed + (remaining % speed != 0);
}

const char* statusName(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Queued: return "Queued";
        case DownloadStatus::Checking: return "Checking";
        case DownloadStatus::Fetching: return "Fetching on debrid service";
        case DownloadStatus::Downloading: return "Downloading";
        case DownloadStatus::Paused: return "Paused";
        case DownloadStatus::Verifying: return "Verifying";
        case DownloadStatus::Completed: return "Completed";
        case DownloadStatus::Installing: return "Installing";
        case DownloadStatus::Committing: return "Committing";
        case DownloadStatus::Installed: return "Installed";
        case DownloadStatus::Error: return "Error";
        case DownloadStatus::Removing: return "Removing";
    }
    return "Unknown";
}

DownloadManager::DownloadManager(std::string rootPath, bool startWorker)
    : rootPath_(std::move(rootPath)),
      torrentRoot_(rootPath_ + "/torrents"),
      downloadRoot_(rootPath_ + "/downloads"),
      statePath_(rootPath_ + "/queue.bencode") {
    makeDirectories(rootPath_);
    makeDirectories(torrentRoot_);
    makeDirectories(downloadRoot_);
    // B-dismissed update-file choosers leave orphaned resolve temp files.
    sweepTempTorrents(rootPath_, "_update_tmp_");
    load();
    if (startWorker) {
        workerStarted_ = true;
        worker_ = std::thread(&DownloadManager::schedulerMain, this);
    }
}

DownloadManager::~DownloadManager() {
    shutdown();
}

DownloadManager::ExternalDeployLease::ExternalDeployLease(
    ExternalDeployLease&& other) noexcept
    : owner_(other.owner_), task_(std::move(other.task_)) {
    other.owner_ = nullptr;
}

DownloadManager::ExternalDeployLease&
DownloadManager::ExternalDeployLease::operator=(
    ExternalDeployLease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        task_ = std::move(other.task_);
        other.owner_ = nullptr;
    }
    return *this;
}

DownloadManager::ExternalDeployLease::~ExternalDeployLease() { release(); }

void DownloadManager::ExternalDeployLease::release() {
    if (!owner_)
        return;
    owner_->endExternalDeploy(task_.id);
    owner_ = nullptr;
}

bool DownloadManager::previewTorrent(const std::string& path,
                                     TorrentPreview& preview,
                                     std::string& error) {
    metainfo_t metainfo;
    if (!metainfo_load(path.c_str(), &metainfo)) {
        error = "The selected file is not a valid or safe .torrent file.";
        return false;
    }
    char hash[41];
    hex20(hash, metainfo.info_hash);
    preview.name = metainfo.name;
    preview.infoHash = hash;
    preview.multi = metainfo.is_multi != 0;
    preview.totalBytes = static_cast<uint64_t>(metainfo.total_length);
    preview.fileCount = metainfo.num_files;
    preview.trackerCount = metainfo.num_trackers;
    preview.pieceCount = metainfo.num_pieces;
    preview.files.reserve(metainfo.num_files);
    for (uint32_t i = 0; i < metainfo.num_files; ++i) {
        TorrentPreview::File file;
        file.path = metainfo.files[i].path;
        file.length = static_cast<uint64_t>(metainfo.files[i].length);
        file.package = isPackageName(metainfo.files[i].path);
        file.compressed = isCompressedName(metainfo.files[i].path);
        if (file.package)
            ++preview.packageCount;
        file.cartridge = isCartridgeName(metainfo.files[i].path);
        if (file.cartridge)
            ++preview.cartridgeCount;
        preview.files.push_back(std::move(file));
    }
    metainfo_free(&metainfo);
    return true;
}

bool DownloadManager::importTorrent(const std::string& path,
                                    TransferMode mode,
                                    const std::vector<uint8_t>& selectedFiles,
                                    std::string& taskId,
                                    std::string& error,
                                    const std::vector<uint8_t>& initialPeers) {
    TorrentPreview preview;
    if (!previewTorrent(path, preview, error))
        return false;
    if (!selectedFiles.empty() && selectedFiles.size() != preview.files.size()) {
        error = "Selected file list does not match torrent contents.";
        return false;
    }
    return importTorrentActions(path,
                                actionsFromLegacySelection(preview, mode,
                                                           selectedFiles),
                                taskId, error, initialPeers);
}

bool DownloadManager::importTorrentActions(
                                    const std::string& path,
                                    const std::vector<uint8_t>& fileActions,
                                    std::string& taskId,
                                    std::string& error,
                                    const std::vector<uint8_t>& initialPeers) {
    TorrentPreview preview;
    if (!previewTorrent(path, preview, error))
        return false;
    if (!fileActions.empty() && fileActions.size() != preview.files.size()) {
        error = "Selected file actions do not match torrent contents.";
        return false;
    }
    if (initialPeers.size() % 6 != 0) {
        error = "Initial peer endpoint list is malformed.";
        return false;
    }

    std::vector<uint8_t> selection = fileActions;
    bool useSelection = !selection.empty();
    uint32_t installPackageCount = 0;
    bool hasSelectedFiles = false;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        uint8_t action = useSelection
            ? selection[i]
            : actionValue(FileAction::Download);
        if (!isValidFileAction(action)) {
            error = "Selected file action is invalid.";
            return false;
        }
        if (action != actionValue(FileAction::Skip))
            hasSelectedFiles = true;
        if (action == actionValue(FileAction::Install)) {
            if (!preview.files[i].package) {
                error = "Only NSP/NSZ package files can be installed.";
                return false;
            }
            ++installPackageCount;
        }
    }
    if (!hasSelectedFiles) {
        error = "Select at least one file.";
        return false;
    }
    TransferMode mode = installPackageCount > 0
        ? TransferMode::StreamInstall
        : TransferMode::DownloadOnly;

    std::lock_guard<std::mutex> lock(mutex_);
    if (findLocked(preview.infoHash)) {
        error = "This torrent is already in the download manager.";
        return false;
    }

    std::string metainfoPath = torrentRoot_ + "/" + preview.infoHash + ".torrent";
    std::string dataPath = downloadRoot_ + "/" + safeComponent(preview.name) +
                           "-" + preview.infoHash.substr(0, 8);
    if (!copyFile(path, metainfoPath)) {
        error = "Unable to copy the torrent file into application storage.";
        return false;
    }
    if (!makeDirectories(dataPath)) {
        unlink(metainfoPath.c_str());
        error = "Unable to create the download directory.";
        return false;
    }

    DownloadTask task;
    task.id = preview.infoHash;
    task.name = preview.name;
    task.metainfoPath = metainfoPath;
    task.dataPath = dataPath;
    task.totalBytes = preview.totalBytes;
    task.status = DownloadStatus::Queued;
    task.mode = mode;
    task.packageCount = installPackageCount;
    task.fileSelection = std::move(selection);
    task.initialPeers = initialPeers;
    // Fast resume: a genuinely fresh download has nothing on disk to find,
    // so an all-zero trusted bitfield lets the engine skip hashing the
    // preallocated files. A re-import over kept data (same deterministic
    // dataPath) stays untrusted so the full scan reclaims existing pieces.
    // Skipped files break the shortcut: the trusted bitfield skips the
    // startup scan that pre-marks their pieces done (storage_range_skipped),
    // so the engine would download the whole torrent and discard everything
    // but the selection.
    bool hasSkipped = false;
    for (const uint8_t action : task.fileSelection)
        hasSkipped |= action == actionValue(FileAction::Skip);
    if (preview.pieceCount > 0 && directoryEmpty(dataPath) && !hasSkipped)
        task.resumeBitfield.assign((preview.pieceCount + 7) / 8, 0);
    tasks_.push_back(std::move(task));
    taskId = preview.infoHash;

    if (!saveLocked(error)) {
        tasks_.pop_back();
        unlink(metainfoPath.c_str());
        removeTree(dataPath);
        return false;
    }
    std::string manifestError;
    if (!saveTaskFileManifest(
            rootPath_, makeTaskFileManifest(taskId, preview,
                                            tasks_.back().fileSelection),
            manifestError)) {
        diagnostic_error("task_files", "save", "task=%s error=%s",
                         taskId.c_str(), manifestError.c_str());
    }
    condition_.notify_all();
    return true;
}

bool DownloadManager::importDebrid(const DebridImport& import,
                                    std::string& taskId, std::string& error) {
    if (import.infoHash.size() != 40) {
        error = "Invalid torrent hash for the debrid task.";
        return false;
    }
    if (!import.fileSelection.empty()) {
        bool hasSelectedFile = false;
        for (uint8_t action : import.fileSelection) {
            if (!isValidFileAction(action)) {
                error = "Selected file action is invalid.";
                return false;
            }
            hasSelectedFile |= action != actionValue(FileAction::Skip);
        }
        if (!hasSelectedFile) {
            error = "Select at least one file.";
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (findLocked(import.infoHash)) {
        error = "This torrent is already in the download manager.";
        return false;
    }

    std::string metainfoPath;
    if (!import.torrentPath.empty()) {
        metainfoPath = torrentRoot_ + "/" + import.infoHash + ".torrent";
        if (!copyFile(import.torrentPath, metainfoPath)) {
            error = "Unable to copy the torrent file into application storage.";
            return false;
        }
    }
    std::string dataPath = downloadRoot_ + "/" + safeComponent(import.name) +
                           "-" + import.infoHash.substr(0, 8);
    if (!makeDirectories(dataPath)) {
        if (!metainfoPath.empty())
            unlink(metainfoPath.c_str());
        error = "Unable to create the download directory.";
        return false;
    }

    DownloadTask task;
    task.id = import.infoHash;
    task.name = import.name;
    task.metainfoPath = metainfoPath;
    task.dataPath = dataPath;
    task.totalBytes = import.totalBytes;
    task.status = DownloadStatus::Queued;
    task.mode = import.mode;
    task.source = TaskSource::Debrid;
    task.debridProvider = import.provider;
    task.debridId = import.debridId;
    task.packageCount = import.mode == TransferMode::StreamInstall
                        ? import.packageCount : 0;
    task.fileSelection = import.fileSelection;
    tasks_.push_back(std::move(task));
    taskId = import.infoHash;

    if (!saveLocked(error)) {
        tasks_.pop_back();
        if (!metainfoPath.empty())
            unlink(metainfoPath.c_str());
        removeTree(dataPath);
        return false;
    }
    if (!tasks_.back().metainfoPath.empty()) {
        TorrentPreview preview;
        std::string previewError;
        if (previewTorrent(tasks_.back().metainfoPath, preview, previewError)) {
            std::vector<uint8_t> actions = tasks_.back().fileSelection;
            if (actions.empty()) {
                actions.reserve(preview.files.size());
                for (const TorrentPreview::File& file : preview.files) {
                    actions.push_back(static_cast<uint8_t>(
                        import.mode == TransferMode::StreamInstall &&
                                file.package
                            ? FileAction::Install : FileAction::Download));
                }
            }
            std::string manifestError;
            if (!saveTaskFileManifest(
                    rootPath_, makeTaskFileManifest(taskId, preview, actions),
                    manifestError)) {
                diagnostic_error("task_files", "save", "task=%s error=%s",
                                 taskId.c_str(), manifestError.c_str());
            }
        }
    }
    condition_.notify_all();
    return true;
}

void DownloadManager::setTorboxApiKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    torboxApiKey_ = key;
}

void DownloadManager::setTorrserverUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    torrserverUrl_ = url;
}

void DownloadManager::setRealdebridApiKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    realdebridApiKey_ = key;
}

std::string DownloadManager::apiKeyFor(DebridProviderKind provider) const {
    if (provider == DebridProviderKind::TorrServer)
        return torrserverUrl_;
    if (provider == DebridProviderKind::RealDebrid)
        return realdebridApiKey_;
    return torboxApiKey_;
}

std::unique_ptr<DebridProvider> DownloadManager::makeProvider(
    DebridProviderKind provider, const std::string& key) {
    if (provider == DebridProviderKind::TorrServer)
        return std::make_unique<TorrserverProvider>(key);
    if (provider == DebridProviderKind::RealDebrid)
        return std::make_unique<RealdebridProvider>(key);
    return std::make_unique<TorboxProvider>(key);
}

// Best-effort: a transfer we drop locally should not sit on the account
// burning the user's quota. Detached because it is one HTTPS round-trip we
// never want a caller — least of all one holding mutex_ — to wait on.
void DownloadManager::removeFromDebridAsync(DebridProviderKind provider,
                                            const std::string& apiKey,
                                            const std::string& debridId) {
    if (apiKey.empty() || debridId.empty())
        return;
    std::thread([provider, apiKey, debridId] {
        std::string error;
        if (!makeProvider(provider, apiKey)->remove(debridId, error))
            log_msg("[debrid] account cleanup failed id=%s: %s\n",
                    debridId.c_str(), error.c_str());
    }).detach();
}

std::string DownloadManager::torboxApiKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return torboxApiKey_;
}

void DownloadManager::setTorrentingEnabled(bool enabled) {
    torrentingEnabled_.store(enabled);
    condition_.notify_all();
}

bool DownloadManager::torrentingEnabled() const {
    return torrentingEnabled_.load();
}

bool DownloadManager::pause(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externallyLeasedLocked(taskId))
        return false;
    DownloadTask* task = findLocked(taskId);
    if (!task)
        return false;
    if (task->status != DownloadStatus::Queued &&
        task->status != DownloadStatus::Checking &&
        task->status != DownloadStatus::Fetching &&
        task->status != DownloadStatus::Downloading &&
        task->status != DownloadStatus::Installing &&
        task->status != DownloadStatus::Committing &&
        task->status != DownloadStatus::Verifying)
        return false;
    task->status = DownloadStatus::Paused;
    task->speedBytesPerSecond = 0;
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::resume(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externallyLeasedLocked(taskId))
        return false;
    DownloadTask* task = findLocked(taskId);
    if (!task || (task->status != DownloadStatus::Paused &&
                  task->status != DownloadStatus::Error))
        return false;
    task->status = DownloadStatus::Queued;
    task->error.clear();
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::retry(const std::string& taskId) {
    return resume(taskId);
}

bool DownloadManager::verify(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externallyLeasedLocked(taskId))
        return false;
    DownloadTask* task = findLocked(taskId);
    if (!task || task->status != DownloadStatus::Completed)
        return false;
    // A recheck rehashes against the local pieces. A debrid task has no
    // pieces — requeueing it would silently re-download the whole thing from
    // the provider, so there is nothing honest to offer here.
    if (task->source == TaskSource::Debrid)
        return false;
    task->status = DownloadStatus::Queued;
    task->error.clear();
    task->piecesVerified = 0;
    task->resumeBitfield.clear();  // a recheck must really rehash
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::moveToFront(const std::string& taskId,
                                  std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externallyLeasedLocked(taskId)) {
        error = "Task files are being copied to /switch.";
        return false;
    }
    auto target = std::find_if(tasks_.begin(), tasks_.end(),
                               [&taskId](const DownloadTask& task) {
        return task.id == taskId;
    });
    if (target == tasks_.end()) {
        error = "Download task not found.";
        return false;
    }
    if (target->status != DownloadStatus::Queued) {
        error = "Only a queued download can be moved.";
        return false;
    }
    auto firstQueued = std::find_if(tasks_.begin(), tasks_.end(),
                                    [](const DownloadTask& task) {
        return task.status == DownloadStatus::Queued;
    });
    if (firstQueued == target)
        return true; // already next up
    // Rotate rather than swap: everything between the two keeps its relative
    // order, so promoting one download does not shuffle the rest of the queue.
    std::rotate(firstQueued, target, target + 1);
    return saveLocked(error);
}

bool DownloadManager::remove(const std::string& taskId, bool deleteData,
                             std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externallyLeasedLocked(taskId)) {
        error = "Task files are being copied to /switch.";
        return false;
    }
    DownloadTask* task = findLocked(taskId);
    if (!task) {
        error = "Download task not found.";
        return false;
    }

    if (task->status == DownloadStatus::Checking ||
        task->status == DownloadStatus::Fetching ||
        task->status == DownloadStatus::Downloading ||
        task->status == DownloadStatus::Installing ||
        task->status == DownloadStatus::Committing ||
        task->status == DownloadStatus::Verifying) {
        task->status = DownloadStatus::Removing;
        task->error = deleteData ? "delete-data" : "keep-data";
        condition_.notify_all();
        return true;
    }
    std::string debridId = task->debridId;
    DebridProviderKind provider = task->debridProvider;
    std::string apiKey = apiKeyFor(provider);
    if (!removeLocked(taskId, deleteData, error))
        return false;
    removeFromDebridAsync(provider, apiKey, debridId);
    return true;
}

std::vector<DownloadTask> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_;
}

std::optional<DownloadTask> DownloadManager::snapshot(
    const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const DownloadTask* task = findLocked(id);
    return task ? std::optional<DownloadTask>(*task) : std::nullopt;
}

std::optional<DownloadManager::ExternalDeployLease>
DownloadManager::beginExternalDeploy(const std::string& taskId,
                                     std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        error = "The download manager is shutting down.";
        return std::nullopt;
    }
    if (!externalDeployTaskId_.empty()) {
        error = "Another /switch copy is already active.";
        return std::nullopt;
    }
    if (installTokenHeld_) {
        error = "A package installation is active.";
        return std::nullopt;
    }
    DownloadTask* task = findLocked(taskId);
    if (!task) {
        error = "Download task not found.";
        return std::nullopt;
    }
    if (!taskReadyForSwitchDeploy(*task)) {
        error = "Finish the download before copying files to /switch.";
        return std::nullopt;
    }
    for (const auto& runner : runners_) {
        if (runner && runner->taskId == taskId) {
            error = "The download is still closing its files.";
            return std::nullopt;
        }
    }
    externalDeployTaskId_ = taskId;
    return ExternalDeployLease(this, *task);
}

bool DownloadManager::externalDeployActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !externalDeployTaskId_.empty();
}

std::string DownloadManager::externalDeployTaskId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return externalDeployTaskId_;
}

bool DownloadManager::hasTask(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const DownloadTask& task : tasks_)
        if (task.id == id)
            return true;
    return false;
}

bool DownloadManager::hasActiveTransfer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const DownloadTask& task : tasks_) {
        switch (task.status) {
            case DownloadStatus::Queued:
            case DownloadStatus::Checking:
            case DownloadStatus::Fetching:
            case DownloadStatus::Downloading:
            case DownloadStatus::Verifying:
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:
                return true;
            case DownloadStatus::Paused:
            case DownloadStatus::Completed:
            case DownloadStatus::Installed:
            case DownloadStatus::Error:
            case DownloadStatus::Removing:
                break;
        }
    }
    return false;
}

bool DownloadManager::save(std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return saveLocked(error);
}

bool DownloadManager::saveLocked(std::string& error) const {
    std::ostringstream state;
    state << "d5:tasks";
    state << "l";
    for (const DownloadTask& task : tasks_) {
        if (task.status == DownloadStatus::Removing)
            continue;
        // Bencode dict keys must stay in lexicographic order.
        state << "d";
        state << "9:completed" << bint(task.completedBytes);
        state << "4:data" << bstr(task.dataPath);
        state << "9:debrid-id" << bstr(task.debridId);
        state << "5:error" << bstr(task.error);
        state << "2:id" << bstr(task.id);
        state << "8:metainfo" << bstr(task.metainfoPath);
        state << "4:mode" << bstr(persistedMode(task.mode));
        state << "4:name" << bstr(task.name);
        state << "13:package-count" << bint(task.packageCount);
        state << "13:packages-done" << bint(task.packagesInstalled);
        state << "11:pieces-done" << bint(task.piecesDone);
        state << "12:pieces-total" << bint(task.piecesTotal);
        state << "8:provider" << bstr(
            task.debridProvider == DebridProviderKind::TorrServer
                ? "torrserver"
                : task.debridProvider == DebridProviderKind::RealDebrid
                ? "realdebrid"
                : "torbox");
        if (!task.resumeBitfield.empty())
            state << "9:resume-bf"
                  << bstr(std::string(task.resumeBitfield.begin(),
                                      task.resumeBitfield.end()));
        state << "9:selection" << bstr(std::string(task.fileSelection.begin(),
                                                   task.fileSelection.end()));
        state << "6:source" << bstr(persistedSource(task.source));
        state << "6:status" << bstr(persistedStatus(task.status));
        state << "5:total" << bint(task.totalBytes);
        state << "16:wanted-completed" << bint(task.wantedCompletedBytes);
        state << "12:wanted-total" << bint(task.wantedTotalBytes);
        state << "e";
    }
    state << "e";
    state << "7:versioni6e";
    state << "e";

    std::string temporary = statePath_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open queue state for writing.";
            return false;
        }
        output << state.str();
        output.flush();
        if (!output.good()) {
            error = "Unable to write queue state.";
            return false;
        }
    }
    if (rename(temporary.c_str(), statePath_.c_str()) != 0) {
        int renameErrno = errno;
        if (renameErrno != EEXIST)
            log_msg("[manager] queue state rename failed: %s\n",
                    std::strerror(renameErrno));
        if (unlink(statePath_.c_str()) != 0 && errno != ENOENT) {
            int unlinkErrno = errno;
            unlink(temporary.c_str());
            error = std::string("Unable to remove old queue state: ") +
                    std::strerror(unlinkErrno);
            return false;
        }
        if (rename(temporary.c_str(), statePath_.c_str()) == 0)
            return true;
        int replaceErrno = errno;
        unlink(temporary.c_str());
        error = std::string("Unable to replace queue state: ") +
                std::strerror(replaceErrno);
        return false;
    }
    return true;
}

void DownloadManager::load() {
    std::ifstream input(statePath_, std::ios::binary);
    if (!input)
        return;
    std::string data((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const char* cursor = data.data();
    const char* end = cursor + data.size();
    be_node_t root;
    if (!be_decode(&cursor, end, &root) || root.type != BE_DICT)
        return;

    be_node_t version;
    if (!be_dict_get(root.buf, root.buf + root.raw_len, "version", 7,
                     &version) ||
        version.type != BE_INT ||
        (version.ival != 1 && version.ival != 2 && version.ival != 3 &&
         version.ival != 4 && version.ival != 5 && version.ival != 6))
        return;

    be_node_t list;
    if (!be_dict_get(root.buf, root.buf + root.raw_len, "tasks", 5, &list) ||
        list.type != BE_LIST)
        return;

    const char* itemCursor = list.buf + 1;
    const char* itemEnd = list.buf + list.raw_len - 1;
    be_node_t item;
    while (be_list_next(&itemCursor, itemEnd, &item)) {
        if (item.type != BE_DICT)
            continue;
        DownloadTask task;
        std::string status;
        if (!dictionaryString(item, "id", task.id) ||
            !dictionaryString(item, "name", task.name) ||
            !dictionaryString(item, "metainfo", task.metainfoPath) ||
            !dictionaryString(item, "data", task.dataPath) ||
            !dictionaryString(item, "status", status) ||
            !dictionaryInteger(item, "total", task.totalBytes))
            continue;
        dictionaryString(item, "error", task.error);
        if (version.ival >= 2) {
            std::string mode;
            uint64_t packageCount = 0;
            uint64_t packagesDone = 0;
            if (dictionaryString(item, "mode", mode))
                task.mode = persistedMode(mode);
            if (dictionaryInteger(item, "package-count", packageCount))
                task.packageCount = static_cast<uint32_t>(packageCount);
            if (dictionaryInteger(item, "packages-done", packagesDone))
                task.packagesInstalled =
                    static_cast<uint32_t>(packagesDone);
        }
        if (version.ival >= 3) {
            std::string selection;
            if (dictionaryString(item, "selection", selection)) {
                task.fileSelection.assign(selection.begin(), selection.end());
            }
        }
        if (version.ival >= 4) {
            std::string source;
            uint64_t torboxId = 0;
            if (dictionaryString(item, "source", source))
                task.source = persistedSource(source);
            // v4 only ever spoke TorBox, and kept the transfer id as an int.
            if (dictionaryInteger(item, "torbox-id", torboxId)) {
                task.debridProvider = DebridProviderKind::TorBox;
                task.debridId = std::to_string(torboxId);
            }
        }
        // v5 gained two independent sets of keys — the resume bitfield and the
        // debrid provider/id. Each is optional, so a state file written by
        // either lineage loads with the other side's fields left at default.
        if (version.ival >= 5) {
            std::string bitfield;
            if (dictionaryString(item, "resume-bf", bitfield))
                task.resumeBitfield.assign(bitfield.begin(), bitfield.end());
            std::string provider;
            std::string debridId;
            if (dictionaryString(item, "provider", provider))
                task.debridProvider =
                    provider == "torrserver"
                        ? DebridProviderKind::TorrServer
                        : provider == "realdebrid"
                        ? DebridProviderKind::RealDebrid
                        : DebridProviderKind::TorBox;
            if (dictionaryString(item, "debrid-id", debridId))
                task.debridId = debridId;
        }
        // v6 persists the last reported download progress so a paused task
        // still shows its bar after an app restart (resume-bf alone is not
        // enough — the UI reads completed/wanted bytes, not the bitfield).
        if (version.ival >= 6) {
            uint64_t completed = 0;
            uint64_t wantedCompleted = 0;
            uint64_t wantedTotal = 0;
            uint64_t piecesDone = 0;
            uint64_t piecesTotal = 0;
            if (dictionaryInteger(item, "completed", completed))
                task.completedBytes = completed;
            if (dictionaryInteger(item, "wanted-completed", wantedCompleted))
                task.wantedCompletedBytes = wantedCompleted;
            if (dictionaryInteger(item, "wanted-total", wantedTotal))
                task.wantedTotalBytes = wantedTotal;
            if (dictionaryInteger(item, "pieces-done", piecesDone))
                task.piecesDone = static_cast<uint32_t>(piecesDone);
            if (dictionaryInteger(item, "pieces-total", piecesTotal))
                task.piecesTotal = static_cast<uint32_t>(piecesTotal);
        }
        task.status = persistedStatus(status);
        if (task.status == DownloadStatus::Completed ||
            task.status == DownloadStatus::Installed)
            task.completedBytes = task.totalBytes;
        bool metainfoRequired = task.source == TaskSource::Torrent ||
                                !task.metainfoPath.empty();
        if (!isManagedChild(downloadRoot_, task.dataPath) ||
            (metainfoRequired &&
             !isManagedChild(torrentRoot_, task.metainfoPath))) {
            task.status = DownloadStatus::Error;
            task.error = "The stored task contains an invalid path.";
        } else if (metainfoRequired &&
                   access(task.metainfoPath.c_str(), R_OK) != 0) {
            task.status = DownloadStatus::Error;
            task.error = "The stored .torrent file is missing.";
        }
        if (version.ival == 3 && task.status != DownloadStatus::Error)
            upgradeLegacySelection(task);
        tasks_.push_back(std::move(task));
    }
}

DownloadTask* DownloadManager::findLocked(const std::string& id) {
    for (DownloadTask& task : tasks_)
        if (task.id == id)
            return &task;
    return nullptr;
}

const DownloadTask* DownloadManager::findLocked(const std::string& id) const {
    for (const DownloadTask& task : tasks_)
        if (task.id == id)
            return &task;
    return nullptr;
}

bool DownloadManager::externallyLeasedLocked(const std::string& taskId) const {
    return externalDeployTaskId_ == taskId;
}

void DownloadManager::endExternalDeploy(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externalDeployTaskId_ != taskId)
        return;
    externalDeployTaskId_.clear();
    condition_.notify_all();
}

bool DownloadManager::removeLocked(const std::string& id, bool deleteData,
                                   std::string& error) {
    for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
        if (it->id != id)
            continue;
        if ((!it->metainfoPath.empty() &&
             !isManagedChild(torrentRoot_, it->metainfoPath)) ||
            !isManagedChild(downloadRoot_, it->dataPath)) {
            error = "Refusing to remove a path outside application storage.";
            it->status = DownloadStatus::Error;
            it->error = error;
            std::string ignored;
            saveLocked(ignored);
            return false;
        }
        if (deleteData && !removeTree(it->dataPath)) {
            error = "Unable to remove all downloaded data.";
            it->status = DownloadStatus::Error;
            it->error = error;
            std::string ignored;
            saveLocked(ignored);
            return false;
        }
        if (!it->metainfoPath.empty())
            unlink(it->metainfoPath.c_str());
        install::removeInstallJournal(installJournalPath(rootPath_, it->id));
        removeTaskFileManifest(rootPath_, it->id);
        tasks_.erase(it);
        return saveLocked(error);
    }
    error = "Download task not found.";
    return false;
}

bool taskClaimableUnderInstallToken(const DownloadTask& task,
                                    bool installTokenHeld) {
    if (task.status != DownloadStatus::Queued)
        return false;
    return !(task.mode == TransferMode::StreamInstall && installTokenHeld);
}

DownloadTask* DownloadManager::claimableLocked() {
    for (DownloadTask& task : tasks_) {
        if (!taskClaimableUnderInstallToken(
                task, installTokenHeld_ || !externalDeployTaskId_.empty()))
            continue;
        // A just-paused task can be resumed to Queued while its runner is
        // still tearing down. Claiming it again would start a second engine
        // on the same data directory.
        bool busy = false;
        for (const auto& runner : runners_) {
            if (runner && runner->taskId == task.id) {
                busy = true;
                break;
            }
        }
        if (!busy)
            return &task;
    }
    return nullptr;
}

// Move finished runners out of runners_ and join them with mutex_ released
// (a join can wait on engine teardown; holding the lock would stall the UI).
void DownloadManager::reapRunnersLocked(std::unique_lock<std::mutex>& lock) {
    std::vector<std::unique_ptr<RunnerSlot>> finished;
    for (auto it = runners_.begin(); it != runners_.end();) {
        if ((*it)->done) {
            finished.push_back(std::move(*it));
            it = runners_.erase(it);
        } else {
            ++it;
        }
    }
    if (finished.empty())
        return;
    lock.unlock();
    for (auto& runner : finished)
        if (runner->thread.joinable())
            runner->thread.join();
    lock.lock();
}

void DownloadManager::schedulerMain() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        condition_.wait(lock, [this] {
            if (stopping_)
                return true;
            for (const auto& runner : runners_)
                if (runner->done)
                    return true;
            return runners_.size() < maxActive_ &&
                   claimableLocked() != nullptr;
        });
        reapRunnersLocked(lock);
        if (stopping_)
            break;
        if (runners_.size() >= maxActive_)
            continue;
        DownloadTask* task = claimableLocked();
        if (!task)
            continue;

        task->status = DownloadStatus::Checking;
        ClaimedTask claim;
        claim.id = task->id;
        claim.name = task->name;
        claim.metainfoPath = task->metainfoPath;
        claim.dataPath = task->dataPath;
        claim.mode = task->mode;
        claim.source = task->source;
        claim.debridProvider = task->debridProvider;
        claim.debridId = task->debridId;
        claim.packagesInstalled = task->packagesInstalled;
        claim.fileSelection = task->fileSelection;
        claim.initialPeers = task->initialPeers;
        // Fast resume: consume the trusted bitfield and persist the disarmed
        // state before the engine touches anything — a crash from here on
        // must fall back to a full scan.
        claim.resumeBitfield = std::move(task->resumeBitfield);
        task->resumeBitfield.clear();
        std::string ignored;
        saveLocked(ignored);

        auto slot = std::make_unique<RunnerSlot>();
        slot->taskId = claim.id;
        slot->holdsInstallToken = claim.mode == TransferMode::StreamInstall;
        if (slot->holdsInstallToken)
            installTokenHeld_ = true;
        uint32_t slotIndex = 0;
        while (slotBitmap_ & (1u << slotIndex))
            ++slotIndex;
        slotBitmap_ |= 1u << slotIndex;
        slot->slotIndex = slotIndex;
        RunnerSlot* raw = slot.get();
        runners_.push_back(std::move(slot));
        raw->thread = std::thread(
            [this, raw, moved = std::move(claim)]() mutable {
                // Thread entry point: an exception that escapes here is an
                // instant std::terminate and takes the whole app with it,
                // losing the state file and the log along the way. A failed
                // task is a failed task — report it and let the slot unwind.
                const std::string id = moved.id;
                try {
                    runTask(raw, std::move(moved));
                } catch (const std::exception& e) {
                    log_msg("[manager] task %s threw %s: %s\n", id.c_str(),
                            typeid(e).name(), e.what());
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (DownloadTask* task = findLocked(id)) {
                        task->status = DownloadStatus::Error;
                        task->error = std::string("Internal error: ") + e.what();
                        task->speedBytesPerSecond = 0;
                        std::string ignored;
                        saveLocked(ignored);
                    }
                } catch (...) {
                    log_msg("[manager] task %s threw a non-std exception\n",
                            id.c_str());
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (DownloadTask* task = findLocked(id)) {
                        task->status = DownloadStatus::Error;
                        task->error = "Internal error during the transfer.";
                        task->speedBytesPerSecond = 0;
                        std::string ignored;
                        saveLocked(ignored);
                    }
                }
                std::lock_guard<std::mutex> guard(mutex_);
                if (raw->holdsInstallToken)
                    installTokenHeld_ = false;
                slotBitmap_ &= ~(1u << raw->slotIndex);
                raw->done = true;
                condition_.notify_all();
            });
    }
    // stopping_: runners break their tick loops on it; join them all.
    std::vector<std::unique_ptr<RunnerSlot>> remaining;
    remaining.swap(runners_);
    installTokenHeld_ = false;
    slotBitmap_ = 0;
    lock.unlock();
    for (auto& runner : remaining)
        if (runner->thread.joinable())
            runner->thread.join();
}

void DownloadManager::setMaxActiveDownloads(uint32_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxActive_ = clampMaxActiveDownloads(count);
    condition_.notify_all();
}

void DownloadManager::runDebridTask(const ClaimedTask& claim) {
    const std::string& activeId = claim.id;
    std::string apiKey;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        apiKey = apiKeyFor(claim.debridProvider);
    }
    if (apiKey.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (DownloadTask* task = findLocked(activeId)) {
            task->status = DownloadStatus::Error;
            task->error =
                claim.debridProvider == DebridProviderKind::TorrServer
                    ? "TorrServer address missing — set it in Settings."
                    : claim.debridProvider == DebridProviderKind::RealDebrid
                    ? "Real-Debrid key missing — link your account in Settings."
                    : "TorBox key missing — link your account in Settings.";
            std::string ignored;
            saveLocked(ignored);
        }
        return;
    }

    DebridTaskSpec spec;
    spec.taskId = activeId;
    spec.debridId = claim.debridId;
    spec.torrentPath = claim.metainfoPath;
    spec.dataPath = claim.dataPath;
    spec.workingRoot = rootPath_;
    spec.installTarget = installTarget_.load(std::memory_order_relaxed);
    spec.mode = claim.mode;
    spec.fileSelection = claim.fileSelection;
    spec.packagesInstalled = claim.packagesInstalled;
    spec.filesResolved = [this, id = activeId](
                             const std::vector<DebridTaskSpec::ResolvedFile>&
                                 resolved) {
        TaskFileManifest manifest;
        manifest.taskId = id;
        manifest.files.reserve(resolved.size());
        for (const DebridTaskSpec::ResolvedFile& source : resolved) {
            TaskFileRecord file;
            file.logicalPath = source.path;
            file.localPath = source.localPath;
            file.size = source.bytes;
            file.action = source.action <=
                                  static_cast<uint8_t>(TaskFileAction::Install)
                ? static_cast<TaskFileAction>(source.action)
                : TaskFileAction::Skip;
            file.package = isPackageName(source.path);
            file.compressed = isCompressedName(source.path);
            file.cartridge = isCartridgeName(source.path);
            manifest.files.push_back(std::move(file));
        }
        std::string manifestError;
        if (!saveTaskFileManifest(rootPath_, manifest, manifestError)) {
            diagnostic_error("task_files", "save_debrid",
                             "task=%s error=%s", id.c_str(),
                             manifestError.c_str());
        }
    };

    // The provider takes a magnet, not our .torrent, and it names files its
    // own way — so the selection travels as (basename, size) pairs rather
    // than as the metainfo's index mask.
    metainfo_t metainfo;
    if (claim.metainfoPath.empty() ||
        !metainfo_load(claim.metainfoPath.c_str(), &metainfo)) {
        spec.magnet = "magnet:?xt=urn:btih:" + activeId;
    } else {
        char hex[41];
        for (int i = 0; i < 20; ++i)
            std::snprintf(hex + i * 2, 3, "%02x", metainfo.info_hash[i]);
        std::vector<std::string> trackers;
        for (uint32_t i = 0; i < metainfo.num_trackers; ++i)
            trackers.emplace_back(metainfo.trackers[i]);
        spec.magnet = buildRichMagnet(hex, metainfo.name, trackers);
        for (uint32_t i = 0; i < metainfo.num_files; ++i) {
            if (i >= claim.fileSelection.size() || !claim.fileSelection[i])
                continue;
            std::string path = metainfo.files[i].path;
            size_t slash = path.find_last_of('/');
            spec.selectionPaths.emplace_back(
                slash == std::string::npos ? path : path.substr(slash + 1),
                static_cast<uint64_t>(metainfo.files[i].length));
        }
        metainfo_free(&metainfo);
    }

    auto shouldStop = [this, &activeId] {
        if (stopping_)
            return true;
        std::lock_guard<std::mutex> lock(mutex_);
        const DownloadTask* task = findLocked(activeId);
        return !task || task->status == DownloadStatus::Paused ||
               task->status == DownloadStatus::Removing;
    };
    auto onProgress = [this, &activeId](const DebridProgress& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        DownloadTask* task = findLocked(activeId);
        if (!task || task->status == DownloadStatus::Removing ||
            task->status == DownloadStatus::Paused)
            return;
        bool packageCommitted = p.packagesInstalled != task->packagesInstalled;
        updateTaskDownloadProgress(*task, p.completedBytes, now_ms());
        if (p.totalBytes)
            task->totalBytes = p.totalBytes;
        task->speedBytesPerSecond = p.speedBytesPerSecond;
        task->fetchProgress = p.fetchProgress;
        task->packagesInstalled = p.packagesInstalled;
        task->currentPackage = p.currentPackage;
        updateTaskInstallProgress(*task, p.installedBytes,
                                  p.installTotalBytes, p.status, now_ms());
        // Only package boundaries hit the state file; the per-chunk progress
        // above is in-memory until then.
        if (packageCommitted) {
            std::string ignored;
            saveLocked(ignored);
        }
    };

    auto provider = makeProvider(claim.debridProvider, apiKey);
    DebridTransfer transfer(*provider);
    std::string createdId;
    std::string runError;
    log_msg("[manager] starting debrid transfer \"%s\"%s%s\n",
            claim.name.c_str(),
            spec.mode == TransferMode::StreamInstall ? " (install)" : "",
            !spec.debridId.empty() ? " (catalog)" : "");
    DebridRunResult result =
        transfer.run(spec, shouldStop, onProgress, createdId, runError);
    log_msg("[debrid] transfer %s %s%s%s\n", activeId.c_str(),
            result == DebridRunResult::Finished ? "finished"
                : result == DebridRunResult::Stopped ? "stopped" : "FAILED",
            runError.empty() ? "" : ": ", runError.c_str());

    // Decided under the lock, acted on outside it: the account cleanup below
    // is a full HTTPS round trip, and holding mutex_ across it freezes every
    // snapshot() the UI makes.
    std::string removeId;
    DebridProviderKind removeProvider = claim.debridProvider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DownloadTask* task = findLocked(activeId);
        if (!task)
            return;
        // The transfer may have created the remote id we never had; record
        // it before anything else so the removal can still clean the account.
        if (!createdId.empty() && task->debridId.empty())
            task->debridId = createdId;
        if (task->status == DownloadStatus::Removing) {
            bool deleteData = task->error == "delete-data";
            removeId = task->debridId;
            removeProvider = task->debridProvider;
            std::string removeError;
            removeLocked(activeId, deleteData, removeError);
        } else {
            if (result == DebridRunResult::Failed) {
                task->status = DownloadStatus::Error;
                task->error = runError;
            }
            task->speedBytesPerSecond = 0;
            std::string ignored;
            saveLocked(ignored);
            return;
        }
    }
    // This runner thread is about to end anyway, so the cleanup rides it out
    // instead of spawning a detached thread that would outlive the manager.
    if (!removeId.empty() && !apiKey.empty()) {
        std::string error;
        if (!makeProvider(removeProvider, apiKey)->remove(removeId, error))
            log_msg("[debrid] account cleanup failed id=%s: %s\n",
                    removeId.c_str(), error.c_str());
        else
            log_msg("[debrid] account cleanup done id=%s\n", removeId.c_str());
    }
    log_msg("[debrid] runner finished for %s\n", activeId.c_str());
}

void DownloadManager::runTask(RunnerSlot* slot, ClaimedTask claim) {
    // Both branches below happen before the arbiter registration: a debrid
    // task runs no engine, so reserving engine RAM for it would starve the
    // torrent slots for nothing.
    if (claim.source == TaskSource::Torrent && !torrentingEnabled_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (DownloadTask* task = findLocked(claim.id)) {
            task->status = DownloadStatus::Error;
            task->error =
                "Torrenting disabled — enable it in Settings to retry.";
            task->speedBytesPerSecond = 0;
            std::string ignored;
            saveLocked(ignored);
        }
        return;
    }
    if (claim.source == TaskSource::Debrid) {
        runDebridTask(claim);
        return;
    }

    // Register this slot's engine overhead with the budget arbiter for the
    // whole task lifetime, error paths included.
    arbiter_.engineSlotStarted();
    struct EngineSlotGuard {
        StreamBudgetArbiter& arbiter;
        ~EngineSlotGuard() { arbiter.engineSlotFinished(); }
    } slotGuard{arbiter_};

    const std::string activeId = claim.id;
    const std::string dataPath = claim.dataPath;
    const TransferMode mode = claim.mode;
    const uint32_t packagesInstalled = claim.packagesInstalled;
    const std::vector<uint8_t>& fileSelection = claim.fileSelection;
    const std::vector<uint8_t>& initialPeers = claim.initialPeers;
    const std::vector<uint8_t>& resumeBitfield = claim.resumeBitfield;

    metainfo_t metainfo;
    if (!metainfo_load(claim.metainfoPath.c_str(), &metainfo)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (DownloadTask* task = findLocked(activeId)) {
            task->status = DownloadStatus::Error;
            task->error = "Unable to read the stored .torrent file.";
            std::string ignored;
            saveLocked(ignored);
        }
        return;
    }

    std::unique_ptr<PackageCoordinator> coordinator;
    torrent_options_t options {};
    {
        coordinator = std::make_unique<PackageCoordinator>(
            metainfo, activeId, rootPath_,
            mode == TransferMode::StreamInstall, fileSelection,
            packagesInstalled,
            installTarget_.load(std::memory_order_relaxed),
            arbiter_,
            [this, activeId](uint32_t completed,
                             const std::string& package,
                             uint64_t installed, uint64_t expected,
                             DownloadStatus status) {
                std::lock_guard<std::mutex> lock(mutex_);
                DownloadTask* task = findLocked(activeId);
                if (!task || task->status == DownloadStatus::Removing ||
                    task->status == DownloadStatus::Paused)
                    return;
                bool packageCommitted =
                    completed != task->packagesInstalled;
                task->packagesInstalled = completed;
                task->currentPackage = package;
                updateTaskInstallProgress(*task, installed, expected, status,
                                          now_ms());
                if (packageCommitted) {
                    std::string ignored;
                    saveLocked(ignored);
                }
            });
        if (!coordinator->error().empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId)) {
                task->status = DownloadStatus::Error;
                task->error = coordinator->error();
                std::string ignored;
                saveLocked(ignored);
            }
            coordinator.reset();
            metainfo_free(&metainfo);
            return;
        }
        options.files = coordinator->configs().data();
        if (mode == TransferMode::StreamInstall) {
            options.strict_piece_order = 1;
            options.piece_order = coordinator->pieceOrder().data();
            options.piece_order_count =
                static_cast<uint32_t>(coordinator->pieceOrder().size());
            options.request_allowed = &PackageCoordinator::requestAllowedThunk;
            options.request_allowed_user = coordinator.get();
            // Initial window only; the loop below resizes it from the
            // install sink's backlog (PERF_PLAN 5.1).
            options.strict_order_lookahead = coordinator->initialLookahead();
            options.strict_fill_pending_first = 1;
            // Per-peer in-flight ceiling (4 MiB = 256 x 16 KiB blocks =
            // MAX_PIPELINE). The engine scales the actual window per peer by
            // measured speed (PERF_PLAN 5.2); this is only the fast-peer cap.
            // Was 64 (1 MiB), which pinned even 2 MB/s peers at the ceiling.
            options.request_pipeline_limit = 256;
            options.hedge_after_ms = 5000;
        }
        options.telemetry_tag = activeId.c_str();
        if (!resumeBitfield.empty()) {
            options.have_bitfield = resumeBitfield.data();
            options.have_bitfield_len =
                static_cast<uint32_t>(resumeBitfield.size());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (DownloadTask* task = findLocked(activeId))
            task->packageCount = coordinator->packageCount();
    }

    torrent_t* torrent = torrent_create_ex(
        &metainfo, static_cast<uint16_t>(kBasePeerPort + slot->slotIndex),
        dataPath.c_str(), &options);
    if (!torrent) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (DownloadTask* task = findLocked(activeId)) {
            task->status = DownloadStatus::Error;
            task->error = "Unable to initialize torrent storage or network.";
            std::string ignored;
            saveLocked(ignored);
        }
        coordinator.reset();
        metainfo_free(&metainfo);
        return;
    }
    if (!initialPeers.empty()) {
        torrent_add_initial_peers(
            torrent, initialPeers.data(),
            static_cast<uint32_t>(initialPeers.size() / 6));
    }

    // BEP-19 web seed: pull whole pieces over HTTP in parallel with the
    // swarm. Deterministic bandwidth independent of how few peers we can
    // reach over plaintext TCP (Bug A). Single-file torrents only — the
    // package torrents pipensx ships are one NSP payload. Verification is
    // the engine's job (torrent_submit_web_piece re-checks the SHA-1).
    constexpr size_t kWebSeedParallel = 8;
    std::unique_ptr<WebSeedSource> webSeed;
    uint32_t webSeedCursor = 0;
    if (metainfo.num_web_seeds > 0 && metainfo.num_files == 1 &&
        metainfo.num_pieces > 0) {
        webSeed = std::make_unique<WebSeedSource>(
            metainfo.web_seeds[0], metainfo.name,
            static_cast<uint64_t>(metainfo.piece_length),
            static_cast<uint64_t>(metainfo.total_length),
            metainfo.num_pieces);
    }

    bool finished = false;
    while (!stopping_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            DownloadTask* task = findLocked(activeId);
            if (!task || task->status == DownloadStatus::Paused ||
                task->status == DownloadStatus::Removing)
                break;
            // The user can flip torrenting off mid-transfer; stop talking to
            // peers on the next tick rather than at the end of the download.
            if (!torrentingEnabled_.load()) {
                task->status = DownloadStatus::Error;
                task->error =
                    "Torrenting disabled — enable it in Settings to retry.";
                task->speedBytesPerSecond = 0;
                std::string ignored;
                saveLocked(ignored);
                break;
            }
        }

        std::string installError = coordinator
            ? coordinator->error() : std::string();
        int running = installError.empty() ? torrent_tick(torrent) : -1;

        // Web-seed pump — runs on this (the torrent) thread, so the
        // submit/query seams never race the engine.
        if (running > 0 && webSeed) {
            WebSeedSource::Completed done;
            while (webSeed->popCompleted(done)) {
                if (done.ok)
                    torrent_submit_web_piece(
                        torrent, done.piece, done.data.data(),
                        static_cast<uint32_t>(done.data.size()));
            }
            uint32_t np = webSeed->numPieces();
            while (webSeed->inFlight() < kWebSeedParallel) {
                bool assignedOne = false;
                for (uint32_t n = 0; n < np; ++n) {
                    uint32_t piece = (webSeedCursor + n) % np;
                    if (torrent_piece_done(torrent, piece))
                        continue;
                    if (webSeed->enqueue(piece)) {
                        webSeedCursor = (piece + 1) % np;
                        assignedOne = true;
                        break;
                    }
                }
                if (!assignedOne)
                    break; // every remaining piece is done or in-flight
            }
        }

        torrent_stat_t stat;
        torrent_stat(torrent, &stat);
        if (running > 0 && mode == TransferMode::StreamInstall) {
            torrent_set_strict_lookahead(
                torrent,
                coordinator->adaptiveLookahead(stat.num_active_peers));
            torrent_set_rate_freeze(
                torrent, coordinator->requestsCurtailed() ? 1 : 0);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            DownloadTask* task = findLocked(activeId);
            if (!task)
                break;
            updateTaskDownloadProgress(*task, stat.completed_bytes,
                                       stat.last_payload_ms);
            task->totalBytes = stat.total_bytes;
            const uint64_t skipped = stat.skipped_bytes > stat.total_bytes
                ? stat.total_bytes : stat.skipped_bytes;
            task->wantedTotalBytes = stat.total_bytes - skipped;
            const uint64_t wantedDone = stat.completed_bytes > skipped
                ? stat.completed_bytes - skipped : 0;
            // Same high-water rule as completedBytes: the UI reads wanted*
            // first, so a verify pass that starts at 0 must not blank the bar.
            if (!(task->status == DownloadStatus::Checking ||
                  task->status == DownloadStatus::Verifying) ||
                wantedDone >= task->wantedCompletedBytes)
                task->wantedCompletedBytes = wantedDone;
            task->speedBytesPerSecond = stat.speed_bps;
            task->peers = stat.num_peers;
            task->dhtGood = stat.dht_good;
            task->dhtDubious = stat.dht_dubious;
            task->piecesDone = stat.num_pieces_done;
            task->piecesTotal = stat.num_pieces;
            task->piecesVerified = stat.num_pieces_verified;
            if (task->status != DownloadStatus::Removing &&
                task->status != DownloadStatus::Paused &&
                task->status != DownloadStatus::Installing &&
                task->status != DownloadStatus::Committing) {
                if (stat.verifying)
                    task->status = DownloadStatus::Verifying;
                else
                    task->status = DownloadStatus::Downloading;
            }
            if (running < 0) {
                task->status = DownloadStatus::Error;
                task->error = !installError.empty()
                    ? installError : torrent_last_error(torrent);
                if (coordinator && installError.empty())
                    coordinator->markRecoverableError(task->error);
                task->speedBytesPerSecond = 0;
            }
        }
        if (running < 0)
            break;
        if (!running) {
            bool installOk = mode != TransferMode::StreamInstall ||
                             coordinator->finish();
            std::lock_guard<std::mutex> lock(mutex_);
            DownloadTask* task = findLocked(activeId);
            if (task && task->status != DownloadStatus::Removing &&
                task->status != DownloadStatus::Paused) {
                if (!installOk) {
                    task->status = DownloadStatus::Error;
                    task->error = coordinator->error();
                } else if (mode == TransferMode::StreamInstall &&
                           task->packagesInstalled != task->packageCount) {
                    task->status = DownloadStatus::Error;
                    task->error =
                        "Torrent ended before all packages were installed.";
                } else {
                    task->status = mode == TransferMode::StreamInstall
                        ? DownloadStatus::Installed
                        : DownloadStatus::Completed;
                    finished = true;
                }
                task->completedBytes = task->totalBytes;
                task->wantedCompletedBytes = task->wantedTotalBytes
                    ? task->wantedTotalBytes : task->totalBytes;
                task->speedBytesPerSecond = 0;
                log_msg("[manager] completed %s, destroying torrent\n",
                        activeId.c_str());
            }
            break;
        }
    }

    webSeed.reset(); // join HTTP fetch threads before tearing down engine
    // Fast resume: snapshot the have-bitfield on the torrent thread while
    // the engine is still alive. Returns 0 (no arming) when the startup
    // scan was interrupted — that bitfield would be incomplete.
    std::vector<uint8_t> teardownBitfield;
    if (uint32_t need = torrent_copy_have_bitfield(torrent, nullptr, 0)) {
        teardownBitfield.resize(need);
        if (!torrent_copy_have_bitfield(torrent, teardownBitfield.data(),
                                        need))
            teardownBitfield.clear();
    }
    torrent_destroy(torrent);
    log_msg("[manager] torrent destroyed %s\n", activeId.c_str());
    if (coordinator) {
        std::lock_guard<std::mutex> lock(mutex_);
        const DownloadTask* task = findLocked(activeId);
        if (!task || task->status == DownloadStatus::Removing)
            coordinator->abandonResume();
    }
    coordinator.reset();
    metainfo_free(&metainfo);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        DownloadTask* task = findLocked(activeId);
        if (task && task->status == DownloadStatus::Removing) {
            bool deleteData = task->error == "delete-data";
            std::string removeError;
            removeLocked(activeId, deleteData, removeError);
        } else if (task) {
            task->speedBytesPerSecond = 0;
            // Arm fast resume for an interrupted task (pause / error /
            // shutdown). A finished task never re-runs and verify() must
            // do a real rehash, so it stays disarmed.
            if (finished)
                task->resumeBitfield.clear();
            else
                task->resumeBitfield = std::move(teardownBitfield);
            std::string ignored;
            saveLocked(ignored);
            if (finished)
                log_msg("[manager] completion saved %s\n",
                        activeId.c_str());
        }
    }
    if (finished)
        condition_.notify_all();
}

void DownloadManager::shutdown() {
    if (!workerStarted_)
        return;
    stopping_ = true;
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
    std::string ignored;
    save(ignored);
    workerStarted_ = false;
}

} // namespace pipensx
