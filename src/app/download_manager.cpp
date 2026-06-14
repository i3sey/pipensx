#include "download_manager.hpp"

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
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

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

DownloadStatus persistedStatus(const std::string& value) {
    if (value == "paused")
        return DownloadStatus::Paused;
    if (value == "completed")
        return DownloadStatus::Completed;
    if (value == "error")
        return DownloadStatus::Error;
    return DownloadStatus::Queued;
}

std::string persistedStatus(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Paused: return "paused";
        case DownloadStatus::Completed: return "completed";
        case DownloadStatus::Error: return "error";
        default: return "queued";
    }
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
    metainfo_free(&metainfo);
    return true;
}

bool DownloadManager::importTorrent(const std::string& path,
                                    std::string& taskId,
                                    std::string& error) {
    TorrentPreview preview;
    if (!previewTorrent(path, preview, error))
        return false;

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
        state << "4:name" << bstr(task.name);
        state << "6:status" << bstr(persistedStatus(task.status));
        state << "5:total" << bint(task.totalBytes);
        state << "e";
    }
    state << "e";
    state << "7:versioni1e";
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
        unlink(temporary.c_str());
        error = "Unable to replace queue state.";
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
        version.type != BE_INT || version.ival != 1)
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
        task.status = persistedStatus(status);
        if (task.status == DownloadStatus::Completed)
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

        torrent_t* torrent = torrent_create(&metainfo, 51413,
                                            dataPath.c_str());
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
                    task->status != DownloadStatus::Paused) {
                    if (stat.verifying)
                        task->status = DownloadStatus::Verifying;
                    else
                        task->status = DownloadStatus::Downloading;
                }
                if (!running) {
                    task->status = DownloadStatus::Completed;
                    task->completedBytes = task->totalBytes;
                    task->speedBytesPerSecond = 0;
                    finished = true;
                    log_msg("[manager] completed %s, destroying torrent\n",
                            activeId.c_str());
                }
            }
            if (!running)
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
