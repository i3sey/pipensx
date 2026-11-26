#include "retro_rom.hpp"

#include "core/sha256.h"
#include "task_files.hpp"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <borealis/extern/nlohmann/json.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

namespace pipensx {
namespace {

using Json = nlohmann::json;

constexpr uint64_t kCartridgeLimit = 2ULL * 1024 * 1024 * 1024;
constexpr uint64_t kDiscLimit = 64ULL * 1024 * 1024 * 1024;
constexpr size_t kCartridgeFileLimit = 8192;
constexpr size_t kDiscFileLimit = 32768;
constexpr size_t kJsonLimit = 8 * 1024 * 1024;

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

std::string folded(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}

bool endsWithCi(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return folded(s.substr(s.size() - suffix.size())) == folded(suffix);
}

bool hexString(const std::string& s, size_t len) {
    return s.size() == len && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool isSaveOrConfigFile(const std::string& path) {
    const std::string lower = folded(path);
    for (const char* ext : {".sav", ".srm", ".eep", ".fla", ".sra", ".mpk", ".mcr", ".mcd",
                            ".ps2", ".vmc", ".state", ".oops", ".rtc", ".cht", ".ini", ".cfg"}) {
        if (endsWithCi(lower, ext))
            return true;
    }
    const size_t dot = lower.find_last_of('.');
    if (dot != std::string::npos && dot + 3 == lower.size() &&
        lower.compare(dot, 3, ".st") == 0) {
        return true;
    }
    return false;
}

bool parseOperationId(const std::string& id, std::string& platformOut, bool& importedOut) {
    if (id.empty() || id.size() > 64)
        return false;
    if (id.rfind("import-", 0) == 0) {
        const size_t rest = 7;
        const size_t dash = id.find('-', rest);
        if (dash == std::string::npos || dash <= rest)
            return false;
        const std::string plat = id.substr(rest, dash - rest);
        const std::string hashPart = id.substr(dash + 1);
        if (!findRetroConsole(plat) || !hexString(hashPart, 12))
            return false;
        platformOut = plat;
        importedOut = true;
        return true;
    }
    const size_t dash = id.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= id.size())
        return false;
    const std::string plat = id.substr(0, dash);
    const std::string topic = id.substr(dash + 1);
    if (!findRetroConsole(plat) || topic.size() > 20 || topic[0] == '0' ||
        topic.find_first_not_of("0123456789") != std::string::npos)
        return false;
    platformOut = plat;
    importedOut = false;
    return true;
}

bool idValid(const std::string& id) {
    std::string plat;
    bool imp = false;
    return parseOperationId(id, plat, imp);
}

bool safe(const std::string& path) {
    return path.size() <= 700 && taskFilePathIsFatCompatible(path);
}

bool destinationValid(const std::string& path) {
    if (!safe(path) || path.size() > 400)
        return false;
    const auto lower = folded(path);
    for (const auto* root : {"nintendo", "atmosphere", "bootloader", "switch/pipensx"}) {
        if (lower == root || lower.rfind(std::string(root) + "/", 0) == 0)
            return false;
    }
    return true;
}

bool deviceRoot(const std::string& path) {
#ifdef __SWITCH__
    return !path.empty() && path.back() == ':' && path.find('/') == std::string::npos;
#else
    (void)path;
    return false;
#endif
}

bool directory(const std::string& path) {
    if (deviceRoot(path))
        return true;
    struct stat st {};
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool directories(const std::string& path) {
    if (path.empty()) return false;
    size_t start = path.front() == '/' ? 1 : 0;
#ifdef __SWITCH__
    if (path.rfind("sdmc:/", 0) == 0)
        start = 6;
#endif
    for (size_t slash = path.find('/', start); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        if (!directory(path.substr(0, slash)))
            return false;
    }
    return directory(path);
}

bool absent(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) != 0 && errno == ENOENT;
}

bool regular(const std::string& path, uint64_t* size = nullptr) {
    if (path.empty()) return false;
    size_t start = path.front() == '/' ? 1 : 0;
#ifdef __SWITCH__
    if (path.rfind("sdmc:/", 0) == 0)
        start = 6;
#endif
    struct stat st {};
    for (size_t slash = path.find('/', start); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        const auto parent = path.substr(0, slash);
        if (!deviceRoot(parent) &&
            (lstat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
            return false;
    }
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    if (size)
        *size = static_cast<uint64_t>(st.st_size);
    return true;
}

bool isDirNoSymlink(const std::string& path) {
    if (path.empty()) return false;
    size_t start = path.front() == '/' ? 1 : 0;
#ifdef __SWITCH__
    if (path.rfind("sdmc:/", 0) == 0)
        start = 6;
#endif
    struct stat st {};
    for (size_t slash = path.find('/', start); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        const auto parent = path.substr(0, slash);
        if (!deviceRoot(parent) &&
            (lstat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
            return false;
    }
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool eraseStage(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    while (auto* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != ".." && !eraseStage(path + "/" + name))
            ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
}

bool erasePreservingSaves(const std::string& path, bool& keptSaves) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) {
        if (isSaveOrConfigFile(path)) {
            keptSaves = true;
            return true;
        }
        return unlink(path.c_str()) == 0;
    }
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    bool childKeptSaves = false;
    while (auto* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            if (!erasePreservingSaves(path + "/" + name, childKeptSaves))
                ok = false;
        }
    }
    closedir(dir);
    if (childKeptSaves) {
        keptSaves = true;
        return ok;
    }
    return ok && rmdir(path.c_str()) == 0;
}

bool readOne(const std::string& path, Json& json) {
    uint64_t size = 0;
    if (!regular(path, &size) || size > kJsonLimit)
        return false;
    std::ifstream file(path, std::ios::binary);
    std::string body(static_cast<size_t>(size), '\0');
    if (!file.read(&body[0], static_cast<std::streamsize>(body.size())) ||
        file.peek() != std::char_traits<char>::eof())
        return false;
    json = Json::parse(body, nullptr, false);
    return !json.is_discarded();
}

bool read(const std::string& path, Json& json) {
    return readOne(path, json) || readOne(path + ".previous", json);
}

bool decode(const Json& json, RetroRomOperation& out);

bool write(const std::string& path, const Json& json, bool validateAsOperation = true) {
    const auto body = json.dump();
    if (body.size() > kJsonLimit)
        return false;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !directories(path.substr(0, slash)))
        return false;
    const auto temporary = path + ".tmp";
    if (!absent(temporary) && (!regular(temporary) || unlink(temporary.c_str()) != 0))
        return false;
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temporary.c_str());
        return false;
    }
    bool ok = fwrite(body.data(), 1, body.size(), file) == body.size();
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;
    if (ok && !absent(path)) {
        Json old;
        RetroRomOperation parsed;
        bool valid = readOne(path, old);
        if (valid && validateAsOperation && json.contains("games"))
            valid = decode(old, parsed);
        if (!regular(path)) {
            ok = false;
        } else if (valid) {
            ok = absent(path + ".previous") ||
                 (regular(path + ".previous") && unlink((path + ".previous").c_str()) == 0);
            if (ok)
                ok = rename(path.c_str(), (path + ".previous").c_str()) == 0;
        } else {
            ok = unlink(path.c_str()) == 0;
        }
    }
    if (ok && rename(temporary.c_str(), path.c_str()) != 0)
        ok = false;
    if (!ok)
        unlink(temporary.c_str());
    return ok;
}

std::string parentDirOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool writeSmallTextFile(const std::string& path, const std::string& text) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !directories(path.substr(0, slash)))
        return false;
    const std::string tmp = path + ".tmp";
    if (!absent(tmp)) unlink(tmp.c_str());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t sent = 0;
    bool ok = true;
    while (sent < text.size()) {
        const ssize_t n = ::write(fd, text.data() + sent, text.size() - sent);
        if (n <= 0) { ok = false; break; }
        sent += static_cast<size_t>(n);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) {
        ok = regular(path) && unlink(path.c_str()) == 0 &&
             rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) unlink(tmp.c_str());
    return ok;
}

bool scanPreservedSavesDir(const std::string& rootDir, const std::string& relSub, unsigned depth,
                           std::vector<std::string>& relSaves) {
    if (depth > 8)
        return false;
    const std::string full = relSub.empty() ? rootDir : (rootDir + "/" + relSub);
    DIR* dir = opendir(full.c_str());
    if (!dir)
        return false;
    bool ok = true;
    while (auto* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string rel = relSub.empty() ? name : (relSub + "/" + name);
        const std::string child = rootDir + "/" + rel;
        struct stat st {};
        if (lstat(child.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!scanPreservedSavesDir(rootDir, rel, depth + 1, relSaves)) {
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (rel == ".pipensx-saves.json")
                continue;
            if (!safe(rel) || !isSaveOrConfigFile(rel)) {
                ok = false;
                break;
            }
            relSaves.push_back(rel);
        } else {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

bool collectOwnedPreservedSaves(const std::string& dir, const std::string& expectedOpId,
                                std::vector<std::string>& relSaves) {
    relSaves.clear();
    if (!isDirNoSymlink(dir))
        return false;
    Json marker;
    if (!readOne(dir + "/.pipensx-saves.json", marker) || !marker.is_object() ||
        marker.value("id", std::string()) != expectedOpId)
        return false;
    return scanPreservedSavesDir(dir, {}, 0, relSaves);
}

bool copyPreservedSaveFile(const std::string& src, const std::string& dst) {
    uint64_t size = 0;
    if (!regular(src, &size) || !directories(parentDirOf(dst)))
        return false;
    FILE* in = fopen(src.c_str(), "rb");
    if (!in)
        return false;
    const int outFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (outFd < 0) {
        fclose(in);
        return false;
    }
    std::array<char, 64 * 1024> buf{};
    uint64_t written = 0;
    bool ok = true;
    while (ok) {
        const size_t n = fread(buf.data(), 1, buf.size(), in);
        if (ferror(in)) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        size_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(outFd, buf.data() + off, n - off);
            if (w <= 0) {
                ok = false;
                break;
            }
            off += static_cast<size_t>(w);
        }
        written += n;
    }
    if (ok && (written != size || fsync(outFd) != 0))
        ok = false;
    if (close(outFd) != 0)
        ok = false;
    fclose(in);
    return ok;
}

Json encode(const RetroRomOperation& op) {
    Json games = Json::array();
    for (const auto& game : op.games) {
        Json item = {
            {"id", game.id},
            {"title", game.title},
            {"path", game.path},
            {"selected", game.selected},
        };
        if (!game.emulator.empty())
            item["emulator"] = game.emulator;
        if (game.size > 0)
            item["size"] = game.size;
        games.push_back(std::move(item));
    }
    Json out = {
        {"format", 1},
        {"id", op.id},
        {"platform", op.platform},
        {"task", op.taskId},
        {"topic", op.topicId},
        {"title", op.title},
        {"destination", op.destination},
        {"emulator", op.emulator},
        {"ready", op.ready},
        {"create_forwarder", op.createForwarder},
        {"games", std::move(games)},
    };
    if (!op.coverUrl.empty())
        out["cover"] = op.coverUrl;
    return out;
}

bool decode(const Json& json, RetroRomOperation& out) {
    if (!json.is_object() || json.value("format", Json()) != 1)
        return false;
    for (const auto* key : {"id", "task", "topic", "title", "destination", "emulator"}) {
        if (!json.contains(key) || !json[key].is_string())
            return false;
    }
    RetroRomOperation op;
    op.id = json["id"];
    op.taskId = json["task"];
    op.topicId = json["topic"];
    op.title = json["title"];
    op.destination = json["destination"];
    op.emulator = json["emulator"];
    std::string inferredPlatform;
    bool imported = false;
    if (!parseOperationId(op.id, inferredPlatform, imported))
        return false;
    if (json.contains("platform")) {
        if (!json["platform"].is_string())
            return false;
        op.platform = json["platform"];
        if (op.platform != inferredPlatform)
            return false;
    } else {
        op.platform = inferredPlatform;
    }
    if (json.contains("cover")) {
        if (!json["cover"].is_string())
            return false;
        op.coverUrl = json["cover"];
        if (op.coverUrl.size() > 1024)
            return false;
    }
    if (!imported) {
        if (op.id != op.platform + "-" + op.topicId)
            return false;
    }
    const auto* console = findRetroConsole(op.platform);
    if (!console || !hexString(op.taskId, 40) || !destinationValid(op.destination) ||
        op.title.empty() || op.title.size() > 1024 ||
        !isValidEmulatorSelection(op.platform, op.emulator) ||
        !json.contains("ready") || !json["ready"].is_boolean() ||
        !json.contains("games") || !json["games"].is_array() ||
        json["games"].size() > kDiscFileLimit)
        return false;
    op.ready = json["ready"];
    if (json.contains("create_forwarder")) {
        if (!json["create_forwarder"].is_boolean())
            return false;
        op.createForwarder = json["create_forwarder"].get<bool>();
    }
    std::set<std::string> paths, ids;
    for (const auto& game : json["games"]) {
        if (!game.is_object())
            return false;
        for (const auto* key : {"id", "title", "path"}) {
            if (!game.contains(key) || !game[key].is_string())
                return false;
        }
        RetroRomGame g;
        g.id = game["id"];
        g.title = game["title"];
        g.path = game["path"];
        if (game.contains("emulator")) {
            if (!game["emulator"].is_string())
                return false;
            g.emulator = game["emulator"];
            if (!isValidEmulatorSelection(op.platform, g.emulator))
                return false;
        }
        if (game.contains("size")) {
            if (!game["size"].is_number_unsigned())
                return false;
            g.size = game["size"].get<uint64_t>();
        }
        if (!safe(g.path) || g.id.size() != op.id.size() + 65 || g.id.rfind(op.id + ":", 0) != 0 ||
            !hexString(g.id.substr(op.id.size() + 1), 64) ||
            g.title.empty() || g.title.size() > 1024 || !paths.insert(folded(g.path)).second ||
            !ids.insert(g.id).second || !game.contains("selected") ||
            !game["selected"].is_boolean())
            return false;
        g.selected = game["selected"];
        op.games.push_back(std::move(g));
    }
    if (op.ready != !op.games.empty())
        return false;
    out = std::move(op);
    return true;
}

std::string hash(const std::string& value) {
    uint8_t digest[32];
    sha256(value.data(), value.size(), digest);
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", byte);
        result += hex;
    }
    return result;
}

bool room(const std::string& root, uint64_t bytes) {
    struct statvfs st {};
    if (statvfs(root.c_str(), &st) != 0)
        return false;
    const uint64_t freeBytes = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    constexpr uint64_t kReserve = 16ULL * 1024 * 1024;
    return freeBytes >= bytes && freeBytes - bytes >= kReserve;
}

std::string suffix(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    return folded(path.substr(dot));
}

std::string baseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stemOf(const std::string& filename) {
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return filename;
    return filename.substr(0, dot);
}

bool rom(const std::string& path, std::string& title) {
    uint64_t size = 0;
    if (!regular(path, &size) || size < 192 || size > 64ULL * 1024 * 1024)
        return false;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::array<uint8_t, 192> h{};
    const bool ok = fread(h.data(), 1, h.size(), file) == h.size();
    fclose(file);
    uint8_t sum = 0x19;
    for (size_t i = 0xa0; i <= 0xbc; ++i)
        sum += h[i];
    if (!ok || h[0xb2] != 0x96 || uint8_t(sum + h[0xbd]) != 0)
        return false;
    title.clear();
    for (size_t i = 0xa0; i < 0xac && h[i]; ++i) {
        if (h[i] < 32 || h[i] > 126) {
            title.clear();
            break;
        }
        title += char(h[i]);
    }
    while (!title.empty() && title.back() == ' ')
        title.pop_back();
    if (title.empty()) {
        title = baseNameOf(path);
        if (title.size() > 4)
            title.resize(title.size() - 4);
    }
    return true;
}

bool consoleHasExtension(const RetroConsoleDef& console, const std::string& ext) {
    for (const auto& candidate : console.launchExtensions) {
        if (ext == candidate)
            return true;
    }
    return false;
}

bool validateRomForConsole(const RetroConsoleDef& console, const std::string& fullPath,
                           bool strictGbaHeader, std::string& titleOut, uint64_t& sizeOut) {
    const std::string ext = suffix(fullPath);
    if (console.id == "psvita" && folded(baseNameOf(fullPath)) == "eboot.bin") {
        const std::string dir = parentDirOf(fullPath);
        const std::string folderName = baseNameOf(dir);
        if (regular(fullPath, &sizeOut) && sizeOut >= 16 && regular(dir + "/sce_sys/param.sfo")) {
            titleOut = (!folderName.empty() && folderName != "files" && folderName.rfind("unpacked", 0) != 0)
                ? folderName
                : "PS Vita Game";
            return true;
        }
        for (const char* prefix : {"sdmc:/switch/Vita3K/vita/ux0/app/", "sdmc:/switch/vita3k/vita/ux0/app/"}) {
            if (!folderName.empty() &&
                regular(std::string(prefix) + folderName + "/eboot.bin", &sizeOut) && sizeOut >= 16 &&
                regular(std::string(prefix) + folderName + "/sce_sys/param.sfo")) {
                titleOut = folderName;
                return true;
            }
        }
        return false;
    }
    if (!consoleHasExtension(console, ext))
        return false;
    if (!regular(fullPath, &sizeOut))
        return false;
    if (console.id == "gba" && strictGbaHeader) {
        return rom(fullPath, titleOut);
    }
    const uint64_t minBytes = (ext == ".m3u") ? 4 : 16;
    if (sizeOut < minBytes)
        return false;
    titleOut = stemOf(baseNameOf(fullPath));
    return true;
}

void collectM3uDiscPaths(const std::string& rootDir, const std::string& m3uRelPath,
                         std::vector<std::string>& resolvedRelPaths) {
    resolvedRelPaths.clear();
    const std::string fullPath = rootDir + "/" + m3uRelPath;
    uint64_t sz = 0;
    if (!regular(fullPath, &sz) || sz == 0 || sz > 64 * 1024)
        return;
    std::ifstream in(fullPath, std::ios::binary);
    std::string content(static_cast<size_t>(sz), '\0');
    if (!in.read(&content[0], static_cast<std::streamsize>(content.size())))
        return;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    const std::string baseDir = parentDirOf(m3uRelPath);
    std::vector<std::string> baseSegs;
    {
        std::istringstream bss(baseDir);
        std::string seg;
        while (std::getline(bss, seg, '/')) {
            if (!seg.empty() && seg != ".")
                baseSegs.push_back(seg);
        }
    }

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        const size_t last = line.find_last_not_of(" \t");
        line = line.substr(first, last - first + 1);
        if (line.empty() || line[0] == '#')
            continue;
        for (char& c : line) {
            if (c == '\\')
                c = '/';
        }
        if (line.front() == '/' || line.find(':') != std::string::npos)
            continue;
        std::vector<std::string> joined = baseSegs;
        std::istringstream lss(line);
        std::string seg;
        bool valid = true;
        while (std::getline(lss, seg, '/')) {
            if (seg.empty() || seg == ".")
                continue;
            if (seg == "..") {
                if (joined.empty()) {
                    valid = false;
                    break;
                }
                joined.pop_back();
            } else {
                joined.push_back(seg);
            }
        }
        if (!valid || joined.empty())
            continue;
        std::string rel;
        for (size_t i = 0; i < joined.size(); ++i) {
            if (i > 0)
                rel += '/';
            rel += joined[i];
        }
        resolvedRelPaths.push_back(std::move(rel));
    }
}

struct RarFileChecksum {
    uint64_t size = 0;
    std::optional<uint32_t> crc;
};

struct RarChecksums {
    int version = 0;
    std::vector<RarFileChecksum> files;
};

uint32_t little(const std::vector<uint8_t>& data, size_t at, size_t count) {
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= uint32_t(data[at + i]) << (i * 8);
    return value;
}

bool variable(const std::vector<uint8_t>& data, size_t& at, uint64_t& value) {
    value = 0;
    for (unsigned i = 0; i < 10 && at < data.size(); ++i) {
        const auto byte = data[at++];
        if (i == 9 && byte > 1)
            return false;
        value |= uint64_t(byte & 127) << (7 * i);
        if (!(byte & 128))
            return true;
    }
    return false;
}

// libarchive 3.7.2 does not verify the CRC of stored RAR5 members.
bool rarChecksums(const std::string& path, RarChecksums& checksums,
                  const std::atomic<bool>& cancelled, size_t maxEntries, std::string& error) {
    uint64_t fileSize = 0;
    if (!regular(path, &fileSize))
        return fail(error, "Missing ROM archive.");
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return fail(error, "Unable to inspect ROM archive.");
    struct Close {
        FILE* file;
        ~Close() { fclose(file); }
    } close{file};
    std::array<uint8_t, 8> signature{};
    const auto count = fread(signature.data(), 1, signature.size(), file);
    if (count >= 7 && std::memcmp(signature.data(), "Rar!\x1a\x07\0", 7) == 0)
        checksums.version = 4;
    else if (count == 8 && std::memcmp(signature.data(), "Rar!\x1a\x07\x01\0", 8) == 0)
        checksums.version = 5;
    else
        return true;
    if (fseeko(file, checksums.version == 4 ? 7 : 8, SEEK_SET) != 0)
        return false;
    size_t blocks = 0;
    auto invalid = [&] { return fail(error, "Invalid or unsupported RAR header."); };
    while (uint64_t(ftello(file)) < fileSize) {
        if (cancelled)
            return fail(error, "Cancelled.");
        if (++blocks > maxEntries)
            return fail(error, "Too many RAR headers.");
        uint64_t packed = 0;
        if (checksums.version == 4) {
            std::vector<uint8_t> header(7);
            if (fread(header.data(), 1, 7, file) != 7)
                return invalid();
            const auto type = header[2];
            const auto flags = little(header, 3, 2), size = little(header, 5, 2);
            if (size < 7)
                return invalid();
            header.resize(size);
            if (fread(header.data() + 7, 1, size - 7, file) != size - 7)
                return invalid();
            if ((crc32(0, header.data() + 2, size - 2) & 65535) != little(header, 0, 2))
                return fail(error, "RAR header CRC mismatch.");
            if (type == 0x73 && (flags & 0x81))
                return fail(error, "Encrypted or multi-volume RAR archives are not supported.");
            if (flags & 0x8000) {
                if (size < 11)
                    return invalid();
                packed = little(header, 7, 4);
            }
            if (type == 0x74) {
                if (size < 32)
                    return invalid();
                if (flags & 7)
                    return fail(error, "Encrypted or multi-volume RAR members are not supported.");
                uint64_t unpacked = little(header, 11, 4);
                if (flags & 0x100) {
                    if (size < 40)
                        return invalid();
                    packed |= uint64_t(little(header, 32, 4)) << 32;
                    unpacked |= uint64_t(little(header, 36, 4)) << 32;
                }
                checksums.files.push_back({unpacked, little(header, 16, 4)});
            }
        } else {
            std::vector<uint8_t> crcBytes(4), sizeBytes;
            if (fread(crcBytes.data(), 1, 4, file) != 4)
                return invalid();
            for (unsigned i = 0; i < 10; ++i) {
                const int byte = fgetc(file);
                if (byte == EOF)
                    return invalid();
                sizeBytes.push_back(static_cast<uint8_t>(byte));
                if (!(byte & 128))
                    break;
            }
            size_t cursor = 0;
            uint64_t size = 0;
            if (!variable(sizeBytes, cursor, size) || size > 2 * 1024 * 1024 || size < 2)
                return invalid();
            std::vector<uint8_t> header(size);
            if (fread(header.data(), 1, size, file) != size)
                return invalid();
            auto crc = crc32(0, sizeBytes.data(), sizeBytes.size());
            crc = crc32(crc, header.data(), header.size());
            if (uint32_t(crc) != little(crcBytes, 0, 4))
                return fail(error, "RAR5 header CRC mismatch.");
            cursor = 0;
            uint64_t type, flags, extra = 0;
            if (!variable(header, cursor, type) || !variable(header, cursor, flags))
                return invalid();
            if (flags & 24)
                return fail(error, "Multi-volume RAR5 archives are not supported.");
            if ((flags & 1) && !variable(header, cursor, extra))
                return invalid();
            if ((flags & 2) && !variable(header, cursor, packed))
                return invalid();
            if (extra > header.size() - cursor)
                return invalid();
            if (type == 4)
                return fail(error, "Password-protected RAR5 archives are not supported.");
            if (type == 1) {
                uint64_t archiveFlags;
                if (!variable(header, cursor, archiveFlags))
                    return invalid();
                if (archiveFlags & 1)
                    return fail(error, "Multi-volume RAR5 archives are not supported.");
            }
            if (type == 2) {
                uint64_t fileFlags, unpacked, attributes;
                if (!variable(header, cursor, fileFlags) || !variable(header, cursor, unpacked) ||
                    !variable(header, cursor, attributes))
                    return invalid();
                if (fileFlags & 8)
                    return fail(error, "RAR5 members with unknown size are not supported.");
                if (fileFlags & 2) {
                    if (header.size() - cursor < 4)
                        return invalid();
                    cursor += 4;
                }
                std::optional<uint32_t> expected;
                if (fileFlags & 4) {
                    if (header.size() - cursor < 4)
                        return invalid();
                    expected = little(header, cursor, 4);
                }
                checksums.files.push_back({unpacked, expected});
            }
        }
        const auto offset = ftello(file);
        if (offset < 0 || uint64_t(offset) > fileSize || packed > fileSize - uint64_t(offset))
            return fail(error, "Truncated or multi-volume RAR archive.");
        if (checksums.files.size() > maxEntries || fseeko(file, packed, SEEK_CUR) != 0)
            return invalid();
    }
    return true;
}

bool archiveContainsExtension(const std::string& archivePath, const std::string& ext) {
    archive* reader = archive_read_new();
    if (!reader) return false;
    struct Release {
        archive* a;
        ~Release() { archive_read_free(a); }
    } release{reader};
    archive_read_support_format_zip(reader);
    archive_read_support_format_7zip(reader);
    archive_read_support_format_rar(reader);
    archive_read_support_format_rar5(reader);
    if (archive_read_open_filename(reader, archivePath.c_str(), 64 * 1024) != ARCHIVE_OK)
        return false;
    archive_entry* entry = nullptr;
    size_t count = 0;
    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK && ++count <= kDiscFileLimit) {
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name) name = archive_entry_pathname(entry);
        if (name && archive_entry_filetype(entry) == AE_IFREG && endsWithCi(name, ext))
            return true;
    }
    return false;
}

struct Workspace {
    std::string root;
    const std::atomic<bool>& cancelled;
    uint64_t maxExpandedBytes;
    size_t maxFiles;
    uint64_t expanded = 0;
    size_t entries = 0;
    RetroRomStore::PrepareProgress progressCb;
    uint64_t progressDoneBytes = 0;
    uint64_t progressTotalBytes = 0;
    size_t progressDoneFiles = 0;
    size_t progressTotalFiles = 0;
    struct File {
        std::string relativePath;
        std::string sourcePath; // empty => root + "/" + relativePath
        unsigned depth;
        bool extracted;
    };
    std::vector<File> queue;
    std::vector<std::string> stagedRegularFiles; // relative inside root

    bool output(const std::string& relative, uint64_t size,
                const std::function<ssize_t(char*, size_t)>& reader, bool unpacked,
                std::string& error, const std::optional<uint32_t>& expectedCrc = std::nullopt) {
        if (!safe(relative) || (!unpacked && ++entries > maxFiles) ||
            (unpacked && size > maxExpandedBytes - expanded) || !room(root, size))
            return fail(error,
                        "ROM preparation exceeds its path, file count, size or free-space limit.");
        const auto path = root + "/" + relative;
        if (!directories(parentDirOf(path)))
            return fail(error, "Unable to create ROM directory.");
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            return fail(error, "ROM destination collision or write failure: " + relative);
        std::array<char, 64 * 1024> buffer{};
        uint64_t written = 0;
        uint32_t crc = 0;
        bool ok = true;
        while (ok) {
            if (cancelled) {
                ok = fail(error, "Cancelled.");
                break;
            }
            const auto count = reader(buffer.data(), buffer.size());
            if (count < 0) {
                if (error.empty())
                    error = "Unable to read ROM input: " + relative;
                ok = false;
                break;
            }
            if (uint64_t(count) > size - written) {
                ok = fail(error,
                          "Archive data exceeds its declared size or is corrupt: " + relative);
                break;
            }
            if (!count)
                break;
            if (expectedCrc)
                crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()),
                            static_cast<uInt>(count));
            size_t offset = 0;
            while (offset < static_cast<size_t>(count)) {
                const auto n = ::write(fd, buffer.data() + offset, static_cast<size_t>(count) - offset);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                offset += static_cast<size_t>(n);
            }
            written += static_cast<uint64_t>(count);
            progressDoneBytes += static_cast<uint64_t>(count);
            if (progressCb) {
                progressCb(std::min(progressDoneBytes, std::max(progressDoneBytes, progressTotalBytes)),
                           std::max(progressDoneBytes, progressTotalBytes),
                           progressDoneFiles,
                           std::max(progressDoneFiles, progressTotalFiles));
            }
        }
        if (ok && expectedCrc && crc != *expectedCrc) {
            error = "ROM archive file CRC mismatch: " + relative;
            ok = false;
        }
        // Only fsync extracted archive members or the final file in a multi-file batch.
        // Calling fsync() 3000+ times on Switch SD card during unpacked folder copies stalls for minutes.
        const bool needSync = unpacked || (progressDoneFiles + 1 >= progressTotalFiles);
        if (written != size || (needSync && fsync(fd) != 0))
            ok = false;
        if (close(fd) != 0)
            ok = false;
        if (!ok) {
            if (error.empty())
                error = "Unable to write complete ROM data.";
            return false;
        }
        if (unpacked)
            expanded += written;
        ++progressDoneFiles;
        if (progressCb) {
            progressCb(std::min(progressDoneBytes, std::max(progressDoneBytes, progressTotalBytes)),
                       std::max(progressDoneBytes, progressTotalBytes),
                       progressDoneFiles,
                       std::max(progressDoneFiles, progressTotalFiles));
        }
        stagedRegularFiles.push_back(relative);
        return true;
    }

    bool unpack(const File& file, std::string& error) {
        if (file.depth >= 3)
            return fail(error, "Nested archive depth exceeds 3: " + file.relativePath);
        const std::string archiveDiskPath =
            file.sourcePath.empty() ? (root + "/" + file.relativePath) : file.sourcePath;
        RarChecksums rar;
        if (!rarChecksums(archiveDiskPath, rar, cancelled, maxFiles, error))
            return false;
        uint64_t declared = 0;
        for (const auto& entry : rar.files) {
            if (entry.size > maxExpandedBytes - expanded - declared)
                return fail(error, "RAR unpacked data exceeds the preparation limit.");
            declared += entry.size;
        }
        size_t rarIndex = 0;
        archive* reader = archive_read_new();
        if (!reader)
            return fail(error, "Unable to initialize ROM archive reader.");
        struct Release {
            archive* a;
            ~Release() { archive_read_free(a); }
        } release{reader};
        archive_read_support_format_zip(reader);
        archive_read_support_format_7zip(reader);
        archive_read_support_format_rar(reader);
        archive_read_support_format_rar5(reader);
        if (archive_read_open_filename(reader, archiveDiskPath.c_str(), 64 * 1024) != ARCHIVE_OK) {
            return fail(error, "Unable to open ROM archive: " + file.relativePath + ": " +
                                   (archive_error_string(reader) ? archive_error_string(reader)
                                                                 : "unsupported format"));
        }
        archive_entry* entry = nullptr;
        int status = ARCHIVE_OK;
        const auto base = "unpacked/" + hash(file.relativePath) + "/";
        std::set<std::string> paths;
        while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
            if (cancelled)
                return fail(error, "Cancelled.");
            if (archive_entry_is_encrypted(entry) > 0)
                return fail(error, "Password-protected ROM archives are not supported.");
            if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
                return fail(error, "ROM archive links are not supported.");
            const char* name = archive_entry_pathname_utf8(entry);
            if (!name)
                name = archive_entry_pathname(entry);
            std::string relative = name ? name : "";
            const auto type = archive_entry_filetype(entry);
            std::optional<uint32_t> expected;
            if (rar.version) {
                if (rarIndex >= rar.files.size() || archive_entry_size(entry) < 0 ||
                    uint64_t(archive_entry_size(entry)) != rar.files[rarIndex].size)
                    return fail(error, "RAR header and decoded entry disagree.");
                expected = rar.files[rarIndex++].crc;
            } else if (archive_format(reader) == ARCHIVE_FORMAT_RAR ||
                       archive_format(reader) == ARCHIVE_FORMAT_RAR_V5) {
                return fail(error, "Embedded or unsupported RAR signature.");
            }
            if (type == AE_IFDIR && !relative.empty() && relative.back() == '/')
                relative.pop_back();
            if (!safe(relative) || !paths.insert(folded(relative)).second || ++entries > maxFiles)
                return fail(error, "Unsafe or colliding ROM archive path.");
            if (type == AE_IFDIR)
                continue;
            if (type != AE_IFREG || !archive_entry_size_is_set(entry) ||
                archive_entry_size(entry) < 0)
                return fail(error, "Unsupported ROM archive member.");
            const auto target = base + relative;
            if (!output(
                    target, static_cast<uint64_t>(archive_entry_size(entry)),
                    [&](char* data, size_t size) {
                        const auto count = archive_read_data(reader, data, size);
                        if (count < 0) {
                            error = std::string("ROM archive decompression failed: ") +
                                    (archive_error_string(reader) ? archive_error_string(reader)
                                                                  : target);
                        }
                        return count;
                    },
                    true, error, expected))
                return false;
            queue.push_back({target, {}, file.depth + 1, true});
        }
        if (status != ARCHIVE_EOF || archive_read_has_encrypted_entries(reader) > 0) {
            return fail(
                error,
                std::string("Corrupt, encrypted or incomplete multi-volume archive: ") +
                    (archive_error_string(reader) ? archive_error_string(reader)
                                                  : file.relativePath));
        }
        if (rar.version && rarIndex != rar.files.size())
            return fail(error, "Incomplete RAR entry list.");
        return true;
    }
};

bool isMultiVolumeArchive(const std::string& path) {
    const auto extension = suffix(path);
    const auto lower = folded(path);
    const auto part = lower.rfind(".part");
    const bool rarPart =
        extension == ".rar" && part != std::string::npos && lower.size() > part + 9 &&
        lower.substr(part + 5, lower.size() - part - 9).find_first_not_of("0123456789") ==
            std::string::npos;
    const bool numbered =
        extension.size() == 4 &&
        extension.find_first_not_of("0123456789", 1) == std::string::npos;
    const bool split =
        extension.size() == 4 && (extension[1] == 'r' || extension[1] == 'z') &&
        extension.find_first_not_of("0123456789", 2) == std::string::npos;
    return rarPart || numbered || split;
}

bool isArchivePath(const std::string& path) {
    const std::string ext = suffix(path);
    return ext == ".zip" || ext == ".7z" || ext == ".rar";
}

// Convert orphan .ccd/.mds into sibling .cue and repair FILE case mismatches in .cue files.
void normalizeDiscDescriptors(const std::string& root, const RetroConsoleDef& console,
                              std::vector<std::string>& relativeFiles) {
    if (!console.discBased)
        return;

    std::map<std::string, std::vector<std::string>> byDir;
    for (const auto& rel : relativeFiles)
        byDir[parentDirOf(rel)].push_back(baseNameOf(rel));

    std::vector<std::string> created;
    for (const auto& kv : byDir) {
        const std::string& dir = kv.first;
        const std::vector<std::string>& names = kv.second;

        std::map<std::string, std::string> lowerToActual;
        std::set<std::string> cueStemsLower;
        for (const auto& n : names) {
            lowerToActual[folded(n)] = n;
            if (suffix(n) == ".cue")
                cueStemsLower.insert(folded(stemOf(n)));
        }

        // Repair FILE "..." casing in existing .cue files.
        for (const auto& n : names) {
            if (suffix(n) != ".cue") continue;
            const std::string cuePath = root + "/" + (dir.empty() ? n : (dir + "/" + n));
            uint64_t sz = 0;
            if (!regular(cuePath, &sz) || sz == 0 || sz > 512 * 1024) continue;
            std::ifstream in(cuePath, std::ios::binary);
            std::string content(static_cast<size_t>(sz), '\0');
            if (!in.read(&content[0], static_cast<std::streamsize>(content.size()))) continue;
            std::istringstream iss(content);
            std::string line;
            std::string rebuilt;
            bool modified = false;
            while (std::getline(iss, line)) {
                bool hadCr = !line.empty() && line.back() == '\r';
                if (hadCr) line.pop_back();
                const std::string trimmed = [&]() {
                    size_t s = line.find_first_not_of(" \t");
                    return s == std::string::npos ? std::string() : line.substr(s);
                }();
                if (folded(trimmed.substr(0, 5)) == "file ") {
                    const size_t q1 = line.find('"');
                    const size_t q2 = q1 != std::string::npos ? line.find('"', q1 + 1) : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        const std::string ref = line.substr(q1 + 1, q2 - q1 - 1);
                        const std::string refBase = baseNameOf(ref);
                        const auto it = lowerToActual.find(folded(refBase));
                        if (it != lowerToActual.end() && it->second != ref) {
                            line.replace(q1 + 1, q2 - q1 - 1, it->second);
                            modified = true;
                        }
                    }
                }
                rebuilt += line;
                if (hadCr) rebuilt += '\r';
                rebuilt += '\n';
            }
            if (modified)
                writeSmallTextFile(cuePath, rebuilt);
        }

        // Generate .cue for orphan .ccd (and orphan .mds for PS1 when .mdf/.img/.bin sibling exists).
        for (const auto& n : names) {
            const std::string ext = suffix(n);
            if (ext != ".ccd" && !(ext == ".mds" && console.id == "ps1"))
                continue;
            const std::string stem = stemOf(n);
            if (cueStemsLower.count(folded(stem)))
                continue;
            std::string imageActual;
            for (const char* imgExt : {".img", ".bin", ".mdf"}) {
                auto it = lowerToActual.find(folded(stem + imgExt));
                if (it != lowerToActual.end()) {
                    imageActual = it->second;
                    break;
                }
            }
            if (imageActual.empty())
                continue;
            const char* mode = (console.id == "ps1" || console.id == "ps2") ? "MODE2/2352" : "MODE1/2352";
            const std::string cueContent =
                "FILE \"" + imageActual + "\" BINARY\n  TRACK 01 " + mode + "\n    INDEX 01 00:00:00\n";
            const std::string cueRel = (dir.empty() ? "" : (dir + "/")) + stem + ".cue";
            const std::string cueAbs = root + "/" + cueRel;
            if (absent(cueAbs) && writeSmallTextFile(cueAbs, cueContent)) {
                cueStemsLower.insert(folded(stem));
                created.push_back(cueRel);
            }
        }
    }
    relativeFiles.insert(relativeFiles.end(), created.begin(), created.end());
}

bool extractDiscNumberAndKey(const std::string& relPath, std::string& groupKeyOut,
                             std::string& cleanTitleOut, int& discNumberOut) {
    const std::string base = stemOf(baseNameOf(relPath));
    const std::string lowerBase = folded(base);

    auto tryParseIn = [&](const std::string& text, const std::string& lower,
                          std::string& strippedOut, int& discOut) -> bool {
        for (const char* tag : {"disc ", "disk ", "cd ", "cd"}) {
            const size_t tlen = std::strlen(tag);
            size_t pos = 0;
            while ((pos = lower.find(tag, pos)) != std::string::npos) {
                if (pos > 0 && std::isalnum(static_cast<unsigned char>(lower[pos - 1]))) {
                    pos += tlen;
                    continue;
                }
                size_t numStart = pos + tlen;
                if (numStart < lower.size() && std::isdigit(static_cast<unsigned char>(lower[numStart]))) {
                    size_t numEnd = numStart;
                    while (numEnd < lower.size() && std::isdigit(static_cast<unsigned char>(lower[numEnd])))
                        ++numEnd;
                    const int num = std::atoi(lower.substr(numStart, numEnd - numStart).c_str());
                    if (num >= 1 && num <= 16) {
                        size_t remStart = pos;
                        size_t remEnd = numEnd;
                        // Eat surrounding parens/brackets if present, e.g. "(Disc 1 of 3)".
                        if (remStart > 0 && (text[remStart - 1] == '(' || text[remStart - 1] == '[')) {
                            const char close = text[remStart - 1] == '(' ? ')' : ']';
                            const size_t cpos = text.find(close, numEnd);
                            if (cpos != std::string::npos && cpos - numEnd <= 12) {
                                --remStart;
                                remEnd = cpos + 1;
                            }
                        }
                        std::string clean = text.substr(0, remStart) + text.substr(remEnd);
                        while (!clean.empty() && (clean.back() == ' ' || clean.back() == '-' || clean.back() == '_'))
                            clean.pop_back();
                        size_t frontTrim = 0;
                        while (frontTrim < clean.size() && (clean[frontTrim] == ' ' || clean[frontTrim] == '-'))
                            ++frontTrim;
                        clean = clean.substr(frontTrim);
                        // Normalize double spaces
                        std::string collapsed;
                        bool prevSpace = false;
                        for (char c : clean) {
                            if (c == ' ') {
                                if (!prevSpace) collapsed += c;
                                prevSpace = true;
                            } else {
                                collapsed += c;
                                prevSpace = false;
                            }
                        }
                        if (!collapsed.empty()) {
                            strippedOut = collapsed;
                            discOut = num;
                            return true;
                        }
                    }
                }
                pos += tlen;
            }
        }
        return false;
    };

    std::string cleanTitle;
    if (tryParseIn(base, lowerBase, cleanTitle, discNumberOut)) {
        // Scope group key by top container folder (first two segments like unpacked/<hash> or files)
        std::string scope = parentDirOf(relPath);
        const std::string pBase = baseNameOf(scope);
        const std::string pLower = folded(pBase);
        std::string unused;
        int dummyDisc = 0;
        if (tryParseIn(pBase, pLower, unused, dummyDisc)) {
            scope = parentDirOf(scope);
        }
        groupKeyOut = folded(scope) + "|" + folded(cleanTitle);
        cleanTitleOut = cleanTitle;
        return true;
    }

    // Also check parent folder name e.g. "Disc 1/Game.cue"
    const std::string pDir = parentDirOf(relPath);
    const std::string pBase = baseNameOf(pDir);
    if (!pBase.empty() && tryParseIn(pBase, folded(pBase), cleanTitle, discNumberOut)) {
        const std::string grand = parentDirOf(pDir);
        groupKeyOut = folded(grand) + "|" + folded(base);
        cleanTitleOut = base;
        return true;
    }
    return false;
}

std::string commonPrefixPath(const std::vector<std::string>& paths) {
    if (paths.empty()) return {};
    std::string prefix = parentDirOf(paths[0]);
    for (size_t i = 1; i < paths.size(); ++i) {
        const std::string dir = parentDirOf(paths[i]);
        while (!prefix.empty() && dir != prefix &&
               dir.rfind(prefix + "/", 0) != 0) {
            prefix = parentDirOf(prefix);
        }
    }
    return prefix;
}

bool isValidVitaTitleId(const std::string& id) {
    if (id.size() != 9)
        return false;
    for (size_t i = 0; i < 4; ++i) {
        const char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return false;
    }
    for (size_t i = 4; i < 9; ++i) {
        const char c = id[i];
        if (!(c >= '0' && c <= '9'))
            return false;
    }
    return true;
}

std::string upperAscii(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - ('a' - 'A'));
    }
    return value;
}

std::string resolveVitaUx0Root(const std::string& sdRoot) {
    if (isDirNoSymlink(sdRoot + "/switch/Vita3K/vita/ux0"))
        return sdRoot + "/switch/Vita3K/vita/ux0";
    if (isDirNoSymlink(sdRoot + "/switch/vita3k/vita/ux0"))
        return sdRoot + "/switch/vita3k/vita/ux0";
    if (isDirNoSymlink(sdRoot + "/switch/Vita3K"))
        return sdRoot + "/switch/Vita3K/vita/ux0";
    return sdRoot + "/switch/vita3k/vita/ux0";
}

} // namespace

std::mutex RetroRomStore::mutex_;
std::mutex RetroRomStore::preparationMutex_;

RetroRomStore::RetroRomStore(std::string appRoot, std::string sdRoot)
    : appRoot_(std::move(appRoot)), sdRoot_(std::move(sdRoot)) {
    while (sdRoot_.size() > 1 && sdRoot_.back() == '/') {
        if (sdRoot_.size() == 6 && sdRoot_.rfind("sdmc:/", 0) == 0) {
            sdRoot_.pop_back();
            break;
        }
        sdRoot_.pop_back();
    }
}

std::string RetroRomStore::operationsRoot() const { return appRoot_ + "/retro/operations"; }
std::string RetroRomStore::libraryRoot() const { return appRoot_ + "/retro/library"; }
std::string RetroRomStore::byTaskRoot() const { return appRoot_ + "/retro/by-task"; }
std::string RetroRomStore::settingsPath() const { return appRoot_ + "/retro/settings.json"; }

std::string RetroRomStore::preparedRoot(const RetroRomOperation& op) const {
    return sdRoot_ + "/" + op.destination + (op.imported() ? "" : ("/" + op.id));
}

std::string RetroRomStore::gameFullPath(const RetroRomOperation& op, const RetroRomGame& game) const {
    return preparedRoot(op) + "/" + game.path;
}

std::string RetroRomStore::normalizeDestination(const std::string& destination) {
    size_t first = destination.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    size_t last = destination.find_last_not_of(" \t\r\n");
    std::string trimmed = destination.substr(first, last - first + 1);
    if (trimmed.size() >= 5 && folded(trimmed.substr(0, 5)) == "sdmc:")
        trimmed = trimmed.substr(5);
    std::string collapsed;
    collapsed.reserve(trimmed.size());
    bool prevSlash = false;
    for (char c : trimmed) {
        if (c == '/') {
            if (!prevSlash)
                collapsed += c;
            prevSlash = true;
        } else {
            collapsed += c;
            prevSlash = false;
        }
    }
    while (!collapsed.empty() && collapsed.front() == '/')
        collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == '/')
        collapsed.pop_back();
    return collapsed;
}

bool RetroRomStore::acceptsDestination(const std::string& destination) {
    return destinationValid(destination);
}

bool RetroRomStore::acceptsSetupDirectory(const std::string& directory) {
    if (directory.empty() || directory.find_first_of("\"\r\n") != std::string::npos)
        return false;
    const std::string clean = normalizeDestination(directory);
    return !clean.empty() && destinationValid(clean);
}

bool RetroRomStore::prepareLaunchArguments(const RetroSettings& settings,
                                           const std::string& consoleId,
                                           const std::string& emulatorId,
                                           const std::string& nroPath,
                                           const std::string& romPath,
                                           std::string& arguments,
                                           std::string& error) const {
    if (!RetroEmulatorInstaller::launchArguments(emulatorId, nroPath, romPath, arguments, error))
        return false;

    const RetroEmulatorProfile* profile = findRetroEmulatorProfile(emulatorId);
    const bool isRetroArch = (profile && profile->isRetroArchCore) ||
                             nroPath.rfind("sdmc:/retroarch/cores/", 0) == 0 ||
                             endsWithCi(nroPath, "_libretro_libnx.nro");
    if (!isRetroArch || !findRetroConsole(consoleId) || settings.emulatorFor(consoleId) != emulatorId)
        return true;

    auto lookupDir = [&](const std::map<std::string, std::string>& m, std::string& cleanOut) -> bool {
        const auto it = m.find(consoleId);
        if (it == m.end() || it->second.empty())
            return false;
        if (!acceptsSetupDirectory(it->second)) {
            error = "Invalid setup directory for " + consoleId + ".";
            return false;
        }
        cleanOut = normalizeDestination(it->second);
        return true;
    };

    std::string biosDir, saveDir, stateDir;
    error.clear();
    const bool hasBios = lookupDir(settings.biosDirs, biosDir);
    if (!error.empty()) return false;
    const bool hasSave = lookupDir(settings.saveDirs, saveDir);
    if (!error.empty()) return false;
    const bool hasState = lookupDir(settings.stateDirs, stateDir);
    if (!error.empty()) return false;

    if (!hasBios && !hasSave && !hasState)
        return true;

    std::string cfgContent;
    if (hasBios) {
        const std::string full = sdRoot_ + "/" + biosDir;
        if (!isDirNoSymlink(full))
            return fail(error, "Configured BIOS directory is unavailable.");
        cfgContent += "system_directory = \"/" + biosDir + "\"\n";
    }
    if (hasSave) {
        const std::string full = sdRoot_ + "/" + saveDir;
        if (!directories(full) || !isDirNoSymlink(full))
            return fail(error, "Unable to prepare configured save directory.");
        cfgContent += "savefile_directory = \"/" + saveDir + "\"\n";
    }
    if (hasState) {
        const std::string full = sdRoot_ + "/" + stateDir;
        if (!directories(full) || !isDirNoSymlink(full))
            return fail(error, "Unable to prepare configured savestate directory.");
        cfgContent += "savestate_directory = \"/" + stateDir + "\"\n";
    }

    const std::string cfgDir = appRoot_ + "/retro/configs";
    const std::string cfgAbs = cfgDir + "/" + consoleId + ".cfg";
    std::string cfgSdRel;
    if (appRoot_.size() > sdRoot_.size() &&
        appRoot_.rfind(sdRoot_, 0) == 0 &&
        appRoot_[sdRoot_.size()] == '/') {
        cfgSdRel = appRoot_.substr(sdRoot_.size() + 1) + "/retro/configs/" + consoleId + ".cfg";
    } else if (appRoot_ == sdRoot_) {
        cfgSdRel = "retro/configs/" + consoleId + ".cfg";
    } else {
        return fail(error, "Retro configuration root does not match storage root.");
    }
    if (cfgSdRel.find_first_of("\"\r\n") != std::string::npos || !safe(cfgSdRel))
        return fail(error, "Invalid RetroArch override config path.");

    std::lock_guard<std::mutex> lock(mutex_);
    if (!directories(cfgDir) || !isDirNoSymlink(cfgDir))
        return fail(error, "Unable to create RetroArch override config directory.");

    bool needWrite = true;
    uint64_t existingSize = 0;
    if (regular(cfgAbs, &existingSize) && existingSize == cfgContent.size()) {
        std::ifstream existing(cfgAbs, std::ios::binary);
        std::string current(static_cast<size_t>(existingSize), '\0');
        if (existing.read(&current[0], static_cast<std::streamsize>(current.size())) &&
            current == cfgContent) {
            needWrite = false;
        }
    }
    if (needWrite && !writeSmallTextFile(cfgAbs, cfgContent))
        return fail(error, "Unable to write RetroArch override config.");

    arguments += " --appendconfig \"/" + cfgSdRel + "\"";
    return true;
}

bool RetroRomStore::loadUnlocked(const std::string& id, RetroRomOperation& operation,
                                 std::string& error) const {
    if (!idValid(id))
        return fail(error, "Invalid retro operation ID.");
    Json json;
    RetroRomOperation parsed;
    for (const auto* end : {".json", ".json.previous"}) {
        if (read(operationsRoot() + "/" + id + end, json) && decode(json, parsed) &&
            parsed.id == id) {
            operation = std::move(parsed);
            error.clear();
            return true;
        }
    }
    return fail(error, "No valid retro operation record.");
}

bool RetroRomStore::load(const std::string& id, RetroRomOperation& operation,
                         std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadUnlocked(id, operation, error);
}

bool RetroRomStore::findOperationByTask(const std::string& taskId, RetroRomOperation& operation,
                                        std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hexString(taskId, 40))
        return fail(error, "Invalid task ID.");

    // Fast path via by-task marker
    const std::string markerPath = byTaskRoot() + "/" + taskId;
    uint64_t msize = 0;
    if (regular(markerPath, &msize) && msize > 0 && msize <= 128) {
        std::ifstream in(markerPath, std::ios::binary);
        std::string opId(static_cast<size_t>(msize), '\0');
        if (in.read(&opId[0], static_cast<std::streamsize>(opId.size()))) {
            while (!opId.empty() && (opId.back() == '\n' || opId.back() == '\r' || opId.back() == ' '))
                opId.pop_back();
            if (loadUnlocked(opId, operation, error))
                return true;
        }
    }

    // Fallback scan of operations directory
    DIR* dir = opendir(operationsRoot().c_str());
    if (!dir)
        return fail(error, "No matching retro operation.");
    std::set<std::string> ids;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        for (const auto* end : {".json.previous", ".json"}) {
            const size_t sz = std::strlen(end);
            if (name.size() > sz && name.compare(name.size() - sz, sz, end) == 0) {
                const auto id = name.substr(0, name.size() - sz);
                if (idValid(id)) ids.insert(id);
                break;
            }
        }
    }
    closedir(dir);
    for (const auto& id : ids) {
        RetroRomOperation op;
        std::string unused;
        if (loadUnlocked(id, op, unused) && op.taskId == taskId) {
            operation = std::move(op);
            error.clear();
            return true;
        }
    }
    return fail(error, "No matching retro operation.");
}

bool RetroRomStore::plan(const RetroRelease& release, const std::string& destination,
                         const std::string& emulator, RetroRomOperation& operation,
                         std::string& error, bool createForwarder) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!findRetroConsole(release.platform))
        return fail(error, "Unsupported retro console.");
    RetroRomOperation op;
    op.platform = release.platform;
    op.id = release.platform + "-" + release.topicId;
    op.topicId = release.topicId;
    op.title = release.title;
    op.taskId = folded(release.infoHash);
    op.destination = destination;
    op.emulator = emulator;
    op.coverUrl = release.coverUrl;
    op.createForwarder = createForwarder;
    RetroRomOperation validated;
    if (!decode(encode(op), validated))
        return fail(error, "Invalid retro download plan.");
    if (!directories(operationsRoot()))
        return fail(error, "Unable to create retro operation directory.");
    RetroRomOperation previous;
    if (loadUnlocked(op.id, previous, error)) {
        if (previous.taskId != op.taskId || previous.destination != op.destination ||
            previous.emulator != op.emulator)
            return fail(error, "A different plan already exists for this release.");
        bool updated = false;
        if (previous.coverUrl.empty() && !op.coverUrl.empty()) {
            previous.coverUrl = op.coverUrl;
            updated = true;
        }
        if (!previous.ready && previous.createForwarder != op.createForwarder) {
            previous.createForwarder = op.createForwarder;
            updated = true;
        }
        if (updated)
            write(operationsRoot() + "/" + op.id + ".json", encode(previous));
        writeSmallTextFile(byTaskRoot() + "/" + op.taskId, op.id);
        operation = std::move(previous);
        error.clear();
        return true;
    }
    if (!absent(operationsRoot() + "/" + op.id + ".json") ||
        !absent(operationsRoot() + "/" + op.id + ".json.previous"))
        return fail(error, "An unreadable retro record already exists.");
    if (!write(operationsRoot() + "/" + op.id + ".json", encode(op)))
        return fail(error, "Unable to save retro download plan.");
    writeSmallTextFile(byTaskRoot() + "/" + op.taskId, op.id);
    operation = std::move(op);
    error.clear();
    return true;
}

bool RetroRomStore::list(std::vector<RetroRomOperation>& operations, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    DIR* dir = opendir(operationsRoot().c_str());
    if (!dir) {
        if (errno == ENOENT) {
            operations.clear();
            error.clear();
            return true;
        }
        return fail(error, "Unable to list retro operations.");
    }
    std::set<std::string> ids;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        for (const auto* end : {".json.previous", ".json"}) {
            const size_t size = std::strlen(end);
            if (name.size() > size && name.compare(name.size() - size, size, end) == 0) {
                const auto id = name.substr(0, name.size() - size);
                if (idValid(id))
                    ids.insert(id);
                break;
            }
        }
        if (ids.size() > 2000) {
            closedir(dir);
            return fail(error, "Too many retro operations.");
        }
    }
    closedir(dir);
    std::vector<RetroRomOperation> result;
    for (const auto& id : ids) {
        RetroRomOperation op;
        if (!loadUnlocked(id, op, error))
            return false;
        result.push_back(std::move(op));
    }
    operations = std::move(result);
    error.clear();
    return true;
}

bool RetroRomStore::prepare(const std::string& id, const std::vector<RetroRomInput>& files,
                            const std::atomic<bool>& cancelled, RetroRomOperation& result,
                            std::string& error, PrepareProgress progress) const {
    std::lock_guard<std::mutex> preparation(preparationMutex_);
    std::unique_lock<std::mutex> stateLock(mutex_);
    error.clear();
    if (cancelled)
        return fail(error, "Cancelled.");
    RetroRomOperation op;
    if (!loadUnlocked(id, op, error))
        return false;
    const auto* console = findRetroConsole(op.platform);
    if (!console)
        return fail(error, "Unsupported retro console.");
    stateLock.unlock();

    const uint64_t maxSizeLimit = console->discBased || console->id == "nds" ||
                                  console->id == "3ds" ? kDiscLimit : kCartridgeLimit;
    const size_t maxFileCount = console->discBased || console->id == "nds" ||
                                console->id == "3ds" ? kDiscFileLimit : kCartridgeFileLimit;

    const auto parent = sdRoot_ + "/" + op.destination;
    if (!directories(parent))
        return fail(error, "Unable to create selected ROM directory.");
    const auto final = parent + "/" + op.id;

    std::vector<std::string> preservedSaveRels;
    bool migratePreservedSaves = false;
    if (!absent(final)) {
        if (!absent(final + "/receipt.json") || !absent(final + "/receipt.json.previous")) {
            Json receipt;
            RetroRomOperation published;
            if (!read(final + "/receipt.json", receipt) || !decode(receipt, published) ||
                !published.ready || published.id != op.id || published.taskId != op.taskId ||
                published.destination != op.destination || published.emulator != op.emulator)
                return fail(error,
                            "ROM destination already exists and does not belong to this operation.");
            for (const auto& game : published.games) {
                if (cancelled)
                    return fail(error, "Cancelled.");
                std::string title;
                uint64_t sz = 0;
                if (!validateRomForConsole(*console, final + "/" + game.path,
                                           console->id == "gba", title, sz))
                    return fail(error, "A prepared ROM is missing or invalid.");
            }
            stateLock.lock();
            if (!write(operationsRoot() + "/" + id + ".json", encode(published)))
                return fail(error, "Unable to recover prepared ROM record.");
            result = std::move(published);
            return true;
        }
        if (collectOwnedPreservedSaves(final, op.id, preservedSaveRels) && !files.empty()) {
            migratePreservedSaves = true;
        } else {
            return fail(error,
                        "ROM destination already exists and does not belong to this operation.");
        }
    }

    if (cancelled)
        return fail(error, "Cancelled.");
    if (files.empty() || files.size() > maxFileCount)
        return fail(error, "No complete selected download files.");

    uint64_t totalCopyBytes = 0;
    uint64_t totalAllBytes = 0;
    std::set<std::string> paths;
    for (const auto& file : files) {
        uint64_t bytes = 0;
        if (!safe(file.path) || !regular(file.absolutePath, &bytes) || bytes != file.bytes ||
            bytes > maxSizeLimit - totalAllBytes)
            return fail(error, "Missing, unsafe or oversized download input.");
        if (!paths.insert(folded(file.path)).second)
            return fail(error, "Selected download paths collide.");
        if (isMultiVolumeArchive(file.path))
            return fail(error, "Multi-volume ROM archives are not supported.");
        totalAllBytes += bytes;
        if (!isArchivePath(file.path))
            totalCopyBytes += bytes;
    }
    if (!room(parent, std::max<uint64_t>(totalCopyBytes, 4096)))
        return fail(error, "Insufficient free space for ROM preparation.");

    const auto stage = parent + "/.pipensx-" + op.id + ".pending";
    if (!absent(stage)) {
        Json owner;
        if (!read(stage + "/owner.json", owner) ||
            owner != Json({{"id", id}, {"task", op.taskId}}) || !eraseStage(stage))
            return fail(error, "ROM staging directory is not owned by this operation.");
    }
    if (!directory(stage) || !write(stage + "/owner.json", {{"id", id}, {"task", op.taskId}}, false))
        return fail(error, "Unable to prepare ROM staging directory.");

    struct Cleanup {
        std::string path;
        ~Cleanup() { eraseStage(path); }
    } cleanup{stage};

    Workspace workspace{stage, cancelled, maxSizeLimit, maxFileCount, 0, 0,
                        std::move(progress), 0, totalAllBytes, 0, files.size(), {}, {}};

    // Stage raw files directly; extract top-level archives directly from source path without double copy.
    for (const auto& file : files) {
        if (cancelled)
            return fail(error, "Cancelled.");
        bool unpackDirect = isArchivePath(file.path);
        if (unpackDirect && console->packageInstallInEmulator && suffix(file.path) == ".zip") {
            // Vita3K can install .zip packages directly unless the .zip wraps a .vpk.
            unpackDirect = archiveContainsExtension(file.absolutePath, ".vpk");
        }
        if (unpackDirect) {
            workspace.queue.push_back({"files/" + file.path, file.absolutePath, 0, false});
            continue;
        }
        const auto relative = "files/" + file.path;
        FILE* input = fopen(file.absolutePath.c_str(), "rb");
        if (!input)
            return fail(error, "Unable to read downloaded ROM data.");
        const bool ok = workspace.output(
            relative, file.bytes,
            [&](char* data, size_t size) -> ssize_t {
                const auto count = fread(data, 1, size, input);
                return ferror(input) ? -1 : ssize_t(count);
            },
            false, error);
        fclose(input);
        if (!ok)
            return false;
        workspace.queue.push_back({relative, {}, 0, false});
    }

    for (size_t i = 0; i < workspace.queue.size(); ++i) {
        const auto item = workspace.queue[i];
        if (isMultiVolumeArchive(item.relativePath))
            return fail(error, "Multi-volume ROM archives are not supported.");
        bool shouldUnpack = !item.sourcePath.empty() || (item.depth > 0 && isArchivePath(item.relativePath));
        if (shouldUnpack && console->packageInstallInEmulator && item.depth > 0 &&
            suffix(item.relativePath) == ".zip") {
            shouldUnpack = archiveContainsExtension(stage + "/" + item.relativePath, ".vpk");
        }
        if (shouldUnpack) {
            if (!workspace.unpack(item, error))
                return false;
        }
    }

    // Normalize disc descriptors (.ccd/.mds -> .cue, .cue FILE casing repair)
    normalizeDiscDescriptors(stage, *console, workspace.stagedRegularFiles);

    // Track per-directory disc descriptors so raw track .bin/.img files are suppressed
    std::set<std::string> dirsWithCueOrGdi;
    std::set<std::string> dirsWithWiiuRpx;
    for (const auto& rel : workspace.stagedRegularFiles) {
        const auto ext = suffix(rel);
        if (console->discBased && (ext == ".cue" || ext == ".gdi" || ext == ".m3u"))
            dirsWithCueOrGdi.insert(parentDirOf(rel));
        if (console->id == "wiiu" && ext == ".rpx")
            dirsWithWiiuRpx.insert(parentDirOf(parentDirOf(rel)));
    }

    struct RawCandidate {
        std::string relPath;
        std::string title;
        uint64_t size = 0;
    };
    std::vector<RawCandidate> candidates;

    for (const auto& rel : workspace.stagedRegularFiles) {
        const std::string ext = suffix(rel);
        if (!consoleHasExtension(*console, ext))
            continue;
        if (console->discBased &&
            (ext == ".bin" || ext == ".img" || ext == ".mdf" || ext == ".raw") &&
            dirsWithCueOrGdi.count(parentDirOf(rel)))
            continue;
        if (console->id == "wiiu" && (ext == ".wua" || ext == ".wux" || ext == ".wud") == false &&
            ext != ".rpx")
            continue;
        std::string title;
        uint64_t sz = 0;
        const std::string abs = stage + "/" + rel;
        if (console->id == "gba") {
            if (!rom(abs, title))
                continue;
            regular(abs, &sz);
        } else {
            if (!validateRomForConsole(*console, abs, false, title, sz))
                continue;
            // Friendly title cleanup (strip trailing Wii U /code/.rpx folder nuance if needed)
            if (console->id == "wiiu" && ext == ".rpx") {
                const std::string pDir = baseNameOf(parentDirOf(parentDirOf(rel)));
                if (!pDir.empty() && pDir != "files" && pDir.rfind("unpacked", 0) != 0)
                    title = pDir;
            }
        }
        if (title.empty()) title = rel;
        candidates.push_back({rel, title, sz});
    }

    // PS Vita NoNpDrm / unpacked folder dumps (eboot.bin + sce_sys/param.sfo)
    if (console->id == "psvita" && candidates.empty()) {
        std::vector<std::string> ebootRels;
        bool hasAppEboot = false;
        for (const auto& rel : workspace.stagedRegularFiles) {
            if (folded(baseNameOf(rel)) != "eboot.bin")
                continue;
            const std::string abs = stage + "/" + rel;
            std::string title;
            uint64_t sz = 0;
            if (!validateRomForConsole(*console, abs, false, title, sz))
                continue;
            const std::string relFolded = folded(rel);
            if (relFolded.find("/app/") != std::string::npos || relFolded.rfind("app/", 0) == 0)
                hasAppEboot = true;
            ebootRels.push_back(rel);
        }
        for (const auto& rel : ebootRels) {
            const std::string relFolded = folded(rel);
            // NoNpDrm releases often ship patch/<TITLEID>/eboot.bin alongside app/<TITLEID>/eboot.bin.
            // Treat patch/addcont as companions to the base app rather than separate games.
            if (hasAppEboot &&
                (relFolded.find("/patch/") != std::string::npos ||
                 relFolded.rfind("patch/", 0) == 0 ||
                 relFolded.find("/addcont/") != std::string::npos ||
                 relFolded.rfind("addcont/", 0) == 0)) {
                continue;
            }
            const std::string abs = stage + "/" + rel;
            std::string title;
            uint64_t sz = 0;
            if (!validateRomForConsole(*console, abs, false, title, sz))
                continue;
            const std::string relDir = parentDirOf(rel);
            const std::string relDirFolded = folded(relDir);
            uint64_t folderBytes = 0;
            for (const auto& member : workspace.stagedRegularFiles) {
                const std::string mf = folded(member);
                if (relDirFolded.empty() || mf == relDirFolded ||
                    mf.rfind(relDirFolded + "/", 0) == 0) {
                    uint64_t msz = 0;
                    if (regular(stage + "/" + member, &msz))
                        folderBytes += msz;
                }
            }
            if (title == "PS Vita Game" && !op.title.empty())
                title = op.title;
            candidates.push_back({rel, title, folderBytes > 0 ? folderBytes : sz});
        }
    }

    // Generate .m3u playlists for multi-disc sets and honor existing .m3u playlists on supportsM3u consoles
    std::vector<RawCandidate> finalCandidates;
    if (console->supportsM3u && candidates.size() > 1) {
        std::map<std::string, size_t> byRelFolded;
        for (size_t i = 0; i < candidates.size(); ++i)
            byRelFolded[folded(candidates[i].relPath)] = i;
        std::vector<bool> consumed(candidates.size(), false);
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (suffix(candidates[i].relPath) != ".m3u")
                continue;
            std::vector<std::string> refs;
            collectM3uDiscPaths(stage, candidates[i].relPath, refs);
            uint64_t totalDiscBytes = 0;
            bool matched = false;
            for (const auto& ref : refs) {
                const auto it = byRelFolded.find(folded(ref));
                if (it != byRelFolded.end() && it->second != i &&
                    suffix(candidates[it->second].relPath) != ".m3u") {
                    consumed[it->second] = true;
                    totalDiscBytes += candidates[it->second].size;
                    matched = true;
                }
            }
            if (matched && totalDiscBytes > candidates[i].size)
                candidates[i].size = totalDiscBytes;
        }

        struct DiscEntry {
            size_t candidateIdx;
            int discNum;
            std::string cleanTitle;
        };
        std::map<std::string, std::vector<DiscEntry>> discGroups;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (consumed[i] || suffix(candidates[i].relPath) == ".m3u")
                continue;
            std::string key, cleanTitle;
            int discNum = 0;
            if (extractDiscNumberAndKey(candidates[i].relPath, key, cleanTitle, discNum))
                discGroups[key].push_back({i, discNum, cleanTitle});
        }
        for (auto& kv : discGroups) {
            auto& vec = kv.second;
            if (vec.size() < 2) continue;
            std::sort(vec.begin(), vec.end(),
                      [](const DiscEntry& a, const DiscEntry& b) { return a.discNum < b.discNum; });
            std::vector<std::string> discRelPaths;
            uint64_t groupSize = 0;
            bool distinctDiscs = true;
            for (size_t k = 0; k < vec.size(); ++k) {
                if (k > 0 && vec[k].discNum == vec[k - 1].discNum) {
                    distinctDiscs = false;
                    break;
                }
                discRelPaths.push_back(candidates[vec[k].candidateIdx].relPath);
                groupSize += candidates[vec[k].candidateIdx].size;
            }
            if (!distinctDiscs) continue;
            const std::string commonDir = commonPrefixPath(discRelPaths);
            if (commonDir.empty()) continue;
            const std::string m3uRel = commonDir + "/" + vec[0].cleanTitle + ".m3u";
            std::string m3uBody;
            for (const auto& p : discRelPaths) {
                const std::string linePath =
                    p.size() > commonDir.size() + 1 ? p.substr(commonDir.size() + 1) : p;
                m3uBody += linePath + "\n";
            }
            if (absent(stage + "/" + m3uRel) && writeSmallTextFile(stage + "/" + m3uRel, m3uBody)) {
                for (const auto& d : vec) consumed[d.candidateIdx] = true;
                finalCandidates.push_back({m3uRel, vec[0].cleanTitle, groupSize});
            }
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!consumed[i]) finalCandidates.push_back(std::move(candidates[i]));
        }
    } else {
        finalCandidates = std::move(candidates);
    }

    op.games.clear();
    for (const auto& c : finalCandidates) {
        RetroRomGame g;
        g.id = op.id + ":" + hash(folded(c.relPath));
        g.title = c.title;
        g.path = c.relPath;
        g.size = c.size;
        g.selected = false;
        op.games.push_back(std::move(g));
    }

    if (op.games.empty()) {
        return fail(error,
                    console->id == "gba"
                        ? "No valid GBA ROMs found; downloaded files have been kept."
                        : "No valid ROMs found for " + console->defaultName +
                              "; downloaded files have been kept.");
    }
    if (migratePreservedSaves) {
        for (const auto& rel : preservedSaveRels) {
            const std::string dst = stage + "/" + rel;
            if (absent(dst) && !copyPreservedSaveFile(final + "/" + rel, dst))
                return fail(error, "Unable to preserve existing saves during ROM preparation.");
        }
    }
    if (op.games.size() == 1)
        op.games[0].selected = true;
    op.ready = true;
    if (!write(stage + "/receipt.json", encode(op)))
        return fail(error, "Unable to save prepared ROM receipt.");
    if (cancelled)
        return fail(error, "Cancelled.");
    if (migratePreservedSaves && !eraseStage(final))
        return fail(error, "Unable to replace preserved-save staging directory.");
    if (!absent(final) || rename(stage.c_str(), final.c_str()) != 0)
        return fail(error, "ROM destination collision or publication failure.");
    stateLock.lock();
    if (!write(operationsRoot() + "/" + id + ".json", encode(op)))
        return fail(error, "ROMs published; operation record needs recovery.");
    result = std::move(op);
    return true;
}

bool RetroRomStore::select(const std::string& id, const std::vector<std::string>& games,
                           std::string& error) const {
    std::lock_guard<std::mutex> preparation(preparationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    RetroRomOperation op;
    if (!loadUnlocked(id, op, error))
        return false;
    if (!op.ready)
        return fail(error, "ROM operation is not prepared.");
    std::set<std::string> wanted(games.begin(), games.end());
    for (auto& game : op.games)
        game.selected = wanted.erase(game.id) != 0;
    if (!wanted.empty())
        return fail(error, "Unknown ROM selection.");
    const auto receipt = preparedRoot(op) + "/receipt.json";
    Json receiptJson;
    RetroRomOperation owner;
    if (!read(receipt, receiptJson) || !decode(receiptJson, owner) || owner.id != op.id ||
        owner.taskId != op.taskId || owner.destination != op.destination ||
        owner.emulator != op.emulator || !write(receipt, encode(op)) ||
        !write(operationsRoot() + "/" + id + ".json", encode(op)))
        return fail(error, "Unable to save ROM selection.");
    error.clear();
    return true;
}

bool RetroRomStore::removeOperation(const std::string& operationId, bool deletePreparedFiles,
                                    std::string& error) const {
    std::lock_guard<std::mutex> preparation(preparationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (!idValid(operationId))
        return fail(error, "Invalid retro operation ID.");
    RetroRomOperation op;
    const bool hasOp = loadUnlocked(operationId, op, error);
    error.clear();
    if (deletePreparedFiles && hasOp && !op.imported()) {
        const std::string prepDir = preparedRoot(op);
        if (!absent(prepDir)) {
            bool kept = false;
            erasePreservingSaves(prepDir, kept);
            if (kept && isDirNoSymlink(prepDir)) {
                writeSmallTextFile(prepDir + "/.pipensx-saves.json",
                                   Json({{"id", op.id}}).dump());
            }
        }
    }
    if (hasOp && !op.taskId.empty())
        unlink((byTaskRoot() + "/" + op.taskId).c_str());
    unlink((operationsRoot() + "/" + operationId + ".json").c_str());
    unlink((operationsRoot() + "/" + operationId + ".json.previous").c_str());
    return true;
}

bool RetroRomStore::loadLibrary(const std::string& id, RetroRomOperation& entry,
                                std::string& error) const {
    if (!idValid(id))
        return fail(error, "Invalid library operation ID.");
    for (const auto* ending : {".json", ".json.previous"}) {
        Json json;
        if (readOne(libraryRoot() + "/" + id + ending, json) && decode(json, entry) &&
            entry.id == id)
            return true;
    }
    return fail(error, "No valid retro library record.");
}

bool RetroRomStore::addSelectedToLibrary(const std::string& id, std::string& error) const {
    std::lock_guard<std::mutex> preparation(preparationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    RetroRomOperation op, receipt;
    if (!loadUnlocked(id, op, error))
        return false;
    const auto* console = findRetroConsole(op.platform);
    if (!console)
        return fail(error, "Unsupported retro console.");
    const auto final = preparedRoot(op);
    Json json;
    if (!op.ready || !read(final + "/receipt.json", json) || !decode(json, receipt) ||
        encode(receipt) != encode(op))
        return fail(error, "Prepared ROM receipt does not match the operation; recover it first.");

    RetroRomOperation entry = op;
    entry.games.clear();
    const auto path = libraryRoot() + "/" + id + ".json";
    if (!absent(path) || !absent(path + ".previous")) {
        if (!loadLibrary(id, entry, error))
            return false;
        if (entry.taskId != op.taskId || entry.destination != op.destination ||
            entry.emulator != op.emulator)
            return fail(error, "Retro library record belongs to another preparation.");
    }
    if (entry.coverUrl.empty() && !op.coverUrl.empty())
        entry.coverUrl = op.coverUrl;

    size_t selected = 0;
    for (const auto& game : op.games) {
        if (!game.selected)
            continue;
        ++selected;
        std::string title;
        uint64_t sz = 0;
        if (!validateRomForConsole(*console, final + "/" + game.path,
                                   console->id == "gba", title, sz))
            return fail(error, "Selected ROM is missing or invalid.");
        const auto existing = std::find_if(entry.games.begin(), entry.games.end(),
                                           [&](const auto& g) { return g.id == game.id; });
        if (existing == entry.games.end())
            entry.games.push_back(game);
    }
    if (!selected)
        return fail(error, "Select at least one prepared game.");
    entry.ready = !entry.games.empty();
    if (!directories(libraryRoot()) || !write(path, encode(entry)))
        return fail(error, "Unable to save retro library.");
    return true;
}

bool RetroRomStore::library(std::vector<RetroRomOperation>& entries, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    DIR* dir = opendir(libraryRoot().c_str());
    if (!dir) {
        if (errno != ENOENT)
            return fail(error, "Unable to read retro library.");
        entries.clear();
        return true;
    }
    std::set<std::string> ids;
    while (auto* item = readdir(dir)) {
        const std::string name = item->d_name;
        for (const auto* ending : {".json.previous", ".json"}) {
            const size_t n = std::strlen(ending);
            if (name.size() > n && name.compare(name.size() - n, n, ending) == 0) {
                const auto id = name.substr(0, name.size() - n);
                if (idValid(id))
                    ids.insert(id);
                break;
            }
        }
        if (ids.size() > 2000) {
            closedir(dir);
            return fail(error, "Too many retro library records.");
        }
    }
    closedir(dir);
    std::vector<RetroRomOperation> result;
    for (const auto& id : ids) {
        RetroRomOperation entry;
        if (!loadLibrary(id, entry, error))
            return false;
        if (!entry.games.empty())
            result.push_back(std::move(entry));
    }
    entries = std::move(result);
    return true;
}

bool RetroRomStore::removeFromLibrary(const std::string& gameId, std::string& error) const {
    return removeFromLibrary(gameId, false, error);
}

bool RetroRomStore::removeFromLibrary(const std::string& gameId, bool deleteFiles,
                                      std::string& error) const {
    std::lock_guard<std::mutex> preparation(preparationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    const size_t colon = gameId.find(':');
    if (colon == std::string::npos)
        return fail(error, "Invalid library game ID.");
    RetroRomOperation entry;
    if (!loadLibrary(gameId.substr(0, colon), entry, error))
        return false;
    const auto game = std::find_if(entry.games.begin(), entry.games.end(),
                                   [&](const auto& g) { return g.id == gameId; });
    if (game == entry.games.end())
        return fail(error, "Unknown library game.");

    const RetroRomGame removed = *game;
    entry.games.erase(game);
    entry.ready = !entry.games.empty();
    if (!write(libraryRoot() + "/" + entry.id + ".json", encode(entry)))
        return fail(error, "Unable to remove retro library entry.");

    // Never delete files for user-imported directories, saves/configs, or shared remaining entries.
    if (deleteFiles && !entry.imported()) {
        const std::string prepDir = preparedRoot(entry);
        const std::string fullRom = prepDir + "/" + removed.path;
        if (!entry.games.empty()) {
            // Other games from this compilation remain in the library: delete only the removed
            // ROM if no remaining game references it and it isn't referenced by another disc/m3u.
            const std::string ext = suffix(removed.path);
            bool unlinked = false;
            if (!isSaveOrConfigFile(removed.path) && ext != ".m3u" && ext != ".cue" &&
                regular(fullRom)) {
                unlinked = (unlink(fullRom.c_str()) == 0);
            }
            if (unlinked) {
                Json receiptJson;
                RetroRomOperation receipt;
                if (read(prepDir + "/receipt.json", receiptJson) && decode(receiptJson, receipt)) {
                    receipt.games.erase(
                        std::remove_if(receipt.games.begin(), receipt.games.end(),
                                       [&](const RetroRomGame& g) { return g.id == removed.id; }),
                        receipt.games.end());
                    receipt.ready = !receipt.games.empty();
                    write(prepDir + "/receipt.json", encode(receipt));
                }
                RetroRomOperation op;
                std::string unused;
                if (loadUnlocked(entry.id, op, unused)) {
                    op.games.erase(
                        std::remove_if(op.games.begin(), op.games.end(),
                                       [&](const RetroRomGame& g) { return g.id == removed.id; }),
                        op.games.end());
                    op.ready = !op.games.empty();
                    write(operationsRoot() + "/" + entry.id + ".json", encode(op));
                }
            }
        } else {
            if (!absent(prepDir)) {
                bool keptSaves = false;
                erasePreservingSaves(prepDir, keptSaves);
                if (keptSaves && isDirNoSymlink(prepDir)) {
                    writeSmallTextFile(prepDir + "/.pipensx-saves.json",
                                       Json({{"id", entry.id}}).dump());
                }
            }
            if (entry.platform == "psvita" && folded(baseNameOf(removed.path)) == "eboot.bin") {
                const std::string rawId = baseNameOf(parentDirOf(removed.path));
                if (isValidVitaTitleId(rawId)) {
                    const std::string ux0Root = resolveVitaUx0Root(sdRoot_);
                    const std::string tid = upperAscii(rawId);
                    eraseStage(ux0Root + "/app/" + tid);
                    eraseStage(ux0Root + "/addcont/" + tid);
                }
            }
            RetroRomOperation op;
            std::string unused;
            if (loadUnlocked(entry.id, op, unused) && op.ready) {
                op.ready = false;
                op.games.clear();
                write(operationsRoot() + "/" + entry.id + ".json", encode(op));
            }
        }
    }
    return true;
}

bool RetroRomStore::setGameEmulator(const std::string& gameId, const std::string& emulator,
                                    std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    const size_t colon = gameId.find(':');
    if (colon == std::string::npos)
        return fail(error, "Invalid library game ID.");
    RetroRomOperation entry;
    if (!loadLibrary(gameId.substr(0, colon), entry, error))
        return false;
    if (!isValidEmulatorSelection(entry.platform, emulator))
        return fail(error, "Invalid emulator selection.");
    const auto game = std::find_if(entry.games.begin(), entry.games.end(),
                                   [&](const auto& g) { return g.id == gameId; });
    if (game == entry.games.end())
        return fail(error, "Unknown library game.");
    game->emulator = emulator;
    if (!write(libraryRoot() + "/" + entry.id + ".json", encode(entry)))
        return fail(error, "Unable to update emulator selection.");
    return true;
}

bool RetroRomStore::resolveLibraryGame(const std::string& gameId, std::string& romPath,
                                       std::string& error, RetroRomGame* resolvedGame,
                                       RetroRomOperation* entryOut) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    romPath.clear();
    const size_t colon = gameId.find(':');
    if (colon == std::string::npos)
        return fail(error, "Invalid library game ID.");
    RetroRomOperation entry;
    if (!loadLibrary(gameId.substr(0, colon), entry, error))
        return false;
    const auto* console = findRetroConsole(entry.platform);
    if (!console)
        return fail(error, "Unsupported retro console.");
    const auto game = std::find_if(entry.games.begin(), entry.games.end(),
                                   [&](const auto& g) { return g.id == gameId; });
    if (game == entry.games.end())
        return fail(error, "Unknown library game.");

    const auto final = preparedRoot(entry);
    if (!entry.imported()) {
        RetroRomOperation receipt;
        Json json;
        if (!read(final + "/receipt.json", json) || !decode(json, receipt) ||
            receipt.id != entry.id || receipt.taskId != entry.taskId ||
            receipt.destination != entry.destination || receipt.emulator != entry.emulator ||
            std::none_of(receipt.games.begin(), receipt.games.end(),
                         [&](const auto& g) { return g.id == gameId && g.path == game->path; }))
            return fail(error, "Prepared ROM receipt is missing or changed.");
    }

    const auto path = final + "/" + game->path;
    std::string title;
    uint64_t sz = 0;
    const bool strictGba = console->id == "gba" && !entry.imported();
    if (!validateRomForConsole(*console, path, strictGba, title, sz))
        return fail(error, "Library ROM is missing or invalid.");

    romPath = path;
    if (resolvedGame)
        *resolvedGame = *game;
    if (entryOut)
        *entryOut = entry;
    return true;
}

bool RetroRomStore::importDirectory(const std::string& platform, const std::string& dirRelativeToSd,
                                    std::vector<RetroRomGame>& imported, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    imported.clear();
    const auto* console = findRetroConsole(platform);
    if (!console)
        return fail(error, "Unsupported retro console.");
    const std::string relDir = normalizeDestination(dirRelativeToSd);
    if (!destinationValid(relDir))
        return fail(error, "Invalid ROM import folder.");
    const std::string rootDir = sdRoot_ + "/" + relDir;
    if (!isDirNoSymlink(rootDir))
        return fail(error, "Import folder does not exist.");

    std::vector<std::string> foundRelPaths;
    auto scan = [&](auto& self, const std::string& relSub, unsigned depth) -> void {
        if (depth > 4 || foundRelPaths.size() >= 1024)
            return;
        const std::string full = relSub.empty() ? rootDir : (rootDir + "/" + relSub);
        DIR* d = opendir(full.c_str());
        if (!d) return;
        std::vector<std::string> names;
        while (auto* ent = readdir(d)) {
            const std::string name = ent->d_name;
            if (name.empty() || name[0] == '.' || !safe(name))
                continue;
            names.push_back(name);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            if (foundRelPaths.size() >= 1024) break;
            const std::string childRel = relSub.empty() ? name : (relSub + "/" + name);
            const std::string childFull = rootDir + "/" + childRel;
            struct stat st {};
            if (lstat(childFull.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
                continue;
            if (S_ISDIR(st.st_mode)) {
                self(self, childRel, depth + 1);
            } else if (S_ISREG(st.st_mode) && consoleHasExtension(*console, suffix(name))) {
                foundRelPaths.push_back(childRel);
            }
        }
    };
    scan(scan, {}, 0);

    std::set<std::string> dirsWithCueOrGdi;
    std::set<std::string> consumedByM3u;
    std::map<std::string, uint64_t> m3uTotalBytes;
    for (const auto& r : foundRelPaths) {
        const auto ext = suffix(r);
        if (console->discBased && (ext == ".cue" || ext == ".gdi" || ext == ".m3u"))
            dirsWithCueOrGdi.insert(parentDirOf(r));
        if (console->supportsM3u && ext == ".m3u") {
            std::string mTitle;
            uint64_t mSize = 0;
            if (validateRomForConsole(*console, rootDir + "/" + r, false, mTitle, mSize)) {
                std::vector<std::string> refs;
                collectM3uDiscPaths(rootDir, r, refs);
                uint64_t sum = 0;
                for (const auto& ref : refs) {
                    if (suffix(ref) == ".m3u")
                        continue;
                    uint64_t discSz = 0;
                    if (regular(rootDir + "/" + ref, &discSz)) {
                        consumedByM3u.insert(folded(ref));
                        sum += discSz;
                    }
                }
                if (sum > mSize)
                    m3uTotalBytes[folded(r)] = sum;
            }
        }
    }

    const std::string importOpId =
        "import-" + platform + "-" + hash(platform + ":" + folded(relDir)).substr(0, 12);

    RetroRomOperation entry;
    entry.id = importOpId;
    entry.platform = platform;
    entry.taskId = std::string(40, '0');
    entry.topicId = "0";
    entry.title = baseNameOf(relDir);
    entry.destination = relDir;
    entry.emulator = "";
    const std::string jsonPath = libraryRoot() + "/" + importOpId + ".json";
    RetroRomOperation existingEntry;
    std::map<std::string, RetroRomGame> prevByPath;
    if (!absent(jsonPath) && loadLibrary(importOpId, existingEntry, error)) {
        for (const auto& g : existingEntry.games)
            prevByPath[folded(g.path)] = g;
    }
    error.clear();

    for (const auto& r : foundRelPaths) {
        const std::string ext = suffix(r);
        if (console->discBased &&
            (ext == ".bin" || ext == ".img" || ext == ".mdf" || ext == ".raw") &&
            dirsWithCueOrGdi.count(parentDirOf(r)))
            continue;
        if (console->supportsM3u && ext != ".m3u" && consumedByM3u.count(folded(r)))
            continue;
        std::string title;
        uint64_t sz = 0;
        if (!validateRomForConsole(*console, rootDir + "/" + r, false, title, sz))
            continue;
        if (ext == ".m3u") {
            const auto it = m3uTotalBytes.find(folded(r));
            if (it != m3uTotalBytes.end() && it->second > 0)
                sz = it->second;
        }
        RetroRomGame g;
        g.id = importOpId + ":" + hash(folded(r));
        g.title = title;
        g.path = r;
        g.size = sz;
        g.selected = true;
        auto prevIt = prevByPath.find(folded(r));
        if (prevIt != prevByPath.end())
            g.emulator = prevIt->second.emulator;
        entry.games.push_back(g);
        imported.push_back(g);
    }

    if (entry.games.empty())
        return fail(error, "No matching ROM files found in folder.");

    entry.ready = true;
    if (!directories(libraryRoot()) || !write(jsonPath, encode(entry)))
        return fail(error, "Unable to save imported retro library.");
    return true;
}

bool RetroRomStore::loadSettings(RetroSettings& out, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    out = RetroSettings{};
    Json j;
    if (absent(settingsPath()) && absent(settingsPath() + ".previous"))
        return true;
    if (!read(settingsPath(), j) || !j.is_object() || j.value("format", 0) != 1)
        return true;
    if (j.contains("last_console") && j["last_console"].is_string()) {
        const std::string lc = j["last_console"];
        if (findRetroConsole(lc))
            out.lastConsole = lc;
    }
    if (j.contains("auto_create_forwarder") && j["auto_create_forwarder"].is_boolean()) {
        out.autoCreateForwarder = j["auto_create_forwarder"].get<bool>();
    }
    if (j.contains("default_emulators") && j["default_emulators"].is_object()) {
        for (auto it = j["default_emulators"].begin(); it != j["default_emulators"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string val = it.value();
            if (findRetroConsole(it.key()) && isValidEmulatorSelection(it.key(), val))
                out.defaultEmulator[it.key()] = val;
        }
    }
    if (j.contains("rom_dirs") && j["rom_dirs"].is_object()) {
        for (auto it = j["rom_dirs"].begin(); it != j["rom_dirs"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string dir = normalizeDestination(it.value());
            if (findRetroConsole(it.key()) && acceptsDestination(dir))
                out.romDirs[it.key()] = dir;
        }
    }
    if (j.contains("nro_paths") && j["nro_paths"].is_object()) {
        for (auto it = j["nro_paths"].begin(); it != j["nro_paths"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string val = it.value();
            if (findRetroConsole(it.key()) && isCustomEmulatorPath(val))
                out.nroPaths[it.key()] = val;
        }
    }
    auto loadDirMap = [&](const char* key, std::map<std::string, std::string>& target) {
        if (!j.contains(key) || !j[key].is_object())
            return;
        for (auto it = j[key].begin(); it != j[key].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string raw = it.value();
            if (findRetroConsole(it.key()) && acceptsSetupDirectory(raw))
                target[it.key()] = normalizeDestination(raw);
        }
    };
    loadDirMap("bios_dirs", out.biosDirs);
    loadDirMap("save_dirs", out.saveDirs);
    loadDirMap("state_dirs", out.stateDirs);
    return true;
}

bool RetroRomStore::saveSettings(const RetroSettings& in, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    Json defs = Json::object();
    for (const auto& kv : in.defaultEmulator) {
        if (findRetroConsole(kv.first) && isValidEmulatorSelection(kv.first, kv.second) &&
            !kv.second.empty())
            defs[kv.first] = kv.second;
    }
    Json dirs = Json::object();
    for (const auto& kv : in.romDirs) {
        const std::string clean = normalizeDestination(kv.second);
        if (findRetroConsole(kv.first) && acceptsDestination(clean))
            dirs[kv.first] = clean;
    }
    Json nros = Json::object();
    for (const auto& kv : in.nroPaths) {
        if (findRetroConsole(kv.first) && isCustomEmulatorPath(kv.second))
            nros[kv.first] = kv.second;
    }
    auto saveDirMap = [&](const std::map<std::string, std::string>& src) {
        Json out = Json::object();
        for (const auto& kv : src) {
            if (findRetroConsole(kv.first) && acceptsSetupDirectory(kv.second))
                out[kv.first] = normalizeDestination(kv.second);
        }
        return out;
    };
    const Json payload = {
        {"format", 1},
        {"last_console", findRetroConsole(in.lastConsole) ? in.lastConsole : "gba"},
        {"auto_create_forwarder", in.autoCreateForwarder},
        {"default_emulators", std::move(defs)},
        {"rom_dirs", std::move(dirs)},
        {"nro_paths", std::move(nros)},
        {"bios_dirs", saveDirMap(in.biosDirs)},
        {"save_dirs", saveDirMap(in.saveDirs)},
        {"state_dirs", saveDirMap(in.stateDirs)},
    };
    if (!write(settingsPath(), payload, false))
        return fail(error, "Unable to save retro settings.");
    return true;
}

namespace {

bool isRetroJunkFile(const std::string& path) {
    const std::string base = folded(baseNameOf(path));
    if (base == "thumbs.db" || base == ".ds_store" || base == "desktop.ini")
        return true;
    const std::string ext = suffix(path);
    for (const char* junk : {".txt", ".nfo", ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp",
                             ".pdf", ".url", ".html", ".htm", ".md", ".sfv", ".md5", ".sha1",
                             ".sha256", ".log", ".doc", ".docx", ".rtf", ".torrent"}) {
        if (ext == junk)
            return true;
    }
    return false;
}

bool isDiscCompanionExtension(const std::string& ext) {
    for (const char* e : {".bin", ".img", ".raw", ".sub", ".ccd", ".mds", ".sbi", ".cue", ".gdi", ".m3u"}) {
        if (ext == e)
            return true;
    }
    return false;
}

bool isRawTrackExtension(const std::string& ext) {
    return ext == ".bin" || ext == ".img" || ext == ".mdf" || ext == ".raw" ||
           ext == ".sub" || ext == ".sbi";
}

} // namespace

bool planRetroAutoSelection(const std::string& platform, bool isRomset,
                            const std::vector<std::string>& filePaths,
                            std::vector<uint8_t>& actionsOut) {
    actionsOut.clear();
    if (isRomset || filePaths.empty())
        return false;
    const auto* console = findRetroConsole(platform);
    if (!console)
        return false;

    for (const auto& p : filePaths) {
        if (isMultiVolumeArchive(p))
            return false;
    }

    // Single-file torrent: accept any recognized console ROM or single-volume archive.
    if (filePaths.size() == 1) {
        const std::string ext = suffix(filePaths[0]);
        if (consoleHasExtension(*console, ext) || isArchivePath(filePaths[0])) {
            actionsOut = {1};
            return true;
        }
        return false;
    }

    // PS Vita unpacked folder dump (eboot.bin + sce_sys/param.sfo)
    if (console->id == "psvita") {
        bool hasEboot = false;
        bool hasParamSfo = false;
        for (const auto& p : filePaths) {
            const std::string lower = folded(p);
            if (folded(baseNameOf(p)) == "eboot.bin")
                hasEboot = true;
            if (endsWithCi(lower, "/sce_sys/param.sfo") || lower == "sce_sys/param.sfo")
                hasParamSfo = true;
        }
        if (hasEboot && hasParamSfo) {
            actionsOut.assign(filePaths.size(), 1);
            for (size_t i = 0; i < filePaths.size(); ++i) {
                if (isRetroJunkFile(filePaths[i]))
                    actionsOut[i] = 0;
            }
            return true;
        }
    }

    // Wii U unpacked folder dump (code/*.rpx + content/ or meta/)
    if (console->id == "wiiu") {
        bool hasRpx = false;
        bool hasContentOrMeta = false;
        for (const auto& p : filePaths) {
            const std::string lower = folded(p);
            if (suffix(p) == ".rpx" &&
                (lower.find("/code/") != std::string::npos || lower.rfind("code/", 0) == 0))
                hasRpx = true;
            if (lower.find("/content/") != std::string::npos || lower.rfind("content/", 0) == 0 ||
                lower.find("/meta/") != std::string::npos || lower.rfind("meta/", 0) == 0)
                hasContentOrMeta = true;
        }
        if (hasRpx && hasContentOrMeta) {
            actionsOut.assign(filePaths.size(), 1);
            for (size_t i = 0; i < filePaths.size(); ++i) {
                if (isRetroJunkFile(filePaths[i]))
                    actionsOut[i] = 0;
            }
            return true;
        }
    }

    std::vector<size_t> launchIdx;
    std::vector<size_t> archiveIdx;
    std::vector<size_t> companionIdx;
    for (size_t i = 0; i < filePaths.size(); ++i) {
        if (isRetroJunkFile(filePaths[i]))
            continue;
        const std::string ext = suffix(filePaths[i]);
        const bool isLaunch = consoleHasExtension(*console, ext);
        const bool isArch = isArchivePath(filePaths[i]) &&
                            !(console->packageInstallInEmulator && ext == ".zip");
        if (isArch) {
            archiveIdx.push_back(i);
        } else if (isLaunch) {
            launchIdx.push_back(i);
        } else if (console->discBased && isDiscCompanionExtension(ext)) {
            companionIdx.push_back(i);
        } else {
            // Unrecognized non-junk file: fall back to manual file selection.
            return false;
        }
    }

    auto selectIndices = [&](const std::vector<size_t>& a,
                             const std::vector<size_t>& b = {},
                             const std::vector<size_t>& c = {}) {
        actionsOut.assign(filePaths.size(), 0);
        for (size_t idx : a) actionsOut[idx] = 1;
        for (size_t idx : b) actionsOut[idx] = 1;
        for (size_t idx : c) actionsOut[idx] = 1;
        return true;
    };

    // Non-disc consoles: exactly 1 ROM or 1 archive (+ optional junk files).
    if (!console->discBased) {
        if (launchIdx.size() + archiveIdx.size() == 1)
            return selectIndices(launchIdx, archiveIdx);
        return false;
    }

    // Disc-based consoles: single archive or multi-disc archive set (+ optional junk).
    if (!archiveIdx.empty()) {
        if (!launchIdx.empty() || !companionIdx.empty())
            return false;
        if (archiveIdx.size() == 1)
            return selectIndices(archiveIdx);
        // Check if multiple archives are distinct discs of the same game (Disc 1, Disc 2, ...).
        std::string sharedKey;
        std::set<int> discNums;
        for (size_t idx : archiveIdx) {
            std::string key, clean;
            int disc = 0;
            if (!extractDiscNumberAndKey(filePaths[idx], key, clean, disc))
                return false;
            if (sharedKey.empty())
                sharedKey = key;
            else if (sharedKey != key)
                return false;
            if (!discNums.insert(disc).second)
                return false;
        }
        return selectIndices(archiveIdx);
    }

    if (launchIdx.empty())
        return false;

    // Disc-based unpacked files: identify primary disc descriptors.
    std::set<std::string> dirsWithDescriptor;
    size_t m3uCount = 0;
    for (size_t idx : launchIdx) {
        const std::string ext = suffix(filePaths[idx]);
        if (ext == ".cue" || ext == ".gdi" || ext == ".m3u")
            dirsWithDescriptor.insert(folded(parentDirOf(filePaths[idx])));
        if (ext == ".m3u")
            ++m3uCount;
    }
    for (size_t idx : companionIdx) {
        const std::string ext = suffix(filePaths[idx]);
        if (ext == ".ccd" || ext == ".mds" || ext == ".cue" || ext == ".gdi")
            dirsWithDescriptor.insert(folded(parentDirOf(filePaths[idx])));
    }

    if (console->supportsM3u && m3uCount == 1)
        return selectIndices(launchIdx, companionIdx);

    std::vector<size_t> primaryDiscs;
    for (size_t idx : launchIdx) {
        const std::string ext = suffix(filePaths[idx]);
        if (isRawTrackExtension(ext) &&
            dirsWithDescriptor.count(folded(parentDirOf(filePaths[idx]))))
            continue;
        primaryDiscs.push_back(idx);
    }
    if (primaryDiscs.empty())
        primaryDiscs = launchIdx;

    if (primaryDiscs.size() == 1)
        return selectIndices(launchIdx, companionIdx);

    // Multiple primary discs: accept if they form a single multi-disc set (Disc 1, Disc 2, ...).
    std::string sharedKey;
    std::set<int> discNums;
    for (size_t idx : primaryDiscs) {
        std::string key, clean;
        int disc = 0;
        if (!extractDiscNumberAndKey(filePaths[idx], key, clean, disc))
            return false;
        if (sharedKey.empty())
            sharedKey = key;
        else if (sharedKey != key)
            return false;
        if (!discNums.insert(disc).second)
            return false;
    }
    return selectIndices(launchIdx, companionIdx);
}

} // namespace pipensx
