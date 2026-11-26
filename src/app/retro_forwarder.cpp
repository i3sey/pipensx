#include "retro_forwarder.hpp"
#include "retro_catalog.hpp"
#include "core/sha256.h"
#include <algorithm>
#include <array>
#include <borealis/extern/nlohmann/json.hpp>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include "../../vendor/autorun-forwarder/builder.h"
#include "install/install_backend.hpp"
#include <borealis/extern/nanovg/stb_image.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../vendor/borealis/library/lib/extern/glfw/deps/stb_image_write.h"
#pragma GCC diagnostic pop
#include <switch.h>
#endif

namespace pipensx {
namespace {
using Json = nlohmann::json;
constexpr uint64_t idPrefix = 0x0100D70000000000ULL;
bool fail(std::string &error, const std::string &message) {
    error = message;
    return false;
}
bool hex(const std::string &s, size_t n) {
    return s.size() == n && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool gameValid(const std::string &id) {
    const auto colon = id.find(':');
    if (colon == std::string::npos || !hex(id.substr(colon + 1), 64))
        return false;
    const std::string opId = id.substr(0, colon);
    if (opId.empty() || opId.size() > 64)
        return false;
    if (opId.rfind("import-", 0) == 0) {
        const size_t dash = opId.find('-', 7);
        if (dash == std::string::npos || dash <= 7)
            return false;
        const std::string plat = opId.substr(7, dash - 7);
        return findRetroConsole(plat) != nullptr && hex(opId.substr(dash + 1), 12);
    }
    const size_t dash = opId.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= opId.size())
        return false;
    const std::string plat = opId.substr(0, dash);
    const std::string topic = opId.substr(dash + 1);
    return findRetroConsole(plat) != nullptr && topic.size() <= 20 && topic[0] != '0' &&
           topic.find_first_not_of("0123456789") == std::string::npos;
}

bool nroPathValid(const std::string &nro) {
    if (nro.size() < 11 || nro.size() > 512 || nro.rfind("sdmc:/", 0) != 0)
        return false;
    if (nro.find("..") != std::string::npos)
        return false;
    for (unsigned char c : nro) {
        if (c < 0x20 || c == '\\' || c == '"' || c == '\'')
            return false;
    }
    const std::string tail = nro.substr(nro.size() - 4);
    std::string lower;
    for (char c : tail)
        lower += (c >= 'A' && c <= 'Z') ? char(c + 'a' - 'A') : c;
    return lower == ".nro";
}
std::string digest(const void *data, size_t size) {
    uint8_t bytes[32];
    sha256(data, size, bytes);
    std::string out;
    for (uint8_t byte : bytes) {
        char value[3];
        std::snprintf(value, sizeof(value), "%02x", byte);
        out += value;
    }
    return out;
}
std::string key(const std::string &game) { return digest(game.data(), game.size()); }
bool absent(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) != 0 && errno == ENOENT;
}
bool regular(const std::string &path) {
    struct stat st{};
    for (size_t slash = path.find('/', path.front() == '/' ? 1 : 0); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        const auto parent = path.substr(0, slash);
#ifdef __SWITCH__
        if (parent == "sdmc:")
            continue;
#endif
        if (lstat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return false;
    }
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool dirs(const std::string &path) {
    for (size_t end = path.front() == '/' ? 1 : 0; end <= path.size(); ++end) {
        if (end == 0 || (end != path.size() && path[end] != '/'))
            continue;
        const auto part = path.substr(0, end);
#ifdef __SWITCH__
        if (part == "sdmc:")
            continue;
#endif
        struct stat st{};
        if ((mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) || lstat(part.c_str(), &st) != 0 ||
            !S_ISDIR(st.st_mode))
            return false;
    }
    return true;
}
struct Receipt {
    std::string game, fingerprint, previous;
    uint64_t tid = 0;
    bool installed = true;
};
bool decode(const Json &j, Receipt &r) {
    if (!j.is_object() || j.value("format", Json()) != 1)
        return false;
    for (auto field : {"game", "title_id", "fingerprint", "previous"})
        if (!j.contains(field) || !j[field].is_string())
            return false;
    Receipt out;
    out.game = j["game"];
    out.fingerprint = j["fingerprint"];
    out.previous = j["previous"];
    if (j.contains("installed")) {
        if (!j["installed"].is_boolean())
            return false;
        out.installed = j["installed"];
    }
    const std::string tid = j["title_id"];
    if (!gameValid(out.game) || !hex(out.fingerprint, 64) ||
        (!out.previous.empty() && !hex(out.previous, 64)) || !hex(tid, 16))
        return false;
    for (char c : tid)
        out.tid = (out.tid << 4) | uint64_t(c <= '9' ? c - '0' : c - 'a' + 10);
    if ((out.tid & 0xffffff0000000000ULL) != idPrefix || (out.tid & 0x1fff))
        return false;
    r = std::move(out);
    return true;
}
bool readOne(const std::string &path, Receipt &r) {
    if (!regular(path))
        return false;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size < 0 || st.st_size > 16384)
        return false;
    std::string body(static_cast<size_t>(st.st_size), '\0');
    std::ifstream in(path, std::ios::binary);
    if (!in.read(body.data(), body.size()) || in.peek() != std::char_traits<char>::eof())
        return false;
    return decode(Json::parse(body, nullptr, false), r);
}
bool read(const std::string &path, Receipt &r, bool &found, std::string &error) {
    found = false;
    if (absent(path) && absent(path + ".previous"))
        return true;
    if (!(readOne(path, r) || readOne(path + ".previous", r)))
        return fail(error, "Unreadable HOME shortcut receipt; no application was changed.");
    found = true;
    return true;
}
bool write(const std::string &path, const Receipt &r, std::string &error) {
    std::string tid = RetroForwarderService::titleIdText(r.tid);
    std::transform(tid.begin(), tid.end(), tid.begin(),
                   [](char c) { return c >= 'A' && c <= 'F' ? char(c + 'a' - 'A') : c; });
    const auto body = Json({{"format", 1},
                            {"game", r.game},
                            {"title_id", tid},
                            {"fingerprint", r.fingerprint},
                            {"previous", r.previous},
                            {"installed", r.installed}})
                          .dump();
    const auto tmp = path + ".tmp";
    if (!absent(tmp) && (!regular(tmp) || unlink(tmp.c_str()) != 0))
        return fail(error, "Unsafe HOME shortcut staging file.");
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return fail(error, "Unable to stage HOME shortcut receipt.");
    bool ok = true;
    size_t sent = 0;
    while (sent < body.size()) {
        const auto n = ::write(fd, body.data() + sent, body.size() - sent);
        if (n <= 0) {
            ok = false;
            break;
        }
        sent += n;
    }
    if (fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (ok && !absent(path)) {
        if (!regular(path))
            ok = false;
        else {
            Receipt previous;
            if (!readOne(path, previous))
                ok = unlink(path.c_str()) == 0;
            else if (!absent(path + ".previous") &&
                     (!regular(path + ".previous") || unlink((path + ".previous").c_str()) != 0))
                ok = false;
            else
                ok = rename(path.c_str(), (path + ".previous").c_str()) == 0;
        }
    }
    if (ok)
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) {
        unlink(tmp.c_str());
        return fail(error, "Unable to save HOME shortcut receipt.");
    }
    return true;
}
bool belongs(const Receipt &r, const std::string &live) {
    return live == r.fingerprint || (!r.previous.empty() && live == r.previous);
}
bool reservations(const std::string &root, std::set<uint64_t> &ids, std::string &error) {
    DIR *dir = opendir(root.c_str());
    if (!dir)
        return fail(error, "Unable to read HOME shortcut reservations.");
    std::set<std::string> names;
    while (auto *e = readdir(dir)) {
        const std::string name = e->d_name;
        for (auto suffix : {".json.previous", ".json"}) {
            const size_t size = std::strlen(suffix);
            if (name.size() == 64 + size && name.compare(64, size, suffix) == 0 &&
                hex(name.substr(0, 64), 64))
                names.insert(name.substr(0, 64));
        }
        if (names.size() > 10000) {
            closedir(dir);
            return fail(error, "Too many HOME shortcut receipts.");
        }
    }
    closedir(dir);
    for (const auto &name : names) {
        Receipt r;
        bool found;
        if (!read(root + "/" + name + ".json", r, found, error) || !found || key(r.game) != name)
            return fail(error, "Invalid HOME shortcut reservation.");
        ids.insert(r.tid);
    }
    return true;
}
bool packageValid(const RetroForwarderPackage &p) {
    if (!hex(p.fingerprint, 64) || p.files.size() != 3)
        return false;
    size_t bytes = 0;
    for (size_t i = 0; i < 3; ++i) {
        const auto &f = p.files[i];
        const auto suffix = i == 2 ? ".cnmt.nca" : ".nca";
        if (f.bytes.empty() || f.bytes.size() > 16 * 1024 * 1024 - bytes ||
            f.name.size() != 32 + std::strlen(suffix) || !hex(f.name.substr(0, 32), 32) ||
            f.name.substr(32) != suffix ||
            digest(f.bytes.data(), f.bytes.size()).substr(0, 32) != f.name.substr(0, 32))
            return false;
        bytes += f.bytes.size();
    }
    return p.fingerprint == p.files[0].name.substr(0, 32) + p.files[1].name.substr(0, 32);
}
} // namespace
std::mutex RetroForwarderService::mutex_;
RetroForwarderService::RetroForwarderService(std::string root, Resolve resolve,
                                             RetroForwarderPlatform platform)
    : root_(std::move(root) + "/retro/forwarders"), resolve_(std::move(resolve)),
      platform_(std::move(platform)) {}
std::string RetroForwarderService::titleIdText(uint64_t id) {
    char value[17];
    std::snprintf(value, sizeof(value), "%016llX", static_cast<unsigned long long>(id));
    return value;
}
uint64_t RetroForwarderService::candidateId(const std::string &game, unsigned probe) {
    const auto hash = key(game + ":" + std::to_string(probe));
    uint64_t value = 0;
    for (size_t i = 0; i < 7; ++i)
        value = (value << 4) | uint64_t(hash[i] <= '9' ? hash[i] - '0' : hash[i] - 'a' + 10);
    return idPrefix | ((value & 0x7ffffffULL) << 13);
}
bool RetroForwarderService::create(const std::string &game, uint64_t &id,
                                   std::string &error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    id = 0;
    error.clear();
    if (!gameValid(game) || !resolve_ || !platform_.inspect || !platform_.build ||
        !platform_.install || !platform_.remove)
        return fail(error, "HOME shortcut service is unavailable.");
    RetroForwarderTarget target;
    if (!resolve_(game, target, error))
        return false;
    if (target.title.empty() || target.title.size() > 1024 ||
        !nroPathValid(target.nro) || target.arguments.empty() ||
        target.arguments.size() > 1600 || target.title.find('\0') != std::string::npos ||
        target.arguments.find('\0') != std::string::npos)
        return fail(error, "Invalid HOME shortcut target.");
    if (target.title.size() > 511) {
        size_t end = 511;
        while (end && (static_cast<unsigned char>(target.title[end]) & 0xc0) == 0x80)
            --end;
        target.title.resize(end);
    }
    if (!dirs(root_))
        return fail(error, "Unable to create HOME shortcut receipt directory.");
    const auto path = root_ + "/" + key(game) + ".json";
    Receipt r;
    bool found;
    if (!read(path, r, found, error))
        return false;
    std::string live;
    if (found) {
        if (r.game != game)
            return fail(error, "HOME shortcut receipt belongs to another game.");
        if (!platform_.inspect(r.tid, live, error))
            return false;
        if (!live.empty() && !belongs(r, live))
            return fail(error, "HOME application changed; refusing to replace it.");
    } else {
        std::set<uint64_t> reserved;
        if (!reservations(root_, reserved, error))
            return false;
        r.game = game;
        for (unsigned probe = 0; probe < 32; ++probe) {
            const auto candidate = candidateId(game, probe);
            if (reserved.count(candidate))
                continue;
            if (!platform_.inspect(candidate, live, error))
                return false;
            if (live.empty()) {
                r.tid = candidate;
                break;
            }
        }
        if (!r.tid)
            return fail(error, "No free HOME application ID for this game.");
    }
    RetroForwarderPackage package;
    if (!platform_.build(r.tid, target, package, error))
        return false;
    if (!packageValid(package))
        return fail(error, "Invalid generated HOME shortcut package.");
    if (live == package.fingerprint) {
        r.fingerprint = live;
        r.previous.clear();
        r.installed = true;
        if (!write(path, r, error))
            return false;
        id = r.tid;
        return true;
    }
    r.previous = live;
    r.fingerprint = package.fingerprint;
    r.installed = true;
    if (!write(path, r, error))
        return false;
    std::string check;
    if (!platform_.inspect(r.tid, check, error))
        return false;
    if (check != live)
        return fail(error, "HOME application changed during preparation.");
    if (!live.empty() && !platform_.remove(r.tid, live, error))
        return false;
    if (!platform_.install(r.tid, package, error))
        return false;
    if (!platform_.inspect(r.tid, check, error) || check != package.fingerprint)
        return fail(error,
                    error.empty() ? "Installed HOME shortcut could not be verified." : error);
    r.previous.clear();
    r.installed = true;
    if (!write(path, r, error))
        return false;
    id = r.tid;
    return true;
}
bool RetroForwarderService::remove(const std::string &game, std::string &error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (!gameValid(game) || !platform_.inspect || !platform_.remove)
        return fail(error, "HOME shortcut service is unavailable.");
    const auto path = root_ + "/" + key(game) + ".json";
    Receipt r;
    bool found;
    if (!read(path, r, found, error))
        return false;
    if (!found)
        return true;
    if (r.game != game)
        return fail(error, "HOME shortcut receipt belongs to another game.");
    std::string live;
    if (!platform_.inspect(r.tid, live, error))
        return false;
    if (!live.empty() && (!belongs(r, live) || !platform_.remove(r.tid, live, error)))
        return fail(error,
                    error.empty() ? "HOME application changed; refusing to delete it." : error);
    if (!platform_.inspect(r.tid, live, error) || !live.empty())
        return fail(error, error.empty() ? "HOME shortcut removal could not be verified." : error);
    r.previous.clear();
    r.installed = false;
    return write(path, r, error);
}
bool RetroForwarderService::hasShortcut(const std::string &game) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!gameValid(game))
        return false;
    Receipt r;
    bool found = false;
    std::string error;
    if (!read(root_ + "/" + key(game) + ".json", r, found, error) || !found || r.game != game)
        return false;
#ifdef __SWITCH__
    if (platform_.inspect) {
        std::string live;
        if (platform_.inspect(r.tid, live, error))
            return !live.empty() && belongs(r, live);
    }
#endif
    return r.installed;
}
namespace {
#ifdef __SWITCH__
bool resultError(std::string &error, const char *action, Result rc) {
    char value[128];
    std::snprintf(value, sizeof(value), "%s (0x%08x).", action, rc);
    return fail(error, value);
}
std::string contentText(const NcmContentId &id) {
    std::string value;
    for (uint8_t b : id.c) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02x", b);
        value += h;
    }
    return value;
}
bool inspectInstalled(uint64_t id, std::string &fingerprint, std::string &error) {
    fingerprint.clear();
    bool registered = false, foreign = false, complete = false;
    for (s32 offset = 0; offset < 16384; offset += 64) {
        std::array<NsApplicationRecord, 64> records{};
        s32 count = 0;
        const Result rc = nsListApplicationRecord(records.data(), records.size(), offset, &count);
        if (R_FAILED(rc))
            return resultError(error, "Unable to list HOME applications", rc);
        if (count < 0 || count > 64)
            return fail(error, "Invalid HOME application list.");
        for (s32 i = 0; i < count; ++i)
            if ((records[i].application_id & ~0x1fffULL) == id) {
                registered = true;
                if (records[i].application_id != id)
                    foreign = true;
            }
        if (count < 64) {
            complete = true;
            break;
        }
    }
    if (!complete)
        return fail(error, "HOME application list is incomplete.");
    unsigned metas = 0;
    std::string found;
    for (auto storage : {NcmStorageId_SdCard, NcmStorageId_BuiltInUser}) {
        NcmContentMetaDatabase db{};
        Result rc = ncmOpenContentMetaDatabase(&db, storage);
        if (R_FAILED(rc))
            return resultError(error, "Unable to inspect HOME metadata", rc);
        bool ok = true;
        for (auto type : {NcmContentMetaType_Application, NcmContentMetaType_Patch,
                          NcmContentMetaType_AddOnContent, NcmContentMetaType_Delta,
                          NcmContentMetaType_DataPatch}) {
            std::array<NcmContentMetaKey, 2> keys{};
            s32 total = 0, written = 0;
            rc = ncmContentMetaDatabaseList(&db, &total, &written, keys.data(), keys.size(), type,
                                            0, id, id + 0x1fff, NcmContentInstallType_Unknown);
            if (R_FAILED(rc)) {
                ok = resultError(error, "Unable to inspect HOME metadata", rc);
                break;
            }
            if (total < 0 || written < 0 || written > 2 || written != std::min(total, s32(2))) {
                ok = fail(error, "Invalid HOME metadata list.");
                break;
            }
            if (total > 1 || metas + unsigned(total) > 1)
                foreign = true;
            metas += unsigned(total);
            if (total == 1) {
                const auto &key = keys[0];
                if (storage != NcmStorageId_SdCard || type != NcmContentMetaType_Application ||
                    key.id != id || key.version != 0 ||
                    key.install_type != NcmContentInstallType_Full) {
                    foreign = true;
                    continue;
                }
                NcmContentId program{}, control{};
                rc = ncmContentMetaDatabaseGetContentIdByType(&db, &program, &key,
                                                              NcmContentType_Program);
                if (R_SUCCEEDED(rc))
                    rc = ncmContentMetaDatabaseGetContentIdByType(&db, &control, &key,
                                                                  NcmContentType_Control);
                if (R_FAILED(rc)) {
                    ok = resultError(error, "Unable to identify HOME application content", rc);
                    break;
                }
                NcmContentStorage content{};
                rc = ncmOpenContentStorage(&content, storage);
                if (R_FAILED(rc)) {
                    ok = resultError(error, "Unable to inspect HOME content storage", rc);
                    break;
                }
                bool hasProgram = false, hasControl = false;
                rc = ncmContentStorageHas(&content, &hasProgram, &program);
                if (R_SUCCEEDED(rc))
                    rc = ncmContentStorageHas(&content, &hasControl, &control);
                ncmContentStorageClose(&content);
                if (R_FAILED(rc)) {
                    ok = resultError(error, "Unable to check HOME content", rc);
                    break;
                }
                if (!hasProgram || !hasControl)
                    foreign = true;
                found = contentText(program) + contentText(control);
            }
        }
        ncmContentMetaDatabaseClose(&db);
        if (!ok)
            return false;
    }
    if (!registered && !metas)
        return true;
    fingerprint = registered && !foreign && metas == 1 && !found.empty() ? found : "occupied";
    return true;
}
std::vector<uint8_t> asset(const char *name, size_t limit) {
    std::ifstream in(name, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    const auto size = in.tellg();
    if (size <= 0 || uint64_t(size) > limit)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        return {};
    return bytes;
}
uint64_t little(const uint8_t *bytes, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
std::vector<uint8_t> buildCoverIcon256(const std::vector<uint8_t> &raw) {
    if (raw.empty() || raw.size() > 8 * 1024 * 1024)
        return {};
    int srcW = 0, srcH = 0, srcCh = 0;
    uint8_t *decoded = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()),
                                             &srcW, &srcH, &srcCh, 3);
    if (!decoded)
        return {};
    struct FreeImg {
        uint8_t *p;
        ~FreeImg() { stbi_image_free(p); }
    } guard{decoded};
    if (srcW <= 0 || srcH <= 0 || srcW > 4096 || srcH > 4096)
        return {};

    // Average muted edge fill from border pixels
    uint64_t sumR = 0, sumG = 0, sumB = 0, samples = 0;
    for (int x = 0; x < srcW; x += std::max(1, srcW / 16)) {
        for (int y : {0, srcH - 1}) {
            const size_t idx = (static_cast<size_t>(y) * srcW + x) * 3;
            sumR += decoded[idx];
            sumG += decoded[idx + 1];
            sumB += decoded[idx + 2];
            ++samples;
        }
    }
    const uint8_t bgR = samples ? static_cast<uint8_t>((sumR / samples) * 28 / 100 + 14) : 18;
    const uint8_t bgG = samples ? static_cast<uint8_t>((sumG / samples) * 28 / 100 + 16) : 20;
    const uint8_t bgB = samples ? static_cast<uint8_t>((sumB / samples) * 28 / 100 + 22) : 26;

    constexpr int kSize = 256;
    std::vector<uint8_t> canvas(static_cast<size_t>(kSize) * kSize * 3);
    for (int i = 0; i < kSize * kSize; ++i) {
        canvas[i * 3] = bgR;
        canvas[i * 3 + 1] = bgG;
        canvas[i * 3 + 2] = bgB;
    }

    const float aspect = static_cast<float>(srcW) / static_cast<float>(srcH);
    int dstW = kSize, dstH = kSize, ox = 0, oy = 0;
    if (aspect < 0.88f || aspect > 1.14f) {
        const float scale = std::min(static_cast<float>(kSize) / srcW,
                                     static_cast<float>(kSize) / srcH);
        dstW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
        dstH = std::max(1, static_cast<int>(std::lround(srcH * scale)));
        ox = (kSize - dstW) / 2;
        oy = (kSize - dstH) / 2;
    }

    for (int y = 0; y < dstH; ++y) {
        const float sy = (dstH > 1) ? (y + 0.5f) * srcH / dstH - 0.5f : 0.0f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, srcH - 1);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
        for (int x = 0; x < dstW; ++x) {
            const float sx = (dstW > 1) ? (x + 0.5f) * srcW / dstW - 0.5f : 0.0f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, srcW - 1);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
            const uint8_t *p00 = decoded + (static_cast<size_t>(y0) * srcW + x0) * 3;
            const uint8_t *p10 = decoded + (static_cast<size_t>(y0) * srcW + x1) * 3;
            const uint8_t *p01 = decoded + (static_cast<size_t>(y1) * srcW + x0) * 3;
            const uint8_t *p11 = decoded + (static_cast<size_t>(y1) * srcW + x1) * 3;
            uint8_t *out = canvas.data() + (static_cast<size_t>(oy + y) * kSize + (ox + x)) * 3;
            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] + (p10[c] - p00[c]) * fx;
                const float bot = p01[c] + (p11[c] - p01[c]) * fx;
                out[c] = static_cast<uint8_t>(std::clamp(std::lround(top + (bot - top) * fy), 0L, 255L));
            }
        }
    }

    auto writeCb = [](void *ctx, void *data, int size) {
        if (!ctx || !data || size <= 0) return;
        auto *buf = static_cast<std::vector<uint8_t> *>(ctx);
        const auto *bytes = static_cast<const uint8_t *>(data);
        buf->insert(buf->end(), bytes, bytes + size);
    };
    for (int q : {85, 72, 58}) {
        std::vector<uint8_t> encoded;
        if (stbi_write_jpg_to_func(writeCb, &encoded, kSize, kSize, 3, canvas.data(), q) &&
            !encoded.empty() && encoded.size() <= 115 * 1024)
            return encoded;
    }
    return {};
}

bool buildPackage(uint64_t id, const RetroForwarderTarget &target, RetroForwarderPackage &out,
                  std::string &error) {
    const auto loader = asset("romfs:/autorun-loader.nso", 4 * 1024 * 1024),
               npdm = asset("romfs:/autorun-loader.npdm", 1024 * 1024);
    std::vector<uint8_t> icon = buildCoverIcon256(target.iconJpeg);
    if (icon.empty())
        icon = asset("romfs:/autorun-icon.jpg", 1024 * 1024);
    void *data = nullptr;
    size_t size = 0;
    const Result rc =
        pipensxBuildForwarderEx(id, target.title.c_str(), "pipensx Retro", nullptr, nullptr,
                                target.nro.c_str(), target.arguments.c_str(), loader.data(),
                                loader.size(), npdm.data(), npdm.size(), icon.data(), icon.size(),
                                &data, &size);
    if (R_FAILED(rc))
        return resultError(error, "Unable to build HOME shortcut", rc);
    struct Free {
        void *data;
        ~Free() { std::free(data); }
    } cleanup{data};
    if (!data || size < 16 || size > 16 * 1024 * 1024)
        return fail(error, "Invalid HOME package size.");
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (std::memcmp(bytes, "PFS0", 4) || little(bytes + 4, 4) != 3)
        return fail(error, "Invalid HOME package header.");
    const uint64_t strings = little(bytes + 8, 4), header = 16 + 3 * 24 + strings;
    if (strings > 512 || header > size)
        return fail(error, "Invalid HOME package table.");
    RetroForwarderPackage package;
    for (unsigned i = 0; i < 3; ++i) {
        const auto *entry = bytes + 16 + i * 24;
        const uint64_t offset = little(entry, 8), length = little(entry + 8, 8),
                       name = little(entry + 16, 4);
        if (name >= strings || offset > size - header || length > size - header - offset)
            return fail(error, "Invalid HOME package entry.");
        const auto *start = reinterpret_cast<const char *>(bytes + 16 + 3 * 24 + name);
        const auto *end = static_cast<const char *>(std::memchr(start, 0, strings - name));
        if (!end)
            return fail(error, "Invalid HOME package filename.");
        package.files.push_back(
            {std::string(start, end),
             std::vector<uint8_t>(bytes + header + offset, bytes + header + offset + length)});
    }
    package.fingerprint = package.files[0].name.substr(0, 32) + package.files[1].name.substr(0, 32);
    if (!packageValid(package))
        return fail(error, "Generated HOME content failed verification.");
    out = std::move(package);
    return true;
}
#endif
} // namespace
RetroForwarderPlatform retroForwarderPlatform(const std::string &root) {
#ifdef __SWITCH__
    RetroForwarderPlatform platform;
    platform.inspect = inspectInstalled;
    platform.build = buildPackage;
    platform.remove = [](uint64_t id, const std::string &expected, std::string &error) {
        std::string live;
        if (!inspectInstalled(id, live, error))
            return false;
        if (live.empty())
            return true;
        if (live != expected)
            return fail(error, "HOME application changed before removal.");
        const Result rc = nsDeleteApplicationCompletely(id);
        return R_SUCCEEDED(rc) || resultError(error, "Unable to remove HOME shortcut", rc);
    };
    platform.install = [root](uint64_t id, const RetroForwarderPackage &package,
                              std::string &error) {
        std::string live;
        if (!inspectInstalled(id, live, error))
            return false;
        if (!live.empty())
            return fail(error, "HOME application ID is no longer free.");
        auto backend = install::createInstallBackend(root + "/retro/install",
                                                     install::InstallStorageTarget::SdCard);
        if (!backend ||
            !backend->beginPackage("retro-forwarder-" + RetroForwarderService::titleIdText(id),
                                   "Retro HOME.nsp"))
            return fail(error, backend ? backend->error() : "HOME installer unavailable.");
        for (const auto &file : package.files) {
            if (!backend->beginFile(file.name, file.bytes.size()) ||
                !backend->writeFile(file.bytes.data(), file.bytes.size()) || !backend->endFile()) {
                error = backend->error();
                backend->rollbackPackage();
                return false;
            }
        }
        if (backend->packageApplicationId() != id) {
            backend->rollbackPackage();
            return fail(error, "HOME package application ID mismatch.");
        }
        bool already = false;
        if (!backend->commitPackage(already)) {
            error = backend->error();
            backend->rollbackPackage();
            return false;
        }
        return true;
    };
    return platform;
#else
    (void)root;
    const auto unavailable = [](std::string &error) {
        return fail(error, "HOME shortcut operations require Nintendo Switch.");
    };
    return {
        [unavailable](uint64_t, std::string &, std::string &error) { return unavailable(error); },
        [unavailable](uint64_t, const RetroForwarderTarget &, RetroForwarderPackage &,
                      std::string &error) { return unavailable(error); },
        [unavailable](uint64_t, const RetroForwarderPackage &, std::string &error) {
            return unavailable(error);
        },
        [unavailable](uint64_t, const std::string &, std::string &error) {
            return unavailable(error);
        }};
#endif
}
} // namespace pipensx
