#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipensx {

// One row of ModCD's table.md: a moddable title and how many mods it carries.
struct ModIndexEntry {
    std::string titleId;   // 16 upper-case hex, normalised to the base app id
    std::string gameName;
    uint32_t modCount = 0;
};

struct ModIndexSnapshot {
    std::vector<ModIndexEntry> items;
    std::string rawTable;
};

// ModCD (https://github.com/kawaii-flesh/ModCD) publishes every mod it knows
// about as one markdown table. pipensx only wants the title-id column, so the
// catalogue rides along with the catalog refresh, is cached on the SD card and
// is then answered entirely from memory — never per card, never per detail page.
class ModIndexService {
public:
    explicit ModIndexService(std::string rootPath,
                             std::string bundledPath = {});

    // Disk only (cache, then the optional bundled fixture). Never touches the
    // network, so it is safe on the startup path.
    bool load(std::string& error);

    // Pool-thread safe: fetch table.md from the trusted source, parse it and
    // persist the cache. Fills `snapshot` and never touches byBaseId_, so the
    // live map keeps serving the previous table through a failure.
    bool fetchLatest(ModIndexSnapshot& snapshot, std::string& error) const;

    // UI-thread only: byBaseId_ is read unsynchronised by the render thread
    // every frame, so it may only be reassigned here.
    void adopt(ModIndexSnapshot snapshot);

    // True when ModCD lists this title (or one of its update/DLC ids).
    bool has(const std::string& titleId) const;
    // Mods listed for this title. 0 means either no entry at all or an entry
    // whose mod markers did not parse — pair it with has() to tell them apart.
    uint32_t modCount(const std::string& titleId) const;

    size_t size() const { return byBaseId_.size(); }

    static bool parseTable(const std::string& markdown,
                           std::vector<ModIndexEntry>& entries,
                           std::string& error);

    // True when `url` is on the trusted-host allowlist for mod-index bytes.
    static bool isTrustedSource(const std::string& url);

    // "0100f2c0115b6800" (update) and "0100F2C0115B7001" (DLC) both normalise
    // to the base application id "0100F2C0115B6000". Empty when `titleId` is
    // not 16 hex characters.
    static std::string normalizeTitleId(const std::string& titleId);

private:
    bool loadFile(const std::string& path, const std::string& label);

    std::string rootPath_;
    std::string catalogRoot_;
    std::string cachePath_;
    std::string bundledPath_;
    std::unordered_map<std::string, uint32_t> byBaseId_;
};

} // namespace pipensx
