#include "task_files.hpp"

#include "download_manager.hpp"
#include "nx_file_types.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/metainfo.h"
#include "../platform/storage.h"
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr int64_t kManifestVersion = 1;
constexpr size_t kMaxManifestFiles = 4096;
constexpr size_t kMaxPathBytes = 1024;
constexpr size_t kMaxScanDepth = 32;
constexpr size_t kMaxManifestBytes = 8 * 1024 * 1024;

std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

std::string bint(uint64_t value) {
    return "i" + std::to_string(value) + "e";
}

bool mkdirs(const std::string& path) {
    if (path.empty() || path.size() >= 1024)
        return false;
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* p = buffer + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

std::string manifestPath(const std::string& root, const std::string& id) {
    return root + "/task-files/" + id + ".bencode";
}

bool readString(const be_node_t& dict, const char* key, std::string& out) {
    be_node_t value;
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &value) || value.type != BE_STR)
        return false;
    out.assign(value.sval, value.slen);
    return true;
}

bool readInteger(const be_node_t& dict, const char* key, uint64_t& out) {
    be_node_t value;
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &value) || value.type != BE_INT ||
        value.ival < 0)
        return false;
    out = static_cast<uint64_t>(value.ival);
    return true;
}

bool isSettled(DownloadStatus status) {
    return status == DownloadStatus::Completed ||
           status == DownloadStatus::Installed;
}

TaskFileAction actionAt(const DownloadTask& task, size_t index,
                        bool package) {
    if (index < task.fileSelection.size()) {
        const uint8_t raw = task.fileSelection[index];
        if (raw <= static_cast<uint8_t>(TaskFileAction::Install))
            return static_cast<TaskFileAction>(raw);
    }
    return task.mode == TransferMode::StreamInstall && package
        ? TaskFileAction::Install : TaskFileAction::Download;
}

TaskFileManifest deriveTorrentManifest(const DownloadTask& task,
                                       const metainfo_t& metainfo) {
    TaskFileManifest manifest;
    manifest.taskId = task.id;
    manifest.files.reserve(metainfo.num_files);
    for (uint32_t i = 0; i < metainfo.num_files; ++i) {
        TaskFileRecord file;
        file.logicalPath = metainfo.is_multi
            ? std::string(metainfo.name) + "/" + metainfo.files[i].path
            : metainfo.files[i].path;
        file.localPath = file.logicalPath;
        file.size = static_cast<uint64_t>(metainfo.files[i].length);
        file.sourceIndex = i;
        const TorrentPreview::File previewFile{
            metainfo.files[i].path, file.size,
            isPackageName(metainfo.files[i].path),
            isCompressedName(metainfo.files[i].path),
            isCartridgeName(metainfo.files[i].path)};
        file.package = previewFile.package;
        file.compressed = previewFile.compressed;
        file.cartridge = previewFile.cartridge;
        file.action = actionAt(task, i, file.package);
        manifest.files.push_back(std::move(file));
    }
    return manifest;
}

bool scanTree(const std::string& root, const std::string& relative,
              size_t depth, std::vector<TaskFileRecord>& files,
              bool& unsafe, std::string& error) {
    if (depth > kMaxScanDepth || files.size() >= kMaxManifestFiles) {
        error = "The downloaded file tree is too large.";
        return false;
    }
    const std::string path = relative.empty() ? root : root + "/" + relative;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        error = "Unable to read the download directory.";
        return false;
    }
    bool ok = true;
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        const std::string rel = relative.empty()
            ? entry->d_name : relative + "/" + entry->d_name;
        if (!taskFilePathIsSafe(rel)) {
            unsafe = true;
            continue;
        }
        const std::string child = root + "/" + rel;
        struct stat st {};
        if (lstat(child.c_str(), &st) != 0) {
            unsafe = true;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            unsafe = true;
            TaskFileRecord record;
            record.logicalPath = rel;
            record.localPath = rel;
            files.push_back(std::move(record));
        } else if (S_ISDIR(st.st_mode)) {
            if (!scanTree(root, rel, depth + 1, files, unsafe, error)) {
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            TaskFileRecord record;
            record.logicalPath = rel;
            record.localPath = rel;
            record.size = static_cast<uint64_t>(st.st_size);
            record.action = TaskFileAction::Download;
            files.push_back(std::move(record));
        } else {
            unsafe = true;
        }
    }
    closedir(dir);
    return ok;
}

std::string lowerAscii(std::string value) {
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}

std::string escapedBytes(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char ch : value) {
        if (ch >= 0x20 && ch < 0x7f) {
            result.push_back(static_cast<char>(ch));
        } else {
            result += "\\x";
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0f]);
        }
    }
    return result;
}

} // namespace

bool taskFilePathIsSafe(const std::string& path) {
    if (path.empty() || path.size() >= kMaxPathBytes || path.front() == '/' ||
        path.front() == '\\' || path.back() == '/')
        return false;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const std::string component = path.substr(
            start, slash == std::string::npos ? std::string::npos
                                               : slash - start);
        if (component.empty() || component == "." || component == ".." ||
            component.find('\\') != std::string::npos)
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

bool taskFilePathIsValidUtf8(const std::string& path) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(path.data());
    size_t i = 0;
    while (i < path.size()) {
        const unsigned char first = bytes[i++];
        if (first < 0x80)
            continue;
        unsigned need = 0;
        uint32_t value = 0;
        if ((first & 0xe0) == 0xc0) {
            need = 1;
            value = first & 0x1f;
            if (value < 2)
                return false;
        } else if ((first & 0xf0) == 0xe0) {
            need = 2;
            value = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            need = 3;
            value = first & 0x07;
        } else {
            return false;
        }
        if (i + need > path.size())
            return false;
        for (unsigned j = 0; j < need; ++j) {
            const unsigned char next = bytes[i++];
            if ((next & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (next & 0x3f);
        }
        if ((need == 2 && value < 0x800) ||
            (need == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

bool taskFilePathIsFatCompatible(const std::string& path) {
    if (!taskFilePathIsSafe(path) || !taskFilePathIsValidUtf8(path))
        return false;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const std::string component = path.substr(
            start, slash == std::string::npos ? std::string::npos
                                               : slash - start);
        if (component.size() > 255 || component.back() == ' ' ||
            component.back() == '.')
            return false;
        for (unsigned char ch : component)
            if (ch < 0x20 || ch == '<' || ch == '>' || ch == ':' ||
                ch == '"' || ch == '|' || ch == '?' || ch == '*')
                return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

TaskFileManifest makeTaskFileManifest(
    const std::string& taskId, const TorrentPreview& preview,
    const std::vector<uint8_t>& actions) {
    TaskFileManifest manifest;
    manifest.taskId = taskId;
    manifest.files.reserve(preview.files.size());
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const TorrentPreview::File& source = preview.files[i];
        TaskFileRecord file;
        file.logicalPath = preview.multi
            ? preview.name + "/" + source.path : source.path;
        file.localPath = file.logicalPath;
        file.size = source.length;
        file.sourceIndex = static_cast<uint32_t>(i);
        file.package = source.package;
        file.compressed = source.compressed;
        file.cartridge = source.cartridge;
        const uint8_t raw = i < actions.size()
            ? actions[i] : static_cast<uint8_t>(TaskFileAction::Download);
        file.action = raw <= static_cast<uint8_t>(TaskFileAction::Install)
            ? static_cast<TaskFileAction>(raw) : TaskFileAction::Download;
        manifest.files.push_back(std::move(file));
    }
    return manifest;
}

bool saveTaskFileManifest(const std::string& appRoot,
                          const TaskFileManifest& manifest,
                          std::string& error) {
    if (manifest.taskId.empty() || manifest.files.size() > kMaxManifestFiles) {
        error = "Invalid task file manifest.";
        return false;
    }
    std::string blob = "d5:filesl";
    for (const TaskFileRecord& file : manifest.files) {
        if (!taskFilePathIsSafe(file.logicalPath) ||
            (!file.localPath.empty() && !taskFilePathIsSafe(file.localPath))) {
            error = "Task file manifest contains an unsafe path.";
            return false;
        }
        blob += "d6:action" + bint(static_cast<uint8_t>(file.action));
        blob += "9:cartridge" + bint(file.cartridge ? 1 : 0);
        blob += "10:compressed" + bint(file.compressed ? 1 : 0);
        blob += "5:index" + bint(file.sourceIndex);
        blob += "5:local" + bstr(file.localPath);
        blob += "7:package" + bint(file.package ? 1 : 0);
        blob += "4:path" + bstr(file.logicalPath);
        blob += "4:size" + bint(file.size);
        blob += "e";
    }
    blob += "e4:task" + bstr(manifest.taskId);
    blob += "7:version" + bint(kManifestVersion) + "e";

    const std::string directory = appRoot + "/task-files";
    if (!mkdirs(directory)) {
        error = "Unable to create task file manifest directory.";
        return false;
    }
    const std::string path = manifestPath(appRoot, manifest.taskId);
    const std::string temporary = path + ".tmp";
    std::FILE* output = std::fopen(temporary.c_str(), "wb");
    if (!output) {
        error = "Unable to open task file manifest.";
        return false;
    }
    bool ok = std::fwrite(blob.data(), 1, blob.size(), output) == blob.size();
    ok = std::fflush(output) == 0 && ok;
#if !defined(_WIN32)
    if (ok)
        fsync(fileno(output));
#endif
    ok = std::fclose(output) == 0 && ok;
    if (!ok || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        error = "Unable to save task file manifest.";
        return false;
    }
    return true;
}

bool loadTaskFileManifest(const std::string& appRoot,
                          const std::string& taskId,
                          TaskFileManifest& manifest,
                          std::string& error) {
    std::ifstream input(manifestPath(appRoot, taskId), std::ios::binary);
    if (!input) {
        error = "Task file manifest is missing.";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxManifestBytes) {
        error = "Task file manifest is too large.";
        return false;
    }
    input.seekg(0, std::ios::beg);
    const std::string blob((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const char* cursor = blob.data();
    const char* end = cursor + blob.size();
    be_node_t root;
    if (!be_decode(&cursor, end, &root) || cursor != end ||
        root.type != BE_DICT) {
        error = "Task file manifest is malformed.";
        return false;
    }
    uint64_t version = 0;
    std::string storedTask;
    be_node_t list;
    if (!readInteger(root, "version", version) ||
        version != static_cast<uint64_t>(kManifestVersion) ||
        !readString(root, "task", storedTask) || storedTask != taskId ||
        !be_dict_get(root.buf, root.buf + root.raw_len, "files", 5, &list) ||
        list.type != BE_LIST) {
        error = "Task file manifest has an unsupported schema.";
        return false;
    }

    TaskFileManifest parsed;
    parsed.taskId = taskId;
    const char* itemCursor = list.buf + 1;
    const char* itemEnd = list.buf + list.raw_len - 1;
    be_node_t item;
    while (be_list_next(&itemCursor, itemEnd, &item)) {
        if (item.type != BE_DICT || parsed.files.size() >= kMaxManifestFiles) {
            error = "Task file manifest contains invalid entries.";
            return false;
        }
        TaskFileRecord file;
        uint64_t action = 0, cartridge = 0, compressed = 0, index = 0;
        uint64_t package = 0, size = 0;
        if (!readInteger(item, "action", action) || action > 2 ||
            !readInteger(item, "cartridge", cartridge) || cartridge > 1 ||
            !readInteger(item, "compressed", compressed) || compressed > 1 ||
            !readInteger(item, "index", index) || index > UINT32_MAX ||
            !readString(item, "local", file.localPath) ||
            !readInteger(item, "package", package) || package > 1 ||
            !readString(item, "path", file.logicalPath) ||
            !readInteger(item, "size", size) ||
            !taskFilePathIsSafe(file.logicalPath) ||
            (!file.localPath.empty() && !taskFilePathIsSafe(file.localPath))) {
            error = "Task file manifest contains an invalid file.";
            return false;
        }
        file.action = static_cast<TaskFileAction>(action);
        file.cartridge = cartridge != 0;
        file.compressed = compressed != 0;
        file.sourceIndex = static_cast<uint32_t>(index);
        file.package = package != 0;
        file.size = size;
        parsed.files.push_back(std::move(file));
    }
    manifest = std::move(parsed);
    return true;
}

void removeTaskFileManifest(const std::string& appRoot,
                            const std::string& taskId) {
    const std::string path = manifestPath(appRoot, taskId);
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

bool buildTaskFileInventory(const std::string& appRoot,
                            const DownloadTask& task,
                            TaskFileInventory& inventory,
                            std::string& error) {
    TaskFileManifest manifest;
    std::string ignored;
    bool completeManifest = loadTaskFileManifest(appRoot, task.id, manifest,
                                                 ignored);
    metainfo_t metainfo {};
    bool haveMetainfo = false;
    if (!task.metainfoPath.empty())
        haveMetainfo = metainfo_load(task.metainfoPath.c_str(), &metainfo) != 0;
    if (!completeManifest && haveMetainfo)
        manifest = deriveTorrentManifest(task, metainfo);
    if (!completeManifest && !haveMetainfo) {
        manifest.taskId = task.id;
        bool unsafe = false;
        if (!scanTree(task.dataPath, "", 0, manifest.files, unsafe, error))
            return false;
        if (unsafe) {
            TaskFileRecord marker;
            marker.logicalPath = "_unsafe_entry";
            marker.localPath = marker.logicalPath;
            manifest.files.push_back(std::move(marker));
        }
    }

    TaskFileInventory result;
    result.taskId = task.id;
    result.rootPath = task.dataPath;
    result.settled = isSettled(task.status);
    result.completeManifest = completeManifest || haveMetainfo;
    result.files.reserve(manifest.files.size());
    for (const TaskFileRecord& record : manifest.files) {
        TaskFileInfo file;
        static_cast<TaskFileRecord&>(file) = record;
        if (!taskFilePathIsValidUtf8(record.logicalPath) ||
            (!record.localPath.empty() &&
             !taskFilePathIsValidUtf8(record.localPath))) {
            file.logicalPath = escapedBytes(record.logicalPath);
            file.state = TaskFileState::Unsafe;
            result.files.push_back(std::move(file));
            continue;
        }
        if (record.logicalPath == "_unsafe_entry") {
            file.state = TaskFileState::Unsafe;
            result.files.push_back(std::move(file));
            continue;
        }
        if (record.action == TaskFileAction::Skip) {
            file.state = TaskFileState::Skipped;
        } else if (record.action == TaskFileAction::Install) {
            file.state = task.status == DownloadStatus::Installed
                ? TaskFileState::Installed : TaskFileState::Pending;
        } else if (!result.settled) {
            file.state = TaskFileState::Pending;
        } else {
            char located[1024] {};
            if (task.source == TaskSource::Torrent && haveMetainfo &&
                record.sourceIndex < metainfo.num_files &&
                storage_locate_file_path(&metainfo, task.dataPath.c_str(),
                                         record.sourceIndex, located,
                                         sizeof(located))) {
                file.absolutePath = located;
            } else if (!record.localPath.empty()) {
                file.absolutePath = task.dataPath + "/" + record.localPath;
            }
            struct stat st {};
            if (file.absolutePath.empty() ||
                lstat(file.absolutePath.c_str(), &st) != 0) {
                file.state = TaskFileState::Missing;
            } else if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                file.state = TaskFileState::Unsafe;
            } else if (static_cast<uint64_t>(st.st_size) != record.size) {
                file.state = TaskFileState::Missing;
            } else {
                file.state = TaskFileState::Present;
                result.presentBytes += record.size;
            }
        }
        result.files.push_back(std::move(file));
    }
    if (haveMetainfo)
        metainfo_free(&metainfo);
    std::sort(result.files.begin(), result.files.end(),
              [](const TaskFileInfo& a, const TaskFileInfo& b) {
                  const std::string lowerA = lowerAscii(a.logicalPath);
                  const std::string lowerB = lowerAscii(b.logicalPath);
                  return lowerA == lowerB ? a.logicalPath < b.logicalPath
                                          : lowerA < lowerB;
              });
    inventory = std::move(result);
    return true;
}

} // namespace pipensx
