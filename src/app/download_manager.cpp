#include "download_manager.hpp"
#include "../install/install_backend.hpp"
#include "../install/package_stream.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/metainfo.h"
#include "../core/util.h"
}

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

bool hasPackageExtension(const std::string& path) {
    std::string lower = path;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.size() >= 4 &&
           (lower.substr(lower.size() - 4) == ".nsp" ||
            lower.substr(lower.size() - 4) == ".nsz");
}

bool isCompressedPackage(const std::string& path) {
    std::string lower = path;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.size() >= 4 &&
           lower.substr(lower.size() - 4) == ".nsz";
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

class PackageCoordinator {
public:
    using Progress = std::function<void(
        uint32_t, const std::string&, uint64_t, uint64_t, DownloadStatus)>;

    PackageCoordinator(const metainfo_t& metainfo, std::string taskId,
                       const std::string& workingRoot,
                       uint32_t completedPackages, Progress progress)
        : metainfo_(metainfo), taskId_(std::move(taskId)),
          backend_(install::createInstallBackend(workingRoot)),
          completedPackages_(completedPackages),
          progress_(std::move(progress)) {
        configs_.resize(metainfo_.num_files);
        uint32_t ordinal = 0;
        for (uint32_t i = 0; i < metainfo_.num_files; ++i) {
            if (!hasPackageExtension(metainfo_.files[i].path)) {
                configs_[i].mode = STORAGE_FILE_DISK;
                continue;
            }
            packageOrdinals_[i] = ordinal;
            configs_[i].mode = ordinal < completedPackages_
                             ? STORAGE_FILE_SKIP : STORAGE_FILE_SINK;
            configs_[i].sink = &PackageCoordinator::sinkThunk;
            configs_[i].user = this;
            ++ordinal;
        }
        packageCount_ = ordinal;
        buildPieceOrder();
    }

    ~PackageCoordinator() {
        if (backend_)
            backend_->rollbackPackage();
    }

    const std::vector<storage_file_config_t>& configs() const {
        return configs_;
    }
    const std::vector<uint32_t>& pieceOrder() const { return pieceOrder_; }
    uint32_t packageCount() const { return packageCount_; }
    const std::string& error() const { return error_; }

private:
    static int sinkThunk(void* user, uint32_t fileIndex,
                         int64_t fileOffset, const uint8_t* data, size_t size) {
        return static_cast<PackageCoordinator*>(user)->sink(
            fileIndex, fileOffset, data, size) ? 1 : 0;
    }

    bool sink(uint32_t fileIndex, int64_t fileOffset,
              const uint8_t* data, size_t size) {
        auto ordinalIt = packageOrdinals_.find(fileIndex);
        if (fileIndex >= metainfo_.num_files ||
            ordinalIt == packageOrdinals_.end()) {
            error_ = "Invalid package stream routing.";
            return false;
        }
        if (ordinalIt->second < completedPackages_)
            return true;
        const mi_file_t& file = metainfo_.files[fileIndex];
        if (!stream_) {
            if (fileOffset != 0) {
                error_ = "Package stream did not start at offset zero.";
                return false;
            }
            if (!backend_->beginPackage(taskId_, file.path)) {
                error_ = backend_->error();
                return false;
            }
            activeFileIndex_ = fileIndex;
            currentPackage_ = file.path;
            install::PackageCallbacks callbacks;
            callbacks.beginFile = [this](const std::string& name,
                                         uint64_t fileSize) {
                bool ok = backend_->beginFile(name, fileSize);
                if (!ok)
                    error_ = backend_->error();
                return ok;
            };
            callbacks.setFileSize = [this](uint64_t fileSize) {
                bool ok = backend_->setFileSize(fileSize);
                if (!ok)
                    error_ = backend_->error();
                return ok;
            };
            callbacks.writeFile = [this](const uint8_t* bytes,
                                         size_t byteCount) {
                bool ok = backend_->writeFile(bytes, byteCount);
                if (ok) {
                    progress_(completedPackages_, currentPackage_,
                              backend_->installedBytes(),
                              backend_->expectedBytes(),
                              DownloadStatus::Installing);
                } else {
                    error_ = backend_->error();
                }
                return ok;
            };
            callbacks.endFile = [this] {
                bool ok = backend_->endFile();
                if (!ok)
                    error_ = backend_->error();
                return ok;
            };
            stream_ = std::make_unique<install::PackageStream>(
                isCompressedPackage(file.path), std::move(callbacks));
            progress_(completedPackages_, currentPackage_, 0, 0,
                      DownloadStatus::Installing);
        }
        if (activeFileIndex_ != fileIndex ||
            static_cast<uint64_t>(fileOffset) != stream_->consumed()) {
            error_ = "Package bytes arrived out of order.";
            return false;
        }
        if (!stream_->write(data, size)) {
            if (error_.empty())
                error_ = stream_->error();
            log_msg("[install] stream error package='%s' offset=%lld: %s\n",
                    currentPackage_.c_str(),
                    static_cast<long long>(fileOffset), error_.c_str());
            backend_->rollbackPackage();
            return false;
        }
        if (static_cast<uint64_t>(fileOffset) + size ==
            static_cast<uint64_t>(file.length)) {
            progress_(completedPackages_, currentPackage_,
                      backend_->installedBytes(),
                      backend_->expectedBytes(),
                      DownloadStatus::Committing);
            if (!stream_->finish()) {
                if (error_.empty())
                    error_ = stream_->error();
                log_msg("[install] finalize error package='%s': %s\n",
                        currentPackage_.c_str(), error_.c_str());
                backend_->rollbackPackage();
                return false;
            }
            bool alreadyInstalled = false;
            if (!backend_->commitPackage(alreadyInstalled)) {
                error_ = backend_->error();
                log_msg("[install] commit error package='%s': %s\n",
                        currentPackage_.c_str(), error_.c_str());
                backend_->rollbackPackage();
                return false;
            }
            ++completedPackages_;
            stream_.reset();
            activeFileIndex_ = UINT32_MAX;
            progress_(completedPackages_, currentPackage_, 0, 0,
                      DownloadStatus::Downloading);
            currentPackage_.clear();
        }
        return true;
    }

    void buildPieceOrder() {
        std::vector<uint8_t> added(metainfo_.num_pieces, 0);
        for (const auto& item : packageOrdinals_) {
            if (item.second < completedPackages_)
                continue;
            const mi_file_t& file = metainfo_.files[item.first];
            if (file.length <= 0)
                continue;
            uint32_t first = static_cast<uint32_t>(
                file.offset / metainfo_.piece_length);
            uint32_t last = static_cast<uint32_t>(
                (file.offset + file.length - 1) / metainfo_.piece_length);
            for (uint32_t piece = first;
                 piece <= last && piece < metainfo_.num_pieces; ++piece) {
                if (!added[piece]) {
                    added[piece] = 1;
                    pieceOrder_.push_back(piece);
                }
            }
        }
        for (uint32_t piece = 0; piece < metainfo_.num_pieces; ++piece)
            if (!added[piece])
                pieceOrder_.push_back(piece);
    }

    const metainfo_t& metainfo_;
    std::string taskId_;
    std::unique_ptr<install::InstallBackend> backend_;
    std::vector<storage_file_config_t> configs_;
    std::vector<uint32_t> pieceOrder_;
    std::map<uint32_t, uint32_t> packageOrdinals_;
    std::unique_ptr<install::PackageStream> stream_;
    uint32_t completedPackages_ = 0;
    uint32_t packageCount_ = 0;
    uint32_t activeFileIndex_ = UINT32_MAX;
    std::string currentPackage_;
    std::string error_;
    Progress progress_;
};

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

} // namespace

const char* statusName(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Queued: return "Queued";
        case DownloadStatus::Checking: return "Checking";
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
    load();
    if (startWorker) {
        workerStarted_ = true;
        worker_ = std::thread(&DownloadManager::workerMain, this);
    }
}

DownloadManager::~DownloadManager() {
    shutdown();
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
    preview.totalBytes = static_cast<uint64_t>(metainfo.total_length);
    preview.fileCount = metainfo.num_files;
    preview.trackerCount = metainfo.num_trackers;
    for (uint32_t i = 0; i < metainfo.num_files; ++i)
        if (hasPackageExtension(metainfo.files[i].path))
            ++preview.packageCount;
    metainfo_free(&metainfo);
    return true;
}

bool DownloadManager::importTorrent(const std::string& path,
                                    TransferMode mode,
                                    std::string& taskId,
                                    std::string& error) {
    TorrentPreview preview;
    if (!previewTorrent(path, preview, error))
        return false;
    if (mode == TransferMode::StreamInstall && preview.packageCount == 0) {
        error = "This torrent does not contain NSP or NSZ packages.";
        return false;
    }

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
    task.packageCount = preview.packageCount;
    tasks_.push_back(std::move(task));
    taskId = preview.infoHash;

    if (!saveLocked(error)) {
        tasks_.pop_back();
        unlink(metainfoPath.c_str());
        removeTree(dataPath);
        return false;
    }
    condition_.notify_all();
    return true;
}

bool DownloadManager::pause(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask* task = findLocked(taskId);
    if (!task)
        return false;
    if (task->status != DownloadStatus::Queued &&
        task->status != DownloadStatus::Checking &&
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
    DownloadTask* task = findLocked(taskId);
    if (!task || task->status != DownloadStatus::Completed)
        return false;
    task->status = DownloadStatus::Queued;
    task->error.clear();
    task->piecesVerified = 0;
    std::string ignored;
    saveLocked(ignored);
    condition_.notify_all();
    return true;
}

bool DownloadManager::remove(const std::string& taskId, bool deleteData,
                             std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask* task = findLocked(taskId);
    if (!task) {
        error = "Download task not found.";
        return false;
    }

    if (task->status == DownloadStatus::Checking ||
        task->status == DownloadStatus::Downloading ||
        task->status == DownloadStatus::Installing ||
        task->status == DownloadStatus::Committing ||
        task->status == DownloadStatus::Verifying) {
        task->status = DownloadStatus::Removing;
        task->error = deleteData ? "delete-data" : "keep-data";
        condition_.notify_all();
        return true;
    }
    return removeLocked(taskId, deleteData, error);
}

std::vector<DownloadTask> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_;
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
        state << "d";
        state << "4:data" << bstr(task.dataPath);
        state << "5:error" << bstr(task.error);
        state << "2:id" << bstr(task.id);
        state << "8:metainfo" << bstr(task.metainfoPath);
        state << "4:mode" << bstr(persistedMode(task.mode));
        state << "4:name" << bstr(task.name);
        state << "13:package-count" << bint(task.packageCount);
        state << "13:packages-done" << bint(task.packagesInstalled);
        state << "6:status" << bstr(persistedStatus(task.status));
        state << "5:total" << bint(task.totalBytes);
        state << "e";
    }
    state << "e";
    state << "7:versioni2e";
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
        (version.ival != 1 && version.ival != 2))
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
        task.status = persistedStatus(status);
        if (task.status == DownloadStatus::Completed ||
            task.status == DownloadStatus::Installed)
            task.completedBytes = task.totalBytes;
        if (!isManagedChild(torrentRoot_, task.metainfoPath) ||
            !isManagedChild(downloadRoot_, task.dataPath)) {
            task.status = DownloadStatus::Error;
            task.error = "The stored task contains an invalid path.";
        } else if (access(task.metainfoPath.c_str(), R_OK) != 0) {
            task.status = DownloadStatus::Error;
            task.error = "The stored .torrent file is missing.";
        }
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

bool DownloadManager::removeLocked(const std::string& id, bool deleteData,
                                   std::string& error) {
    for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
        if (it->id != id)
            continue;
        if (!isManagedChild(torrentRoot_, it->metainfoPath) ||
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
        unlink(it->metainfoPath.c_str());
        tasks_.erase(it);
        return saveLocked(error);
    }
    error = "Download task not found.";
    return false;
}

void DownloadManager::workerMain() {
    while (!stopping_) {
        std::string activeId;
        std::string metainfoPath;
        std::string dataPath;
        TransferMode mode = TransferMode::DownloadOnly;
        uint32_t packagesInstalled = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                if (stopping_)
                    return true;
                for (const DownloadTask& task : tasks_)
                    if (task.status == DownloadStatus::Queued)
                        return true;
                return false;
            });
            if (stopping_)
                break;
            for (DownloadTask& task : tasks_) {
                if (task.status == DownloadStatus::Queued) {
                    task.status = DownloadStatus::Checking;
                    activeId = task.id;
                    metainfoPath = task.metainfoPath;
                    dataPath = task.dataPath;
                    mode = task.mode;
                    packagesInstalled = task.packagesInstalled;
                    break;
                }
            }
        }

        metainfo_t metainfo;
        if (!metainfo_load(metainfoPath.c_str(), &metainfo)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId)) {
                task->status = DownloadStatus::Error;
                task->error = "Unable to read the stored .torrent file.";
                std::string ignored;
                saveLocked(ignored);
            }
            continue;
        }

        std::unique_ptr<PackageCoordinator> coordinator;
        torrent_options_t options {};
        if (mode == TransferMode::StreamInstall) {
            coordinator = std::make_unique<PackageCoordinator>(
                metainfo, activeId, rootPath_, packagesInstalled,
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
                    task->installedBytes = installed;
                    task->installTotalBytes = expected;
                    task->status = status;
                    if (packageCommitted) {
                        std::string ignored;
                        saveLocked(ignored);
                    }
                });
            options.files = coordinator->configs().data();
            options.strict_piece_order = 1;
            options.piece_order = coordinator->pieceOrder().data();
            options.piece_order_count =
                static_cast<uint32_t>(coordinator->pieceOrder().size());
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId))
                task->packageCount = coordinator->packageCount();
        }

        torrent_t* torrent = torrent_create_ex(
            &metainfo, 51413, dataPath.c_str(),
            mode == TransferMode::StreamInstall ? &options : nullptr);
        if (!torrent) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (DownloadTask* task = findLocked(activeId)) {
                task->status = DownloadStatus::Error;
                task->error = "Unable to initialize torrent storage or network.";
                std::string ignored;
                saveLocked(ignored);
            }
            metainfo_free(&metainfo);
            continue;
        }

        bool finished = false;
        while (!stopping_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                DownloadTask* task = findLocked(activeId);
                if (!task || task->status == DownloadStatus::Paused ||
                    task->status == DownloadStatus::Removing)
                    break;
            }

            int running = torrent_tick(torrent);
            torrent_stat_t stat;
            torrent_stat(torrent, &stat);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                DownloadTask* task = findLocked(activeId);
                if (!task)
                    break;
                task->completedBytes = stat.completed_bytes;
                task->totalBytes = stat.total_bytes;
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
                    task->error = coordinator && !coordinator->error().empty()
                                ? coordinator->error()
                                : torrent_last_error(torrent);
                    task->speedBytesPerSecond = 0;
                } else if (!running) {
                    if (mode == TransferMode::StreamInstall &&
                        task->packagesInstalled != task->packageCount) {
                        task->status = DownloadStatus::Error;
                        task->error =
                            "Torrent ended before all packages were installed.";
                    } else {
                        task->status =
                            mode == TransferMode::StreamInstall
                            ? DownloadStatus::Installed
                            : DownloadStatus::Completed;
                        finished = true;
                    }
                    task->completedBytes = task->totalBytes;
                    task->speedBytesPerSecond = 0;
                    log_msg("[manager] completed %s, destroying torrent\n",
                            activeId.c_str());
                }
            }
            if (running <= 0)
                break;
        }

        torrent_destroy(torrent);
        log_msg("[manager] torrent destroyed %s\n", activeId.c_str());
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
