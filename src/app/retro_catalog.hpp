#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pipensx {

struct RetroConsoleDef {
    std::string id;
    std::string jsonFile;
    std::string shortBadge;
    std::string defaultName;
    std::string family; // "nintendo", "sony", "sega"
    std::string year;
    std::string defaultRomSubdir;
    std::vector<std::string> launchExtensions;
    bool discBased = false;
    bool supportsM3u = false;
    bool packageInstallInEmulator = false;
};

inline const std::vector<RetroConsoleDef>& retroConsoles() {
    static const std::vector<RetroConsoleDef> consoles = {
        {"nes", "nes_games.json", "NES", "NES / Famicom", "nintendo", "1983", "roms/nes",
         {".nes", ".fds", ".unf", ".unif"}, false, false, false},
        {"snes", "snes_games.json", "SNES", "Super Nintendo", "nintendo", "1990", "roms/snes",
         {".sfc", ".smc", ".fig", ".swc", ".bs"}, false, false, false},
        {"n64", "n64_games.json", "N64", "Nintendo 64", "nintendo", "1996", "roms/n64",
         {".z64", ".n64", ".v64"}, false, false, false},
        {"gbc", "gbc_games.json", "GBC", "Game Boy / Color", "nintendo", "1989", "roms/gbc",
         {".gbc", ".gb", ".sgb"}, false, false, false},
        {"gba", "gba_games.json", "GBA", "Game Boy Advance", "nintendo", "2001", "roms/gba",
         {".gba", ".agb"}, false, false, false},
        {"nds", "nds_games.json", "NDS", "Nintendo DS", "nintendo", "2004", "roms/nds",
         {".nds", ".dsi", ".ids"}, false, false, false},
        {"3ds", "3ds_games.json", "3DS", "Nintendo 3DS", "nintendo", "2011", "roms/3ds",
         {".3ds", ".cci", ".cxi", ".3dsx", ".app", ".cia"}, false, false, false},
        {"gamecube", "gamecube_games.json", "GC", "Nintendo GameCube", "nintendo", "2001", "roms/gamecube",
         {".rvz", ".iso", ".gcm", ".gcz", ".ciso", ".wia", ".dol", ".m3u"}, true, true, false},
        {"wii", "wii_games.json", "Wii", "Nintendo Wii", "nintendo", "2006", "roms/wii",
         {".rvz", ".wbfs", ".iso", ".gcz", ".ciso", ".wia", ".wad", ".m3u"}, true, true, false},
        {"wiiu", "wiiu_games.json", "Wii U", "Nintendo Wii U", "nintendo", "2012", "roms/wiiu",
         {".wua", ".wux", ".wud", ".rpx"}, true, false, false},
        {"ps1", "ps1_games.json", "PS1", "PlayStation 1", "sony", "1994", "roms/ps1",
         {".m3u", ".chd", ".cue", ".pbp", ".iso", ".ecm", ".bin", ".img"}, true, true, false},
        {"ps2", "ps2_games.json", "PS2", "PlayStation 2", "sony", "2000", "roms/ps2",
         {".chd", ".iso", ".cso", ".zso", ".gz", ".cue", ".mdf", ".nrg", ".bin", ".img"}, true, false, false},
        {"psp", "psp_games.json", "PSP", "PlayStation Portable", "sony", "2004", "roms/psp",
         {".iso", ".cso", ".pbp", ".chd"}, true, false, false},
        {"psvita", "psvita_games.json", "PS Vita", "PlayStation Vita", "sony", "2011", "roms/psvita",
         {".vpk", ".zip"}, true, false, true},
        {"sega_ms", "sega_ms_games.json", "SMS", "Sega Master System", "sega", "1985", "roms/sega_ms",
         {".sms", ".sg", ".bin"}, false, false, false},
        {"sega_gg", "sega_gg_games.json", "GG", "Sega Game Gear", "sega", "1990", "roms/sega_gg",
         {".gg", ".bin"}, false, false, false},
        {"sega_md", "sega_md_games.json", "MD", "Sega Mega Drive", "sega", "1988", "roms/sega_md",
         {".md", ".gen", ".smd", ".bin"}, false, false, false},
        {"sega_cd", "sega_cd_games.json", "Sega CD", "Sega CD / Mega CD", "sega", "1991", "roms/sega_cd",
         {".m3u", ".chd", ".cue", ".iso", ".bin"}, true, true, false},
        {"sega_32x", "sega_32x_games.json", "32X", "Sega 32X", "sega", "1994", "roms/sega_32x",
         {".32x", ".bin"}, false, false, false},
        {"dreamcast", "dreamcast_games.json", "DC", "Sega Dreamcast", "sega", "1998", "roms/dreamcast",
         {".m3u", ".chd", ".gdi", ".cdi", ".cue"}, true, true, false},
    };
    return consoles;
}

inline const RetroConsoleDef* findRetroConsole(const std::string& id) {
    for (const auto& console : retroConsoles()) {
        if (console.id == id)
            return &console;
    }
    return nullptr;
}

struct RetroRelease {
    std::string platform, topicId, infoHash, magnetUri;
    std::string title, serial, contentType, description, coverUrl;
    std::string size, year, genre, developer, publisher;
    std::string interfaceLang, voiceLang;
    std::vector<std::string> screenshots;
    bool romset = false;

    std::string key() const { return platform + ":" + topicId; }
};

struct RetroSource {
    std::string dataBaseUrl;
    std::string imageBaseUrl;
};

std::vector<RetroSource> retroCatalogSources(const std::string& preferredSource);
std::vector<RetroSource> gbaCatalogSources(const std::string& preferredSource);

// Publishes only a complete valid snapshot; failure preserves the caller's data.
bool parseRetroCatalog(const std::string& platform, const std::string& body,
                       const RetroSource& source, std::vector<RetroRelease>& releases,
                       std::string& error);
bool parseGbaCatalog(const std::string& body, const RetroSource& source,
                     std::vector<RetroRelease>& releases, std::string& error);

uint64_t parseRetroSizeBytes(const std::string& text);

inline std::string extractRetroYear(const std::string& raw) {
    for (size_t i = 0; i + 4 <= raw.size(); ++i) {
        if (i > 0 && raw[i - 1] >= '0' && raw[i - 1] <= '9')
            continue;
        if (i + 4 < raw.size() && raw[i + 4] >= '0' && raw[i + 4] <= '9')
            continue;
        if (raw[i] >= '0' && raw[i] <= '9' &&
            raw[i + 1] >= '0' && raw[i + 1] <= '9' &&
            raw[i + 2] >= '0' && raw[i + 2] <= '9' &&
            raw[i + 3] >= '0' && raw[i + 3] <= '9') {
            const int y = (raw[i] - '0') * 1000 + (raw[i + 1] - '0') * 100 +
                          (raw[i + 2] - '0') * 10 + (raw[i + 3] - '0');
            if (y >= 1950 && y <= 2099)
                return raw.substr(i, 4);
        }
    }
    return {};
}

struct RetroStats {
    uint64_t seeds = 0, leeches = 0, downloads = 0;
};

struct RetroEmulatorPackage {
    std::string id, name, author, version, description, category;
    std::string downloadUrl, filename, installPath, extractDir;
    std::vector<std::string> supportedConsoleIds;
    uint64_t bytes = 0;
    bool archive = false;
};

struct RetroCatalogSnapshot {
    std::string platform = "gba";
    std::vector<RetroRelease> releases;
    std::map<std::string, RetroStats> stats;
    std::vector<RetroEmulatorPackage> emulators;
    RetroSource source;
};

struct RetroCatalogResult {
    RetroCatalogSnapshot snapshot;
    std::string error;
    bool refreshed = false;
    bool cached = false;
};

class RetroCatalog {
public:
    using Sink = std::function<bool(const char*, size_t)>;
    using Fetcher = std::function<bool(const std::string&, const Sink&,
                                      const std::atomic<bool>&, std::string&)>;
    explicit RetroCatalog(std::string appRoot, Fetcher fetcher = {});

    bool load(RetroCatalogSnapshot& snapshot, std::string& error) const;
    bool load(const std::string& platform, RetroCatalogSnapshot& snapshot, std::string& error) const;
    bool loadShared(std::map<std::string, RetroStats>& stats,
                    std::vector<RetroEmulatorPackage>& emulators,
                    RetroSource& source, std::string& error) const;

    bool refresh(RetroCatalogSnapshot& snapshot, const std::string& preferredSource,
                 const std::atomic<bool>& cancelled, std::string& error) const;
    bool refresh(const std::string& platform, RetroCatalogSnapshot& snapshot,
                 const std::string& preferredSource, bool forceShared,
                 const std::atomic<bool>& cancelled, std::string& error) const;

    std::future<RetroCatalogResult> refreshAsync(const std::string& preferredSource,
        std::shared_ptr<std::atomic<bool>> cancelled) const;
    std::future<RetroCatalogResult> refreshAsync(const std::string& platform,
        const std::string& preferredSource, bool forceShared,
        std::shared_ptr<std::atomic<bool>> cancelled) const;
private:
    std::string root_;
    Fetcher fetcher_;
    std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
};

} // namespace pipensx
