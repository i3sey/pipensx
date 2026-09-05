#include "switch_deploy.hpp"

#include "install_space.hpp"
#include "nx_file_types.hpp"
#include "port_archive.hpp"
#include "../install/install_backend.hpp"
#include "../install/package_stream.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/sha256.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr size_t kCopyBufferBytes = 256 * 1024;
constexpr uint32_t kNroMagic = 0x304f524e;
constexpr int64_t kReceiptVersion = 4;
constexpr int64_t kJobVersion = 1;
constexpr int64_t kMoveJobVersion = 1;
constexpr size_t kMaxStateBytes = 8 * 1024 * 1024;
constexpr size_t kMaxReceiptUnpacked = 16384;

struct ReceiptFile {
    std::string path;
    uint64_t size = 0;
    std::array<uint8_t, 32> digest {};
    SwitchDeployTarget target = SwitchDeployTarget::SwitchDirectory;
};

std::string lowerAscii(std::string value) {
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}

bool asciiEqual(const std::string& a, const std::string& b) {
    return lowerAscii(a) == lowerAscii(b);
}

// Every 16-hex title id embedded in a path, uppercased. Forwarder NSP file
// names carry the id in brackets ("Port [01d2c0b236000000].nsp"), and the
// receipt records them so Uninstall can link a title to its deployment
// without the metadata index (which covers retail releases only).
std::vector<std::string> titleIdsInPath(const std::string& path) {
    std::vector<std::string> ids;
    for (size_t i = 0; i + 16 <= path.size(); ++i) {
        bool hex = true;
        for (size_t j = 0; j < 16; ++j) {
            const char c = path[i + j];
            if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') &&
                !(c >= 'A' && c <= 'F')) {
                hex = false;
                break;
            }
        }
        if (!hex)
            continue;
        std::string id = path.substr(i, 16);
        for (char& c : id)
            if (c >= 'a' && c <= 'f')
                c = static_cast<char>(c - 'a' + 'A');
        ids.push_back(std::move(id));
        i += 15;
    }
    return ids;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        result.push_back(path.substr(
            start, slash == std::string::npos ? std::string::npos
                                               : slash - start));
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return result;
}

std::string joinPath(const std::vector<std::string>& parts, size_t begin,
                     size_t end) {
    std::string result;
    for (size_t i = begin; i < end; ++i) {
        if (!result.empty())
            result += '/';
        result += parts[i];
    }
    return result;
}

std::string sdRootForSwitchRoot(const std::string& switchRoot) {
    const size_t slash = switchRoot.find_last_of('/');
    return slash == std::string::npos ? switchRoot
                                      : switchRoot.substr(0, slash);
}

const std::string& deployRoot(const std::string& switchRoot,
                              const std::string& sdRoot,
                              SwitchDeployTarget target) {
    return target == SwitchDeployTarget::SdRoot ? sdRoot : switchRoot;
}

const char* receiptTargetName(SwitchDeployTarget target) {
    return target == SwitchDeployTarget::SdRoot ? "sd" : "switch";
}

bool managedChild(const std::string& root, const std::string& path) {
    std::string prefix = root;
    while (!prefix.empty() && prefix.back() == '/')
        prefix.pop_back();
    prefix += '/';
    return path.rfind(prefix, 0) == 0 &&
           taskFilePathIsSafe(path.substr(prefix.size()));
}

bool hashFile(const std::string& path, std::array<uint8_t, 32>& digest) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    sha256_ctx_t context;
    sha256_init(&context);
    std::vector<uint8_t> buffer(kCopyBufferBytes);
    size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
        sha256_update(&context, buffer.data(), count);
    bool ok = std::ferror(file) == 0;
    if (std::fclose(file) != 0)
        ok = false;
    if (!ok)
        return false;
    sha256_final(&context, digest.data());
    return true;
}

bool validNro(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    uint32_t magic = 0;
    const bool ok = std::fseek(file, 0x10, SEEK_SET) == 0 &&
                    std::fread(&magic, 1, sizeof(magic), file) == sizeof(magic);
    std::fclose(file);
    return ok && magic == kNroMagic;
}

bool destinationParentsSafe(const std::string& root,
                            const std::string& relative) {
    struct stat rootStat {};
    if (lstat(root.c_str(), &rootStat) != 0 || !S_ISDIR(rootStat.st_mode) ||
        S_ISLNK(rootStat.st_mode))
        return false;
    const std::vector<std::string> parts = splitPath(relative);
    std::string current = root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        current += '/' + parts[i];
        struct stat st {};
        if (lstat(current.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
                return false;
        } else if (errno != ENOENT) {
            return false;
        }
    }
    return true;
}

// One mkdir level where EEXIST only counts when the name is already a
// directory — a regular file holding a directory's name must fail here with
// ENOTDIR, not deeper with a cryptic ENOENT (B2).
bool mkdirOne(char* path) {
    struct stat st {};
    if (mkdir(path, 0755) == 0)
        return true;
    if (errno != EEXIST)
        return false;
    if (stat(path, &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        errno = ENOTDIR;
    return S_ISDIR(st.st_mode) != 0;
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
        const bool ok = mkdirOne(buffer);
        *p = '/';
        if (!ok)
            return false;
    }
    return mkdirOne(buffer);
}

std::string parentPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Sidecar next to the destination, not a shared ".hidden" name in the folder.
// Horizon's SD FAT rejects some leading-dot names, and one temp per directory
// made the second file in the same folder fail O_EXCL.
std::string copyTemporaryPath(const std::string& destination,
                              const std::string& taskId) {
    const std::string suffix =
        ".pipensx-part-" + taskId.substr(0, std::min<size_t>(8, taskId.size()));
    std::string temporary = destination + suffix;
    const size_t slash = temporary.find_last_of('/');
    const size_t base = slash == std::string::npos ? 0 : slash + 1;
    if (temporary.size() - base <= 255)
        return temporary;
    return parentPath(destination) + "/pipensx-part-" +
           taskId.substr(0, std::min<size_t>(8, taskId.size()));
}

std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

std::string bint(uint64_t value) {
    return "i" + std::to_string(value) + "e";
}

bool atomicWrite(const std::string& path, const std::string& blob) {
    const std::string directory = parentPath(path);
    if (!directory.empty() && !mkdirs(directory))
        return false;
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    bool ok = std::fwrite(blob.data(), 1, blob.size(), file) == blob.size();
    ok = std::fflush(file) == 0 && ok;
#if !defined(_WIN32)
    if (ok)
        fsync(fileno(file));
#endif
    ok = std::fclose(file) == 0 && ok;
    if (!ok || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

std::string receiptPath(const std::string& root, const std::string& taskId) {
    return root + "/deployments/" + taskId + ".bencode";
}

std::string autoCopyPath(const std::string& root, const std::string& taskId) {
    return root + "/deployments/" + taskId + ".auto";
}

std::string jobPath(const std::string& root) {
    return root + "/deploy-job.bencode";
}

std::string moveJobPath(const std::string& root) {
    return root + "/deploy-move-job.bencode";
}

struct MovePair {
    std::string source;
    std::string destination;
    uint64_t size = 0;
};

bool saveMoveJob(const std::string& root, const std::string& taskId,
                 const std::vector<SwitchDeployEntry>& entries) {
    std::string blob = "d5:filesl";
    for (const SwitchDeployEntry& entry : entries) {
        if (!entry.moveSource ||
            entry.state == SwitchDeployEntryState::ExistingIdentical)
            continue;
        blob += "d4:dest" + bstr(entry.destinationPath);
        blob += "4:size" + bint(entry.size);
        blob += "6:source" + bstr(entry.sourcePath) + "e";
    }
    blob += "e4:task" + bstr(taskId);
    blob += "7:version" + bint(kMoveJobVersion) + "e";
    return atomicWrite(moveJobPath(root), blob);
}

bool saveJob(const std::string& root, const std::string& taskId,
             const std::string& temporary) {
    std::string blob = "d4:task" + bstr(taskId);
    blob += "4:temp" + bstr(temporary);
    blob += "7:version" + bint(kJobVersion) + "e";
    return atomicWrite(jobPath(root), blob);
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

bool readBlob(const std::string& path, std::string& blob) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxStateBytes)
        return false;
    input.seekg(0, std::ios::beg);
    blob.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    return true;
}

bool logicalFilePresent(const std::string& path, uint64_t expectedSize);

bool loadMoveJob(const std::string& root, std::string& taskId,
                 std::vector<MovePair>& pairs) {
    std::string blob;
    if (!readBlob(moveJobPath(root), blob))
        return false;
    const char* cursor = blob.data();
    const char* end = cursor + blob.size();
    be_node_t rootNode;
    uint64_t version = 0;
    be_node_t files;
    if (!be_decode(&cursor, end, &rootNode) || cursor != end ||
        rootNode.type != BE_DICT ||
        !readInteger(rootNode, "version", version) ||
        version != static_cast<uint64_t>(kMoveJobVersion) ||
        !readString(rootNode, "task", taskId) ||
        !be_dict_get(rootNode.buf, rootNode.buf + rootNode.raw_len,
                     "files", 5, &files) || files.type != BE_LIST)
        return false;
    std::vector<MovePair> parsed;
    const char* itemCursor = files.buf + 1;
    const char* itemEnd = files.buf + files.raw_len - 1;
    be_node_t item;
    while (be_list_next(&itemCursor, itemEnd, &item)) {
        MovePair pair;
        if (parsed.size() >= kMaxReceiptUnpacked || item.type != BE_DICT ||
            !readString(item, "source", pair.source) ||
            !readString(item, "dest", pair.destination) ||
            !readInteger(item, "size", pair.size))
            return false;
        parsed.push_back(std::move(pair));
    }
    pairs = std::move(parsed);
    return true;
}

bool rollbackMoveJob(const std::string& appRoot,
                     const std::string& downloadRoot,
                     const std::string& sdRoot) {
    std::string taskId;
    std::vector<MovePair> pairs;
    if (!loadMoveJob(appRoot, taskId, pairs))
        return false;
    bool ok = true;
    for (auto it = pairs.rbegin(); it != pairs.rend(); ++it) {
        if (!managedChild(downloadRoot, it->source) ||
            !managedChild(sdRoot, it->destination)) {
            ok = false;
            continue;
        }
        struct stat source {};
        struct stat destination {};
        const bool sourceExists = lstat(it->source.c_str(), &source) == 0;
        const bool destinationExists =
            lstat(it->destination.c_str(), &destination) == 0;
        if (sourceExists && !destinationExists)
            continue; // This intent had not been committed yet.
        if (!sourceExists && destinationExists &&
            logicalFilePresent(it->destination, it->size)) {
            if (!mkdirs(parentPath(it->source)) ||
                std::rename(it->destination.c_str(), it->source.c_str()) != 0)
                ok = false;
            continue;
        }
        ok = false;
    }
    if (ok)
        std::remove(moveJobPath(appRoot).c_str());
    return ok;
}

bool saveReceipt(const std::string& root, const SwitchDeployPlan& plan,
                 const std::vector<std::string>& unpacked,
                 const std::vector<std::string>& titleIds) {
    std::string blob = "d5:filesl";
    for (const SwitchDeployEntry& entry : plan.files) {
        blob += "d6:digest";
        blob += bstr(std::string(reinterpret_cast<const char*>(
                                     entry.sha256.data()),
                                 entry.sha256.size()));
        blob += "4:path" + bstr(entry.destinationRelativePath);
        blob += "4:root" + bstr(receiptTargetName(entry.target));
        blob += "4:size" + bint(entry.size) + "e";
    }
    blob += "e4:task" + bstr(plan.taskId);
    blob += "8:unpackedl";
    size_t unpackedCount = 0;
    for (const std::string& path : unpacked) {
        if (unpackedCount >= kMaxReceiptUnpacked)
            break;
        blob += bstr(path);
        ++unpackedCount;
    }
    blob += "e5:titlel";
    for (const std::string& id : titleIds)
        blob += bstr(id);
    blob += "e7:version" + bint(kReceiptVersion) + "e";
    return atomicWrite(receiptPath(root, plan.taskId), blob);
}

// Loads a deployment receipt. Version 1 receipts have no unpacked member
// list — *unpackedKnown reports whether the receipt carried one (v2), which
// tells the uninstall plan whether it must rebuild the list from the
// archive headers in the task data.
bool loadReceipt(const std::string& root, const std::string& taskId,
                 std::vector<ReceiptFile>& files,
                 std::vector<std::string>& unpacked,
                 bool* unpackedKnown = nullptr,
                 std::vector<std::string>* titleIds = nullptr) {
    if (unpackedKnown)
        *unpackedKnown = false;
    unpacked.clear();
    std::string blob;
    if (!readBlob(receiptPath(root, taskId), blob))
        return false;
    const char* cursor = blob.data();
    const char* end = cursor + blob.size();
    be_node_t rootNode;
    if (!be_decode(&cursor, end, &rootNode) || cursor != end ||
        rootNode.type != BE_DICT)
        return false;
    uint64_t version = 0;
    std::string storedTask;
    be_node_t list;
    if (!readInteger(rootNode, "version", version) ||
        version < 1 || version > static_cast<uint64_t>(kReceiptVersion) ||
        !readString(rootNode, "task", storedTask) || storedTask != taskId ||
        !be_dict_get(rootNode.buf, rootNode.buf + rootNode.raw_len, "files", 5,
                     &list) || list.type != BE_LIST)
        return false;
    std::vector<ReceiptFile> parsed;
    const char* itemCursor = list.buf + 1;
    const char* itemEnd = list.buf + list.raw_len - 1;
    be_node_t item;
    while (be_list_next(&itemCursor, itemEnd, &item)) {
        if (parsed.size() >= kMaxReceiptUnpacked)
            return false;
        ReceiptFile file;
        std::string digest;
        uint64_t size = 0;
        if (item.type != BE_DICT || !readString(item, "digest", digest) ||
            digest.size() != file.digest.size() ||
            !readString(item, "path", file.path) ||
            !taskFilePathIsFatCompatible(file.path) ||
            !readInteger(item, "size", size))
            return false;
        if (version >= 4) {
            std::string target;
            if (!readString(item, "root", target))
                return false;
            if (target == "sd")
                file.target = SwitchDeployTarget::SdRoot;
            else if (target != "switch")
                return false;
        }
        file.size = size;
        std::memcpy(file.digest.data(), digest.data(), digest.size());
        parsed.push_back(std::move(file));
    }
    if (version >= 2) {
        be_node_t unpackedList;
        if (!be_dict_get(rootNode.buf, rootNode.buf + rootNode.raw_len,
                         "unpacked", 8, &unpackedList) ||
            unpackedList.type != BE_LIST)
            return false;
        const char* unpackedCursor = unpackedList.buf + 1;
        const char* unpackedEnd = unpackedList.buf + unpackedList.raw_len - 1;
        be_node_t entry;
        while (be_list_next(&unpackedCursor, unpackedEnd, &entry)) {
            if (unpacked.size() >= kMaxReceiptUnpacked)
                return false;
            if (entry.type != BE_STR || entry.slen == 0)
                return false;
            const std::string path(entry.sval, entry.slen);
            if (!taskFilePathIsFatCompatible(path))
                return false;
            unpacked.push_back(path);
        }
        if (unpackedKnown)
            *unpackedKnown = true;
    }
    files = std::move(parsed);
    if (titleIds) {
        titleIds->clear();
        be_node_t titleList;
        if (be_dict_get(rootNode.buf, rootNode.buf + rootNode.raw_len,
                        "title", 5, &titleList)) {
            if (titleList.type != BE_LIST)
                return false;
            const char* titleCursor = titleList.buf + 1;
            const char* titleEnd = titleList.buf + titleList.raw_len - 1;
            be_node_t titleEntry;
            while (be_list_next(&titleCursor, titleEnd, &titleEntry)) {
                if (titleIds->size() >= 64 || titleEntry.type != BE_STR ||
                    titleEntry.slen == 0)
                    return false;
                titleIds->emplace_back(titleEntry.sval, titleEntry.slen);
            }
        }
    }
    return true;
}

constexpr uint64_t kFat32FileMax = 0xFFFFFFFFu;
constexpr uint64_t kSplitPartBytes = 0xFFFF0000u;

bool splitFolderParts(const std::string& path, uint64_t expectedSize,
                      std::vector<std::string>& parts) {
    parts.clear();
    if (expectedSize <= kFat32FileMax)
        return false;
    struct stat root {};
    if (lstat(path.c_str(), &root) != 0 || !S_ISDIR(root.st_mode) ||
        S_ISLNK(root.st_mode))
        return false;
    std::map<uint32_t, std::string> indexed;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    while (struct dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        const std::string name = entry->d_name;
        if (name.empty() || name.size() > 10 ||
            !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                return ch >= '0' && ch <= '9';
            })) {
            ok = false;
            break;
        }
        errno = 0;
        char* numberEnd = nullptr;
        const unsigned long number = std::strtoul(name.c_str(), &numberEnd,
                                                   10);
        if (errno != 0 || !numberEnd || *numberEnd != '\0' ||
            number > UINT32_MAX ||
            !indexed.emplace(static_cast<uint32_t>(number), name).second) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    const uint64_t count =
        (expectedSize + kSplitPartBytes - 1) / kSplitPartBytes;
    if (!ok || indexed.size() != count)
        return false;
    parts.reserve(indexed.size());
    for (uint32_t i = 0; i < indexed.size(); ++i) {
        auto found = indexed.find(i);
        if (found == indexed.end())
            return false;
        const std::string part = path + "/" + found->second;
        struct stat st {};
        const uint64_t wanted = std::min<uint64_t>(
            kSplitPartBytes, expectedSize - static_cast<uint64_t>(i) *
                                      kSplitPartBytes);
        if (lstat(part.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
            S_ISLNK(st.st_mode) ||
            static_cast<uint64_t>(st.st_size) != wanted)
            return false;
        parts.push_back(part);
    }
    return true;
}

bool logicalFilePresent(const std::string& path, uint64_t expectedSize) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
        return false;
    if (S_ISREG(st.st_mode))
        return static_cast<uint64_t>(st.st_size) == expectedSize;
    std::vector<std::string> parts;
    return splitFolderParts(path, expectedSize, parts);
}

bool hashLogicalFile(const std::string& path, uint64_t expectedSize,
                     std::array<uint8_t, 32>& digest,
                     std::atomic<bool>* cancelled = nullptr,
                     const std::function<void(uint64_t)>* progress = nullptr) {
    std::vector<std::string> paths;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
        return false;
    if (S_ISREG(st.st_mode)) {
        if (static_cast<uint64_t>(st.st_size) != expectedSize)
            return false;
        paths.push_back(path);
    } else if (!splitFolderParts(path, expectedSize, paths)) {
        return false;
    }
    sha256_ctx_t context;
    sha256_init(&context);
    std::vector<uint8_t> buffer(kCopyBufferBytes);
    uint64_t total = 0;
    for (const std::string& part : paths) {
        std::FILE* input = std::fopen(part.c_str(), "rb");
        if (!input)
            return false;
        bool ok = true;
        while (!cancelled || !cancelled->load(std::memory_order_relaxed)) {
            const size_t count =
                std::fread(buffer.data(), 1, buffer.size(), input);
            if (count == 0)
                break;
            sha256_update(&context, buffer.data(), count);
            total += count;
            if (progress)
                (*progress)(count);
            std::this_thread::yield();
        }
        if (std::ferror(input) != 0 || std::fclose(input) != 0)
            ok = false;
        if (!ok || (cancelled &&
                    cancelled->load(std::memory_order_relaxed)))
            return false;
    }
    if (total != expectedSize)
        return false;
    sha256_final(&context, digest.data());
    return true;
}

bool sourceFileSafe(const TaskFileInventory& inventory,
                    const TaskFileInfo& file) {
    return file.state == TaskFileState::Present &&
           !file.absolutePath.empty() &&
           managedChild(inventory.rootPath, file.absolutePath) &&
           logicalFilePresent(file.absolutePath, file.size);
}

void setProblem(SwitchDeployInspection& result, SwitchDeployProblem problem,
                std::string detail) {
    if (result.problem == SwitchDeployProblem::None) {
        result.problem = problem;
        result.detail = std::move(detail);
    }
}

bool copyFile(const SwitchDeployEntry& entry, const std::string& appRoot,
              const std::string& taskId, std::atomic<bool>& cancelled,
              const std::function<void(uint64_t)>& progress,
              std::array<uint8_t, 32>& digest, std::string& error) {
    const std::string parent = parentPath(entry.destinationPath);
    if (!mkdirs(parent)) {
        error = "Unable to create the destination directory.";
        return false;
    }
    const std::string temporary =
        copyTemporaryPath(entry.destinationPath, taskId);
    if (!saveJob(appRoot, taskId, temporary)) {
        error = "Unable to save the copy recovery journal.";
        return false;
    }
    std::FILE* input = std::fopen(entry.sourcePath.c_str(), "rb");
    if (!input) {
        error = std::string("Unable to open a file for copying (") +
                std::strerror(errno) + ").";
        return false;
    }
    int outputFd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (outputFd < 0 && errno == EEXIST) {
        std::remove(temporary.c_str());
        outputFd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    }
    if (outputFd < 0) {
        const int createdErrno = errno;
        std::fclose(input);
        error = std::string("Unable to create the temporary copy file (") +
                std::strerror(createdErrno) + ").";
        return false;
    }
    std::FILE* output = fdopen(outputFd, "wb");
    if (!output) {
        close(outputFd);
        std::fclose(input);
        std::remove(temporary.c_str());
        error = "Unable to open a file for copying.";
        return false;
    }
    sha256_ctx_t context;
    sha256_init(&context);
    std::vector<uint8_t> buffer(kCopyBufferBytes);
    uint64_t written = 0;
    bool ok = true;
    while (!cancelled.load(std::memory_order_relaxed)) {
        const size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count == 0)
            break;
        if (std::fwrite(buffer.data(), 1, count, output) != count) {
            ok = false;
            break;
        }
        sha256_update(&context, buffer.data(), count);
        written += count;
        progress(count);
        std::this_thread::yield();
    }
    if (std::ferror(input) != 0 || written != entry.size)
        ok = false;
    if (std::fflush(output) != 0)
        ok = false;
#if !defined(_WIN32)
    if (ok && fsync(fileno(output)) != 0 && errno != EINVAL &&
        errno != ENOTSUP)
        ok = false;
#endif
    if (std::fclose(input) != 0)
        ok = false;
    if (std::fclose(output) != 0)
        ok = false;
    if (cancelled.load(std::memory_order_relaxed)) {
        std::remove(temporary.c_str());
        return false;
    }
    if (!ok) {
        std::remove(temporary.c_str());
        error = "Unable to copy the complete file.";
        return false;
    }
    sha256_final(&context, digest.data());
    struct stat destination {};
    if (lstat(entry.destinationPath.c_str(), &destination) == 0 ||
        errno != ENOENT) {
        std::remove(temporary.c_str());
        error = "A destination file appeared during copying.";
        return false;
    }
#ifdef __SWITCH__
    if (std::rename(temporary.c_str(), entry.destinationPath.c_str()) != 0) {
#else
    if (link(temporary.c_str(), entry.destinationPath.c_str()) != 0) {
#endif
        std::remove(temporary.c_str());
        error = "Unable to commit the copied file.";
        return false;
    }
#ifndef __SWITCH__
    std::remove(temporary.c_str());
#endif
    std::remove(jobPath(appRoot).c_str());
    return true;
}

bool moveFile(const SwitchDeployEntry& entry,
              std::atomic<bool>& cancelled,
              const std::function<void(uint64_t)>& progress,
              std::array<uint8_t, 32>& digest, std::string& error) {
    if (cancelled.load(std::memory_order_relaxed))
        return false;
    if (!hashLogicalFile(entry.sourcePath, entry.size, digest, &cancelled,
                         &progress)) {
        if (!cancelled.load(std::memory_order_relaxed))
            error = "Unable to hash the complete file before moving it.";
        return false;
    }
    if (cancelled.load(std::memory_order_relaxed))
        return false;
    if (!mkdirs(parentPath(entry.destinationPath))) {
        error = "Unable to create the LayeredFS destination directory.";
        return false;
    }
    struct stat destination {};
    if (lstat(entry.destinationPath.c_str(), &destination) == 0 ||
        errno != ENOENT) {
        error = "A LayeredFS destination file appeared before the move.";
        return false;
    }
    if (std::rename(entry.sourcePath.c_str(), entry.destinationPath.c_str()) !=
        0) {
        error = std::string("Unable to atomically move a LayeredFS file (") +
                std::strerror(errno) + ").";
        return false;
    }
    return true;
}

bool titleIdMatches(uint64_t actual, const std::vector<std::string>& expected) {
    char text[17] {};
    std::snprintf(text, sizeof(text), "%016llX",
                  static_cast<unsigned long long>(actual));
    for (const std::string& id : expected)
        if (asciiEqual(text, id))
            return true;
    return false;
}

bool installLocalPackage(
    install::InstallBackend& backend, const std::string& taskId,
    const SwitchDeployPackage& package,
    const std::vector<std::string>& expectedTitleIds,
    std::atomic<bool>& cancelled,
    const std::function<void(uint64_t, uint64_t, DownloadStatus)>& progress,
    std::string& error) {
    if (!backend.beginPackage(taskId, package.sourceRelativePath)) {
        error = backend.error();
        return false;
    }
    install::PackageCallbacks callbacks;
    callbacks.skipFile = [&backend](const std::string& name) {
        return backend.shouldSkipFile(name);
    };
    callbacks.beginFile = [&backend, &error](const std::string& name,
                                              uint64_t size) {
        if (backend.beginFile(name, size))
            return true;
        error = backend.error();
        return false;
    };
    callbacks.setFileSize = [&backend, &error](uint64_t size) {
        if (backend.setFileSize(size))
            return true;
        error = backend.error();
        return false;
    };
    callbacks.writeFile = [&backend, &cancelled, &progress,
                           &error](const uint8_t* bytes, size_t size) {
        if (cancelled.load(std::memory_order_relaxed))
            return false;
        if (!backend.writeFile(bytes, size)) {
            error = backend.error();
            return false;
        }
        if (progress)
            progress(backend.installedBytes(), backend.expectedBytes(),
                     DownloadStatus::Installing);
        return true;
    };
    callbacks.endFile = [&backend, &error] {
        if (backend.endFile())
            return true;
        error = backend.error();
        return false;
    };
    install::PackageStream stream(package.compressed, std::move(callbacks),
                                  taskId);
    std::FILE* input = std::fopen(package.sourcePath.c_str(), "rb");
    if (!input) {
        backend.rollbackPackage();
        error = "Unable to open the downloaded package.";
        return false;
    }
    std::vector<uint8_t> buffer(1u << 20);
    uint64_t consumed = 0;
    bool ok = true;
    while (!cancelled.load(std::memory_order_relaxed)) {
        const size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count == 0)
            break;
        consumed += count;
        if (!stream.write(buffer.data(), count)) {
            ok = false;
            break;
        }
    }
    ok = std::ferror(input) == 0 && consumed == package.size && ok;
    if (std::fclose(input) != 0)
        ok = false;
    if (cancelled.load(std::memory_order_relaxed)) {
        backend.rollbackPackage();
        error = "Cancelled.";
        return false;
    }
    if (!ok || !stream.finish()) {
        if (error.empty())
            error = stream.error().empty() ? "Unable to read the complete package."
                                           : stream.error();
        backend.rollbackPackage();
        return false;
    }
    if (!expectedTitleIds.empty()) {
        const uint64_t actual = backend.packageApplicationId();
        if (actual == 0 || !titleIdMatches(actual, expectedTitleIds)) {
            error = actual == 0
                ? "Unable to verify the package title id from its CNMT."
                : "The package CNMT title id does not match the LayeredFS payload.";
            backend.rollbackPackage();
            return false;
        }
    }
    if (progress)
        progress(backend.installedBytes(), backend.expectedBytes(),
                 DownloadStatus::Committing);
    bool alreadyInstalled = false;
    if (!backend.commitPackage(alreadyInstalled)) {
        error = backend.error();
        backend.rollbackPackage();
        return false;
    }
    return true;
}

std::string receiptTopFolder(const std::string& relative) {
    const size_t slash = relative.find('/');
    return slash == std::string::npos ? relative : relative.substr(0, slash);
}

// Removes the parent chain of `path` under `root` while the directories are
// empty; never removes `root` itself. Stops at the first non-empty or
// failing directory, so a folder holding files outside the receipt survives.
void pruneEmptyParents(const std::string& root, std::string path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return;
    path.erase(slash);
    while (path.size() > root.size() && path.rfind(root + "/", 0) == 0) {
        if (rmdir(path.c_str()) != 0)
            return;
        const size_t next = path.find_last_of('/');
        if (next == std::string::npos)
            return;
        path.erase(next);
    }
}

// Recursive best-effort delete used only for whole-folder removal (the v1
// receipt whose archive is gone). Never follows symlinks.
bool scanDestinationFiles(const std::string& root,
                          const std::string& relative,
                          std::set<std::string>& files,
                          const std::set<std::string>* logicalFiles = nullptr) {
    const std::string path = relative.empty() ? root : root + "/" + relative;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return errno == ENOENT;
    bool ok = true;
    while (struct dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        const std::string childRelative = relative.empty()
            ? entry->d_name : relative + "/" + entry->d_name;
        const std::string child = root + "/" + childRelative;
        struct stat st {};
        if (lstat(child.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            const std::string folded = lowerAscii(childRelative);
            if (logicalFiles && logicalFiles->count(folded)) {
                files.insert(std::move(folded));
            } else if (!scanDestinationFiles(root, childRelative, files,
                                             logicalFiles)) {
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            files.insert(lowerAscii(childRelative));
        } else {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

bool regularPathExists(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           !S_ISLNK(st.st_mode);
}

bool directoryPathExists(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
           !S_ISLNK(st.st_mode);
}

bool configContainsTitle(const std::string& path,
                         const std::vector<std::string>& titleIds) {
    std::string config;
    if (!readBlob(path, config))
        return false;
    config = lowerAscii(std::move(config));
    for (const std::string& id : titleIds)
        if (config.find(lowerAscii(id)) != std::string::npos)
            return true;
    return false;
}

void detectPerformanceState(const std::string& sdRoot,
                            const std::vector<std::string>& titleIds,
                            bool& toolDetected, bool& profileDetected) {
    const std::string sysClkConfig = sdRoot + "/config/sys-clk/config.ini";
    toolDetected = regularPathExists(sysClkConfig) ||
                   directoryPathExists(
                       sdRoot + "/atmosphere/contents/00FF0000636C6BFF");
    profileDetected =
        configContainsTitle(sysClkConfig, titleIds);
}

bool removeTreeBestEffort(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return std::remove(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        if (!removeTreeBestEffort(path + "/" + entry->d_name))
            ok = false;
    }
    closedir(dir);
    if (!ok)
        return false;
    return rmdir(path.c_str()) == 0;
}

} // namespace

SwitchDeployInspection inspectSwitchDeploy(TaskFileInventory inventory,
                                           const std::string& targetRoot) {
    SwitchDeployInspection result;
    result.inventory = std::move(inventory);
    result.plan.taskId = result.inventory.taskId;
    result.plan.targetRoot = targetRoot;
    const std::string sdRoot = sdRootForSwitchRoot(targetRoot);
    if (!result.inventory.settled) {
        setProblem(result, SwitchDeployProblem::NotReady,
                   "Finish the download before copying files to /switch.");
        return result;
    }
    if (!result.inventory.completeManifest) {
        setProblem(result, SwitchDeployProblem::NotReady,
                   "The downloaded file list is unavailable.");
        return result;
    }
    if (result.inventory.files.empty()) {
        setProblem(result, SwitchDeployProblem::LayoutNotFound,
                   "No downloaded files were found.");
        return result;
    }
    for (const TaskFileInfo& file : result.inventory.files) {
        if (file.state == TaskFileState::Unsafe) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       "The download contains a symlink or unsafe file.");
            return result;
        }
    }

    // Unified loose-payload discovery: every directory that directly owns a
    // valid NRO is an application root. The old implementation searched for
    // one ancestor literally named `switch`, which made equivalent folder and
    // archive layouts behave differently and rejected multi-port releases.
    std::map<std::string, std::string> roots;
    for (const TaskFileInfo& file : result.inventory.files) {
        if (file.action != TaskFileAction::Download || file.package ||
            file.cartridge || file.state != TaskFileState::Present ||
            !hasNroExtension(file.logicalPath) ||
            !sourceFileSafe(result.inventory, file) ||
            !validNro(file.absolutePath))
            continue;
        const std::vector<std::string> parts = splitPath(file.logicalPath);
        if (parts.empty())
            continue;
        const std::string root = joinPath(parts, 0, parts.size() - 1);
        roots.emplace(lowerAscii(root), root);
    }
    for (const TaskFileInfo& file : result.inventory.files) {
        if (!file.package || file.action != TaskFileAction::Download ||
            file.state != TaskFileState::Present ||
            !sourceFileSafe(result.inventory, file))
            continue;
        SwitchDeployPackage package;
        package.sourcePath = file.absolutePath;
        package.sourceRelativePath = file.logicalPath;
        package.size = file.size;
        package.compressed = file.compressed ||
                             isCompressedName(file.logicalPath);
        result.plan.packages.push_back(std::move(package));
    }

    std::map<std::string, std::string> plannedFiles;
    std::set<std::string> layeredExpected;
    std::map<std::string, std::string> layeredRomfsRoots;
    std::set<std::string> layeredIds;
    for (const TaskFileInfo& file : result.inventory.files) {
        size_t atmosphereOffset = 0;
        std::string titleId;
        if (file.package || file.cartridge ||
            !isLayeredFsRomfsPath(file.logicalPath, &atmosphereOffset,
                                  &titleId))
            continue;
        if (file.action != TaskFileAction::Download) {
            ++result.plan.ignoredFiles;
            continue;
        }
        if (file.state != TaskFileState::Present ||
            !sourceFileSafe(result.inventory, file)) {
            setProblem(result, SwitchDeployProblem::MissingSource,
                       file.logicalPath);
            return result;
        }
        std::string destinationRelative =
            file.logicalPath.substr(atmosphereOffset);
        std::replace(destinationRelative.begin(), destinationRelative.end(),
                     '\\', '/');
        if (!taskFilePathIsFatCompatible(destinationRelative)) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       file.logicalPath);
            return result;
        }
        for (char& ch : titleId)
            if (ch >= 'a' && ch <= 'f')
                ch = static_cast<char>(ch - 'a' + 'A');
        layeredIds.insert(titleId);
        const std::vector<std::string> destinationParts =
            splitPath(destinationRelative);
        if (destinationParts.size() < 5) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       file.logicalPath);
            return result;
        }
        const std::string romfsRoot = joinPath(destinationParts, 0, 4);
        layeredRomfsRoots.emplace(lowerAscii(romfsRoot), romfsRoot);
        const std::string foldedDestination = lowerAscii(destinationRelative);
        const std::string plannedKey = "sd:" + foldedDestination;
        if (!plannedFiles.emplace(plannedKey, destinationRelative).second) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       "LayeredFS destination paths collide on FAT.");
            return result;
        }
        layeredExpected.insert(foldedDestination);

        SwitchDeployEntry entry;
        entry.sourcePath = file.absolutePath;
        entry.sourceRelativePath = file.logicalPath;
        entry.destinationRelativePath = destinationRelative;
        entry.destinationPath = sdRoot + "/" + destinationRelative;
        entry.size = file.size;
        entry.target = SwitchDeployTarget::SdRoot;
        entry.moveSource = true;
        result.plan.totalBytes += entry.size;
        result.plan.bytesToMove += entry.size;
        if (!destinationParentsSafe(sdRoot, destinationRelative)) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       destinationRelative);
            return result;
        }
        struct stat destination {};
        if (lstat(entry.destinationPath.c_str(), &destination) != 0) {
            if (errno != ENOENT) {
                setProblem(result, SwitchDeployProblem::Io,
                           entry.destinationPath);
                return result;
            }
            entry.state = SwitchDeployEntryState::Missing;
        } else if (!logicalFilePresent(entry.destinationPath, entry.size)) {
            entry.state = SwitchDeployEntryState::ExistingConflict;
            ++result.plan.conflictFiles;
        } else {
            std::array<uint8_t, 32> destinationDigest {};
            if (!hashLogicalFile(entry.sourcePath, entry.size, entry.sha256) ||
                !hashLogicalFile(entry.destinationPath, entry.size,
                                 destinationDigest)) {
                setProblem(result, SwitchDeployProblem::Io,
                           "Unable to hash an existing LayeredFS file.");
                return result;
            }
            if (entry.sha256 == destinationDigest) {
                entry.state = SwitchDeployEntryState::ExistingIdentical;
                ++result.plan.identicalFiles;
                result.plan.bytesToMove -= entry.size;
            } else {
                entry.state = SwitchDeployEntryState::ExistingConflict;
                ++result.plan.conflictFiles;
            }
        }
        result.plan.files.push_back(std::move(entry));
    }
    if (!layeredIds.empty()) {
        result.plan.layeredFs = true;
        result.plan.layeredTitleIds.assign(layeredIds.begin(),
                                           layeredIds.end());
        for (const auto& root : layeredRomfsRoots) {
            std::set<std::string> existing;
            if (!scanDestinationFiles(sdRoot, root.second, existing,
                                      &layeredExpected)) {
                setProblem(result, SwitchDeployProblem::UnsafePath,
                           "The existing LayeredFS tree is unsafe.");
                return result;
            }
            for (const std::string& path : existing) {
                if (layeredExpected.count(path) == 0) {
                    ++result.plan.conflictFiles;
                    break;
                }
            }
        }
        detectPerformanceState(sdRoot, result.plan.layeredTitleIds,
                               result.plan.performanceToolDetected,
                               result.plan.performanceProfileDetected);
    }
    for (const TaskFileInfo& file : result.inventory.files) {
        if (file.action != TaskFileAction::Download || file.package ||
            file.cartridge || file.state != TaskFileState::Present ||
            !isPortArchiveName(file.logicalPath) ||
            !sourceFileSafe(result.inventory, file))
            continue;
        SwitchDeployArchive archive;
        archive.sourcePath = file.absolutePath;
        archive.sourceRelativePath = file.logicalPath;
        archive.size = file.size;
        PortArchiveProbe probe;
        if (probePortArchive(file.absolutePath, probe)) {
            archive.unpackBytes = probe.unpackBytes;
            archive.maxSolidBlockBytes = probe.maxSolidBlockBytes;
            archive.switchFiles = probe.switchFiles;
            archive.destinationRelativePaths = probe.files;
            archive.extractable = true;
            for (const std::string& relative : probe.files) {
                const std::string folded = "switch:" + lowerAscii(relative);
                auto duplicate = plannedFiles.find(folded);
                if (duplicate != plannedFiles.end()) {
                    setProblem(result, SwitchDeployProblem::UnsafePath,
                               "Payload destination paths collide on FAT.");
                    return result;
                }
                plannedFiles.emplace(folded, relative);
                if (!destinationParentsSafe(targetRoot, relative)) {
                    setProblem(result, SwitchDeployProblem::UnsafePath,
                               relative);
                    return result;
                }
                struct stat destination {};
                if (lstat((targetRoot + "/" + relative).c_str(),
                          &destination) == 0) {
                    ++result.plan.conflictFiles;
                } else if (errno != ENOENT) {
                    setProblem(result, SwitchDeployProblem::Io, relative);
                    return result;
                }
            }
        } else {
            archive.extractable = false;
            archive.detail = probe.error;
        }
        result.plan.archives.push_back(std::move(archive));
        result.plan.totalBytes += file.size;
        if (result.plan.archives.back().extractable) {
            const uint64_t need = result.plan.archives.back().unpackBytes
                ? result.plan.archives.back().unpackBytes
                : file.size;
            result.plan.bytesToCopy += need;
        }
    }
    const bool hasExtractableArchive = std::any_of(
        result.plan.archives.begin(), result.plan.archives.end(),
        [](const SwitchDeployArchive& archive) { return archive.extractable; });
    if (roots.empty() && !hasExtractableArchive &&
        !result.plan.layeredFs) {
        bool hasPackagePayload = false;
        bool hasLooseFiles = false;
        for (const TaskFileInfo& file : result.inventory.files) {
            if (file.package &&
                (file.action == TaskFileAction::Download ||
                 file.action == TaskFileAction::Install))
                hasPackagePayload = true;
            if (!file.package && !file.cartridge &&
                file.action == TaskFileAction::Download &&
                (file.state == TaskFileState::Present ||
                 file.state == TaskFileState::Installed) &&
                !isPortArchiveName(file.logicalPath))
                hasLooseFiles = true;
        }
        if (hasPackagePayload && !hasLooseFiles) {
            setProblem(result, SwitchDeployProblem::NotAPort,
                       "This download contains native packages only.");
        } else if (!hasLooseFiles && !result.plan.archives.empty()) {
            setProblem(result, SwitchDeployProblem::NotAPort,
                       "The selected archives contain no NRO port payload.");
        } else {
            setProblem(result, SwitchDeployProblem::LayoutNotFound,
                       "A downloadable application directory with a valid "
                       "NRO was not found.");
        }
        return result;
    }
    if (roots.empty() && !result.plan.layeredFs) {
        if (result.plan.conflictFiles != 0) {
            setProblem(result, SwitchDeployProblem::Conflict,
                       "Existing destination files differ from the download.");
            return result;
        }
        const StorageSpaceSnapshot storage = queryStorageSpace(targetRoot);
        if (!storage.available) {
            setProblem(result, SwitchDeployProblem::Io, storage.error);
            return result;
        }
        result.plan.freeBytes = storage.freeBytes;
        if (result.plan.bytesToCopy > storage.freeBytes) {
            setProblem(result, SwitchDeployProblem::NoSpace,
                       "There is not enough free space on the SD card.");
            return result;
        }
        return result;
    }
    struct LayoutPath {
        std::string spelling;
        bool file = false;
    };
    std::map<std::string, LayoutPath> layoutPaths;
    for (const TaskFileInfo& file : result.inventory.files) {
        const std::vector<std::string> parts = splitPath(file.logicalPath);
        const std::string foldedLogical = lowerAscii(file.logicalPath);
        const std::pair<const std::string, std::string>* selectedRoot = nullptr;
        for (const auto& root : roots) {
            const bool inside = root.first.empty() ||
                foldedLogical == root.first ||
                foldedLogical.rfind(root.first + "/", 0) == 0;
            if (inside && (!selectedRoot ||
                           root.first.size() > selectedRoot->first.size()))
                selectedRoot = &root;
        }
        if (!selectedRoot) {
            if (file.action == TaskFileAction::Download &&
                !isPortArchiveName(file.logicalPath) &&
                !isLayeredFsRomfsPath(file.logicalPath))
                ++result.plan.ignoredFiles;
            continue;
        }
        if (file.action != TaskFileAction::Download || file.package ||
            file.cartridge) {
            ++result.plan.ignoredFiles;
            continue;
        }
        if (file.state != TaskFileState::Present ||
            !sourceFileSafe(result.inventory, file)) {
            /* 0-byte / dotfiles (LainNX `.cosmo`) are never created on disk.
               Aborting the whole port for one of those leaves the NRO
               uncopied. Skip anything that is not the NRO itself. */
            if (hasNroExtension(file.logicalPath)) {
                setProblem(result, SwitchDeployProblem::MissingSource,
                           file.logicalPath);
                return result;
            }
            ++result.plan.ignoredFiles;
            continue;
        }
        const std::vector<std::string> rootParts =
            splitPath(selectedRoot->second);
        const size_t sourceBegin = selectedRoot->second.empty()
            ? 0 : rootParts.size();
        std::string destinationRelative;
        if (selectedRoot->second.empty()) {
            destinationRelative = joinPath(parts, 0, parts.size());
        } else {
            destinationRelative = rootParts.back();
            const std::string suffix = joinPath(parts, sourceBegin,
                                                parts.size());
            if (!suffix.empty())
                destinationRelative += "/" + suffix;
        }
        if (!taskFilePathIsFatCompatible(destinationRelative)) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       file.logicalPath);
            return result;
        }
        const std::vector<std::string> destinationParts =
            splitPath(destinationRelative);
        if (destinationParts.empty() ||
            asciiEqual(destinationParts.front(), "pipensx")) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       "Writing inside the pipensx application directory is forbidden.");
            return result;
        }
        std::string layoutPath;
        for (size_t i = 0; i < destinationParts.size(); ++i) {
            if (!layoutPath.empty())
                layoutPath += '/';
            layoutPath += destinationParts[i];
            const bool isFile = i + 1 == destinationParts.size();
            const std::string folded = lowerAscii(layoutPath);
            auto collision = layoutPaths.find(folded);
            if (collision != layoutPaths.end()) {
                if (collision->second.spelling != layoutPath ||
                    collision->second.file != isFile || isFile) {
                    const char* detail =
                        collision->second.spelling != layoutPath
                            ? "Destination paths collide when case is ignored."
                            : collision->second.file == isFile
                                  ? "The layout contains a duplicate destination path."
                                  : "The layout contains a file/directory conflict.";
                    setProblem(result, SwitchDeployProblem::UnsafePath,
                               detail);
                    return result;
                }
            } else {
                layoutPaths.emplace(folded,
                                    LayoutPath{layoutPath, isFile});
            }
        }

        const std::string foldedDestination = lowerAscii(destinationRelative);
        const std::string plannedKey = "switch:" + foldedDestination;
        auto planned = plannedFiles.find(plannedKey);
        if (planned != plannedFiles.end()) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       "Payload destination paths collide on FAT.");
            return result;
        }
        plannedFiles.emplace(plannedKey, destinationRelative);

        SwitchDeployEntry entry;
        entry.sourcePath = file.absolutePath;
        entry.sourceRelativePath = file.logicalPath;
        entry.destinationRelativePath = destinationRelative;
        entry.destinationPath = targetRoot + "/" + destinationRelative;
        entry.size = file.size;
        entry.nro = hasNroExtension(file.logicalPath);
        result.plan.totalBytes += entry.size;
        if (!destinationParentsSafe(targetRoot, destinationRelative)) {
            setProblem(result, SwitchDeployProblem::UnsafePath,
                       destinationRelative);
            return result;
        }
        struct stat destination {};
        if (lstat(entry.destinationPath.c_str(), &destination) != 0) {
            if (errno != ENOENT) {
                setProblem(result, SwitchDeployProblem::Io,
                           entry.destinationPath);
                return result;
            }
            entry.state = SwitchDeployEntryState::Missing;
            result.plan.bytesToCopy += entry.size;
        } else if (!S_ISREG(destination.st_mode) ||
                   S_ISLNK(destination.st_mode) ||
                   static_cast<uint64_t>(destination.st_size) != entry.size) {
            entry.state = SwitchDeployEntryState::ExistingConflict;
            ++result.plan.conflictFiles;
        } else {
            std::array<uint8_t, 32> destinationDigest {};
            if (!hashFile(entry.sourcePath, entry.sha256) ||
                !hashFile(entry.destinationPath, destinationDigest)) {
                setProblem(result, SwitchDeployProblem::Io,
                           "Unable to hash an existing file.");
                return result;
            }
            if (entry.sha256 == destinationDigest) {
                entry.state = SwitchDeployEntryState::ExistingIdentical;
                ++result.plan.identicalFiles;
            } else {
                entry.state = SwitchDeployEntryState::ExistingConflict;
                ++result.plan.conflictFiles;
            }
        }
        result.plan.files.push_back(std::move(entry));
    }
    if (result.plan.files.empty() && result.plan.archives.empty()) {
        setProblem(result, SwitchDeployProblem::LayoutNotFound,
                   "The NRO application directories have no downloadable "
                   "files.");
        return result;
    }
    if (result.plan.conflictFiles != 0) {
        setProblem(result, SwitchDeployProblem::Conflict,
                   "Existing destination files differ from the download.");
        return result;
    }
    const StorageSpaceSnapshot storage = queryStorageSpace(targetRoot);
    if (!storage.available) {
        setProblem(result, SwitchDeployProblem::Io, storage.error);
        return result;
    }
    result.plan.freeBytes = storage.freeBytes;
    if (result.plan.bytesToCopy > storage.freeBytes) {
        setProblem(result, SwitchDeployProblem::NoSpace,
                   "There is not enough free space on the SD card.");
        return result;
    }
    return result;
}

SwitchDeployService::SwitchDeployService(DownloadManager& manager,
                                         std::string appRoot,
                                         std::string targetRoot)
    : manager_(manager), appRoot_(std::move(appRoot)),
      targetRoot_(std::move(targetRoot)) {
    while (targetRoot_.size() > 1 && targetRoot_.back() == '/')
        targetRoot_.pop_back();
    cleanupInterruptedJob();
}

SwitchDeployService::~SwitchDeployService() { shutdown(); }

SwitchDeployInspection SwitchDeployService::inspect(
    const std::string& taskId) const {
    const std::optional<DownloadTask> task = manager_.snapshot(taskId);
    if (!task) {
        SwitchDeployInspection result;
        result.problem = SwitchDeployProblem::TaskNotFound;
        result.detail = "Download task not found.";
        return result;
    }
    if (!taskReadyForSwitchDeploy(*task)) {
        SwitchDeployInspection result;
        result.problem = SwitchDeployProblem::NotReady;
        if (task->mode == TransferMode::StreamInstall &&
            task->status != DownloadStatus::Completed &&
            task->status != DownloadStatus::Installed) {
            result.detail =
                "Finish the download and installation before copying files "
                "to /switch.";
        } else {
            result.detail =
                "Finish the download before copying files to /switch.";
        }
        return result;
    }
    TaskFileInventory inventory;
    std::string error;
    if (!buildTaskFileInventory(appRoot_, *task, inventory, error)) {
        SwitchDeployInspection result;
        result.problem = SwitchDeployProblem::Io;
        result.detail = std::move(error);
        return result;
    }
    SwitchDeployInspection inspection =
        inspectSwitchDeploy(std::move(inventory), targetRoot_);
    if (task->mode == TransferMode::PortInstall &&
        receiptState(taskId) == SwitchDeployReceiptState::Valid) {
        inspection.problem = SwitchDeployProblem::None;
        inspection.detail.clear();
        inspection.plan.files.clear();
        inspection.plan.archives.clear();
        inspection.plan.bytesToCopy = 0;
        inspection.plan.bytesToMove = 0;
        inspection.plan.totalBytes = 0;
        inspection.plan.conflictFiles = 0;
    } else if (task->mode == TransferMode::PortInstall &&
               inspection.problem == SwitchDeployProblem::NotAPort &&
               !inspection.plan.packages.empty()) {
        inspection.problem = SwitchDeployProblem::None;
        inspection.detail.clear();
    }
    if (task->mode != TransferMode::PortInstall) {
        inspection.plan.packages.clear();
    } else if (task->packagesInstalled != 0) {
        const size_t done = std::min<size_t>(task->packagesInstalled,
                                             inspection.plan.packages.size());
        inspection.plan.packages.erase(inspection.plan.packages.begin(),
                                       inspection.plan.packages.begin() + done);
    }
    return inspection;
}

bool SwitchDeployService::inventory(const std::string& taskId,
                                    TaskFileInventory& inventory,
                                    std::string& error) const {
    const std::optional<DownloadTask> task = manager_.snapshot(taskId);
    if (!task) {
        error = "Download task not found.";
        return false;
    }
    return buildTaskFileInventory(appRoot_, *task, inventory, error);
}

bool SwitchDeployService::start(const std::string& taskId,
                                std::string& error,
                                bool includeArchives) {
    if (workerBusy_.load()) {
        error = "Another /switch copy is already active.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool inspectOnly =
            snapshot_.phase == SwitchDeployPhase::Preparing &&
            snapshot_.taskId == taskId;
        if (snapshot_.active() && !inspectOnly) {
            error = "Another /switch copy is already active.";
            return false;
        }
    }
    if (pollInFlight_.load()) {
        std::lock_guard<std::mutex> lock(offerMutex_);
        const bool offerReady =
            pendingOffer_ && pendingOffer_->taskId == taskId;
        if (!offerReady) {
            error = "Another /switch copy is already active.";
            return false;
        }
    }
    if (worker_.joinable())
        worker_.join();
    auto lease = manager_.beginExternalDeploy(taskId, error);
    if (!lease) {
        clearInspecting(taskId);
        return false;
    }
    cancelled_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {};
        snapshot_.phase = SwitchDeployPhase::Preparing;
        snapshot_.taskId = taskId;
        ++snapshot_.generation;
    }
    {
        std::lock_guard<std::mutex> lock(offerMutex_);
        if (pendingOffer_ && pendingOffer_->taskId == taskId)
            pendingOffer_.reset();
    }
    workerBusy_.store(true);
    worker_ = std::thread([this, lease = std::move(*lease),
                           includeArchives]() mutable {
        run(std::move(lease), includeArchives);
    });
    log_msg("[deploy] worker started %s archives=%d\n", taskId.c_str(),
            includeArchives ? 1 : 0);
    return true;
}

void SwitchDeployService::run(DownloadManager::ExternalDeployLease lease,
                              bool includeArchives) {
    TaskFileInventory inventory;
    std::string error;
    if (!buildTaskFileInventory(appRoot_, lease.task(), inventory, error)) {
        finish(SwitchDeployPhase::Failed, SwitchDeployProblem::Io,
               std::move(error));
        return;
    }
    // The receipt records the title ids of the installed packages (the
    // forwarder NSP carries its id in the file name): Uninstall links a
    // title to its receipt through them, no metadata index needed.
    std::vector<std::string> receiptTitleIds;
    {
        std::set<std::string> seen;
        for (const TaskFileInfo& file : inventory.files)
            if (file.package)
                for (const std::string& id : titleIdsInPath(file.logicalPath))
                    if (seen.insert(id).second)
                        receiptTitleIds.push_back(id);
    }
    const bool portTransaction =
        lease.task().mode == TransferMode::PortInstall;
    const bool payloadReceiptValid =
        portTransaction &&
        receiptState(lease.task().id) == SwitchDeployReceiptState::Valid;
    SwitchDeployInspection inspection = inspectSwitchDeploy(
        std::move(inventory), targetRoot_);
    if (payloadReceiptValid) {
        // A failed package stage retries from the durable payload receipt.
        // Do not re-extract archives (which would now correctly conflict with
        // their own files); only the remaining local packages are retried.
        inspection.problem = SwitchDeployProblem::None;
        inspection.detail.clear();
        inspection.plan.files.clear();
        inspection.plan.archives.clear();
        inspection.plan.bytesToCopy = 0;
        inspection.plan.bytesToMove = 0;
        inspection.plan.totalBytes = 0;
        inspection.plan.conflictFiles = 0;
    } else if (portTransaction &&
               inspection.problem == SwitchDeployProblem::NotAPort &&
               !inspection.plan.packages.empty()) {
        // An arbitrary ZIP/7z candidate can prove not to be a port after its
        // header probe. Fall back to installing its downloaded packages
        // rather than stranding a native release as a false-positive port.
        inspection.problem = SwitchDeployProblem::None;
        inspection.detail.clear();
    }
    if (!inspection.canStart()) {
        finish(SwitchDeployPhase::Failed, inspection.problem,
               std::move(inspection.detail));
        return;
    }
    SwitchDeployPlan plan = std::move(inspection.plan);
    if (payloadReceiptValid && plan.layeredTitleIds.empty()) {
        std::vector<ReceiptFile> receiptFiles;
        std::vector<std::string> receiptUnpacked;
        std::vector<std::string> receiptIds;
        if (loadReceipt(appRoot_, plan.taskId, receiptFiles,
                        receiptUnpacked, nullptr, &receiptIds)) {
            plan.layeredTitleIds = std::move(receiptIds);
            // A v4 receipt stores the destination root per file. Any file
            // written under the SD root (atmosphere) marks a LayeredFS
            // transaction, so a package-stage retry still warns about
            // overclocking and re-detects the performance tool.
            for (const ReceiptFile& file : receiptFiles) {
                if (file.target == SwitchDeployTarget::SdRoot) {
                    plan.layeredFs = true;
                    break;
                }
            }
        }
        if (plan.layeredFs)
            detectPerformanceState(
                sdRootForSwitchRoot(targetRoot_), plan.layeredTitleIds,
                plan.performanceToolDetected,
                plan.performanceProfileDetected);
    }
    if (plan.layeredFs)
        receiptTitleIds.clear();
    for (const std::string& id : plan.layeredTitleIds) {
        bool seen = false;
        for (const std::string& existing : receiptTitleIds)
            seen = seen || asciiEqual(existing, id);
        if (!seen)
            receiptTitleIds.push_back(id);
    }
    std::string completionDetail;
    if (!includeArchives) {
        for (const SwitchDeployArchive& archive : plan.archives) {
            if (!archive.extractable)
                continue;
            const uint64_t need =
                archive.unpackBytes ? archive.unpackBytes : archive.size;
            if (need <= plan.bytesToCopy)
                plan.bytesToCopy -= need;
            if (archive.size <= plan.totalBytes)
                plan.totalBytes -= archive.size;
        }
        plan.archives.clear();
    } else {
        std::string skipped;
        for (auto it = plan.archives.begin(); it != plan.archives.end();) {
            if (it->extractable) {
                ++it;
                continue;
            }
            if (!skipped.empty())
                skipped += ", ";
            skipped += it->sourceRelativePath;
            if (!it->detail.empty())
                skipped += " (" + it->detail + ")";
            it = plan.archives.erase(it);
        }
        if (!skipped.empty()) {
            completionDetail = "Skipped archive(s): " + skipped;
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.detail = completionDetail;
            ++snapshot_.generation;
        }
    }
    if (plan.files.empty() && plan.archives.empty() &&
        (!portTransaction || plan.packages.empty())) {
        if (portTransaction)
            manager_.finishExternalPortInstall(plan.taskId, true);
        finish(SwitchDeployPhase::Completed, SwitchDeployProblem::None,
               std::move(completionDetail));
        return;
    }
    const bool hasMoveFiles = std::any_of(
        plan.files.begin(), plan.files.end(),
        [](const SwitchDeployEntry& entry) {
            return entry.moveSource &&
                   entry.state == SwitchDeployEntryState::Missing;
        });
    bool moveJournalActive = false;
    if (hasMoveFiles) {
        if (!saveMoveJob(appRoot_, plan.taskId, plan.files)) {
            finish(SwitchDeployPhase::Failed, SwitchDeployProblem::Io,
                   "Unable to save the LayeredFS move recovery journal.");
            return;
        }
        moveJournalActive = true;
    }
    auto rollbackMoves = [this, &moveJournalActive] {
        if (!moveJournalActive)
            return true;
        const bool ok = rollbackMoveJob(appRoot_, manager_.downloadRoot(),
                                        sdRootForSwitchRoot(targetRoot_));
        if (ok)
            moveJournalActive = false;
        return ok;
    };

    // Extracted member paths, in extraction order, deduplicated: the receipt
    // v2 unpacked list. The extraction callback is the authoritative source
    // of what landed in /switch (the archive probe headers are only the
    // fallback for v1 receipts at uninstall time).
    std::vector<std::string> unpacked;
    std::unordered_set<std::string> unpackedSeen;
    std::stable_sort(plan.files.begin(), plan.files.end(),
                     [](const SwitchDeployEntry& a,
                        const SwitchDeployEntry& b) {
                         return a.nro < b.nro;
                     });
    const size_t copyFiles = plan.files.size() - plan.identicalFiles;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.phase = copyFiles ? SwitchDeployPhase::Copying
                                    : (plan.archives.empty()
                                           ? SwitchDeployPhase::Copying
                                           : SwitchDeployPhase::Extracting);
        snapshot_.totalBytes = plan.bytesToCopy + plan.bytesToMove;
        const size_t remainingPackages = portTransaction &&
                plan.packages.size() > lease.task().packagesInstalled
            ? plan.packages.size() - lease.task().packagesInstalled : 0;
        snapshot_.totalFiles = copyFiles + plan.archives.size() +
                               remainingPackages;
        snapshot_.identicalFiles = plan.identicalFiles;
        snapshot_.layeredFs = plan.layeredFs;
        snapshot_.performanceToolDetected = plan.performanceToolDetected;
        snapshot_.performanceProfileDetected =
            plan.performanceProfileDetected;
        ++snapshot_.generation;
    }
    for (SwitchDeployEntry& entry : plan.files) {
        if (entry.state == SwitchDeployEntryState::ExistingIdentical)
            continue;
        if (cancelled_.load(std::memory_order_relaxed)) {
            const bool restored = rollbackMoves();
            finish(SwitchDeployPhase::Cancelled,
                   restored ? SwitchDeployProblem::None
                            : SwitchDeployProblem::Io,
                   restored ? std::string()
                            : "Some moved files could not be restored; recovery will retry at next launch.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.phase = SwitchDeployPhase::Copying;
            snapshot_.currentPath = entry.destinationRelativePath;
            ++snapshot_.generation;
        }
        auto progress = [this](uint64_t bytes) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.bytesCopied += bytes;
            ++snapshot_.generation;
        };
        const bool transferred = entry.moveSource
            ? moveFile(entry, cancelled_, progress, entry.sha256, error)
            : copyFile(entry, appRoot_, plan.taskId, cancelled_, progress,
                       entry.sha256, error);
        if (!transferred) {
            if (!entry.destinationRelativePath.empty())
                error += " (" + entry.destinationRelativePath + ")";
            const bool restored = rollbackMoves();
            if (!restored)
                error += " Some moved files could not be restored; recovery will retry at next launch.";
            if (cancelled_.load(std::memory_order_relaxed))
                finish(SwitchDeployPhase::Cancelled,
                       restored ? SwitchDeployProblem::None
                                : SwitchDeployProblem::Io,
                       restored ? std::string() : std::move(error));
            else
                finish(SwitchDeployPhase::Failed, SwitchDeployProblem::Io,
                       std::move(error));
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++snapshot_.filesCopied;
        ++snapshot_.generation;
    }
    for (const SwitchDeployArchive& archive : plan.archives) {
        if (cancelled_.load(std::memory_order_relaxed)) {
            const bool restored = rollbackMoves();
            finish(SwitchDeployPhase::Cancelled,
                   restored ? SwitchDeployProblem::None
                            : SwitchDeployProblem::Io,
                   restored ? std::string()
                            : "Some moved files could not be restored; recovery will retry at next launch.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.phase = SwitchDeployPhase::Extracting;
            snapshot_.currentPath = archive.sourceRelativePath;
            ++snapshot_.generation;
        }
        log_msg("[deploy] extracting %s solid=%llu unpack=%llu files=%zu\n",
                archive.sourceRelativePath.c_str(),
                static_cast<unsigned long long>(archive.maxSolidBlockBytes),
                static_cast<unsigned long long>(archive.unpackBytes),
                archive.switchFiles);
        auto progress = [this](uint64_t bytes) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.bytesCopied += bytes;
            ++snapshot_.generation;
        };
        auto current = [this, &unpacked, &unpackedSeen](const std::string& path) {
            if (unpackedSeen.insert(path).second)
                unpacked.push_back(path);
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.currentPath = path;
            ++snapshot_.generation;
        };
        if (!extractPortArchive(archive.sourcePath, targetRoot_, cancelled_,
                                progress, current, error)) {
            const bool restored = rollbackMoves();
            if (!restored)
                error += " Some moved files could not be restored; recovery will retry at next launch.";
            if (cancelled_.load(std::memory_order_relaxed))
                finish(SwitchDeployPhase::Cancelled,
                       restored ? SwitchDeployProblem::None
                                : SwitchDeployProblem::Io,
                       restored ? std::string() : std::move(error));
            else {
                const SwitchDeployProblem problem =
                    error.find("Not enough free RAM") != std::string::npos
                        ? SwitchDeployProblem::NoRam
                        : SwitchDeployProblem::Io;
                finish(SwitchDeployPhase::Failed, problem, std::move(error));
            }
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++snapshot_.filesCopied;
        ++snapshot_.generation;
    }
    if (!payloadReceiptValid &&
        (!plan.files.empty() || !plan.archives.empty() || portTransaction) &&
        !saveReceipt(appRoot_, plan, unpacked, receiptTitleIds)) {
        const bool restored = rollbackMoves();
        finish(SwitchDeployPhase::Failed, SwitchDeployProblem::Io,
               restored
                   ? "The deployment receipt could not be saved; moved files were restored."
                   : "The deployment receipt could not be saved and some moved files could not be restored; recovery will retry at next launch.");
        return;
    }
    if (moveJournalActive) {
        std::remove(moveJobPath(appRoot_).c_str());
        moveJournalActive = false;
    }
    for (const SwitchDeployEntry& entry : plan.files)
        if (entry.moveSource &&
            entry.state == SwitchDeployEntryState::ExistingIdentical)
            std::remove(entry.sourcePath.c_str());

    if (portTransaction) {
        std::unique_ptr<install::InstallBackend> backend =
            install::createInstallBackend(appRoot_, manager_.installTarget());
        uint32_t completed = lease.task().packagesInstalled;
        for (size_t i = 0; i < plan.packages.size(); ++i) {
            if (i < completed)
                continue;
            if (cancelled_.load(std::memory_order_relaxed)) {
                manager_.finishExternalPortInstall(plan.taskId, false,
                                                   "Cancelled.");
                finish(SwitchDeployPhase::Cancelled,
                       SwitchDeployProblem::None, {});
                return;
            }
            const SwitchDeployPackage& package = plan.packages[i];
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.phase = SwitchDeployPhase::InstallingPackages;
                snapshot_.currentPath = package.sourceRelativePath;
                ++snapshot_.generation;
            }
            auto progress = [this, &plan, &completed, &package](
                                uint64_t installed, uint64_t total,
                                DownloadStatus status) {
                manager_.updateExternalPortInstall(
                    plan.taskId, completed, package.sourceRelativePath,
                    installed, total, status);
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.phase = status == DownloadStatus::Committing
                    ? SwitchDeployPhase::CommittingPackage
                    : SwitchDeployPhase::InstallingPackages;
                snapshot_.bytesCopied = installed;
                snapshot_.totalBytes = total;
                ++snapshot_.generation;
            };
            if (!installLocalPackage(*backend, plan.taskId, package,
                                     plan.layeredTitleIds, cancelled_,
                                     progress, error)) {
                manager_.finishExternalPortInstall(plan.taskId, false, error);
                if (cancelled_.load(std::memory_order_relaxed))
                    finish(SwitchDeployPhase::Cancelled,
                           SwitchDeployProblem::None, {});
                else
                    finish(SwitchDeployPhase::Failed,
                           SwitchDeployProblem::Io, std::move(error));
                return;
            }
            ++completed;
            manager_.updateExternalPortInstall(
                plan.taskId, completed, package.sourceRelativePath, 0, 0,
                DownloadStatus::Installing);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++snapshot_.filesCopied;
                ++snapshot_.generation;
            }
        }
        manager_.finishExternalPortInstall(plan.taskId, true);
    }
    finish(SwitchDeployPhase::Completed, SwitchDeployProblem::None,
           std::move(completionDetail));
}

void SwitchDeployService::finish(SwitchDeployPhase phase,
                                 SwitchDeployProblem problem,
                                 std::string detail) {
    std::string taskId;
    std::string loggedDetail;
    std::remove(jobPath(appRoot_).c_str());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskId = snapshot_.taskId;
        snapshot_.phase = phase;
        snapshot_.problem = problem;
        snapshot_.detail = std::move(detail);
        snapshot_.currentPath.clear();
        ++snapshot_.generation;
        loggedDetail = snapshot_.detail;
    }
    const char* phaseName = phase == SwitchDeployPhase::Completed
        ? "completed"
        : phase == SwitchDeployPhase::Cancelled
              ? "cancelled"
              : phase == SwitchDeployPhase::Failed ? "failed" : "idle";
    log_msg("[deploy] %s %s problem=%d %s\n", phaseName, taskId.c_str(),
            static_cast<int>(problem), loggedDetail.c_str());
    if (!taskId.empty()) {
        std::lock_guard<std::mutex> lock(offerMutex_);
        if (pendingOffer_ && pendingOffer_->taskId == taskId)
            pendingOffer_.reset();
        offerHandled_.insert(std::move(taskId));
    }
    workerBusy_.store(false);
}

void SwitchDeployService::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

void SwitchDeployService::shutdown() {
    cancel();
    if (pollWorker_.joinable())
        pollWorker_.join();
    if (worker_.joinable())
        worker_.join();
    workerBusy_.store(false);
}

SwitchDeploySnapshot SwitchDeployService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

SwitchDeployReceiptState SwitchDeployService::receiptState(
    const std::string& taskId) const {
    std::vector<ReceiptFile> files;
    std::vector<std::string> unpacked;
    if (!loadReceipt(appRoot_, taskId, files, unpacked))
        return SwitchDeployReceiptState::None;
    const std::string sdRoot = sdRootForSwitchRoot(targetRoot_);
    for (const ReceiptFile& file : files) {
        const std::string& root = deployRoot(targetRoot_, sdRoot, file.target);
        if (!destinationParentsSafe(root, file.path))
            return SwitchDeployReceiptState::Modified;
        const std::string path = root + "/" + file.path;
        if (!logicalFilePresent(path, file.size))
            return SwitchDeployReceiptState::Modified;
        std::array<uint8_t, 32> digest {};
        if (!hashLogicalFile(path, file.size, digest) || digest != file.digest)
            return SwitchDeployReceiptState::Modified;
    }
    // Unpacked members carry no recorded size or digest — existence is all
    // the receipt can verify.
    for (const std::string& relative : unpacked) {
        const std::string path = targetRoot_ + "/" + relative;
        struct stat st {};
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
            S_ISLNK(st.st_mode))
            return SwitchDeployReceiptState::Modified;
    }
    return SwitchDeployReceiptState::Valid;
}

bool SwitchDeployService::armAutoCopy(const std::string& taskId) {
    return atomicWrite(autoCopyPath(appRoot_, taskId), "1");
}

void SwitchDeployService::clearAutoCopy(const std::string& taskId) {
    std::remove(autoCopyPath(appRoot_, taskId).c_str());
}

bool SwitchDeployService::autoCopyArmed(const std::string& taskId) const {
    struct stat st {};
    return lstat(autoCopyPath(appRoot_, taskId).c_str(), &st) == 0 &&
           S_ISREG(st.st_mode);
}

void SwitchDeployService::markInspecting(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.active() && snapshot_.phase != SwitchDeployPhase::Preparing)
        return;
    snapshot_.phase = SwitchDeployPhase::Preparing;
    snapshot_.taskId = taskId;
    snapshot_.problem = SwitchDeployProblem::None;
    snapshot_.currentPath.clear();
    snapshot_.detail.clear();
    snapshot_.bytesCopied = 0;
    snapshot_.totalBytes = 0;
    snapshot_.filesCopied = 0;
    snapshot_.totalFiles = 0;
    ++snapshot_.generation;
}

void SwitchDeployService::clearInspecting(const std::string& taskId) {
    if (workerBusy_.load())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.phase != SwitchDeployPhase::Preparing ||
        snapshot_.taskId != taskId)
        return;
    const uint64_t generation = snapshot_.generation + 1;
    snapshot_ = {};
    snapshot_.generation = generation;
}

    bool SwitchDeployService::considerDeployOffer(const std::string& taskId) {
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            if (offerHandled_.count(taskId) || pendingOffer_)
                return false;
        }
        const std::optional<DownloadTask> task = manager_.snapshot(taskId);
        if (!task || !taskReadyForSwitchDeploy(*task))
            return false;
        const bool markerArmed = autoCopyArmed(taskId);
        const bool autoArmed = markerArmed ||
                               task->mode == TransferMode::PortInstall;
        if (task->mode != TransferMode::StreamInstall && !autoArmed)
            return false;
        // A saved receipt means this task was already copied to /switch once.
        // Do not offer it again — if the user deleted or changed the installed
        // files afterwards, restoring them is a deliberate manual action
        // (Details → Install port), not something to silently restart.
        if (receiptState(taskId) != SwitchDeployReceiptState::None &&
            !(task->mode == TransferMode::PortInstall &&
              task->packagesInstalled < task->packageCount)) {
            if (markerArmed)
                clearAutoCopy(taskId);
            std::lock_guard<std::mutex> lock(offerMutex_);
            offerHandled_.insert(taskId);
            return false;
        }
        markInspecting(taskId);
        SwitchDeployInspection inspection = inspect(taskId);
        log_msg("[deploy] inspect %s problem=%d can_start=%d files=%zu "
                "archives=%zu: %s\n",
                taskId.c_str(), static_cast<int>(inspection.problem),
                inspection.canStart() ? 1 : 0, inspection.plan.files.size(),
                inspection.plan.archives.size(),
                inspection.detail.empty() ? "-" : inspection.detail.c_str());
        if (autoArmed) {
            const bool missingLayout =
                inspection.problem == SwitchDeployProblem::LayoutNotFound ||
                inspection.problem == SwitchDeployProblem::AmbiguousLayout;
            if (!inspection.canStart() &&
                !switchDeployOffersCopy(inspection.problem) && !missingLayout) {
                clearInspecting(taskId);
                return false;
            }
            if (inspection.problem == SwitchDeployProblem::NotAPort) {
                clearAutoCopy(taskId);
                {
                    std::lock_guard<std::mutex> lock(offerMutex_);
                    offerHandled_.insert(taskId);
                }
                clearInspecting(taskId);
                return false;
            }
            if (inspection.canStart()) {
                uint64_t looseBytes = 0;
                for (const SwitchDeployEntry& entry : inspection.plan.files) {
                    if (entry.state == SwitchDeployEntryState::Missing)
                        looseBytes += entry.size;
                }
                if (looseBytes == 0 && inspection.plan.archives.empty() &&
                    inspection.plan.packages.empty()) {
                    clearAutoCopy(taskId);
                    {
                        std::lock_guard<std::mutex> lock(offerMutex_);
                        offerHandled_.insert(taskId);
                    }
                    clearInspecting(taskId);
                    return false;
                }
            }
            bool collide = false;
            {
                std::lock_guard<std::mutex> lock(offerMutex_);
                if (offerHandled_.count(taskId) || pendingOffer_)
                    collide = true;
                else
                    pendingOffer_ = PendingOffer{taskId, std::move(inspection),
                                                 true};
            }
            if (collide) {
                clearInspecting(taskId);
                return false;
            }
            log_msg("[deploy] auto-copy ready %s\n", taskId.c_str());
            return true;
        }
        clearInspecting(taskId);
        if (!inspection.canStart())
            return false;
        uint64_t looseBytes = 0;
        for (const SwitchDeployEntry& entry : inspection.plan.files) {
            if (entry.state == SwitchDeployEntryState::Missing)
                looseBytes += entry.size;
        }
        if (looseBytes == 0 && inspection.plan.archives.empty())
            return false;
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            if (offerHandled_.count(taskId) || pendingOffer_)
                return false;
            pendingOffer_ = PendingOffer{taskId, std::move(inspection), false};
        }
        log_msg("[deploy] offer ready %s\n", taskId.c_str());
        return true;
    }

void SwitchDeployService::scheduleDeployOfferPoll() {
    if (pollInFlight_.exchange(true))
        return;
    if (pollWorker_.joinable())
        pollWorker_.join();
    pollWorker_ = std::thread([this]() {
        pollDeployOffers();
        pollInFlight_.store(false);
    });
}

void SwitchDeployService::pollDeployOffers() {
    if (snapshot().active())
        return;
    for (const DownloadTask& task : manager_.snapshot()) {
        if (!taskReadyForSwitchDeploy(task))
            continue;
        if (considerDeployOffer(task.id))
            return;
    }
}

    std::optional<SwitchDeployService::PendingOffer>
    SwitchDeployService::takePendingDeployOffer() {
        std::lock_guard<std::mutex> lock(offerMutex_);
        std::optional<PendingOffer> offer = std::move(pendingOffer_);
        pendingOffer_.reset();
        return offer;
    }

    void SwitchDeployService::dismissDeployOffer(const std::string& taskId) {
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            offerHandled_.insert(taskId);
            if (pendingOffer_ && pendingOffer_->taskId == taskId)
                pendingOffer_.reset();
        }
        clearInspecting(taskId);
        log_msg("[deploy] offer dismissed %s\n", taskId.c_str());
    }

void SwitchDeployService::cleanupInterruptedJob() {
    struct stat moveJob {};
    if (lstat(moveJobPath(appRoot_).c_str(), &moveJob) == 0) {
        if (rollbackMoveJob(appRoot_, manager_.downloadRoot(),
                            sdRootForSwitchRoot(targetRoot_)))
            log_msg("[deploy] restored interrupted LayeredFS move\n");
        else
            log_msg("[deploy] interrupted LayeredFS move needs manual recovery\n");
    }
    std::string blob;
    if (!readBlob(jobPath(appRoot_), blob))
        return;
    const char* cursor = blob.data();
    const char* end = cursor + blob.size();
    be_node_t root;
    std::string temporary;
    uint64_t version = 0;
    if (be_decode(&cursor, end, &root) && cursor == end &&
        root.type == BE_DICT && readInteger(root, "version", version) &&
        version == static_cast<uint64_t>(kJobVersion) &&
        readString(root, "temp", temporary) &&
        managedChild(targetRoot_, temporary) &&
        temporary.find(".pipensx-part-") != std::string::npos) {
        std::remove(temporary.c_str());
    }
    std::remove(jobPath(appRoot_).c_str());
}

namespace {

// Task ids of every receipt under <root>/deployments, sorted so the plan is
// deterministic across runs.
std::vector<std::string> listReceiptTaskIds(const std::string& root) {
    std::vector<std::string> ids;
    const std::string directory = root + "/deployments";
    DIR* dir = opendir(directory.c_str());
    if (!dir)
        return ids;
    while (struct dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() != 48 ||
            name.compare(name.size() - 8, 8, ".bencode") != 0)
            continue;
        ids.push_back(name.substr(0, 40));
    }
    closedir(dir);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Does the receipt link taskId to titleId? Receipt v3 records the title ids
// of the installed packages; older receipts fall back to the bracketed id in
// the task manifest's package file names. False for unreadable receipts and
// for titles the receipt does not belong to.
bool receiptMatchesTitle(const std::string& root, const std::string& taskId,
                         const std::string& titleId) {
    std::vector<ReceiptFile> files;
    std::vector<std::string> unpacked;
    std::vector<std::string> titleIds;
    if (!loadReceipt(root, taskId, files, unpacked, nullptr, &titleIds))
        return false;
    for (const std::string& id : titleIds)
        if (asciiEqual(id, titleId))
            return true;
    if (!titleIds.empty())
        return false; // v3 names the titles — no other source to consult
    TaskFileManifest manifest;
    std::string manifestError;
    if (!loadTaskFileManifest(root, taskId, manifest, manifestError))
        return false;
    for (const TaskFileRecord& file : manifest.files)
        if (file.package)
            for (const std::string& id : titleIdsInPath(file.logicalPath))
                if (asciiEqual(id, titleId))
                    return true;
    return false;
}

} // namespace

PortUninstallService::PortUninstallService(DownloadManager& manager,
                                           std::string appRoot,
                                           std::string targetRoot)
    : manager_(manager), appRoot_(std::move(appRoot)),
      targetRoot_(std::move(targetRoot)) {
    while (targetRoot_.size() > 1 && targetRoot_.back() == '/')
        targetRoot_.pop_back();
}

bool PortUninstallService::receiptExists(const std::string& taskId) const {
    struct stat st {};
    return lstat(receiptPath(appRoot_, taskId).c_str(), &st) == 0 &&
           S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool PortUninstallService::plan(const std::string& titleId,
                                const std::vector<std::string>& taskIds,
                                PortUninstallPlan& result) const {
    result = {};
    result.titleId = titleId;
    std::set<std::string> switchFiles;
    std::set<std::string> sdRootFiles;
    std::set<std::string> switchSplitFiles;
    std::set<std::string> sdRootSplitFiles;
    std::set<std::string> wholeFolders;
    uint64_t switchBytes = 0;
    uint64_t sdRootBytes = 0;
    bool anyReceipt = false;
    // Every receipt on disk is matched to the title two ways: the recorded
    // title ids (receipt v3), or — for receipts written before titles were
    // recorded — the bracketed id in the task manifest's package file names
    // (forwarder NSPs carry it, e.g. "Port [01d2c0b236000000].nsp").
    // `taskIds` carries the metadata-index infohashes and only acts as a
    // last-resort fallback for ordinary NSP titles; the index covers retail
    // releases only, which is exactly how ports were missed before.
    std::set<std::string> fallback(taskIds.begin(), taskIds.end());
    std::vector<std::string> matched;
    for (const std::string& taskId : listReceiptTaskIds(appRoot_)) {
        if (receiptMatchesTitle(appRoot_, taskId, titleId))
            matched.push_back(taskId);
        else if (fallback.count(taskId))
            matched.push_back(taskId);
    }
    if (matched.empty())
        return false;
    for (const std::string& taskId : matched) {
        std::vector<ReceiptFile> files;
        std::vector<std::string> unpacked;
        bool unpackedKnown = false;
        if (!loadReceipt(appRoot_, taskId, files, unpacked,
                         &unpackedKnown))
            continue;
        anyReceipt = true;
        result.taskIds.push_back(taskId);
        const std::optional<DownloadTask> task = manager_.snapshot(taskId);
        if (task) {
            result.hasTask = true;
            if (!task->dataPath.empty()) {
                struct stat st {};
                if (lstat(task->dataPath.c_str(), &st) == 0)
                    result.taskHasData = true;
            }
        }
        if (!unpackedKnown) {
            // v1 receipt: rebuild the unpacked member list from the archive
            // headers still present in the task data. When the archive is
            // gone (or the task is), the exact list is unknowable — remove
            // the receipt's top-level folders entirely instead, which is
            // what the dialog warns about in that case.
            bool listed = false;
            if (task) {
                TaskFileInventory inventory;
                std::string inventoryError;
                if (buildTaskFileInventory(appRoot_, *task, inventory,
                                           inventoryError)) {
                    bool archiveGone = false;
                    for (const TaskFileInfo& file : inventory.files) {
                        if (!isPortArchiveName(file.logicalPath))
                            continue;
                        if (file.state != TaskFileState::Present ||
                            file.absolutePath.empty()) {
                            archiveGone = true;
                            break;
                        }
                        PortArchiveProbe probe;
                        if (!probePortArchive(file.absolutePath, probe) ||
                            !probe.ok) {
                            archiveGone = true;
                            break;
                        }
                        for (const std::string& path : probe.files)
                            if (unpacked.size() < kMaxReceiptUnpacked)
                                unpacked.push_back(path);
                    }
                    listed = !archiveGone;
                }
            }
            if (listed) {
                for (const std::string& path : unpacked)
                    switchFiles.insert(path);
            } else {
                for (const ReceiptFile& file : files) {
                    const std::string folder =
                        receiptTopFolder(file.path);
                    if (!folder.empty() && lowerAscii(folder) != "pipensx")
                        wholeFolders.insert(folder);
                }
                continue;
            }
        }
        for (const std::string& path : unpacked) {
            switchFiles.insert(path);
            // The receipt records sizes only for the copied files; stat the
            // extracted members so the dialog reports the real footprint.
            const std::string full = targetRoot_ + "/" + path;
            struct stat st {};
            if (lstat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                switchBytes += static_cast<uint64_t>(st.st_size);
        }
        for (const ReceiptFile& file : files) {
            if (file.target == SwitchDeployTarget::SdRoot) {
                sdRootFiles.insert(file.path);
                std::vector<std::string> parts;
                if (splitFolderParts(sdRootForSwitchRoot(targetRoot_) + "/" +
                                         file.path,
                                     file.size, parts))
                    sdRootSplitFiles.insert(file.path);
                sdRootBytes += file.size;
            } else {
                switchFiles.insert(file.path);
                std::vector<std::string> parts;
                if (splitFolderParts(targetRoot_ + "/" + file.path,
                                     file.size, parts))
                    switchSplitFiles.insert(file.path);
                switchBytes += file.size;
            }
        }
    }
    if (!anyReceipt)
        return false;
    result.switchFiles.assign(switchFiles.begin(), switchFiles.end());
    result.sdRootFiles.assign(sdRootFiles.begin(), sdRootFiles.end());
    result.switchSplitFiles.assign(switchSplitFiles.begin(),
                                   switchSplitFiles.end());
    result.sdRootSplitFiles.assign(sdRootSplitFiles.begin(),
                                   sdRootSplitFiles.end());
    result.wholeFolders.assign(wholeFolders.begin(), wholeFolders.end());
    result.switchBytes = switchBytes;
    result.sdRootBytes = sdRootBytes;
    return true;
}

bool PortUninstallService::deleteDeployed(
    const PortUninstallPlan& plan, PortUninstallReport& report) const {
    bool ok = true;
    auto fail = [&](const std::string& detail) {
        ok = false;
        if (report.error.empty())
            report.error = detail;
    };
    auto removeExact = [&](const std::string& root,
                           const std::vector<std::string>& files,
                           const std::vector<std::string>& splitFiles) {
        for (const std::string& relative : files) {
            const std::string full = root + "/" + relative;
            struct stat st {};
            if (lstat(full.c_str(), &st) != 0) {
                if (errno == ENOENT)
                    ++report.filesMissing;
                else
                    fail("Unable to remove " + relative + ".");
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (!std::binary_search(splitFiles.begin(), splitFiles.end(),
                                        relative))
                    continue;
                if (!removeTreeBestEffort(full)) {
                    ++report.filesFailed;
                    fail("Unable to remove split file " + relative + ".");
                    continue;
                }
                ++report.filesRemoved;
                pruneEmptyParents(root, full);
                continue;
            }
            if (std::remove(full.c_str()) != 0) {
                ++report.filesFailed;
                fail("Unable to remove " + relative + ".");
                continue;
            }
            ++report.filesRemoved;
            pruneEmptyParents(root, full);
        }
    };
    removeExact(targetRoot_, plan.switchFiles, plan.switchSplitFiles);
    removeExact(sdRootForSwitchRoot(targetRoot_), plan.sdRootFiles,
                plan.sdRootSplitFiles);
    for (const std::string& folder : plan.wholeFolders) {
        const std::string full = targetRoot_ + "/" + folder;
        struct stat st {};
        if (lstat(full.c_str(), &st) != 0) {
            if (errno == ENOENT)
                continue;
            fail("Unable to remove /switch/" + folder + ".");
            continue;
        }
        if (!removeTreeBestEffort(full)) {
            ++report.filesFailed;
            fail("Unable to remove /switch/" + folder + " entirely.");
        }
    }
    return ok;
}

bool PortUninstallService::removeTasks(const PortUninstallPlan& plan,
                                       PortUninstallReport& report) const {
    bool ok = true;
    for (const std::string& taskId : plan.taskIds) {
        if (!manager_.snapshot(taskId))
            continue; // already gone — nothing to remove
        std::string error;
        if (!manager_.remove(taskId, true, error) &&
            manager_.snapshot(taskId)) {
            ok = false;
            if (report.error.empty())
                report.error = error.empty()
                    ? "Unable to remove the download task."
                    : error;
        }
    }
    return ok;
}

bool PortUninstallService::uninstallPort(
    const PortUninstallPlan& plan,
    const std::function<bool(std::string&)>& uninstallShortcut,
    PortUninstallReport& report) const {
    report = {};
    report.filesDeleted = deleteDeployed(plan, report);
    report.tasksRemoved = removeTasks(plan, report);
    std::string shortcutError;
    report.shortcutRemoved =
        !uninstallShortcut || uninstallShortcut(shortcutError);
    if (!report.shortcutRemoved && report.error.empty())
        report.error = shortcutError.empty()
            ? "Unable to uninstall the application."
            : shortcutError;
    if (report.complete()) {
        // Full success only: the receipts and auto-copy markers go last, so
        // a failed run leaves everything in place and Uninstall again stays
        // safe.
        for (const std::string& taskId : plan.taskIds) {
            std::remove(receiptPath(appRoot_, taskId).c_str());
            std::remove(autoCopyPath(appRoot_, taskId).c_str());
        }
    }
    log_msg("[port-uninstall] %s title=%s tasks=%zu files=%zu missing=%zu "
            "failed=%zu shortcut=%d %s\n",
            report.complete() ? "removed" : "partial", plan.titleId.c_str(),
            plan.taskIds.size(), report.filesRemoved, report.filesMissing,
            report.filesFailed, report.shortcutRemoved ? 1 : 0,
            report.error.c_str());
    return report.complete();
}

} // namespace pipensx
