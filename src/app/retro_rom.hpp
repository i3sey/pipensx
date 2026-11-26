#pragma once

#include "retro_catalog.hpp"
#include "retro_emulator.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pipensx {

struct RetroRomInput {
    std::string path, absolutePath;
    uint64_t bytes = 0;
};

struct RetroRomGame {
    std::string id, title, path;
    std::string emulator;
    uint64_t size = 0;
    bool selected = false;
};

struct RetroRomOperation {
    std::string id, taskId, topicId, title, destination, emulator;
    std::string platform = "gba";
    std::string coverUrl;
    bool ready = false;
    bool createForwarder = false;
    std::vector<RetroRomGame> games;

    bool imported() const { return id.rfind("import-", 0) == 0; }
};

struct RetroSettings {
    std::string lastConsole = "gba";
    bool autoCreateForwarder = true;
    std::map<std::string, std::string> defaultEmulator;
    std::map<std::string, std::string> romDirs;
    std::map<std::string, std::string> nroPaths;
    std::map<std::string, std::string> biosDirs;
    std::map<std::string, std::string> saveDirs;
    std::map<std::string, std::string> stateDirs;

    std::string emulatorFor(const std::string& consoleId) const {
        const auto it = defaultEmulator.find(consoleId);
        if (it != defaultEmulator.end() && isValidEmulatorSelection(consoleId, it->second) &&
            !it->second.empty()) {
            return it->second;
        }
        const auto list = emulatorsForConsole(consoleId);
        return list.empty() ? std::string() : list.front()->id;
    }

    std::string detectionFor(const std::string& consoleId, const std::string& emulatorId) const {
        if (emulatorId.empty())
            return emulatorId;
        if (emulatorId == emulatorFor(consoleId)) {
            const auto it = nroPaths.find(consoleId);
            if (it != nroPaths.end() && isCustomEmulatorPath(it->second))
                return it->second;
        }
        return emulatorId;
    }

    std::string romDirFor(const std::string& consoleId) const {
        const auto it = romDirs.find(consoleId);
        if (it != romDirs.end() && !it->second.empty())
            return it->second;
        if (const auto* console = findRetroConsole(consoleId))
            return console->defaultRomSubdir;
        return "roms/" + consoleId;
    }
};

class RetroRomStore {
  public:
    RetroRomStore(std::string appRoot, std::string sdRoot);

    static bool acceptsDestination(const std::string& destination);
    static bool acceptsSetupDirectory(const std::string& directory);
    static std::string normalizeDestination(const std::string& destination);
    const std::string& appRoot() const { return appRoot_; }

    bool prepareLaunchArguments(const RetroSettings& settings, const std::string& consoleId,
                                const std::string& emulatorId, const std::string& nroPath,
                                const std::string& romPath, std::string& arguments,
                                std::string& error) const;

    bool plan(const RetroRelease& release, const std::string& destination,
              const std::string& emulator, RetroRomOperation& operation, std::string& error,
              bool createForwarder = false) const;
    bool load(const std::string& id, RetroRomOperation& operation, std::string& error) const;
    bool findOperationByTask(const std::string& taskId, RetroRomOperation& operation,
                             std::string& error) const;
    bool list(std::vector<RetroRomOperation>& operations, std::string& error) const;
    using PrepareProgress = std::function<void(uint64_t doneBytes, uint64_t totalBytes,
                                               size_t doneFiles, size_t totalFiles)>;
    bool prepare(const std::string& id, const std::vector<RetroRomInput>& files,
                 const std::atomic<bool>& cancelled, RetroRomOperation& result,
                 std::string& error, PrepareProgress progress = {}) const;
    bool select(const std::string& id, const std::vector<std::string>& games,
                std::string& error) const;
    bool removeOperation(const std::string& operationId, bool deletePreparedFiles,
                         std::string& error) const;

    bool addSelectedToLibrary(const std::string& operationId, std::string& error) const;
    bool library(std::vector<RetroRomOperation>& entries, std::string& error) const;
    bool removeFromLibrary(const std::string& gameId, std::string& error) const;
    bool removeFromLibrary(const std::string& gameId, bool deleteFiles, std::string& error) const;
    bool setGameEmulator(const std::string& gameId, const std::string& emulator,
                         std::string& error) const;
    bool resolveLibraryGame(const std::string& gameId, std::string& romPath, std::string& error,
                            RetroRomGame* game = nullptr,
                            RetroRomOperation* entryOut = nullptr) const;
    bool importDirectory(const std::string& platform, const std::string& dirRelativeToSd,
                         std::vector<RetroRomGame>& imported, std::string& error) const;

    bool loadSettings(RetroSettings& out, std::string& error) const;
    bool saveSettings(const RetroSettings& in, std::string& error) const;

  private:
    std::string operationsRoot() const;
    std::string libraryRoot() const;
    std::string byTaskRoot() const;
    std::string settingsPath() const;
    std::string preparedRoot(const RetroRomOperation& op) const;
    std::string gameFullPath(const RetroRomOperation& op, const RetroRomGame& game) const;

    bool loadLibrary(const std::string& id, RetroRomOperation& entry, std::string& error) const;
    bool loadUnlocked(const std::string& id, RetroRomOperation& operation,
                      std::string& error) const;

    std::string appRoot_, sdRoot_;
    static std::mutex mutex_;
    static std::mutex preparationMutex_;
};

// Recognizes single-game retro torrent layouts (single ROM/archive, ROM + metadata/readme,
// disc sets .cue/.bin/.gdi/.m3u/multi-disc, Wii U code+content+meta, PS Vita eboot.bin+sce_sys).
// Populates actionsOut with 0 (Skip) or 1 (Download) when safe for one-tap install.
bool planRetroAutoSelection(const std::string& platform, bool isRomset,
                            const std::vector<std::string>& filePaths,
                            std::vector<uint8_t>& actionsOut);

} // namespace pipensx
