#pragma once

#include "nx_file_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

struct DownloadTask;
struct TorrentPreview;

enum class TaskFileAction : uint8_t {
    Skip = 0,
    Download = 1,
    Install = 2,
};

enum class TaskFileState {
    Pending,
    Present,
    Installed,
    Skipped,
    Missing,
    Unsafe,
};

struct TaskFileRecord {
    std::string logicalPath;
    std::string localPath;
    uint64_t size = 0;
    uint32_t sourceIndex = UINT32_MAX;
    TaskFileAction action = TaskFileAction::Download;
    bool package = false;
    bool compressed = false;
    bool cartridge = false;
};

struct TaskFileManifest {
    std::string taskId;
    std::vector<TaskFileRecord> files;
};

struct TaskFileInfo : TaskFileRecord {
    std::string absolutePath;
    TaskFileState state = TaskFileState::Missing;
    SwitchPathKind kind = SwitchPathKind::Junk;
    // Predicted SD destination from the file name (no deploy inspect).
    // Empty destinationRoot means the file stays in the task downloads folder
    // or is a package committed to NCM.
    std::string destinationRoot;
    std::string destinationExample;
    size_t destinationCount = 0;
    bool staysInDownloads = false;
    std::vector<std::string> destinationPaths;
};

struct TaskFileInventory {
    std::string taskId;
    std::string rootPath;
    std::vector<TaskFileInfo> files;
    uint64_t presentBytes = 0;
    bool settled = false;
    bool completeManifest = true;
};

void annotateTaskFileDestinations(TaskFileInventory& inventory);

TaskFileManifest makeTaskFileManifest(
    const std::string& taskId, const TorrentPreview& preview,
    const std::vector<uint8_t>& actions);

bool saveTaskFileManifest(const std::string& appRoot,
                          const TaskFileManifest& manifest,
                          std::string& error);
bool loadTaskFileManifest(const std::string& appRoot,
                          const std::string& taskId,
                          TaskFileManifest& manifest,
                          std::string& error);
void removeTaskFileManifest(const std::string& appRoot,
                            const std::string& taskId);

bool buildTaskFileInventory(const std::string& appRoot,
                            const DownloadTask& task,
                            TaskFileInventory& inventory,
                            std::string& error);

bool refreshTorrentManifestLocalPaths(const std::string& appRoot,
                                      const DownloadTask& task,
                                      std::string& error);

bool taskReadyForSwitchDeploy(const DownloadTask& task);

bool taskFilePathIsSafe(const std::string& path);
bool taskFilePathIsValidUtf8(const std::string& path);
bool taskFilePathIsFatCompatible(const std::string& path);

} // namespace pipensx
