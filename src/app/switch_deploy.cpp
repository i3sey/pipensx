#include "switch_deploy.hpp"

#include "install_space.hpp"

extern "C" {
#include "../core/bencode.h"
#include "../core/sha256.h"
#include "../core/util.h"
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr size_t kCopyBufferBytes = 256 * 1024;
constexpr uint32_t kNroMagic = 0x304f524e;
constexpr int64_t kReceiptVersion = 1;
constexpr int64_t kJobVersion = 1;
constexpr size_t kMaxStateBytes = 8 * 1024 * 1024;

struct ReceiptFile {
    std::string path;
    uint64_t size = 0;
    std::array<uint8_t, 32> digest {};
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

bool hasNroExtension(const std::string& path) {
    const std::string lower = lowerAscii(path);
    return lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".nro") == 0;
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

std::string parentPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
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

std::string jobPath(const std::string& root) {
    return root + "/deploy-job.bencode";
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

bool saveReceipt(const std::string& root, const SwitchDeployPlan& plan) {
    std::string blob = "d5:filesl";
    for (const SwitchDeployEntry& entry : plan.files) {
        blob += "d6:digest";
        blob += bstr(std::string(reinterpret_cast<const char*>(
                                     entry.sha256.data()),
                                 entry.sha256.size()));
        blob += "4:path" + bstr(entry.destinationRelativePath);
        blob += "4:size" + bint(entry.size) + "e";
    }
    blob += "e4:task" + bstr(plan.taskId);
    blob += "7:version" + bint(kReceiptVersion) + "e";
    return atomicWrite(receiptPath(root, plan.taskId), blob);
}

bool loadReceipt(const std::string& root, const std::string& taskId,
                 std::vector<ReceiptFile>& files) {
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
        version != static_cast<uint64_t>(kReceiptVersion) ||
        !readString(rootNode, "task", storedTask) || storedTask != taskId ||
        !be_dict_get(rootNode.buf, rootNode.buf + rootNode.raw_len, "files", 5,
                     &list) || list.type != BE_LIST)
        return false;
    std::vector<ReceiptFile> parsed;
    const char* itemCursor = list.buf + 1;
    const char* itemEnd = list.buf + list.raw_len - 1;
    be_node_t item;
    while (be_list_next(&itemCursor, itemEnd, &item)) {
        if (parsed.size() >= 4096)
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
        file.size = size;
        std::memcpy(file.digest.data(), digest.data(), digest.size());
        parsed.push_back(std::move(file));
    }
    files = std::move(parsed);
    return true;
}

bool sourceFileSafe(const TaskFileInventory& inventory,
                    const TaskFileInfo& file) {
    if (file.state != TaskFileState::Present || file.absolutePath.empty() ||
        !managedChild(inventory.rootPath, file.absolutePath))
        return false;
    struct stat st {};
    return lstat(file.absolutePath.c_str(), &st) == 0 &&
           S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) &&
           static_cast<uint64_t>(st.st_size) == file.size;
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
    const std::string temporary = parent + "/.pipensx-part-" +
        taskId.substr(0, std::min<size_t>(8, taskId.size()));
    if (!saveJob(appRoot, taskId, temporary)) {
        error = "Unable to save the copy recovery journal.";
        return false;
    }
    std::FILE* input = std::fopen(entry.sourcePath.c_str(), "rb");
    if (!input) {
        error = "Unable to open a file for copying.";
        return false;
    }
    const int outputFd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                              0644);
    if (outputFd < 0) {
        std::fclose(input);
        error = "Unable to create the temporary copy file.";
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

} // namespace

SwitchDeployInspection inspectSwitchDeploy(TaskFileInventory inventory,
                                           const std::string& targetRoot) {
    SwitchDeployInspection result;
    result.inventory = std::move(inventory);
    result.plan.taskId = result.inventory.taskId;
    result.plan.targetRoot = targetRoot;
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

    std::set<std::string> roots;
    for (const TaskFileInfo& file : result.inventory.files) {
        if (file.action != TaskFileAction::Download || file.package ||
            file.cartridge || file.state != TaskFileState::Present ||
            !hasNroExtension(file.logicalPath) ||
            !sourceFileSafe(result.inventory, file) ||
            !validNro(file.absolutePath))
            continue;
        const std::vector<std::string> parts = splitPath(file.logicalPath);
        for (size_t i = 0; i < parts.size(); ++i) {
            if (asciiEqual(parts[i], "switch")) {
                roots.insert(lowerAscii(joinPath(parts, 0, i + 1)));
                break;
            }
        }
    }
    if (roots.empty()) {
        setProblem(result, SwitchDeployProblem::LayoutNotFound,
                   "A switch directory with a valid NRO was not found.");
        return result;
    }
    if (roots.size() != 1) {
        setProblem(result, SwitchDeployProblem::AmbiguousLayout,
                   "More than one switch directory contains an NRO.");
        return result;
    }
    const std::string selectedRoot = *roots.begin();
    struct LayoutPath {
        std::string spelling;
        bool file = false;
    };
    std::map<std::string, LayoutPath> layoutPaths;
    for (const TaskFileInfo& file : result.inventory.files) {
        const std::vector<std::string> parts = splitPath(file.logicalPath);
        size_t switchIndex = parts.size();
        for (size_t i = 0; i < parts.size(); ++i) {
            if (asciiEqual(parts[i], "switch") &&
                lowerAscii(joinPath(parts, 0, i + 1)) == selectedRoot) {
                switchIndex = i;
                break;
            }
        }
        if (switchIndex == parts.size()) {
            if (file.action == TaskFileAction::Download)
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
            setProblem(result, SwitchDeployProblem::MissingSource,
                       file.logicalPath);
            return result;
        }
        const std::string destinationRelative =
            joinPath(parts, switchIndex + 1, parts.size());
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
    if (result.plan.files.empty()) {
        setProblem(result, SwitchDeployProblem::LayoutNotFound,
                   "The switch directory has no downloadable files.");
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
    if (task->mode != TransferMode::DownloadOnly) {
        SwitchDeployInspection result;
        result.problem = SwitchDeployProblem::NotReady;
        result.detail = "Only ordinary downloads can be copied to /switch.";
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
    return inspectSwitchDeploy(std::move(inventory), targetRoot_);
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
                                std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.active()) {
            error = "Another /switch copy is already active.";
            return false;
        }
    }
    if (worker_.joinable())
        worker_.join();
    auto lease = manager_.beginExternalDeploy(taskId, error);
    if (!lease)
        return false;
    cancelled_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {};
        snapshot_.phase = SwitchDeployPhase::Preparing;
        snapshot_.taskId = taskId;
        ++snapshot_.generation;
    }
    worker_ = std::thread([this, lease = std::move(*lease)]() mutable {
        run(std::move(lease));
    });
    return true;
}

void SwitchDeployService::run(DownloadManager::ExternalDeployLease lease) {
    TaskFileInventory inventory;
    std::string error;
    if (!buildTaskFileInventory(appRoot_, lease.task(), inventory, error)) {
        finish(SwitchDeployPhase::Failed, SwitchDeployProblem::Io,
               std::move(error));
        return;
    }
    SwitchDeployInspection inspection = inspectSwitchDeploy(
        std::move(inventory), targetRoot_);
    if (!inspection.canStart()) {
        finish(SwitchDeployPhase::Failed, inspection.problem,
               std::move(inspection.detail));
        return;
    }
    SwitchDeployPlan plan = std::move(inspection.plan);
    std::stable_sort(plan.files.begin(), plan.files.end(),
                     [](const SwitchDeployEntry& a,
                        const SwitchDeployEntry& b) {
                         return a.nro < b.nro;
                     });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.phase = SwitchDeployPhase::Copying;
        snapshot_.totalBytes = plan.bytesToCopy;
        snapshot_.totalFiles = plan.files.size() - plan.identicalFiles;
        snapshot_.identicalFiles = plan.identicalFiles;
        ++snapshot_.generation;
    }
    for (SwitchDeployEntry& entry : plan.files) {
        if (entry.state == SwitchDeployEntryState::ExistingIdentical)
            continue;
        if (cancelled_.load(std::memory_order_relaxed)) {
            finish(SwitchDeployPhase::Cancelled, SwitchDeployProblem::None, {});
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.currentPath = entry.destinationRelativePath;
            ++snapshot_.generation;
        }
        auto progress = [this](uint64_t bytes) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.bytesCopied += bytes;
            ++snapshot_.generation;
        };
        if (!copyFile(entry, appRoot_, plan.taskId, cancelled_, progress,
                      entry.sha256, error)) {
            if (cancelled_.load(std::memory_order_relaxed))
                finish(SwitchDeployPhase::Cancelled,
                       SwitchDeployProblem::None, {});
            else
                finish(SwitchDeployPhase::Failed, SwitchDeployProblem::Io,
                       std::move(error));
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++snapshot_.filesCopied;
        ++snapshot_.generation;
    }
    if (!saveReceipt(appRoot_, plan)) {
        finish(SwitchDeployPhase::Completed, SwitchDeployProblem::Io,
               "Files were copied, but the deployment receipt was not saved.");
        return;
    }
    finish(SwitchDeployPhase::Completed, SwitchDeployProblem::None, {});
}

void SwitchDeployService::finish(SwitchDeployPhase phase,
                                 SwitchDeployProblem problem,
                                 std::string detail) {
    std::remove(jobPath(appRoot_).c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.phase = phase;
    snapshot_.problem = problem;
    snapshot_.detail = std::move(detail);
    snapshot_.currentPath.clear();
    ++snapshot_.generation;
}

void SwitchDeployService::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

void SwitchDeployService::shutdown() {
    cancel();
    if (worker_.joinable())
        worker_.join();
}

SwitchDeploySnapshot SwitchDeployService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

SwitchDeployReceiptState SwitchDeployService::receiptState(
    const std::string& taskId) const {
    std::vector<ReceiptFile> files;
    if (!loadReceipt(appRoot_, taskId, files))
        return SwitchDeployReceiptState::None;
    for (const ReceiptFile& file : files) {
        if (!destinationParentsSafe(targetRoot_, file.path))
            return SwitchDeployReceiptState::Modified;
        const std::string path = targetRoot_ + "/" + file.path;
        struct stat st {};
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
            S_ISLNK(st.st_mode) ||
            static_cast<uint64_t>(st.st_size) != file.size)
            return SwitchDeployReceiptState::Modified;
        std::array<uint8_t, 32> digest {};
        if (!hashFile(path, digest) || digest != file.digest)
            return SwitchDeployReceiptState::Modified;
    }
    return SwitchDeployReceiptState::Valid;
}

void SwitchDeployService::cleanupInterruptedJob() {
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

} // namespace pipensx
