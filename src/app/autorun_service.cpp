#include "autorun_service.hpp"
#include "core/sha256.h"
extern "C" {
#include "core/util.h"
}
#include "curl_https.hpp"
#include "download_manager.hpp"
#include "install/install_backend.hpp"
#include "port_selection.hpp"
#include "task_files.hpp"
#include "update_service.hpp"
#ifdef __SWITCH__
#include <switch.h>
#include <borealis/extern/nanovg/stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../vendor/borealis/library/lib/extern/glfw/deps/stb_image_write.h"
#include "../../vendor/autorun-forwarder/builder.h"
#endif

#include <borealis/extern/nlohmann/json.hpp>
#include <curl/curl.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <queue>

namespace pipensx {
namespace {

constexpr size_t kMetadataLimit = 512 * 1024;
constexpr size_t kRuntimeArchiveLimit = 512ull * 1024 * 1024; // 512 MB max

std::string cleanRoot(const std::string& path) {
    std::string root = path;
    while (root.size() > 1 && root.back() == '/') {
        if (root.size() == 6 && root.rfind("sdmc:/", 0) == 0)
            break;
        root.pop_back();
    }
    return root;
}

bool makeDirectories(const std::string& path) {
    char buffer[1024];
    if (path.empty() || path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    char* start = buffer + 1;
    char* colon = std::strchr(buffer, ':');
    if (colon && colon[1] == '/')
        start = colon + 2;
    if (*start == '\0') {
        struct stat st {};
        return stat(buffer, &st) == 0 && S_ISDIR(st.st_mode);
    }
    size_t len = path.size();
    while (len > 1 && buffer[len - 1] == '/' && (len < 2 || buffer[len - 2] != ':')) {
        buffer[--len] = '\0';
    }
    for (char* cursor = start; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *cursor = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

uint16_t readU16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct HttpBuffer {
    std::string data;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeString(char* bytes, size_t size, size_t count, void* opaque) {
    auto* buffer = static_cast<HttpBuffer*>(opaque);
    const size_t received = size * count;
    if (received > buffer->limit - std::min(buffer->limit, buffer->data.size())) {
        buffer->overflow = true;
        return 0;
    }
    buffer->data.append(bytes, received);
    return received;
}

struct FileWriter {
    std::ofstream output;
    size_t written = 0;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeFileCallback(char* bytes, size_t size, size_t count, void* opaque) {
    auto* writer = static_cast<FileWriter*>(opaque);
    const size_t received = size * count;
    if (received > writer->limit - std::min(writer->limit, writer->written)) {
        writer->overflow = true;
        return 0;
    }
    writer->output.write(bytes, static_cast<std::streamsize>(received));
    if (!writer->output.good())
        return 0;
    writer->written += received;
    return received;
}

bool defaultFetchText(const std::string& url, size_t limit, std::string& body,
                      std::string& error) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Failed to initialize curl.";
        return false;
    }
    curlPinHttpsOnly(curl);
    curlApplyTrustedSsl(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx-autorun/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    HttpBuffer buffer;
    buffer.limit = limit;
    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (buffer.overflow) {
        error = "Response exceeded maximum size limit.";
        return false;
    }
    if (res != CURLE_OK) {
        error = std::string("Network request failed: ") + curl_easy_strerror(res);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "Server returned HTTP " + std::to_string(status);
        return false;
    }
    body = std::move(buffer.data);
    return true;
}

bool fetchFileWithProgress(const std::string& url, const std::string& path,
                      size_t limit, std::string& error,
                      const std::atomic<bool>* cancelled,
                      const AutorunService::ProgressCallback& progress) {
    FileWriter writer;
    writer.output.open(path, std::ios::binary | std::ios::trunc);
    writer.limit = limit;
    if (!writer.output) {
        error = "Unable to create destination file: " + path;
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Failed to initialize curl.";
        unlink(path.c_str());
        return false;
    }
    curlPinHttpsOnly(curl);
    curlApplyTrustedSsl(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx-autorun/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);

    struct Transfer {
        const std::atomic<bool>* cancelled;
        const AutorunService::ProgressCallback* progress;
    } transfer{cancelled, &progress};
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t) -> int {
            const auto& transfer = *static_cast<Transfer*>(context);
            if (transfer.cancelled && transfer.cancelled->load()) return 1;
            if (*transfer.progress) (*transfer.progress)(current, total);
            return 0;
        });
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    writer.output.close();

    if (writer.overflow) {
        unlink(path.c_str());
        error = "Download exceeded maximum size limit.";
        return false;
    }
    if (res != CURLE_OK) {
        unlink(path.c_str());
        error = std::string("Download failed: ") + curl_easy_strerror(res);
        return false;
    }
    if (status < 200 || status >= 300) {
        unlink(path.c_str());
        error = "Server returned HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool defaultFetchFile(const std::string& url, const std::string& path,
                      size_t limit, std::string& error) {
    return fetchFileWithProgress(url, path, limit, error, nullptr, {});
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string fileNamePart(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool parseSemVer(const std::string& text, std::array<uint64_t, 3>& version) {
    std::string value = text;
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V'))
        value.erase(value.begin());
    size_t start = 0;
    for (size_t index = 0; index < version.size(); ++index) {
        size_t end = value.find('.', start);
        if ((index + 1 == version.size()) != (end == std::string::npos))
            return false;
        const std::string part = value.substr(start, end - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(),
            [](unsigned char c) { return std::isdigit(c); }))
            return false;
        try {
            version[index] = std::stoull(part);
        } catch (...) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool isNewerRelease(const std::string& candidate, const std::string& current) {
    if (candidate.empty() || candidate == current)
        return false;
    std::array<uint64_t, 3> candParts{};
    std::array<uint64_t, 3> currParts{};
    if (parseSemVer(candidate, candParts) && parseSemVer(current, currParts))
        return candParts > currParts;
    return candidate != current;
}

} // namespace

std::string autorunWineRoot(const std::string& sdRoot) {
    return cleanRoot(sdRoot) + "/switch/wine";
}

std::string autorunRuntimeNroPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/wine-nx-runtime.nro";
}

std::string autorunGamesPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/drive_c/Games";
}

std::string autorunDesktopPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/drive_c/users/wine/Desktop";
}

std::string autorunDocumentsPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/drive_c/users/wine/Documents";
}

std::string autorunRegistryPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/registry/user.reg";
}

std::string autorunSystemRegistryPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/registry/system.reg";
}

std::string autorunVersionPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/version.json";
}

std::string autorunForwardersPath(const std::string& sdRoot) {
    return autorunWineRoot(sdRoot) + "/forwarders";
}

static std::string findExecutableRelativePath(const std::string& baseDir,
                                             const std::string& targetExe) {
    struct stat direct{};
    if (!targetExe.empty() && stat((baseDir + "/" + targetExe).c_str(), &direct) == 0 &&
        S_ISREG(direct.st_mode))
        return targetExe;
    std::string exeName = targetExe;
    size_t slash = exeName.find_last_of("/\\");
    if (slash != std::string::npos)
        exeName = exeName.substr(slash + 1);
    std::string exeLower = lowerAscii(exeName);

    std::queue<std::string> dirs;
    dirs.push("");
    while (!dirs.empty()) {
        std::string rel = dirs.front();
        dirs.pop();
        std::string full = rel.empty() ? baseDir : (baseDir + "/" + rel);
        DIR* dir = opendir(full.c_str());
        if (!dir) continue;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            std::string childRel = rel.empty() ? entry->d_name : (rel + "/" + entry->d_name);
            std::string childFull = baseDir + "/" + childRel;
            struct stat st{};
            if (stat(childFull.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    int depth = 0;
                    for (char c : childRel) if (c == '/') ++depth;
                    if (depth <= 2)
                        dirs.push(childRel);
                } else if (S_ISREG(st.st_mode)) {
                    if (lowerAscii(entry->d_name) == exeLower) {
                        closedir(dir);
                        return childRel;
                    }
                }
            }
        }
        closedir(dir);
    }
    return {};
}

std::string resolveAutorunExecutable(const std::string& sdRoot,
                                     const std::string& gameFolder,
                                     const std::string& expectedExe) {
    const std::string baseDir = autorunGamesPath(sdRoot) + "/" + gameFolder;
    return findExecutableRelativePath(baseDir, expectedExe);
}

bool registerGameInLauncherLibrary(const std::string& sdRoot,
                                   const std::string& exeFullPath,
                                   const std::string& coverPath) {
    const std::string iniPath = autorunWineRoot(sdRoot) + "/launcher-library-v2.ini";
    std::string content;
    std::ifstream in(iniPath);
    if (in) {
        content.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
        in.close();
    }

    // Remove any corrupted [game 0] section from previous buggy writes
    size_t g0 = content.find("[game 0]");
    if (g0 != std::string::npos) {
        size_t nextSect = content.find("\n[", g0 + 1);
        if (nextSect != std::string::npos)
            content.erase(g0, nextSect - g0);
        else
            content.erase(g0);
    }

    if (content.find("path=" + exeFullPath) != std::string::npos) {
        return true;
    }

    // AutoRun indexes games starting at 1: [game 1], [game 2], etc.
    // [game 0] causes "The game library is unreadable" error.
    uint32_t nextGameIndex = 1;
    while (content.find("[game " + std::to_string(nextGameIndex) + "]") != std::string::npos) {
        ++nextGameIndex;
    }

    std::ostringstream ss;
    if (content.empty()) {
        ss << "version=3\n";
    } else {
        ss << content;
        if (!content.empty() && content.back() != '\n')
            ss << "\n";
    }

    ss << "\n[game " << nextGameIndex << "]\n";
    ss << "path=" << exeFullPath << "\n";
    ss << "added-order=" << nextGameIndex << "\n";
    ss << "launched-order=0\n";
    ss << "favorite=0\n";
    if (!coverPath.empty()) {
        ss << "portrait-art=" << coverPath << "\n";
    }

    std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    std::string outStr = ss.str();
    out.write(outStr.data(), static_cast<std::streamsize>(outStr.size()));
    return true;
}

std::string autorunForwarderConfigPath(const std::string& sdRoot, uint64_t titleId) {
    return autorunForwardersPath(sdRoot) + "/" + autorunTitleIdToString(titleId) + ".ini";
}

std::string autorunSysClkConfigPath(const std::string& sdRoot) {
    return cleanRoot(sdRoot) + "/config/sys-clk/config.ini";
}

bool isAutorunInstalled(const std::string& sdRoot) {
    const std::string nro = autorunRuntimeNroPath(sdRoot);
    return access(nro.c_str(), F_OK) == 0;
}

std::string installedAutorunVersion(const std::string& sdRoot) {
    const std::string vpath = autorunVersionPath(sdRoot);
    std::ifstream in(vpath, std::ios::binary);
    if (in) {
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        auto j = nlohmann::json::parse(content, nullptr, false);
        if (!j.is_discarded() && j.is_object() && j.contains("version") &&
            j["version"].is_string()) {
            return j["version"].get<std::string>();
        }
    }
    if (isAutorunInstalled(sdRoot)) {
        return "custom";
    }
    return {};
}

bool prepareAutorunEnvironment(const std::string& sdRoot, std::string& error) {
    const std::string wineRoot = autorunWineRoot(sdRoot);
    const std::string gamesDir = autorunGamesPath(sdRoot);
    const std::string desktopDir = autorunDesktopPath(sdRoot);
    const std::string docsDir = autorunDocumentsPath(sdRoot);
    const std::string regDir = wineRoot + "/registry";

    for (const auto& dir : {wineRoot, gamesDir, desktopDir, docsDir, regDir}) {
        if (!makeDirectories(dir)) {
            error = "Failed to create directory: " + dir;
            return false;
        }
    }

    const std::string userReg = autorunRegistryPath(sdRoot);
    struct stat st {};
    if (stat(userReg.c_str(), &st) != 0) {
        std::ofstream out(userReg, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Failed to initialize user.reg";
            return false;
        }
        out << "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\n\n";
    }

    const std::string sysReg = autorunSystemRegistryPath(sdRoot);
    if (stat(sysReg.c_str(), &st) != 0) {
        std::ofstream out(sysReg, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Failed to initialize system.reg";
            return false;
        }
        out << "WINE REGISTRY Version 2\n;; All keys relative to \\\\System\n\n";
    }

    return true;
}

AutorunValidation validateAutorunFilePaths(
    const std::vector<std::string>& filePaths,
    const std::string& expectedExecutable) {
    AutorunValidation val;
    val.result = AutorunValidationResult::ValidPortable;

    std::vector<std::string> diskImages;
    std::vector<std::string> installers;
    std::vector<std::string> executables;

    std::string expectedLower = lowerAscii(expectedExecutable);

    for (const auto& path : filePaths) {
        std::string fname = lowerAscii(fileNamePart(path));

        if (endsWith(fname, ".iso") || endsWith(fname, ".img") ||
            endsWith(fname, ".mdf") || endsWith(fname, ".nrg") ||
            endsWith(fname, ".cue") || endsWith(fname, ".vhd")) {
            diskImages.push_back(path);
        } else if (endsWith(fname, ".bin")) {
            if (fname.find("disc") != std::string::npos ||
                fname.find("cd") != std::string::npos ||
                fname.find("track") != std::string::npos) {
                diskImages.push_back(path);
            }
        }

        if (fname == "setup.exe" || fname == "install.exe" ||
            fname == "installer.exe" || fname == "autorun.exe" ||
            fname == "unins000.exe") {
            installers.push_back(path);
        }

        if (endsWith(fname, ".exe")) {
            executables.push_back(fileNamePart(path));
            if (!expectedLower.empty() && fname == expectedLower) {
                val.mainExecutable = fileNamePart(path);
            }
        }
    }

    if (!diskImages.empty()) {
        val.result = AutorunValidationResult::DiskImageDetected;
        val.suspiciousFiles = std::move(diskImages);
        val.warning = "This release contains disk image files ('" +
                      fileNamePart(val.suspiciousFiles.front()) +
                      "'). AutoRun requires a pre-installed portable Windows game folder.";
        return val;
    }

    if (!installers.empty()) {
        bool hasNonInstallerExe = false;
        for (const auto& exe : executables) {
            std::string lexe = lowerAscii(exe);
            if (lexe != "setup.exe" && lexe != "install.exe" &&
                lexe != "installer.exe" && lexe != "autorun.exe" &&
                lexe != "unins000.exe") {
                hasNonInstallerExe = true;
                if (val.mainExecutable.empty())
                    val.mainExecutable = exe;
                break;
            }
        }
        if (!hasNonInstallerExe) {
            val.result = AutorunValidationResult::InstallerDetected;
            val.suspiciousFiles = std::move(installers);
            val.warning = "This release appears to be an installer ('" +
                          fileNamePart(val.suspiciousFiles.front()) +
                          "'). AutoRun requires a pre-installed portable Windows game folder.";
            return val;
        }
    }

    if (executables.empty()) {
        val.result = AutorunValidationResult::NoExecutable;
        val.warning = "No Windows executable (.exe) found in this release.";
        return val;
    }

    if (val.mainExecutable.empty()) {
        val.mainExecutable = executables.front();
    }

    return val;
}

AutorunValidation validateAutorunRelease(
    const TorrentPreview& preview,
    const std::string& expectedExecutable) {
    std::vector<std::string> paths;
    paths.reserve(preview.files.size());
    for (const auto& file : preview.files) {
        paths.push_back(torrentLogicalPath(preview, file));
    }
    return validateAutorunFilePaths(paths, expectedExecutable);
}

bool mergeUserRegistry(const std::string& userRegPath,
                       const std::string& registryTweak,
                       std::string& error) {
    std::string trimmed = registryTweak;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return true;
    trimmed = trimmed.substr(start);
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (end != std::string::npos)
        trimmed = trimmed.substr(0, end + 1);

    if (trimmed.empty())
        return true;

    std::string firstHeader;
    std::istringstream stream(trimmed);
    std::string line;
    while (std::getline(stream, line)) {
        size_t lstart = line.find_first_not_of(" \t\r");
        if (lstart != std::string::npos && line[lstart] == '[') {
            size_t lend = line.find(']', lstart);
            if (lend != std::string::npos) {
                firstHeader = line.substr(lstart, lend - lstart + 1);
                break;
            }
        }
    }

    struct stat st {};
    if (stat(userRegPath.c_str(), &st) != 0) {
        size_t slash = userRegPath.find_last_of('/');
        if (slash != std::string::npos) {
            makeDirectories(userRegPath.substr(0, slash));
        }
        std::ofstream out(userRegPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Unable to create user.reg at: " + userRegPath;
            return false;
        }
        out << "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\n\n";
        out << trimmed << "\n";
        return true;
    }

    std::ifstream in(userRegPath, std::ios::binary);
    if (!in) {
        error = "Unable to read user.reg at: " + userRegPath;
        return false;
    }
    std::string existing((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();

    if (!firstHeader.empty() && existing.find(firstHeader) != std::string::npos) {
        return true;
    }

    std::ofstream appendOut(userRegPath, std::ios::binary | std::ios::app);
    if (!appendOut) {
        error = "Unable to append to user.reg at: " + userRegPath;
        return false;
    }
    appendOut << "\n" << trimmed << "\n";
    return true;
}

std::vector<uint8_t> generateShellLink(
    const std::string& targetWindowsPath,
    const std::string& workingDirWindowsPath) {
    std::vector<uint8_t> data;
    data.reserve(512);

    auto append16 = [&data](uint16_t v) {
        data.push_back(static_cast<uint8_t>(v & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto append32 = [&data](uint32_t v) {
        data.push_back(static_cast<uint8_t>(v & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    // 1. ShellLinkHeader (76 bytes)
    append32(0x0000004C); // HeaderSize (76)
    const uint8_t clsid[16] = {
        0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    data.insert(data.end(), clsid, clsid + 16);

    // LinkFlags: HasLinkInfo (0x02) | HasWorkingDir (0x10) | IsUnicode (0x80)
    append32(0x00000092);
    // FileAttributes: FILE_ATTRIBUTE_ARCHIVE (0x20)
    append32(0x00000020);
    // CreationTime, AccessTime, WriteTime
    append32(0); append32(0);
    append32(0); append32(0);
    append32(0); append32(0);
    // FileSize, IconIndex
    append32(0);
    append32(0);
    // ShowCommand: SW_SHOWNORMAL (1)
    append32(0x00000001);
    // HotKey, Reserved1, Reserved2, Reserved3
    append16(0);
    append16(0);
    append32(0);
    append32(0);

    // 2. LinkInfo
    std::string targetWin = targetWindowsPath;
    for (char& c : targetWin) {
        if (c == '/') c = '\\';
    }

    uint32_t headerSize = 0x1C;
    uint32_t volumeIdSize = 0x10;
    uint32_t volumeIdOffset = headerSize;
    uint32_t localBasePathOffset = volumeIdOffset + volumeIdSize + 1;
    uint32_t commonPathSuffixOffset =
        localBasePathOffset + static_cast<uint32_t>(targetWin.size()) + 1;
    uint32_t linkInfoSize = commonPathSuffixOffset + 1;

    append32(linkInfoSize);
    append32(headerSize);
    append32(0x00000001); // VolumeIDAndLocalBasePath
    append32(volumeIdOffset);
    append32(localBasePathOffset);
    append32(0x00000000); // CommonNetworkRelativeLinkOffset
    append32(commonPathSuffixOffset);

    // VolumeID structure (offset 28)
    append32(volumeIdSize);
    append32(3); // DRIVE_FIXED
    append32(0x12345678); // SerialNumber
    append32(0x00000010); // VolumeLabelOffset
    data.push_back('\0'); // VolumeLabel (empty)

    // LocalBasePath
    for (char c : targetWin)
        data.push_back(static_cast<uint8_t>(c));
    data.push_back('\0');

    // CommonPathSuffix
    data.push_back('\0');

    // 3. StringData (WorkingDir)
    std::string workWin = workingDirWindowsPath;
    for (char& c : workWin) {
        if (c == '/') c = '\\';
    }
    append16(static_cast<uint16_t>(workWin.size()));
    for (char c : workWin) {
        append16(static_cast<uint16_t>(static_cast<uint8_t>(c)));
    }

    // 4. Terminal block
    append32(0x00000000);

    return data;
}

bool createWineDesktopShortcut(const std::string& desktopPath,
                               const std::string& shortcutName,
                               const std::string& targetFolder,
                               const std::string& executable,
                               std::string& error) {
    if (desktopPath.empty() || shortcutName.empty() || targetFolder.empty() || executable.empty()) {
        error = "Invalid parameters for desktop shortcut.";
        return false;
    }
    if (!makeDirectories(desktopPath)) {
        error = "Unable to create Wine desktop directory: " + desktopPath;
        return false;
    }
    std::string targetWin = "C:\\Games\\" + targetFolder + "\\" + executable;
    std::string workWin = "C:\\Games\\" + targetFolder;
    const auto exeDir = executable.find_last_of("/\\");
    if (exeDir != std::string::npos)
        workWin += "\\" + executable.substr(0, exeDir);
    std::vector<uint8_t> linkBytes = generateShellLink(targetWin, workWin);
    std::string filePath = desktopPath + "/" + shortcutName + ".lnk";
    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Unable to create shortcut file: " + filePath;
        return false;
    }
    out.write(reinterpret_cast<const char*>(linkBytes.data()),
              static_cast<std::streamsize>(linkBytes.size()));
    if (!out.good()) {
        error = "Failed to write shortcut file: " + filePath;
        return false;
    }
    return true;
}

namespace {

bool seedAutorunSettingsFile(const std::string& path,
                            const std::vector<std::pair<std::string, std::string>>& defaults,
                            std::string& error) {
    if (defaults.empty()) return true;
    std::ifstream in(path, std::ios::binary);
    if (!in && access(path.c_str(), F_OK) == 0) {
        error = "Unable to read AutoRun program settings: " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), {});
    bool changed = false;
    for (const auto& setting : defaults) {
        bool present = false;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = lowerAscii(line.substr(0, eq));
            const auto begin = key.find_first_not_of(" \t");
            const auto end = key.find_last_not_of(" \t");
            if (begin == std::string::npos) continue;
            key = key.substr(begin, end - begin + 1);
            if (key == lowerAscii(setting.first) || (setting.first == "d3d" && key == "d3d9")) {
                present = true;
                break;
            }
        }
        // Seed recommendations once; retain the user's per-game choices.
        if (!present) {
            if (!text.empty() && text.back() != '\n') text += '\n';
            text += setting.first + "=" + setting.second + "\n";
            changed = true;
        }
    }
    if (!changed) return true;
    const auto temp = path + ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out || std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        error = "Unable to write AutoRun program settings: " + path;
        return false;
    }
    return true;
}

bool seedAutorunProgramSettings(const std::string& executablePath,
                               const AutorunProfile& profile, std::string& error) {
    std::vector<std::pair<std::string, std::string>> defaults;
    const auto cpu = lowerAscii(profile.translator);
    if (cpu == "box64" || cpu == "fex")
        defaults.emplace_back("cpu", cpu);
    const auto renderer = lowerAscii(profile.renderer);
    if (renderer == "dxvk" || renderer == "dxvk+vkd3d")
        defaults.emplace_back("d3d", renderer);
    else if (renderer == "wine" || renderer == "wined3d")
        defaults.emplace_back("d3d", "wine");
    if (executablePath.size() < 4 ||
        lowerAscii(executablePath.substr(executablePath.size() - 4)) != ".exe") {
        error = "AutoRun target is not an EXE: " + executablePath;
        return false;
    }
    const auto stem = executablePath.substr(0, executablePath.size() - 4);
    if (!seedAutorunSettingsFile(stem + ".wine-nx.txt", defaults, error))
        return false;
    std::vector<std::pair<std::string, std::string>> box64;
    std::istringstream options(profile.box64Conf);
    std::string line;
    while (std::getline(options, line)) {
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos || line[begin] == '#' || line[begin] == ';') continue;
        const auto eq = line.find('=', begin);
        if (eq == std::string::npos) {
            error = "Invalid Box64 option: " + line;
            return false;
        }
        const auto keyEnd = line.find_last_not_of(" \t", eq - 1);
        const auto key = line.substr(begin, keyEnd - begin + 1);
        auto value = line.substr(eq + 1);
        const auto valueBegin = value.find_first_not_of(" \t\r");
        const auto valueEnd = value.find_last_not_of(" \t\r");
        if (key.rfind("BOX64_DYNAREC_", 0) != 0 ||
            key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") != std::string::npos ||
            valueBegin == std::string::npos) {
            error = "Invalid Box64 option: " + line;
            return false;
        }
        value = value.substr(valueBegin, valueEnd - valueBegin + 1);
        if (value.find_first_not_of("0123456789") != std::string::npos || value.size() > 5) {
            error = "Invalid Box64 option value: " + line;
            return false;
        }
        box64.emplace_back(key, value);
    }
    return seedAutorunSettingsFile(stem + ".box64.txt", box64, error);
}

} // namespace

namespace {

constexpr size_t kDependencyArchiveLimit = 8 * 1024 * 1024;
constexpr size_t kDependencyFileLimit = 2 * 1024 * 1024;

std::string autorunFileSha256(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    std::array<char, 32768> buffer{};
    while (in) {
        in.read(buffer.data(), buffer.size());
        sha256_update(&ctx, buffer.data(), static_cast<size_t>(in.gcount()));
    }
    if (!in.eof()) return {};
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool autorunCopyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return false;
    out << in.rdbuf();
    out.close();
    return !in.bad() && !out.fail();
}

// Select only a root member (never the x64 DLL in the same SDK). Nothing else
// in the publisher's archive is extracted into the game's directory.
bool readDependencyZipMember(const std::string& archive, const std::string& name,
                             std::vector<uint8_t>& output) {
    std::ifstream in(archive, std::ios::binary);
    std::array<uint8_t, 30> header{};
    while (in.read(reinterpret_cast<char*>(header.data()), header.size())) {
        if (readU32le(header.data()) != 0x04034b50) return false;
        const auto flags = readU16le(header.data() + 6);
        const auto method = readU16le(header.data() + 8);
        const auto crc = readU32le(header.data() + 14);
        const auto packed = readU32le(header.data() + 18);
        const auto size = readU32le(header.data() + 22);
        const auto nameSize = readU16le(header.data() + 26);
        const auto extraSize = readU16le(header.data() + 28);
        if ((flags & 9) || packed > kDependencyArchiveLimit) return false;
        std::string member(nameSize, '\0');
        if (!in.read(member.data(), nameSize)) return false;
        in.seekg(extraSize, std::ios::cur);
        if (member != name) {
            in.seekg(packed, std::ios::cur);
            continue;
        }
        if (!size || size > kDependencyFileLimit) return false;
        std::vector<uint8_t> input(packed);
        if (!in.read(reinterpret_cast<char*>(input.data()), packed)) return false;
        output.resize(size);
        if (method == 0) {
            if (packed != size) return false;
            output = std::move(input);
        } else if (method == 8) {
            z_stream stream{};
            stream.next_in = input.data(); stream.avail_in = packed;
            stream.next_out = output.data(); stream.avail_out = size;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
            const auto rc = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (rc != Z_STREAM_END || stream.total_in != packed || stream.total_out != size)
                return false;
        } else return false;
        return static_cast<uint32_t>(crc32(0, output.data(), output.size())) == crc;
    }
    return false;
}

} // namespace

bool installAutorunDependencyFixes(
    const std::string& gameDirectory,
    const std::vector<AutorunDependencyFix>& fixes,
    const AutorunService::AssetFetcher& fetcher,
    std::string& error) {
    struct Pending { const AutorunDependencyFix* fix; std::string target, backup, staged; };
    std::vector<Pending> pending;
    auto fail = [&](const std::string& detail) {
        error = detail;
        for (const auto& item : pending) std::remove(item.staged.c_str());
        return false;
    };
    const auto cache = gameDirectory + "/.pipensx-compat";
    for (const auto& fix : fixes) {
        if (fix.fileName.empty() || fix.fileName.find_first_of("/\\") != std::string::npos ||
            fix.fileName == "." || fix.fileName == ".." ||
            fix.archiveUrl.rfind("https://", 0) != 0 || fix.originalSha256.size() != 64 ||
            fix.replacementSha256.size() != 64 || fix.archiveSha256.size() != 64)
            return fail("Invalid AutoRun dependency rule.");
        const auto target = gameDirectory + "/" + fix.fileName;
        const auto backup = target + ".pipensx-original";
        // Recover a power loss between unlink and rename on FAT. Never treat
        // an unrelated existing backup as the original file.
        if (access(target.c_str(), F_OK) != 0 &&
            autorunFileSha256(backup) == fix.originalSha256) {
            if (!autorunCopyFile(backup, target))
                return fail("Unable to restore dependency: " + target);
        }
        const auto current = autorunFileSha256(target);
        if (current == fix.replacementSha256) continue;
        if (current != fix.originalSha256) continue; // user replacement / other release
        if (!makeDirectories(cache))
            return fail("Unable to create dependency cache: " + cache);
        pending.push_back({&fix, target, backup, target + ".pipensx-new"});
        const auto archive = cache + "/" + fix.fileName + ".zip";
        if (autorunFileSha256(archive) != fix.archiveSha256) {
            const auto part = archive + ".part";
            if (!fetcher || !fetcher(fix.archiveUrl, part, kDependencyArchiveLimit, error)) {
                std::remove(part.c_str());
                return fail("Unable to download " + fix.fileName + ": " + error);
            }
            if (autorunFileSha256(part) != fix.archiveSha256) {
                std::remove(part.c_str());
                return fail("Dependency archive checksum mismatch: " + fix.fileName);
            }
            std::remove(archive.c_str());
            if (std::rename(part.c_str(), archive.c_str()) != 0) {
                std::remove(part.c_str());
                return fail("Unable to cache dependency: " + fix.fileName);
            }
        }
        std::vector<uint8_t> content;
        if (!readDependencyZipMember(archive, fix.fileName, content))
            return fail("Unable to extract dependency: " + fix.fileName);
        std::ofstream out(pending.back().staged, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(content.data()), content.size());
        out.close();
        if (!out || autorunFileSha256(pending.back().staged) != fix.replacementSha256)
            return fail("Dependency file checksum mismatch: " + fix.fileName);
    }
    // Fetch and verify the whole dependency set before changing any game file.
    for (const auto& item : pending) {
        if (access(item.backup.c_str(), F_OK) == 0) {
            if (autorunFileSha256(item.backup) != item.fix->originalSha256)
                return fail("Dependency backup does not match original: " + item.backup);
        } else if (!autorunCopyFile(item.target, item.backup) ||
                   autorunFileSha256(item.backup) != item.fix->originalSha256)
            return fail("Unable to back up dependency: " + item.target);
    }
    std::vector<const Pending*> changed;
    for (const auto& item : pending) {
        changed.push_back(&item);
        // libnx/FAT rename cannot replace an existing file. The verified backup
        // above makes this recoverable, including after a restart.
        if (std::remove(item.target.c_str()) != 0 ||
            std::rename(item.staged.c_str(), item.target.c_str()) != 0) {
            for (const auto* prior : changed) {
                if (!autorunCopyFile(prior->backup, prior->target))
                    log_msg("[autorun] dependency restore failed path='%s'\n", prior->target.c_str());
            }
            return fail("Unable to install dependency: " + item.target);
        }
        log_msg("[autorun] dependency updated file='%s' backup='%s'\n",
                item.target.c_str(), item.backup.c_str());
    }
    return true;
}

namespace {

bool repairKnownAutorunDependencies(const std::string& exePath,
                                   AutorunProfile& launchProfile,
                                   const AutorunAssetFetcher& fetcher,
                                   std::string& error) {
    if (lowerAscii(fileNamePart(exePath)) != "ftlgame.exe") return true;
    const auto directory = exePath.substr(0, exePath.find_last_of('/'));
    if (access((directory + "/ftl.dat").c_str(), F_OK) != 0) return true;
    // FTL 1.6.13b GOG: the old Petite startup code in this exact BASS pair
    // faults in AutoRun 2640 before FTL's entry point. The publisher's newer
    // x86 2.4 libraries preserve the API and were verified on the console.
    static const std::vector<AutorunDependencyFix> fixes = {
        {"bass.dll", "4964a9e7787b28a94f60d9b6edfa82ce813ede9303cabd7863261edbec05efb8",
         "ce5e97630cf4e1ef12a9afad785683414b7504efc443766333360ad2ed142eba",
         "https://www.un4seen.com/files/bass24.zip",
         "3a03ec9a33d0f4f9d167660da51c8bb1432e8977496995455ab137277d69636e"},
        {"bassmix.dll", "2428a9f428ee5e6332b1199ccfc1edd2bac9a2dc811d812576abe2e1fe2c415d",
         "4ccefb281fe22dfaa79b1a70537036665a21bd770a0431146c339b516bce5758",
         "https://www.un4seen.com/files/bassmix24.zip",
         "9e7fe1af3122c0d78d07cb2788cd52aef4e47b6c6f8ad0022ad5e40320516796"}
    };
    // Treat the pair as a unit: do not upgrade BASSmix against an unknown
    // version of BASS or change a user's custom/modded library set.
    for (const auto& fix : fixes) {
        auto hash = autorunFileSha256(directory + "/" + fix.fileName);
        if (hash.empty()) hash = autorunFileSha256(directory + "/" + fix.fileName + ".pipensx-original");
        if (hash != fix.originalSha256 && hash != fix.replacementSha256) return true;
    }
    if (!installAutorunDependencyFixes(directory, fixes,
            fetcher ? fetcher : AutorunAssetFetcher(defaultFetchFile), error))
        return false;
    // Reproduce the settings of the successful hardware run. An explicit
    // per-game choice remains authoritative in seedAutorunProgramSettings.
    if (launchProfile.translator.empty()) launchProfile.translator = "Box64";
    if (launchProfile.box64Conf.empty())
        launchProfile.box64Conf = "BOX64_DYNAREC_BIGBLOCK=0\nBOX64_DYNAREC_CALLRET=0\n"
            "BOX64_DYNAREC_SAFEFLAGS=2\nBOX64_DYNAREC_NATIVEFLAGS=0\nBOX64_DYNAREC_FORWARD=0\n";
    return true;
}

} // namespace

bool removeAutorunGameRegistration(const std::string& sdRoot,
    const std::string& gameFolder, uint64_t titleId, std::string& error) {
    if (!taskFilePathIsFatCompatible(gameFolder)) {
        error = "Invalid AutoRun game folder.";
        return false;
    }
    const auto prefix = autorunGamesPath(sdRoot) + "/" + gameFolder + "/";
    const auto library = autorunWineRoot(sdRoot) + "/launcher-library-v2.ini";
    std::ifstream input(library);
    if (input) {
        std::string result, block, line;
        auto flush = [&] {
            std::istringstream lines(block);
            std::string entry;
            bool matches = false;
            while (std::getline(lines, entry))
                if (entry.rfind("path=" + prefix, 0) == 0) matches = true;
            if (!matches) result += block;
            block.clear();
        };
        while (std::getline(input, line)) {
            if (!line.empty() && line.front() == '[') flush();
            block += line + "\n";
        }
        flush();
        input.close();
        std::ofstream out(library, std::ios::binary | std::ios::trunc);
        out << result;
        out.close();
        if (!out) { error = "Unable to update AutoRun library."; return false; }
    }
    const auto target = autorunWineRoot(sdRoot) + "/target.txt";
    std::ifstream targetIn(target);
    std::string selected;
    std::getline(targetIn, selected);
    if (selected.rfind(prefix, 0) == 0 && unlink(target.c_str()) != 0 && errno != ENOENT) {
        error = "Unable to remove AutoRun target."; return false;
    }
    std::string title, folder, executable, nro, argv, ignored;
    if (readAutorunForwarderConfig(sdRoot, titleId, title, folder, executable, nro, argv, ignored) &&
        title.find_first_of("/\\") == std::string::npos && title != "." && title != "..") {
        for (const auto* user : {"wine", "steamuser", "Public"}) {
            const auto shortcut = autorunWineRoot(sdRoot) + "/drive_c/users/" + user +
                "/Desktop/" + title + ".lnk";
            if (unlink(shortcut.c_str()) != 0 && errno != ENOENT) {
                error = "Unable to remove Wine shortcut."; return false;
            }
        }
    }
    return removeSysClkProfile(sdRoot, titleId, error) &&
           removeAutorunForwarderConfig(sdRoot, titleId, error);
}

std::vector<uint8_t> loadAutorunGameIcon(const std::string& sdRoot,
    const std::string& gameFolder, const std::string& coverUrl) {
#ifdef __SWITCH__
    if (!taskFilePathIsFatCompatible(gameFolder)) return {};
    const auto folder = autorunGamesPath(sdRoot) + "/" + gameFolder;
    const auto source = folder + "/.pipensx-cover";
    std::string error;
    std::string imagePath;
    for (const auto* name : {"cover.jpg", "cover.png"}) {
        if (access((folder + "/" + name).c_str(), R_OK) == 0) {
            imagePath = folder + "/" + name;
            break;
        }
    }
    if (imagePath.empty() && !coverUrl.empty() &&
        defaultFetchFile(coverUrl, source, 8 * 1024 * 1024, error))
        imagePath = source;
    if (imagePath.empty()) return {};
    int width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load(imagePath.c_str(), &width, &height, &channels, 3);
    if (imagePath == source) {
        const auto cover = folder + "/cover.png";
        if (pixels) rename(source.c_str(), cover.c_str());
        else unlink(source.c_str());
    }
    if (!pixels || width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return {};
    }
    std::vector<uint8_t> square(256 * 256 * 3);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x)
            for (int c = 0; c < 3; ++c)
                square[(y * 256 + x) * 3 + c] =
                    pixels[((y * height / 256) * width + x * width / 256) * 3 + c];
    stbi_image_free(pixels);
    std::vector<uint8_t> jpeg;
    stbi_write_jpg_to_func([](void* context, void* data, int size) {
        auto& out = *static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }, &jpeg, 256, 256, 3, square.data(), 90);
    return jpeg;
#else
    (void)sdRoot; (void)gameFolder; (void)coverUrl;
    return {};
#endif
}

bool deployAutorunGame(const std::string& sdRoot,
                       const std::string& gameFolder,
                       const std::string& gameTitle,
                       const AutorunProfile& profile,
                       std::string& error) {
    return deployAutorunGame(sdRoot, gameFolder, gameTitle, profile, error, {});
}

bool deployAutorunGame(const std::string& sdRoot,
                       const std::string& gameFolder,
                       const std::string& gameTitle,
                       const AutorunProfile& profile,
                       std::string& error,
                       const AutorunAssetFetcher& dependencyFetcher) {
    if (!taskFilePathIsFatCompatible(gameFolder)) {
        error = "Game folder must not be empty.";
        return false;
    }
    const std::string wineGamesDir = autorunGamesPath(sdRoot) + "/" + gameFolder;
    if (!makeDirectories(wineGamesDir)) {
        error = "Unable to create game directory: " + wineGamesDir;
        return false;
    }

    // Resolve before writing launcher state; a download can contain only an installer.
    const std::string exeName = !profile.executable.empty()
        ? profile.executable : (gameFolder + ".exe");
    const std::string actualExeRel = findExecutableRelativePath(wineGamesDir, exeName);

    if (actualExeRel.empty()) {
        error = "Game executable not found: " + exeName + ". Install or extract the game first.";
        return false;
    }

    AutorunProfile launchProfile = profile;
    const auto exePath = wineGamesDir + "/" + actualExeRel;
    if (!repairKnownAutorunDependencies(exePath, launchProfile, dependencyFetcher, error))
        return false;
    if (!seedAutorunProgramSettings(exePath, launchProfile, error))
        return false;

    // Write dxvk.conf
    const std::string dxvkPath = wineGamesDir + "/dxvk.conf";
    const std::string dxvkContent = !profile.dxvkConf.empty()
        ? profile.dxvkConf : kDefaultDxvkConf;
    std::ofstream dxvkOut(dxvkPath, std::ios::binary | std::ios::trunc);
    if (!dxvkOut) {
        error = "Unable to write dxvk.conf at: " + dxvkPath;
        return false;
    }
    dxvkOut.write(dxvkContent.data(), static_cast<std::streamsize>(dxvkContent.size()));
    dxvkOut.close();
    if (!dxvkOut) {
        error = "Unable to write dxvk.conf at: " + dxvkPath;
        return false;
    }

    // Merge user.reg
    if (!profile.registry.empty()) {
        const std::string regPath = autorunRegistryPath(sdRoot);
        if (!mergeUserRegistry(regPath, profile.registry, error)) {
            return false;
        }
    }

    // Create Windows .lnk on all user desktops (wine, steamuser, Public)
    const std::string title = !gameTitle.empty() ? gameTitle : gameFolder;
    const std::string wineRoot = autorunWineRoot(sdRoot);
    std::vector<std::string> desktops = {
        autorunDesktopPath(sdRoot),
        wineRoot + "/drive_c/users/steamuser/Desktop",
        wineRoot + "/drive_c/users/Public/Desktop",
    };
    bool shortcutCreated = false;
    for (const auto& dPath : desktops) {
        std::string err;
        if (createWineDesktopShortcut(dPath, title, gameFolder, actualExeRel, err))
            shortcutCreated = true;
    }
    if (!shortcutCreated) {
        error = "Failed to create Wine desktop shortcut in user directories.";
        return false;
    }

    // target.txt preselects the game in AutoRun; direct launch uses NRO argv.
    const std::string targetTxtPath = wineRoot + "/target.txt";
    std::string fullTarget = autorunGamesPath(sdRoot) + "/" + gameFolder + "/" + actualExeRel;
    std::replace(fullTarget.begin(), fullTarget.end(), '\\', '/');
    std::ofstream targetOut(targetTxtPath, std::ios::binary | std::ios::trunc);
    targetOut << fullTarget << "\n";
    targetOut.close();
    if (!targetOut) {
        error = "Unable to write AutoRun target: " + targetTxtPath;
        return false;
    }

    // Register game in AutoRun GUI library (launcher-library-v2.ini)
    std::string coverTarget = autorunGamesPath(sdRoot) + "/" + gameFolder + "/cover.png";
    if (!registerGameInLauncherLibrary(sdRoot, fullTarget, coverTarget)) {
        error = "Unable to register game in AutoRun library.";
        return false;
    }
    return true;
}

bool saveAutorunProfile(const std::string& appRoot,
                        const std::string& taskId,
                        const AutorunProfile& profile,
                        std::string& error) {
    if (appRoot.empty() || taskId.empty()) {
        error = "Invalid arguments for saveAutorunProfile.";
        return false;
    }
    std::string path = appRoot + "/autorun-" + taskId + ".json";
    nlohmann::json j;
    j["title"] = profile.title;
    j["cover_url"] = profile.coverUrl;
    j["executable"] = profile.executable;
    j["target_folder"] = profile.targetFolder;
    j["status"] = profile.status;
    j["fps"] = profile.fps;
    j["overclock"] = profile.overclock;
    j["translator"] = profile.translator;
    j["renderer"] = profile.renderer;
    j["notes"] = profile.notes;
    j["dxvk_conf"] = profile.dxvkConf;
    j["box64_conf"] = profile.box64Conf;
    j["registry"] = profile.registry;

    std::string content = j.dump(2);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Unable to save autorun profile at: " + path;
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

bool loadAutorunProfile(const std::string& appRoot,
                        const std::string& taskId,
                        AutorunProfile& profile,
                        std::string& error) {
    std::string path = appRoot + "/autorun-" + taskId + ".json";
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        error = "Autorun profile not found: " + path;
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Unable to open autorun profile: " + path;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "Malformed autorun profile JSON: " + path;
        return false;
    }
    profile.title = j.value("title", "");
    profile.coverUrl = j.value("cover_url", "");
    profile.executable = j.value("executable", "");
    profile.targetFolder = j.value("target_folder", "");
    profile.status = j.value("status", "");
    profile.fps = j.value("fps", "");
    profile.overclock = j.value("overclock", "");
    profile.translator = j.value("translator", "");
    profile.renderer = j.value("renderer", "");
    profile.notes = j.value("notes", "");
    profile.dxvkConf = j.value("dxvk_conf", "");
    profile.box64Conf = j.value("box64_conf", "");
    profile.registry = j.value("registry", "");
    return true;
}

void removeAutorunProfile(const std::string& appRoot,
                          const std::string& taskId) {
    std::string path = appRoot + "/autorun-" + taskId + ".json";
    std::remove(path.c_str());
}

bool extractRuntimeZip(
    const std::string& archivePath,
    const std::string& destinationDir,
    const std::atomic<bool>& cancelled,
    const std::function<void(uint64_t, uint64_t)>& progress,
    std::string& error) {
    std::FILE* file = std::fopen(archivePath.c_str(), "rb");
    if (!file) {
        error = "Unable to open ZIP archive: " + archivePath;
        return false;
    }

    std::vector<uint8_t> local(30);
    std::vector<uint8_t> name;
    std::vector<uint8_t> extra;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> uncompressed;
    uint64_t totalExtracted = 0;

    while (!cancelled.load(std::memory_order_relaxed)) {
        if (std::fread(local.data(), 1, 30, file) != 30) {
            if (std::feof(file))
                break;
            std::fclose(file);
            error = "Truncated ZIP archive header.";
            return false;
        }

        const uint32_t sig = readU32le(local.data());
        if (sig == 0x02014b50 || sig == 0x06054b50)
            break; // Central directory reached
        if (sig != 0x04034b50) {
            std::fclose(file);
            error = "Invalid ZIP local file signature.";
            return false;
        }

        const uint16_t method = readU16le(local.data() + 8);
        const uint32_t compSize = readU32le(local.data() + 18);
        const uint32_t uncompSize = readU32le(local.data() + 22);
        const uint16_t nameLen = readU16le(local.data() + 26);
        const uint16_t extraLen = readU16le(local.data() + 28);

        name.resize(nameLen);
        extra.resize(extraLen);

        if ((nameLen && std::fread(name.data(), 1, nameLen, file) != nameLen) ||
            (extraLen && std::fread(extra.data(), 1, extraLen, file) != extraLen)) {
            std::fclose(file);
            error = "Truncated ZIP entry metadata.";
            return false;
        }

        std::string member(reinterpret_cast<char*>(name.data()), nameLen);
        const bool isDir = !member.empty() && member.back() == '/';

        // Skip root switch folder entry if present
        if (member == "switch" || member == "switch/")
            continue;

        // Strip known root prefixes from runtime archives
        if (member.rfind("switch/wine/", 0) == 0)
            member = member.substr(12);
        else if (member.rfind("wine/", 0) == 0)
            member = member.substr(5);

        // Security check: reject unsafe paths
        if (member.empty() || member.front() == '/' ||
            member.find("..") != std::string::npos) {
            if (compSize > 0)
                std::fseek(file, compSize, SEEK_CUR);
            continue;
        }

        const std::string targetPath = destinationDir + "/" + member;

        if (isDir) {
            makeDirectories(targetPath);
            if (compSize > 0)
                std::fseek(file, compSize, SEEK_CUR);
            continue;
        }

        // Ensure parent directory exists
        size_t slash = targetPath.find_last_of('/');
        if (slash != std::string::npos) {
            makeDirectories(targetPath.substr(0, slash));
        }

        if (compSize > 0) {
            compressed.resize(compSize);
            if (std::fread(compressed.data(), 1, compSize, file) != compSize) {
                std::fclose(file);
                error = "Truncated entry data in ZIP.";
                return false;
            }
        } else {
            compressed.clear();
        }

        if (method == 0) { // Stored
            std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::fclose(file);
                error = "Unable to create file: " + targetPath;
                return false;
            }
            if (!compressed.empty()) {
                out.write(reinterpret_cast<char*>(compressed.data()),
                          static_cast<std::streamsize>(compressed.size()));
            }
            totalExtracted += uncompSize;
        } else if (method == 8) { // Deflated
            uncompressed.resize(uncompSize);
            z_stream strm{};
            strm.next_in = compressed.data();
            strm.avail_in = static_cast<uInt>(compressed.size());
            strm.next_out = uncompressed.data();
            strm.avail_out = static_cast<uInt>(uncompressed.size());

            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
                std::fclose(file);
                error = "Failed to initialize zlib inflate.";
                return false;
            }
            int zrc = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);

            if (zrc != Z_STREAM_END || strm.total_out != uncompSize) {
                std::fclose(file);
                error = "Failed to decompress entry: " + member;
                return false;
            }

            std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::fclose(file);
                error = "Unable to create file: " + targetPath;
                return false;
            }
            out.write(reinterpret_cast<char*>(uncompressed.data()),
                      static_cast<std::streamsize>(uncompressed.size()));
            totalExtracted += uncompSize;
        } else {
            std::fclose(file);
            error = "Unsupported ZIP compression method for entry: " + member;
            return false;
        }

        if (progress) {
            progress(totalExtracted, 0);
        }
    }

    std::fclose(file);
    if (cancelled.load(std::memory_order_relaxed)) {
        error = "Installation cancelled.";
        return false;
    }
    return true;
}

AutorunService::AutorunService(std::string sdRoot,
                               MetadataFetcher metadataFetcher,
                               AssetFetcher assetFetcher)
    : sdRoot_(std::move(sdRoot)),
      metadataFetcher_(std::move(metadataFetcher)),
      assetFetcher_(std::move(assetFetcher)) {
    if (!metadataFetcher_) {
        metadataFetcher_ = [](const std::string& url, size_t limit,
                              std::string& body, std::string& error) {
            return defaultFetchText(url, limit, body, error);
        };
    }
    if (!assetFetcher_)
        assetFetcher_ = defaultFetchFile;
}

AutorunService::~AutorunService() = default;

bool AutorunService::parseRelease(const std::string& json,
                                  AutorunReleaseInfo& release,
                                  std::string& error) {
    release = {};
    auto root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Invalid GitHub release response.";
        return false;
    }
    if (root.value("draft", false) || root.value("prerelease", false)) {
        error = "Release is not a published stable release.";
        return false;
    }
    release.version = root.value("tag_name", "");
    if (release.version.empty()) {
        error = "Release tag name is missing.";
        return false;
    }

    if (!root.contains("assets") || !root["assets"].is_array()) {
        error = "Release contains no assets.";
        return false;
    }

    for (const auto& asset : root["assets"]) {
        if (!asset.is_object())
            continue;
        const std::string name = lowerAscii(asset.value("name", ""));
        const std::string url = asset.value("browser_download_url", "");

        if (endsWith(name, ".zip") || endsWith(name, ".7z")) {
            if (name.find("wine") != std::string::npos ||
                name.find("autorun") != std::string::npos ||
                name.find("runtime") != std::string::npos) {
                release.archiveUrl = url;
                release.size = asset.value("size", 0ULL);
            }
        }
        if (endsWith(name, ".sha256") || endsWith(name, ".checksum")) {
            release.checksumUrl = url;
        }
    }

    if (release.archiveUrl.empty()) {
        error = "No compatible AutoRun runtime archive found in release assets.";
        return false;
    }

    return true;
}

AutorunUpdateCheck AutorunService::checkForUpdates(
    const std::string& releaseUrl) const {
    AutorunUpdateCheck result;
    result.installedVersion = installedVersion();

    std::vector<std::string> candidateUrls;
    if (!releaseUrl.empty()) {
        candidateUrls.push_back(releaseUrl);
    } else {
        candidateUrls.push_back(
            "https://api.github.com/repos/qret/autorun/releases/latest");
        candidateUrls.push_back(
            "https://api.github.com/repos/autorunhq/autorun/releases/latest");
    }

    bool fetched = false;
    for (const auto& url : candidateUrls) {
        std::string body;
        std::string fetchErr;
        if (metadataFetcher_(url, kMetadataLimit, body, fetchErr)) {
            if (parseRelease(body, result.release, fetchErr)) {
                fetched = true;
                result.error.clear();
                break;
            }
        }
        result.error = fetchErr;
    }

    if (!fetched) {
        result.ok = false;
        return result;
    }

    result.ok = true;
    if (result.installedVersion.empty() || result.installedVersion == "0.0.0") {
        result.updateAvailable = true;
    } else if (result.installedVersion == "custom") {
        result.updateAvailable = false;
    } else {
        result.updateAvailable = isNewerRelease(
            result.release.version, result.installedVersion);
    }

    return result;
}

bool AutorunService::installRuntimeArchive(
    const std::string& archivePath,
    const std::string& releaseVersion,
    std::string& error,
    const std::atomic<bool>& cancelled,
    const ProgressCallback& progress) const {
    if (!prepareAutorunEnvironment(sdRoot_, error))
        return false;

    const std::string wineDir = autorunWineRoot(sdRoot_);
    auto zipProgress = [&progress](uint64_t current, uint64_t total) {
        if (progress) progress(current, total);
    };

    if (!extractRuntimeZip(archivePath, wineDir, cancelled, zipProgress, error))
        return false;

    // Write version.json
    const std::string verPath = autorunVersionPath(sdRoot_);
    nlohmann::json j;
    j["version"] = releaseVersion;
    j["updated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::ofstream out(verPath, std::ios::binary | std::ios::trunc);
    if (out) {
        out << j.dump(2) << "\n";
    }

    return true;
}

bool AutorunService::downloadAndInstallRuntime(
    const AutorunReleaseInfo& release,
    std::string& error,
    const std::atomic<bool>& cancelled,
    const ProgressCallback& progress,
    const ProgressCallback& downloadProgress) const {
    const std::string tempZip = autorunWineRoot(sdRoot_) + "/.runtime_tmp.zip";
    makeDirectories(autorunWineRoot(sdRoot_));

    if (cancelled.load()) { error = "Cancelled."; return false; }
    const auto builtin = assetFetcher_.target<decltype(&defaultFetchFile)>();
    const bool fetched = builtin && *builtin == &defaultFetchFile
        ? fetchFileWithProgress(release.archiveUrl, tempZip, kRuntimeArchiveLimit,
                                error, &cancelled, downloadProgress)
        : assetFetcher_(release.archiveUrl, tempZip, kRuntimeArchiveLimit, error);
    if (!fetched || cancelled.load()) {
        if (cancelled.load()) error = "Cancelled.";
        unlink(tempZip.c_str());
        return false;
    }

    bool ok = installRuntimeArchive(tempZip, release.version, error, cancelled, progress);
    unlink(tempZip.c_str());
    return ok;
}

bool AutorunService::deployTask(
    const DownloadTask& task,
    const AutorunProfile& profile,
    std::string& error) const {
    std::string gameFolder = !profile.targetFolder.empty()
        ? profile.targetFolder
        : fileNamePart(task.dataPath);
    std::string title = !profile.title.empty()
        ? profile.title
        : task.name;
    return deployAutorunGame(sdRoot_, gameFolder, title, profile, error, assetFetcher_);
}

uint64_t generateAutorunTitleId(const std::string& gameTitle) {
    uint32_t h = 0x811c9dc5u;
    for (char c : gameTitle) {
        h ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
        h *= 0x01000193u;
    }
    // High 32 bits: 0x0100BEEF
    // Middle 16 bits: hash
    // Low 16 bits: 0x0000 (Base application standard)
    return 0x0100BEEF00000000ULL | (static_cast<uint64_t>(h & 0xFFFFu) << 16);
}

std::string autorunTitleIdToString(uint64_t titleId) {
    char buf[17] = {0};
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(titleId));
    return std::string(buf);
}

bool createAutorunForwarderConfig(const std::string& sdRoot,
                                  uint64_t titleId,
                                  const std::string& gameTitle,
                                  const std::string& gameFolder,
                                  const std::string& executable,
                                  std::string& error) {
    const std::string dir = autorunForwardersPath(sdRoot);
    if (!makeDirectories(dir)) {
        error = "Unable to create forwarders directory: " + dir;
        return false;
    }
    const std::string confPath = autorunForwarderConfigPath(sdRoot, titleId);
    std::ofstream out(confPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Unable to write forwarder config: " + confPath;
        return false;
    }
    const std::string titleIdHex = autorunTitleIdToString(titleId);
    const std::string nroPath = autorunRuntimeNroPath(sdRoot);
    const std::string wineTarget = autorunGamesPath(sdRoot) + "/" + gameFolder + "/" + executable;
    const std::string nroArgv = "\"" + nroPath + "\" \"" + wineTarget + "\"";

    out << "[forwarder]\n";
    out << "title=" << gameTitle << "\n";
    out << "title_id=" << titleIdHex << "\n";
    out << "target_folder=" << gameFolder << "\n";
    out << "executable=" << executable << "\n";
    out << "nro=" << nroPath << "\n";
    out << "argv=" << nroArgv << "\n";
    return true;
}

bool readAutorunForwarderConfig(const std::string& sdRoot,
                                uint64_t titleId,
                                std::string& gameTitle,
                                std::string& gameFolder,
                                std::string& executable,
                                std::string& nroPath,
                                std::string& nroArgv,
                                std::string& error) {
    const std::string confPath = autorunForwarderConfigPath(sdRoot, titleId);
    std::ifstream in(confPath);
    if (!in) {
        error = "Forwarder config not found: " + confPath;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || std::isspace(static_cast<unsigned char>(val.back())))) val.pop_back();

        if (key == "title") gameTitle = val;
        else if (key == "target_folder") gameFolder = val;
        else if (key == "executable") executable = val;
        else if (key == "nro") nroPath = val;
        else if (key == "argv") nroArgv = val;
    }
    return true;
}

bool removeAutorunForwarderConfig(const std::string& sdRoot,
                                  uint64_t titleId,
                                  std::string& error) {
    const std::string confPath = autorunForwarderConfigPath(sdRoot, titleId);
    if (unlink(confPath.c_str()) != 0 && errno != ENOENT) {
        error = "Unable to remove forwarder config: " + confPath;
        return false;
    }
    return true;
}

bool isSysClkInstalled(const std::string& sdRoot) {
    const std::string cfg = autorunSysClkConfigPath(sdRoot);
    if (access(cfg.c_str(), F_OK) == 0) return true;
    const std::string amsDir = cleanRoot(sdRoot) + "/atmosphere/contents/00FF0000636C6BFF";
    return access(amsDir.c_str(), F_OK) == 0;
}

bool parseOverclockProfile(const std::string& ocStr, SysClkProfile& out) {
    out = SysClkProfile{1785, 768, 1900};
    if (ocStr.empty()) return true;

    // Check for "num / num / num" pattern
    std::string s = ocStr;
    size_t s1 = s.find('/');
    if (s1 != std::string::npos) {
        size_t s2 = s.find('/', s1 + 1);
        if (s2 != std::string::npos) {
            auto extractDigits = [](const std::string& part) -> uint32_t {
                std::string d;
                for (char c : part) {
                    if (std::isdigit(static_cast<unsigned char>(c))) d.push_back(c);
                    else if (!d.empty()) break;
                }
                return d.empty() ? 0 : static_cast<uint32_t>(std::stoul(d));
            };
            uint32_t c = extractDigits(s.substr(0, s1));
            uint32_t g = extractDigits(s.substr(s1 + 1, s2 - s1 - 1));
            uint32_t m = extractDigits(s.substr(s2 + 1));
            if (c > 0 && g > 0 && m > 0) {
                out.cpu = c;
                out.gpu = g;
                out.mem = m;
                return true;
            }
        }
    }

    // Keyword search: cpu, gpu, ram/mem
    std::string lower = lowerAscii(ocStr);
    auto findKeywordVal = [&lower](const std::string& kw) -> uint32_t {
        size_t pos = lower.find(kw);
        if (pos == std::string::npos) return 0;
        pos += kw.size();
        while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '=' || lower[pos] == ':' || lower[pos] == '~'))
            pos++;
        std::string digits;
        while (pos < lower.size() && std::isdigit(static_cast<unsigned char>(lower[pos]))) {
            digits.push_back(lower[pos]);
            pos++;
        }
        return digits.empty() ? 0 : static_cast<uint32_t>(std::stoul(digits));
    };

    uint32_t c = findKeywordVal("cpu");
    uint32_t g = findKeywordVal("gpu");
    uint32_t m = findKeywordVal("ram");
    if (m == 0) m = findKeywordVal("mem");

    if (c > 0) out.cpu = c;
    if (g > 0) out.gpu = g;
    if (m > 0) out.mem = m;

    return true;
}

bool hasSysClkProfile(const std::string& sdRoot, uint64_t titleId) {
    const std::string cfgPath = autorunSysClkConfigPath(sdRoot);
    std::ifstream in(cfgPath);
    if (!in) return false;
    std::string targetSection = "[" + lowerAscii(autorunTitleIdToString(titleId)) + "]";
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.front() == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                std::string sec = lowerAscii(line.substr(0, end + 1));
                if (sec == targetSection) return true;
            }
        }
    }
    return false;
}

bool applySysClkProfile(const std::string& sdRoot,
                        uint64_t titleId,
                        const SysClkProfile& profile,
                        std::string& error) {
    const std::string cfgPath = autorunSysClkConfigPath(sdRoot);
    if (!makeDirectories(cleanRoot(sdRoot) + "/config/sys-clk")) {
        error = "Unable to create sys-clk configuration directory.";
        return false;
    }

    std::string titleIdHex = autorunTitleIdToString(titleId);
    std::string targetSection = "[" + lowerAscii(titleIdHex) + "]";

    std::vector<std::string> lines;
    {
        std::ifstream in(cfgPath);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            }
        }
    }

    std::vector<std::string> newSection;
    newSection.push_back("[" + titleIdHex + "]");
    newSection.push_back("handheld_cpu=" + std::to_string(profile.cpu));
    newSection.push_back("handheld_gpu=" + std::to_string(profile.gpu));
    newSection.push_back("handheld_mem=" + std::to_string(profile.mem));
    newSection.push_back("docked_cpu=" + std::to_string(profile.cpu));
    newSection.push_back("docked_gpu=" + std::to_string(profile.gpu));
    newSection.push_back("docked_mem=" + std::to_string(profile.mem));

    int secStart = -1;
    int secEnd = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty() && lines[i].front() == '[') {
            size_t endBracket = lines[i].find(']');
            if (endBracket != std::string::npos) {
                std::string s = lowerAscii(lines[i].substr(0, endBracket + 1));
                if (s == targetSection) {
                    secStart = static_cast<int>(i);
                    size_t j = i + 1;
                    while (j < lines.size() && (lines[j].empty() || lines[j].front() != '[')) {
                        j++;
                    }
                    secEnd = static_cast<int>(j);
                    break;
                }
            }
        }
    }

    if (secStart >= 0) {
        lines.erase(lines.begin() + secStart, lines.begin() + secEnd);
        lines.insert(lines.begin() + secStart, newSection.begin(), newSection.end());
    } else {
        if (!lines.empty() && !lines.back().empty()) {
            lines.push_back("");
        }
        lines.insert(lines.end(), newSection.begin(), newSection.end());
    }

    std::ofstream out(cfgPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Unable to write sys-clk config: " + cfgPath;
        return false;
    }
    for (const auto& l : lines) {
        out << l << "\n";
    }
    return true;
}

bool removeSysClkProfile(const std::string& sdRoot,
                         uint64_t titleId,
                         std::string& error) {
    const std::string cfgPath = autorunSysClkConfigPath(sdRoot);
    std::ifstream in(cfgPath);
    if (!in) return true;

    std::string targetSection = "[" + lowerAscii(autorunTitleIdToString(titleId)) + "]";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    in.close();

    int secStart = -1;
    int secEnd = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty() && lines[i].front() == '[') {
            size_t endBracket = lines[i].find(']');
            if (endBracket != std::string::npos) {
                std::string s = lowerAscii(lines[i].substr(0, endBracket + 1));
                if (s == targetSection) {
                    secStart = static_cast<int>(i);
                    size_t j = i + 1;
                    while (j < lines.size() && (lines[j].empty() || lines[j].front() != '[')) {
                        j++;
                    }
                    secEnd = static_cast<int>(j);
                    break;
                }
            }
        }
    }

    if (secStart >= 0) {
        lines.erase(lines.begin() + secStart, lines.begin() + secEnd);
        std::ofstream out(cfgPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Unable to update sys-clk config: " + cfgPath;
            return false;
        }
        for (const auto& l : lines) {
            out << l << "\n";
        }
    }
    return true;
}

std::vector<uint8_t> generateNacp(const std::string& title,
                                  const std::string& author,
                                  const std::string& version) {
    constexpr size_t kNacpSize = 0x4000;
    constexpr size_t kNameSize = 0x200;
    constexpr size_t kAuthorSize = 0x100;
    constexpr size_t kEntrySize = kNameSize + kAuthorSize; // 0x300
    constexpr size_t kPlainLangCount = 16;
    constexpr size_t kDisplayVersionOffset = 0x3060;
    constexpr size_t kTitlesDataFormatOffset = 0x3215;

    std::vector<uint8_t> nacp(kNacpSize, 0);

    for (size_t i = 0; i < kPlainLangCount; ++i) {
        size_t base = i * kEntrySize;
        std::strncpy(reinterpret_cast<char*>(nacp.data() + base),
                     title.c_str(), kNameSize - 1);
        std::strncpy(reinterpret_cast<char*>(nacp.data() + base + kNameSize),
                     author.c_str(), kAuthorSize - 1);
    }

    std::strncpy(reinterpret_cast<char*>(nacp.data() + kDisplayVersionOffset),
                 version.c_str(), 15);
    nacp[kTitlesDataFormatOffset] = 0; // plain uncompressed format

    uint32_t langFlags = 0x0000FFFF;
    std::memcpy(nacp.data() + 0x3030, &langFlags, sizeof(langFlags));

    return nacp;
}

std::vector<uint8_t> buildPfs0(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> strings;
    std::vector<uint32_t> nameOffsets;
    for (const auto& f : files) {
        nameOffsets.push_back(static_cast<uint32_t>(strings.size()));
        strings.insert(strings.end(), f.first.begin(), f.first.end());
        strings.push_back(0);
    }

    std::vector<uint8_t> out;
    out.reserve(16 + files.size() * 24 + strings.size() + 1024);

    out.push_back('P'); out.push_back('F'); out.push_back('S'); out.push_back('0');

    auto append32 = [&out](uint32_t val) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };
    auto append64 = [&out](uint64_t val) {
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };

    append32(static_cast<uint32_t>(files.size()));
    append32(static_cast<uint32_t>(strings.size()));
    append32(0); // reserved

    uint64_t offset = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        append64(offset);
        append64(static_cast<uint64_t>(files[i].second.size()));
        append32(nameOffsets[i]);
        append32(0); // reserved
        offset += files[i].second.size();
    }

    out.insert(out.end(), strings.begin(), strings.end());
    for (const auto& f : files) {
        out.insert(out.end(), f.second.begin(), f.second.end());
    }

    return out;
}

bool parsePfs0(const uint8_t* data, size_t size,
               std::vector<std::pair<std::string, std::vector<uint8_t>>>& outFiles) {
    outFiles.clear();
    if (!data || size < 16) return false;
    if (data[0] != 'P' || data[1] != 'F' || data[2] != 'S' || data[3] != '0') return false;

    auto read32 = [data](size_t pos) -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= (static_cast<uint32_t>(data[pos + i]) << (i * 8));
        return v;
    };
    auto read64 = [data](size_t pos) -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= (static_cast<uint64_t>(data[pos + i]) << (i * 8));
        return v;
    };

    uint32_t numFiles = read32(4);
    uint32_t stringTableSize = read32(8);

    uint64_t headerSize = 16ull + static_cast<uint64_t>(numFiles) * 24ull + stringTableSize;
    if (headerSize > size) return false;

    const uint8_t* strings = data + 16 + numFiles * 24;
    const uint8_t* dataSection = data + headerSize;
    uint64_t remainingData = size - headerSize;

    for (uint32_t i = 0; i < numFiles; ++i) {
        size_t entryPos = 16 + i * 24;
        uint64_t offset = read64(entryPos);
        uint64_t fsize = read64(entryPos + 8);
        uint32_t nameOffset = read32(entryPos + 16);

        if (nameOffset >= stringTableSize) return false;
        if (offset + fsize > remainingData) return false;

        std::string name = reinterpret_cast<const char*>(strings + nameOffset);
        std::vector<uint8_t> content(dataSection + offset, dataSection + offset + fsize);
        outFiles.emplace_back(std::move(name), std::move(content));
    }
    return true;
}

std::vector<uint8_t> generateAutorunStubNsp(
    const std::string& title,
    uint64_t titleId,
    const std::vector<uint8_t>& iconJpeg,
    const std::string& target) {
#ifdef __SWITCH__
    auto readAsset = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    const auto main = readAsset("romfs:/autorun-loader.nso");
    const auto npdm = readAsset("romfs:/autorun-loader.npdm");
    const auto icon = iconJpeg.empty() ? readAsset("romfs:/autorun-icon.jpg") : iconJpeg;
    void* data = nullptr;
    size_t size = 0;
    const std::string argv = "sdmc:/switch/wine/wine-nx-runtime.nro" +
        (target.empty() ? std::string() : " \"" + target + "\"");
    const Result rc = pipensxBuildForwarder(titleId, title.c_str(),
        "sdmc:/switch/wine/wine-nx-runtime.nro", argv.c_str(),
        main.data(), main.size(), npdm.data(), npdm.size(), icon.data(), icon.size(),
        &data, &size);
    if (R_FAILED(rc)) {
        log_msg("[autorun] forwarder build failed result=0x%08x\n", rc);
        return {};
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> result(bytes, bytes + size);
    std::free(data);
    return result;
#else
    // A PC cannot derive the console's NCA header key. Never fabricate a package
    // that passes the PC backend but cannot be installed on hardware.
    (void)title; (void)titleId; (void)iconJpeg; (void)target;
    return {};
#endif
}

bool installAutorunForwarder(const std::string& sdRoot,
                             install::InstallBackend& backend,
                             const std::string& gameTitle,
                             const std::string& gameFolder,
                             const std::string& executable,
                             const std::vector<uint8_t>& iconJpeg,
                             std::string& error) {
    if (gameTitle.empty() || gameFolder.empty() || executable.empty()) {
        error = "Invalid arguments for forwarder installation.";
        return false;
    }

    uint64_t titleId = generateAutorunTitleId(gameTitle);
    std::string titleIdHex = autorunTitleIdToString(titleId);

    const std::string resolved = resolveAutorunExecutable(sdRoot, gameFolder, executable);
    if (resolved.empty()) {
        error = "Game executable not found: " + executable;
        return false;
    }
    const std::string target = autorunGamesPath(sdRoot) + "/" + gameFolder + "/" + resolved;
    std::vector<uint8_t> nsp = generateAutorunStubNsp(gameTitle, titleId, iconJpeg, target);
    if (nsp.empty()) {
        error = "Unable to build AutoRun forwarder; check the log and bundled loader assets (requires Switch).";
        return false;
    }

    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    if (!parsePfs0(nsp.data(), nsp.size(), files)) {
        error = "Failed to parse generated stub NSP PFS0.";
        return false;
    }

    std::string taskId = "autorun-fwd-" + titleIdHex;
    std::string packageName = gameTitle + " [" + titleIdHex + "].nsp";

    if (!createAutorunForwarderConfig(sdRoot, titleId, gameTitle, gameFolder, resolved, error))
        return false;

    if (!backend.beginPackage(taskId, packageName)) {
        error = backend.error();
        return false;
    }

    for (const auto& f : files) {
        if (!backend.beginFile(f.first, f.second.size())) {
            error = backend.error();
            backend.rollbackPackage();
            return false;
        }
        if (!backend.writeFile(f.second.data(), f.second.size())) {
            error = backend.error();
            backend.rollbackPackage();
            return false;
        }
        if (!backend.endFile()) {
            error = backend.error();
            backend.rollbackPackage();
            return false;
        }
    }

    bool alreadyInstalled = false;
    if (!backend.commitPackage(alreadyInstalled)) {
        error = backend.error();
        backend.rollbackPackage();
        return false;
    }

    return true;
}

} // namespace pipensx
