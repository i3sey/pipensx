#include "retro_emulator.hpp"

#include "core/client_identity.h"
#include "core/sha256.h"
#include "curl_https.hpp"
#include "port_archive.hpp"
#include "task_files.hpp"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr uint64_t kMaxPackageBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kMaxUnpackedBytes = 256ULL * 1024 * 1024;
constexpr const char* kMgba0105Url =
    "https://github.com/mgba-emu/mgba/releases/download/0.10.5/mGBA-0.10.5-switch.7z";
constexpr uint64_t kMgba0105Bytes = 2211306;
constexpr const char* kMgba0105Sha256 =
    "ef7a7f5cea6d6639c8a12ebb282d915e444bf844b658ff5a4dd72886c8cfa016";

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}

bool endsWithCi(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    return lowerAscii(value.substr(value.size() - suffix.size())) == lowerAscii(suffix);
}

std::string baseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentDirOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool directory(const std::string& path) {
    if (path.empty()) return false;
#ifdef __SWITCH__
    if (path == "sdmc:" || path == "sdmc:/") return true;
#endif
    struct stat st{};
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensureDirectories(const std::string& path) {
    if (path.empty()) return false;
    size_t start = path.front() == '/' ? 1 : 0;
#ifdef __SWITCH__
    if (path.rfind("sdmc:/", 0) == 0) start = 6;
#endif
    for (size_t slash = path.find('/', start); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        if (!directory(path.substr(0, slash))) return false;
    }
    return directory(path);
}

bool absent(const std::string& path) {
    struct stat st{};
    return lstat(path.c_str(), &st) != 0 && errno == ENOENT;
}

bool removeStage(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool ok = true;
    while (auto* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != ".." && !removeStage(path + "/" + name)) ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
}

bool prepareStage(const std::string& path) {
    constexpr const char* owner = "pipensx-mgba-install-v1\n";
    if (!absent(path)) {
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        const auto marker = path + "/.owner";
        if (lstat(marker.c_str(), &st) != 0) {
            if (errno != ENOENT || rmdir(path.c_str()) != 0) return false;
        } else {
            if (!S_ISREG(st.st_mode) || st.st_size != static_cast<off_t>(std::strlen(owner))) return false;
            FILE* file = fopen(marker.c_str(), "rb");
            if (!file) return false;
            std::array<char, 64> body{};
            const bool owned = fread(body.data(), 1, std::strlen(owner), file) == std::strlen(owner) &&
                               std::memcmp(body.data(), owner, std::strlen(owner)) == 0;
            fclose(file);
            if (!owned || !removeStage(path)) return false;
        }
    }
    if (!directory(path)) return false;
    FILE* file = fopen((path + "/.owner").c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(owner, 1, std::strlen(owner), file) == std::strlen(owner);
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok;
}

bool validNro(const std::string& path, std::string& error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 128 ||
        st.st_size > static_cast<off_t>(kMaxUnpackedBytes))
        return fail(error, "Missing or invalid emulator NRO.");
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return fail(error, "Unable to open emulator NRO.");
    std::array<unsigned char, 128> header{};
    const bool read = fread(header.data(), 1, header.size(), file) == header.size();
    fclose(file);
    const auto* p = header.data() + 0x18;
    const uint32_t size = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    if (!read || std::memcmp(header.data() + 0x10, "NRO0", 4) || size < 128 || size > uint64_t(st.st_size))
        return fail(error, "Invalid emulator NRO header.");
    return true;
}

struct Transfer {
    const HomebrewCatalog::Sink& sink;
    const std::atomic<bool>& cancelled;
    const HomebrewCatalog::Progress& progress;
    uint64_t expectedBytes = 0;
    uint64_t received = 0;
    CURL* curl = nullptr;
};

size_t receive(char* data, size_t size, size_t count, void* opaque) {
    auto& t = *static_cast<Transfer*>(opaque);
    const size_t bytes = size * count;
    const uint64_t limit = t.expectedBytes > 0 ? t.expectedBytes : kMaxPackageBytes;
    if (t.cancelled || bytes > limit - t.received || !t.sink(reinterpret_cast<uint8_t*>(data), bytes))
        return 0;
    t.received += bytes;
    return bytes;
}

int report(void* opaque, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto& t = *static_cast<Transfer*>(opaque);
    uint64_t total = t.expectedBytes;
    if (total == 0 && dltotal > 0) total = static_cast<uint64_t>(dltotal);
    const uint64_t now = dlnow > 0 ? static_cast<uint64_t>(dlnow) : t.received;
    if (t.progress) t.progress(now, total);
    return t.cancelled ? 1 : 0;
}

bool fetch(const std::string& url, uint64_t expectedBytes, const HomebrewCatalog::Sink& sink,
           const std::atomic<bool>& cancelled, const HomebrewCatalog::Progress& progress, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) return fail(error, "Unable to initialize emulator HTTP client.");
    Transfer transfer{sink, cancelled, progress, expectedBytes, 0, curl};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, PIPENSX_USER_AGENT);
    curlPinHttpsOnly(curl);
    curlApplyTrustedSsl(curl);
    CurlSocketTuning socketTuning;
    curlTuneDownloadSocket(curl, &socketTuning);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1200L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, report);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (cancelled) return fail(error, "Cancelled.");
    if (result != CURLE_OK || status != 200)
        return fail(error, "Emulator HTTP " + std::to_string(status) + ": " + curl_easy_strerror(result));
    return true;
}

RetroEmulatorPackage makeLibretroPkg(const std::string& id, const std::string& name,
                                     const std::string& coreName,
                                     const std::vector<std::string>& consoles) {
    RetroEmulatorPackage pkg;
    pkg.id = id;
    pkg.name = name;
    pkg.author = "Libretro";
    pkg.version = "latest";
    pkg.category = "retroarch";
    pkg.filename = coreName + "_libretro_libnx.nro.zip";
    pkg.downloadUrl = "https://buildbot.libretro.com/nightly/nintendo/switch/libnx/latest/" + pkg.filename;
    pkg.installPath = "sdmc:/retroarch/cores/" + coreName + "_libretro_libnx.nro";
    pkg.extractDir = "sdmc:/retroarch/cores";
    pkg.supportedConsoleIds = consoles;
    pkg.bytes = 0;
    pkg.archive = true;
    return pkg;
}

RetroEmulatorPackage makeGhPkg(const std::string& id, const std::string& name, const std::string& author,
                               const std::string& version, const std::string& url,
                               const std::string& filename, const std::string& installPath,
                               const std::string& extractDir, bool archive, uint64_t bytes,
                               const std::vector<std::string>& consoles) {
    RetroEmulatorPackage pkg;
    pkg.id = id;
    pkg.name = name;
    pkg.author = author;
    pkg.version = version;
    pkg.downloadUrl = url;
    pkg.filename = filename;
    pkg.installPath = installPath;
    pkg.extractDir = extractDir;
    pkg.archive = archive;
    pkg.bytes = bytes;
    pkg.supportedConsoleIds = consoles;
    return pkg;
}

RetroEmulatorProfile makeRaProfile(const std::string& id, const std::string& displayName,
                                   const std::string& coreName,
                                   const std::vector<std::string>& consoles) {
    RetroEmulatorProfile p;
    p.id = id;
    p.displayName = displayName;
    p.author = "Libretro";
    p.isRetroArchCore = true;
    p.supportedConsoles = consoles;
    p.nroRelativePath = "retroarch/cores/" + coreName + "_libretro_libnx.nro";
    p.expectedArchiveNroName = coreName + "_libretro_libnx.nro";
    p.allowedUrlPrefix = "https://buildbot.libretro.com/nightly/nintendo/switch/libnx/";
    p.fallbackPackage = makeLibretroPkg(id, displayName, coreName, consoles);
    p.launchMode = RetroEmulatorProfile::LaunchMode::PositionalRom;
    p.applicationModeRequired = true;
    p.platform = consoles.empty() ? "" : consoles.front();
    return p;
}

bool extractZipTargetNro(const std::string& zipPath, const std::string& expectedBasename,
                         const std::string& stagedOut, const std::atomic<bool>& cancelled,
                         std::string& error) {
    archive* reader = archive_read_new();
    if (!reader) return fail(error, "Unable to initialize archive reader.");
    struct Release {
        archive* a;
        ~Release() { archive_read_free(a); }
    } release{reader};
    archive_read_support_format_zip(reader);
    if (archive_read_open_filename(reader, zipPath.c_str(), 64 * 1024) != ARCHIVE_OK)
        return fail(error, "Unable to open emulator ZIP archive.");
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    size_t count = 0;
    size_t matches = 0;
    const std::string want = lowerAscii(expectedBasename);
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        if (cancelled) return fail(error, "Cancelled.");
        if (++count > 512) return fail(error, "Emulator archive contains too many entries.");
        if (archive_entry_is_encrypted(entry) > 0)
            return fail(error, "Encrypted emulator archives are not supported.");
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            return fail(error, "Symlinks in emulator archives are not supported.");
        const char* rawName = archive_entry_pathname_utf8(entry);
        if (!rawName) rawName = archive_entry_pathname(entry);
        std::string path = rawName ? rawName : "";
        if (archive_entry_filetype(entry) != AE_IFREG) continue;
        if (!taskFilePathIsSafe(path)) return fail(error, "Unsafe emulator archive path.");
        if (lowerAscii(baseNameOf(path)) != want) continue;
        if (++matches > 1) return fail(error, "Ambiguous emulator NRO inside archive.");
        const la_int64_t declared = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : -1;
        if (declared < 128 || static_cast<uint64_t>(declared) > kMaxUnpackedBytes)
            return fail(error, "Emulator NRO size out of range.");
        const int fd = open(stagedOut.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) return fail(error, "Unable to create staged emulator NRO.");
        std::array<char, 64 * 1024> buf{};
        uint64_t written = 0;
        bool writeOk = true;
        while (true) {
            if (cancelled) { writeOk = fail(error, "Cancelled."); break; }
            const ssize_t n = archive_read_data(reader, buf.data(), buf.size());
            if (n < 0) { writeOk = fail(error, "Unable to decompress emulator NRO."); break; }
            if (n == 0) break;
            if (written + static_cast<uint64_t>(n) > kMaxUnpackedBytes) {
                writeOk = fail(error, "Emulator NRO exceeds unpacked size limit.");
                break;
            }
            size_t off = 0;
            while (off < static_cast<size_t>(n)) {
                const ssize_t w = ::write(fd, buf.data() + off, static_cast<size_t>(n) - off);
                if (w <= 0) { writeOk = fail(error, "Unable to write staged emulator NRO."); break; }
                off += static_cast<size_t>(w);
            }
            if (!writeOk) break;
            written += static_cast<uint64_t>(n);
        }
        if (writeOk && fsync(fd) != 0) writeOk = fail(error, "Unable to flush staged emulator NRO.");
        if (close(fd) != 0) writeOk = fail(error, "Unable to close staged emulator NRO.");
        if (!writeOk) return false;
        if (written != static_cast<uint64_t>(declared))
            return fail(error, "Extracted emulator NRO size mismatch.");
    }
    if (status != ARCHIVE_EOF)
        return fail(error, "Corrupt emulator ZIP archive.");
    if (matches != 1)
        return fail(error, "Expected emulator NRO not found in archive.");
    return true;
}

bool anyRegularFileAtLeast(const std::string& dir, uint64_t minBytes) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (auto* entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        struct stat st{};
        if (lstat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            static_cast<uint64_t>(st.st_size) >= minBytes) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

bool anyRegularFileWithExt(const std::string& dir, const std::string& ext) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (auto* entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || !endsWithCi(name, ext)) continue;
        struct stat st{};
        if (lstat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

bool anyNamedRegularFile(const std::string& dir, const std::vector<std::string>& names) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (auto* entry = readdir(d)) {
        const std::string name = lowerAscii(entry->d_name);
        bool wanted = false;
        for (const auto& target : names) {
            if (name == lowerAscii(target)) { wanted = true; break; }
        }
        if (!wanted) continue;
        struct stat st{};
        if (lstat((dir + "/" + entry->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_size > 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string canonicalSdPath(const std::string&, const std::string& rel) {
    return "sdmc:/" + rel;
}

bool hasConsoleLaunchExt(const RetroConsoleDef& c, const std::string& ext) {
    return std::find(c.launchExtensions.begin(), c.launchExtensions.end(), ext) != c.launchExtensions.end();
}

} // namespace

const std::vector<RetroEmulatorProfile>& retroEmulatorProfiles() {
    static const std::vector<RetroEmulatorProfile> profiles = [] {
        std::vector<RetroEmulatorProfile> list;
        auto add = [&](RetroEmulatorProfile p) { list.push_back(std::move(p)); };

        // GBA & GBC
        add({"mgba", "mGBA", "mgba-emu", false, {"gba", "gbc"},
             "switch/mGBA/mGBA.nro", {"switch/mGBA/mgba.nro", "switch/mgba/mgba.nro"},
             "mgba.nro", "https://github.com/mgba-emu/mgba/",
             makeGhPkg("mgba", "mGBA", "mgba-emu", "0.10.5", kMgba0105Url,
                       "mGBA-switch.7z", "sdmc:/switch/mGBA/mGBA.nro", "sdmc:/switch/mGBA",
                       true, kMgba0105Bytes, {"gba", "gbc"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "gba", ".gba"});

        add({"pgba", "pGBA", "Cpasjuste", false, {"gba"},
             "switch/pGBA/pgba.nro", {"switch/pgba/pgba.nro"},
             "", "https://github.com/Cpasjuste/pemu/",
             makeGhPkg("pgba", "pGBA", "Cpasjuste", "7.2",
                       "https://github.com/Cpasjuste/pemu/releases/download/v7.2/pgba.nro",
                       "pgba.nro", "sdmc:/switch/pGBA/pgba.nro", "", false, 31144928, {"gba"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "gba", ".gba"});

        add(makeRaProfile("ra_mgba", "mGBA (RetroArch)", "mgba", {"gba", "gbc"}));
        add(makeRaProfile("ra_vba_next", "VBA-Next (RetroArch)", "vba_next", {"gba"}));
        add(makeRaProfile("ra_gambatte", "Gambatte (RetroArch)", "gambatte", {"gbc"}));
        add(makeRaProfile("ra_sameboy", "SameBoy (RetroArch)", "sameboy", {"gbc"}));

        // NES
        add({"pnes", "pNES", "Cpasjuste", false, {"nes"},
             "switch/pNES/pnes.nro", {"switch/pnes/pnes.nro"},
             "", "https://github.com/Cpasjuste/pemu/",
             makeGhPkg("pnes", "pNES", "Cpasjuste", "7.2",
                       "https://github.com/Cpasjuste/pemu/releases/download/v7.2/pnes.nro",
                       "pnes.nro", "sdmc:/switch/pNES/pnes.nro", "", false, 32694687, {"nes"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "nes", ".nes"});
        add(makeRaProfile("ra_fceumm", "FCEUmm (RetroArch)", "fceumm", {"nes"}));
        add(makeRaProfile("ra_nestopia", "Nestopia (RetroArch)", "nestopia", {"nes"}));

        // SNES
        add({"psnes", "pSNES", "Cpasjuste", false, {"snes"},
             "switch/pSNES/psnes.nro", {"switch/psnes/psnes.nro"},
             "", "https://github.com/Cpasjuste/pemu/",
             makeGhPkg("psnes", "pSNES", "Cpasjuste", "7.2",
                       "https://github.com/Cpasjuste/pemu/releases/download/v7.2/psnes.nro",
                       "psnes.nro", "sdmc:/switch/pSNES/psnes.nro", "", false, 32052730, {"snes"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "snes", ".sfc"});
        add(makeRaProfile("ra_snes9x", "Snes9x (RetroArch)", "snes9x", {"snes"}));
        add(makeRaProfile("ra_snes9x2010", "Snes9x 2010 (RetroArch)", "snes9x2010", {"snes"}));

        // N64
        add(makeRaProfile("mupen64plus_next", "Mupen64Plus-Next (RetroArch)", "mupen64plus_next", {"n64"}));

        // NDS
        add({"drasticds", "DraSticDS-nx", "NaGaa95", false, {"nds"},
             "switch/DrasticDS/DrasticDS.nro", {"switch/DrasticDS.nro", "switch/drastic/DrasticDS.nro"},
             "", "https://github.com/NaGaa95/DrasticDS_nx/",
             makeGhPkg("drasticds", "DraSticDS-nx", "NaGaa95", "1.2.1",
                       "https://github.com/NaGaa95/DrasticDS_nx/releases/download/1.2.1/DrasticDS.nro",
                       "DrasticDS.nro", "sdmc:/switch/DrasticDS/DrasticDS.nro", "", false, 106086113, {"nds"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "nds", ".nds"});

        add({"melonds", "melonDS", "ArcDelta", false, {"nds"},
             "switch/melonds/melonDS.nro", {"switch/melonDS/melonDS.nro", "switch/melonds/melonds.nro"},
             "", "https://github.com/ArcDelta/melonDS/",
             makeGhPkg("melonds", "melonDS", "ArcDelta", "7.2.1",
                       "https://github.com/ArcDelta/melonDS/releases/download/7.2.1/melonDS.nro",
                       "melonDS.nro", "sdmc:/switch/melonds/melonDS.nro", "", false, 3252598, {"nds"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "nds", ".nds"});
        add(makeRaProfile("ra_melonds", "melonDS (RetroArch)", "melonds", {"nds"}));

        // 3DS
        add({"dekopon", "Dekopon (Citra)", "PalindromicBreadLoaf", false, {"3ds"},
             "switch/dekopon/dekopon.nro", {"switch/dekopon.nro"},
             "", "https://github.com/PalindromicBreadLoaf/dekopon/",
             makeGhPkg("dekopon", "Dekopon (Citra)", "PalindromicBreadLoaf", "2.2.2",
                       "https://github.com/PalindromicBreadLoaf/dekopon/releases/download/v2.2.2/dekopon.nro",
                       "dekopon.nro", "sdmc:/switch/dekopon/dekopon.nro", "", false, 36948012, {"3ds"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "3ds", ".3ds"});

        // GameCube & Wii
        add({"dolphin", "Dolphin-nx", "NaGaa95", false, {"gamecube", "wii"},
             "switch/dolphin/dolphin.nro", {"switch/dolphin.nro", "switch/dolphin/Dolphin.nro"},
             "", "https://github.com/NaGaa95/dolphin-nx/",
             makeGhPkg("dolphin", "Dolphin-nx", "NaGaa95", "1.2.2",
                       "https://github.com/NaGaa95/dolphin-nx/releases/download/1.2.2/dolphin.nro",
                       "dolphin.nro", "sdmc:/switch/dolphin/dolphin.nro", "", false, 99305732, {"gamecube", "wii"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "gamecube", ".iso"});

        // Wii U
        add({"cemu", "Cemu-nx", "NaGaa95", false, {"wiiu"},
             "switch/cemu/cemu.nro", {"switch/cemu.nro", "switch/Cemu/cemu.nro"},
             "", "https://github.com/NaGaa95/Cemu-nx/",
             makeGhPkg("cemu", "Cemu-nx", "NaGaa95", "1.3.2",
                       "https://github.com/NaGaa95/Cemu-nx/releases/download/1.3.2/cemu.nro",
                       "cemu.nro", "sdmc:/switch/cemu/cemu.nro", "", false, 143340642, {"wiiu"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "wiiu", ".wua"});

        // PS1
        add({"duckstation", "DuckStation", "shooterspps", false, {"ps1"},
             "switch/duckstation/duckstation.nro", {"switch/duckstation/DuckStation.nro"},
             "duckstation.nro", "https://github.com/shooterspps/duckstation/",
             makeGhPkg("duckstation", "DuckStation", "shooterspps", "prerelease-3",
                       "https://github.com/shooterspps/duckstation/releases/download/prerelease-3/duckstation_switch_prerelease_3-9cb25f7-261030.zip",
                       "duckstation_switch.zip", "sdmc:/switch/duckstation/duckstation.nro", "sdmc:/switch/duckstation",
                       true, 10768536, {"ps1"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "ps1", ".chd"});
        add(makeRaProfile("ra_pcsx_rearmed", "PCSX-ReARMed (RetroArch)", "pcsx_rearmed", {"ps1"}));

        // PS2
        add({"nethersx2", "NetherSX2-nx", "NaGaa95", false, {"ps2"},
             "switch/NetherSX2/NetherSX2.nro", {"switch/NetherSX2.nro", "switch/nethersx2/NetherSX2.nro"},
             "", "https://github.com/NaGaa95/NetherSX2_nx/",
             makeGhPkg("nethersx2", "NetherSX2-nx", "NaGaa95", "1.4.1",
                       "https://github.com/NaGaa95/NetherSX2_nx/releases/download/1.4.1/NetherSX2.nro",
                       "NetherSX2.nro", "sdmc:/switch/NetherSX2/NetherSX2.nro", "", false, 104552571, {"ps2"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "ps2", ".iso"});

        // PSP
        add({"ppsspp", "PPSSPP-nx", "NaGaa95", false, {"psp"},
             "switch/ppsspp/PPSSPP.nro", {"switch/PPSSPP.nro", "switch/PPSSPP/PPSSPP.nro", "switch/ppsspp/PPSSPPSDL.nro"},
             "", "https://github.com/NaGaa95/ppsspp-nx/",
             makeGhPkg("ppsspp", "PPSSPP-nx", "NaGaa95", "1.1.1",
                       "https://github.com/NaGaa95/ppsspp-nx/releases/download/1.1.1/PPSSPP.nro",
                       "PPSSPP.nro", "sdmc:/switch/ppsspp/PPSSPP.nro", "", false, 75043110, {"psp"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "psp", ".iso"});
        add(makeRaProfile("ra_ppsspp", "PPSSPP (RetroArch)", "ppsspp", {"psp"}));

        // PS Vita
        add({"vita3k", "Vita3K-nx", "NaGaa95", false, {"psvita"},
             "switch/Vita3K/Vita3K.nro", {"switch/Vita3K.nro", "switch/vita3k/Vita3K.nro"},
             "", "https://github.com/NaGaa95/Vita3K-nx/",
             makeGhPkg("vita3k", "Vita3K-nx", "NaGaa95", "1.4.1",
                       "https://github.com/NaGaa95/Vita3K-nx/releases/download/1.4.1/Vita3K.nro",
                       "Vita3K.nro", "sdmc:/switch/Vita3K/Vita3K.nro", "", false, 90740928, {"psvita"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "psvita", ".vpk"});

        // Sega
        add({"pgen", "pGEN", "Cpasjuste", false, {"sega_md", "sega_ms", "sega_gg", "sega_cd"},
             "switch/pGEN/pgen.nro", {"switch/pgen/pgen.nro"},
             "", "https://github.com/Cpasjuste/pemu/",
             makeGhPkg("pgen", "pGEN", "Cpasjuste", "7.2",
                       "https://github.com/Cpasjuste/pemu/releases/download/v7.2/pgen.nro",
                       "pgen.nro", "sdmc:/switch/pGEN/pgen.nro", "", false, 32539784,
                       {"sega_md", "sega_ms", "sega_gg", "sega_cd"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "sega_md", ".md"});

        add(makeRaProfile("ra_genesis_plus_gx", "Genesis Plus GX (RetroArch)", "genesis_plus_gx",
                          {"sega_md", "sega_ms", "sega_gg", "sega_cd"}));
        add(makeRaProfile("picodrive", "PicoDrive (RetroArch)", "picodrive",
                          {"sega_32x", "sega_md", "sega_ms", "sega_cd"}));
        add(makeRaProfile("ra_gearsystem", "Gearsystem (RetroArch)", "gearsystem",
                          {"sega_ms", "sega_gg"}));

        // Dreamcast
        add({"flycast", "Flycast", "flyinghead", false, {"dreamcast"},
             "switch/flycast/flycast.nro", {"switch/flycast/flycast-2.7.nro", "switch/flycast.nro"},
             "", "https://github.com/flyinghead/flycast/",
             makeGhPkg("flycast", "Flycast", "flyinghead", "2.7",
                       "https://github.com/flyinghead/flycast/releases/download/v2.7/flycast-2.7.nro",
                       "flycast.nro", "sdmc:/switch/flycast/flycast.nro", "", false, 31907635, {"dreamcast"}),
             RetroEmulatorProfile::LaunchMode::PositionalRom, true, "dreamcast", ".chd"});
        add(makeRaProfile("ra_flycast", "Flycast (RetroArch)", "flycast", {"dreamcast"}));

        return list;
    }();
    return profiles;
}

const RetroEmulatorProfile* findRetroEmulatorProfile(const std::string& id) {
    for (const auto& p : retroEmulatorProfiles()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::vector<const RetroEmulatorProfile*> emulatorsForConsole(const std::string& consoleId) {
    std::vector<const RetroEmulatorProfile*> out;
    for (const auto& p : retroEmulatorProfiles()) {
        if (std::find(p.supportedConsoles.begin(), p.supportedConsoles.end(), consoleId) !=
            p.supportedConsoles.end())
            out.push_back(&p);
    }
    return out;
}

const RetroEmulatorProfile& mgbaProfile() {
    return *findRetroEmulatorProfile("mgba");
}

bool isCustomEmulatorPath(const std::string& value) {
    if (value.rfind("sdmc:/", 0) != 0 || value.size() <= 10 || value.size() > 700)
        return false;
    if (!endsWithCi(value, ".nro"))
        return false;
    return value.find_first_of("\"\r\n") == std::string::npos &&
           taskFilePathIsFatCompatible(value.substr(6));
}

bool isValidEmulatorSelection(const std::string& consoleId, const std::string& selection) {
    if (selection.empty()) return true;
    if (isCustomEmulatorPath(selection)) return true;
    const auto* profile = findRetroEmulatorProfile(selection);
    if (!profile) return false;
    if (consoleId.empty()) return true;
    return std::find(profile->supportedConsoles.begin(), profile->supportedConsoles.end(), consoleId) !=
           profile->supportedConsoles.end();
}

RetroBiosStatus checkBiosStatus(const std::string& sdRoot, const std::string& consoleId,
                                const std::string& emulatorId, const std::string& systemDirectory) {
    RetroBiosStatus status;
    const std::string systemPath = systemDirectory.empty() ? "retroarch/cores/system" : systemDirectory;
    const std::string raSys = sdRoot + "/" + systemPath;
    const std::string raSysAlt = systemDirectory.empty() ? sdRoot + "/retroarch/system" : raSys;

    if (consoleId == "ps2") {
        status.required = true;
        status.expectedPath = "sdmc:/switch/nethersx2/bios/";
        status.hintKey = "pipensx/retro/bios_ps2";
        status.present = anyRegularFileAtLeast(sdRoot + "/switch/nethersx2/bios", 2 * 1024 * 1024) ||
                         anyRegularFileAtLeast(sdRoot + "/switch/NetherSX2/bios", 2 * 1024 * 1024);
    } else if (consoleId == "ps1") {
        if (emulatorId == "ra_pcsx_rearmed") {
            status.optional = true;
            status.expectedPath = "sdmc:/" + systemPath + "/";
            status.hintKey = "pipensx/retro/bios_ps1_opt";
            status.present = anyRegularFileAtLeast(raSys, 512 * 1024) ||
                             anyRegularFileAtLeast(raSysAlt, 512 * 1024);
        } else {
            status.required = true;
            status.expectedPath = "sdmc:/switch/duckstation/bios/";
            status.hintKey = "pipensx/retro/bios_ps1";
            status.present = anyRegularFileAtLeast(sdRoot + "/switch/duckstation/bios", 512 * 1024);
        }
    } else if (consoleId == "nds") {
        status.required = true;
        status.hintKey = "pipensx/retro/bios_nds";
        if (emulatorId == "drasticds" || emulatorId.empty()) {
            const std::string dir = sdRoot + "/switch/drastic/system";
            status.expectedPath = "sdmc:/switch/drastic/system/";
            status.present = anyNamedRegularFile(dir, {"nds_bios_arm7.bin", "drastic_bios_arm7.bin"}) &&
                             anyNamedRegularFile(dir, {"nds_bios_arm9.bin", "drastic_bios_arm9.bin"}) &&
                             anyNamedRegularFile(dir, {"nds_firmware.bin", "nds_firmware_modified.bin"});
        } else if (emulatorId == "melonds") {
            const std::string dir = sdRoot + "/switch/melonds";
            const std::string dirAlt = sdRoot + "/switch/melonDS";
            status.expectedPath = "sdmc:/switch/melonds/";
            const auto hasAll = [](const std::string& d) {
                return anyNamedRegularFile(d, {"bios7.bin"}) &&
                       anyNamedRegularFile(d, {"bios9.bin"}) &&
                       anyNamedRegularFile(d, {"firmware.bin"});
            };
            status.present = hasAll(dir) || hasAll(dirAlt);
        } else if (emulatorId == "ra_melonds") {
            status.expectedPath = "sdmc:/" + systemPath + "/";
            const auto hasAll = [](const std::string& d) {
                return anyNamedRegularFile(d, {"bios7.bin"}) &&
                       anyNamedRegularFile(d, {"bios9.bin"}) &&
                       anyNamedRegularFile(d, {"firmware.bin"});
            };
            status.present = hasAll(raSys) || hasAll(raSysAlt);
        } else {
            status.required = false;
            status.present = true;
        }
    } else if (consoleId == "sega_cd") {
        status.required = true;
        const std::vector<std::string> bios = {"bios_CD_U.bin", "bios_CD_E.bin", "bios_CD_J.bin"};
        if (emulatorId == "pgen" || emulatorId.empty()) {
            status.expectedPath = "sdmc:/switch/pGEN/";
            status.present = anyNamedRegularFile(sdRoot + "/switch/pGEN", bios);
        } else {
            status.expectedPath = "sdmc:/" + systemPath + "/";
            status.present = anyNamedRegularFile(raSys, bios) || anyNamedRegularFile(raSysAlt, bios);
        }
        status.hintKey = "pipensx/retro/bios_segacd";
    } else if (consoleId == "dreamcast") {
        status.optional = true;
        status.expectedPath = emulatorId == "ra_flycast" ? "sdmc:/" + systemPath + "/dc/"
                                                         : "sdmc:/flycast/data/";
        status.hintKey = "pipensx/retro/bios_dc";
        status.present = emulatorId == "ra_flycast"
            ? anyNamedRegularFile(raSys + "/dc", {"dc_boot.bin"}) || anyNamedRegularFile(raSysAlt + "/dc", {"dc_boot.bin"})
            : anyNamedRegularFile(sdRoot + "/flycast/data", {"dc_boot.bin"}) ||
              anyNamedRegularFile(sdRoot + "/switch/flycast/data", {"dc_boot.bin"});
    } else if (consoleId == "psvita") {
        status.required = true;
        status.expectedPath = "sdmc:/switch/Vita3K/PSP2UPDAT.PUP";
        status.hintKey = "pipensx/retro/bios_psvita";
        status.present = anyRegularFileWithExt(sdRoot + "/switch/vita3k/vita/vs0/sys/external", ".suprx") ||
                         anyRegularFileWithExt(sdRoot + "/switch/Vita3K/vita/vs0/sys/external", ".suprx");
    } else if (consoleId == "wiiu") {
        status.optional = true;
        status.expectedPath = "sdmc:/switch/cemu/keys.txt";
        status.hintKey = "pipensx/retro/bios_wiiu";
        status.present = anyNamedRegularFile(sdRoot + "/switch/cemu", {"keys.txt"});
    }
    return status;
}

namespace {

bool isValidVitaTitleIdStrict(const std::string& id) {
    if (id.size() != 9)
        return false;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok)
            return false;
    }
    return true;
}

std::string upperAsciiLocal(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - ('a' - 'A'));
    }
    return value;
}

bool dirExistsLocal(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool regularFileExistsLocal(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode);
}

std::string resolveVitaBaseDirRel(const std::string& sdRoot) {
    if (dirExistsLocal(sdRoot + "/switch/Vita3K"))
        return "switch/Vita3K";
    if (dirExistsLocal(sdRoot + "/switch/vita3k"))
        return "switch/vita3k";
    return "switch/Vita3K";
}

} // namespace

std::string findPendingVitaFirmwarePup(const std::string& sdRoot) {
    for (const char* relDir : {"switch/Vita3K", "switch/vita3k"}) {
        const std::string dir = sdRoot + "/" + relDir;
        DIR* d = opendir(dir.c_str());
        if (!d)
            continue;
        std::string found;
        while (auto* ent = readdir(d)) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..")
                continue;
            if (!endsWithCi(name, ".pup"))
                continue;
            const std::string full = dir + "/" + name;
            if (!regularFileExistsLocal(full))
                continue;
            if (lowerAscii(name) == "psp2updat.pup") {
                found = canonicalSdPath(sdRoot, std::string(relDir) + "/" + name);
                break;
            }
            if (found.empty())
                found = canonicalSdPath(sdRoot, std::string(relDir) + "/" + name);
        }
        closedir(d);
        if (!found.empty())
            return found;
    }
    return {};
}

std::string extractVitaTitleIdFromRomPath(const std::string& romPath) {
    if (lowerAscii(baseNameOf(romPath)) != "eboot.bin")
        return {};
    const std::string gameDir = parentDirOf(romPath);
    const std::string rawId = baseNameOf(gameDir);
    if (!isValidVitaTitleIdStrict(rawId))
        return {};
    return upperAsciiLocal(rawId);
}

bool isVitaTitleInstalledDecrypted(const std::string& sdRoot, const std::string& titleId) {
    if (!isValidVitaTitleIdStrict(titleId))
        return false;
    const std::string tid = upperAsciiLocal(titleId);
    for (const char* relDir : {"switch/Vita3K", "switch/vita3k"}) {
        const std::string appDir = sdRoot + "/" + relDir + "/vita/ux0/app/" + tid;
        const std::string eboot = appDir + "/eboot.bin";
        const std::string sfo = appDir + "/sce_sys/param.sfo";
        if (!regularFileExistsLocal(eboot) || !regularFileExistsLocal(sfo))
            continue;
        // PFS-encrypted NoNpDrm folders still carry sce_pfs/ and cannot boot directly.
        if (dirExistsLocal(appDir + "/sce_pfs"))
            return false;
        FILE* f = fopen(eboot.c_str(), "rb");
        if (!f)
            return false;
        unsigned char magic[4] = {};
        const size_t n = fread(magic, 1, sizeof(magic), f);
        fclose(f);
        if (n == 4 && magic[0] == 'S' && magic[1] == 'C' && magic[2] == 'E' && magic[3] == 0)
            return true;
    }
    return false;
}

std::string resolveVitaInstallTarget(const std::string& romPath) {
    if (lowerAscii(baseNameOf(romPath)) == "eboot.bin") {
        const std::string gameDir = parentDirOf(romPath);
        const std::string parent = parentDirOf(gameDir);
        const std::string parentName = lowerAscii(baseNameOf(parent));
        if (parentName == "app" || parentName == "patch" || parentName == "addcont") {
            const std::string container = parentDirOf(parent);
            if (!container.empty() && container != parent)
                return container;
        }
        return gameDir;
    }
    return romPath;
}

bool prepareVita3kInstallLaunch(const std::string& sdRoot, const std::string& targetPath,
                                const std::string& returnNro, std::string& nroOut,
                                std::string& argsOut, std::string& error) {
    error.clear();
    nroOut.clear();
    argsOut.clear();
    if (targetPath.empty() || targetPath.find_first_of("\r\n") != std::string::npos)
        return fail(error, "Invalid Vita3K install target.");
    const std::string baseRel = resolveVitaBaseDirRel(sdRoot);
    const std::string coreRel = baseRel + "/.emu/Vita3K.nro";
    const std::string coreFull = sdRoot + "/" + coreRel;
    if (!regularFileExistsLocal(coreFull))
        return fail(error, "Vita3K core is not initialized yet. Open Vita3K once first.");
    const std::string targetFile = sdRoot + "/" + baseRel + "/install_target.txt";
    const std::string tmpFile = targetFile + ".tmp";
    FILE* f = fopen(tmpFile.c_str(), "wb");
    if (!f)
        return fail(error, "Unable to write Vita3K install_target.txt.");
    const std::string payload = targetPath + "\n";
    const bool wrote = fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed || rename(tmpFile.c_str(), targetFile.c_str()) != 0) {
        unlink(tmpFile.c_str());
        return fail(error, "Unable to write Vita3K install_target.txt.");
    }
    nroOut = canonicalSdPath(sdRoot, coreRel);
    argsOut = "\"" + nroOut + "\" --install";
    if (!returnNro.empty())
        argsOut += " --return \"" + returnNro + "\"";
    return true;
}

std::mutex RetroEmulatorInstaller::mutex_;

RetroEmulatorInstaller::RetroEmulatorInstaller(std::string sdRoot, Fetcher fetcher)
    : sdRoot_(std::move(sdRoot)), fetcher_(fetcher ? std::move(fetcher) : fetch) {}

bool RetroEmulatorInstaller::supports(const RetroEmulatorPackage& package) {
    const RetroEmulatorProfile* profile = findRetroEmulatorProfile(package.id);
    if (!profile) return false;
    if (package.version.empty() || package.version.size() > 128) return false;
    if (package.bytes > kMaxPackageBytes) return false;
    if (package.filename.empty() || package.filename == "." || package.filename == ".." ||
        package.filename.find_first_of("/\\:") != std::string::npos)
        return false;
    if (package.downloadUrl.rfind(profile->allowedUrlPrefix, 0) != 0)
        return false;
    if (profile->expectedArchiveNroName.empty()) {
        if (package.archive || !endsWithCi(package.filename, ".nro"))
            return false;
    } else {
        if (!package.archive ||
            (!endsWithCi(package.filename, ".7z") && !endsWithCi(package.filename, ".zip")))
            return false;
    }
    return true;
}

RetroEmulatorPackage RetroEmulatorInstaller::resolvePackage(
    const std::string& emulatorId,
    const std::vector<RetroEmulatorPackage>& catalogPackages) {
    const auto* profile = findRetroEmulatorProfile(emulatorId);
    for (const auto& pkg : catalogPackages) {
        if (pkg.id == emulatorId && supports(pkg)) {
            RetroEmulatorPackage resolved = pkg;
            if ((profile && profile->isRetroArchCore) ||
                resolved.downloadUrl.find("/latest/") != std::string::npos)
                resolved.bytes = 0;
            return resolved;
        }
    }
    if (profile)
        return profile->fallbackPackage;
    return {};
}

bool RetroEmulatorInstaller::detect(std::string& nroPath, std::string& error) const {
    return detect("mgba", nroPath, error);
}

bool RetroEmulatorInstaller::detect(const std::string& emulatorId, std::string& nroPath,
                                    std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (isCustomEmulatorPath(emulatorId)) {
        const std::string rel = emulatorId.substr(6);
        const std::string full = sdRoot_ + "/" + rel;
        if (!validNro(full, error))
            return fail(error, error.empty() ? "Custom NRO not found." : error);
        nroPath = canonicalSdPath(sdRoot_, rel);
        return true;
    }
    const RetroEmulatorProfile* profile = findRetroEmulatorProfile(emulatorId);
    if (!profile)
        return fail(error, "Unknown emulator profile.");
    std::vector<std::string> candidates = {profile->nroRelativePath};
    candidates.insert(candidates.end(), profile->alternateNroRelativePaths.begin(),
                      profile->alternateNroRelativePaths.end());
    for (const auto& rel : candidates) {
        const std::string full = sdRoot_ + "/" + rel;
        std::string checkErr;
        if (validNro(full, checkErr)) {
            nroPath = canonicalSdPath(sdRoot_, rel);
            return true;
        }
    }
    return fail(error, profile->displayName + " is not installed.");
}

bool RetroEmulatorInstaller::install(const RetroEmulatorPackage& package,
                                     const std::atomic<bool>& cancelled,
                                     const Progress& progress, std::string& error,
                                     bool allowReplace) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (cancelled) return fail(error, "Cancelled.");
    if (!supports(package))
        return fail(error, "Unsupported emulator package; a reviewed local adapter is required.");
    const RetroEmulatorProfile* profile = findRetroEmulatorProfile(package.id);
    const std::string destination = sdRoot_ + "/" + profile->nroRelativePath;
    const std::string destDir = parentDirOf(destination);
    if (!ensureDirectories(destDir))
        return fail(error, "Unable to create emulator directory.");
    if (!allowReplace) {
        if (!absent(destination))
            return fail(error, "An emulator NRO already exists; replace was not requested.");
        for (const auto& alt : profile->alternateNroRelativePaths) {
            if (!absent(sdRoot_ + "/" + alt))
                return fail(error, "An emulator NRO already exists; replace was not requested.");
        }
    } else {
        struct stat st{};
        if (lstat(destination.c_str(), &st) == 0 && !S_ISREG(st.st_mode))
            return fail(error, "Existing emulator destination is not a regular file.");
    }

    const std::string stage = destDir + "/.pipensx-install";
    if (!prepareStage(stage)) return fail(error, "Unable to prepare emulator staging directory.");
    struct Cleanup {
        std::string path;
        ~Cleanup() { removeStage(path); }
    } cleanup{stage};

    const uint64_t needBytes = (package.bytes > 0 ? package.bytes : 64ULL * 1024 * 1024) +
                               (package.archive ? 128ULL * 1024 * 1024 : 0) + 1024 * 1024;
    struct statvfs storage{};
    if (statvfs(sdRoot_.c_str(), &storage) != 0 ||
        uint64_t(storage.f_bavail) * storage.f_frsize < needBytes)
        return fail(error, "Insufficient free space for emulator installation.");

    const bool dynamicNightly = (profile && profile->isRetroArchCore) ||
                                package.downloadUrl.find("/latest/") != std::string::npos;
    const uint64_t expectedBytes = dynamicNightly ? 0 : package.bytes;

    const std::string downloadTarget = stage + (package.archive ? "/package" : "/package.nro");
    FILE* output = fopen(downloadTarget.c_str(), "wb");
    if (!output) return fail(error, "Unable to create emulator download.");
    sha256_ctx_t hash;
    sha256_init(&hash);
    uint64_t received = 0;
    const uint64_t maxReceive = expectedBytes > 0 ? expectedBytes : kMaxPackageBytes;
    const auto sink = [&](const uint8_t* data, size_t size) {
        if (cancelled || size > maxReceive - received || fwrite(data, 1, size, output) != size)
            return false;
        sha256_update(&hash, data, size);
        received += size;
        return true;
    };
    bool ok = fetcher_(package.downloadUrl, expectedBytes, sink, cancelled, progress, error);
    if (fflush(output) != 0 || fsync(fileno(output)) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (cancelled) return fail(error, "Cancelled.");
    if (!ok) return fail(error, error.empty() ? "Unable to download emulator package." : error);

    if (expectedBytes > 0 && received != expectedBytes)
        return fail(error, "Emulator package size mismatch.");
    if (package.id == "mgba" && package.version == "0.10.5" && package.downloadUrl == kMgba0105Url) {
        uint8_t digest[32];
        sha256_final(&hash, digest);
        std::string actual;
        for (auto byte : digest) {
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x", byte);
            actual += hex;
        }
        if (actual != kMgba0105Sha256)
            return fail(error, "mGBA SHA-256 mismatch.");
    }

    std::string stagedNro;
    if (!package.archive) {
        stagedNro = downloadTarget;
    } else if (endsWithCi(package.filename, ".zip")) {
        stagedNro = stage + "/extracted.nro";
        if (!extractZipTargetNro(downloadTarget, profile->expectedArchiveNroName, stagedNro,
                                 cancelled, error))
            return false;
    } else {
        PortArchiveProbe probe;
        if (!probePortArchive(downloadTarget, probe, "/payload") ||
            probe.unpackBytes > kMaxUnpackedBytes ||
            probe.maxSolidBlockBytes > kMaxUnpackedBytes ||
            probe.sourceFiles.size() > 256)
            return fail(error, "Unexpected emulator 7z layout.");
        std::string matchedSource;
        size_t matchCount = 0;
        const std::string want = lowerAscii(profile->expectedArchiveNroName);
        for (const auto& s : probe.sourceFiles) {
            if (lowerAscii(baseNameOf(s)) == want) {
                matchedSource = s;
                ++matchCount;
            }
        }
        if (matchCount != 1)
            return fail(error, "Unexpected emulator 7z NRO count.");
        if (!extractPortArchive(downloadTarget, stage + "/switch", stage, cancelled, {}, {}, error, {},
                                "/payload"))
            return false;
        stagedNro = stage + "/payload/" + matchedSource;
    }

    if (!validNro(stagedNro, error)) return false;
    if (cancelled) return fail(error, "Cancelled.");
    if (!allowReplace) {
        if (!absent(destination))
            return fail(error, "An emulator NRO appeared during installation.");
        for (const auto& alt : profile->alternateNroRelativePaths) {
            if (!absent(sdRoot_ + "/" + alt))
                return fail(error, "An emulator NRO appeared during installation.");
        }
    }
    FILE* ready = fopen(stagedNro.c_str(), "r+b");
    if (!ready) return fail(error, "Unable to flush staged emulator NRO.");
    const bool synced = fsync(fileno(ready)) == 0;
    const bool closed = fclose(ready) == 0;
    if (!synced || !closed) return fail(error, "Unable to flush staged emulator NRO.");

#ifdef __SWITCH__
    if (allowReplace && !absent(destination)) {
        if (unlink(destination.c_str()) != 0)
            return fail(error, "Unable to replace existing emulator NRO.");
    }
    if (rename(stagedNro.c_str(), destination.c_str()) != 0)
        return fail(error, "Unable to publish emulator NRO.");
#else
    if (allowReplace && !absent(destination)) {
        if (rename(stagedNro.c_str(), destination.c_str()) != 0)
            return fail(error, "Unable to replace emulator NRO.");
    } else if (link(stagedNro.c_str(), destination.c_str()) != 0) {
        return fail(error, "Unable to publish emulator NRO.");
    }
#endif
    if (allowReplace) {
        const std::string primaryLower = lowerAscii(profile->nroRelativePath);
        for (const auto& alt : profile->alternateNroRelativePaths) {
            if (lowerAscii(alt) == primaryLower)
                continue;
            const std::string altFull = sdRoot_ + "/" + alt;
            struct stat stAlt{};
            if (lstat(altFull.c_str(), &stAlt) == 0 && S_ISREG(stAlt.st_mode))
                unlink(altFull.c_str());
        }
    }
    return true;
}

bool RetroEmulatorInstaller::uninstall(const std::string& emulatorId, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    const RetroEmulatorProfile* profile = findRetroEmulatorProfile(emulatorId);
    if (!profile) return fail(error, "Unknown emulator profile.");
    std::vector<std::string> candidates = {profile->nroRelativePath};
    candidates.insert(candidates.end(), profile->alternateNroRelativePaths.begin(),
                      profile->alternateNroRelativePaths.end());
    bool removed = false;
    for (const auto& rel : candidates) {
        const std::string full = sdRoot_ + "/" + rel;
        struct stat st{};
        if (lstat(full.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            return fail(error, "Unable to inspect emulator NRO.");
        }
        if (!S_ISREG(st.st_mode))
            return fail(error, "Emulator NRO path is not a regular file.");
        if (unlink(full.c_str()) != 0)
            return fail(error, "Unable to remove emulator NRO.");
        removed = true;
    }
    if (!removed) return fail(error, profile->displayName + " is not installed.");
    return true;
}

bool RetroEmulatorInstaller::launchArguments(const std::string& nroPath, const std::string& romPath,
                                             std::string& arguments, std::string& error) {
    return launchArguments("", nroPath, romPath, arguments, error);
}

bool RetroEmulatorInstaller::launchArguments(const std::string& emulatorId, const std::string& nroPath,
                                             const std::string& romPath, std::string& arguments,
                                             std::string& error) {
    error.clear();
    const auto safe = [](const std::string& path) {
        return path.rfind("sdmc:/", 0) == 0 && path.size() <= 768 &&
               path.find_first_of("\"\r\n") == std::string::npos &&
               taskFilePathIsFatCompatible(path.substr(6));
    };
    if (!safe(nroPath) || !endsWithCi(nroPath, ".nro"))
        return fail(error, "Invalid emulator NRO launch path.");
    const RetroEmulatorProfile* profile = findRetroEmulatorProfile(emulatorId);
    // RetroArch on Switch defines SD_PREFIX = "" and its path_is_absolute() does not
    // recognize "sdmc:/", so passing sdmc:/ breaks .m3u/.cue relative resolution and history.
    const bool stripSdmcPrefix = (profile && profile->isRetroArchCore) ||
                                 nroPath.rfind("sdmc:/retroarch/cores/", 0) == 0 ||
                                 endsWithCi(nroPath, "_libretro_libnx.nro");
    auto formatArg = [stripSdmcPrefix](const std::string& p) {
        return (stripSdmcPrefix && p.rfind("sdmc:/", 0) == 0) ? p.substr(5) : p;
    };
    if (profile && profile->launchMode == RetroEmulatorProfile::LaunchMode::NoRomOpenEmulator) {
        arguments = "\"" + formatArg(nroPath) + "\"";
        return true;
    }
    if (profile && profile->id == "vita3k") {
        if (!safe(romPath))
            return fail(error, "Invalid ROM launch path.");
        if (lowerAscii(baseNameOf(romPath)) == "eboot.bin") {
            const std::string gameDir = parentDirOf(romPath);
            const std::string titleId = baseNameOf(gameDir);
            if (!titleId.empty()) {
                arguments = "\"" + formatArg(nroPath) + "\" -g \"" + titleId + "\"";
                return true;
            }
        }
        const size_t dot = romPath.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string() : lowerAscii(romPath.substr(dot));
        if (ext == ".vpk" || ext == ".zip") {
            arguments = "\"" + formatArg(nroPath) + "\"";
            return true;
        }
        return fail(error, "Unsupported ROM extension for selected emulator.");
    }
    const size_t dot = romPath.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string() : lowerAscii(romPath.substr(dot));
    const bool isVitaEboot =
        lowerAscii(baseNameOf(romPath)) == "eboot.bin" &&
        ((profile && std::find(profile->supportedConsoles.begin(), profile->supportedConsoles.end(), "psvita") !=
                         profile->supportedConsoles.end()) ||
         isCustomEmulatorPath(emulatorId));
    if (!safe(romPath) || (extension.empty() && !isVitaEboot))
        return fail(error, "Invalid ROM launch path.");
    bool extValid = isVitaEboot;
    if (!extValid && profile) {
        for (const auto& cid : profile->supportedConsoles) {
            if (const auto* console = findRetroConsole(cid)) {
                if (hasConsoleLaunchExt(*console, extension)) { extValid = true; break; }
            }
        }
    } else if (!extValid && emulatorId.empty()) {
        // Legacy 3-arg call with mGBA NRO restricts to GBA; custom NROs accept any known console ext
        if (nroPath == "sdmc:/" + mgbaProfile().nroRelativePath) {
            extValid = (extension == ".gba" || extension == ".agb");
        } else {
            for (const auto& console : retroConsoles()) {
                if (hasConsoleLaunchExt(console, extension)) { extValid = true; break; }
            }
        }
    } else if (!extValid && isCustomEmulatorPath(emulatorId)) {
        for (const auto& console : retroConsoles()) {
            if (hasConsoleLaunchExt(console, extension)) { extValid = true; break; }
        }
    }
    if (!extValid)
        return fail(error, "Unsupported ROM extension for selected emulator.");
    arguments = "\"" + formatArg(nroPath) + "\" \"" + formatArg(romPath) + "\"";
    return true;
}

} // namespace pipensx
