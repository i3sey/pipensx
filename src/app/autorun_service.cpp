#include "autorun_service.hpp"
#include "curl_https.hpp"
#include "download_manager.hpp"
#include "port_selection.hpp"
#include "task_files.hpp"
#include "update_service.hpp"

#include <borealis/extern/nlohmann/json.hpp>
#include <curl/curl.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

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
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx-autorun/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

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

bool defaultFetchFile(const std::string& url, const std::string& path,
                      size_t limit, std::string& error) {
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
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx-autorun/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);

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
    std::array<uint64_t, 3> candParts{};
    std::array<uint64_t, 3> currParts{};
    return parseSemVer(candidate, candParts) &&
           parseSemVer(current, currParts) && candParts > currParts;
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

bool deployAutorunGame(const std::string& sdRoot,
                       const std::string& gameFolder,
                       const std::string& gameTitle,
                       const AutorunProfile& profile,
                       std::string& error) {
    if (gameFolder.empty()) {
        error = "Game folder must not be empty.";
        return false;
    }
    const std::string wineGamesDir = autorunGamesPath(sdRoot) + "/" + gameFolder;
    if (!makeDirectories(wineGamesDir)) {
        error = "Unable to create game directory: " + wineGamesDir;
        return false;
    }

    // 1. Write dxvk.conf
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

    // 2. Merge user.reg
    if (!profile.registry.empty()) {
        const std::string regPath = autorunRegistryPath(sdRoot);
        if (!mergeUserRegistry(regPath, profile.registry, error)) {
            return false;
        }
    }

    // 3. Create Windows .lnk on desktop
    const std::string exeName = !profile.executable.empty()
        ? profile.executable : (gameFolder + ".exe");
    const std::string title = !gameTitle.empty() ? gameTitle : gameFolder;
    const std::string desktop = autorunDesktopPath(sdRoot);
    if (!createWineDesktopShortcut(desktop, title, gameFolder, exeName, error)) {
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
    j["executable"] = profile.executable;
    j["target_folder"] = profile.targetFolder;
    j["status"] = profile.status;
    j["fps"] = profile.fps;
    j["overclock"] = profile.overclock;
    j["translator"] = profile.translator;
    j["renderer"] = profile.renderer;
    j["notes"] = profile.notes;
    j["dxvk_conf"] = profile.dxvkConf;
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
    profile.executable = j.value("executable", "");
    profile.targetFolder = j.value("target_folder", "");
    profile.status = j.value("status", "");
    profile.fps = j.value("fps", "");
    profile.overclock = j.value("overclock", "");
    profile.translator = j.value("translator", "");
    profile.renderer = j.value("renderer", "");
    profile.notes = j.value("notes", "");
    profile.dxvkConf = j.value("dxvk_conf", "");
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
    if (!assetFetcher_) {
        assetFetcher_ = [](const std::string& url, const std::string& path,
                           size_t limit, std::string& error) {
            return defaultFetchFile(url, path, limit, error);
        };
    }
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

    const std::string url = !releaseUrl.empty()
        ? releaseUrl
        : "https://api.github.com/repos/Pipen-dev/wine-nx/releases/latest";

    std::string body;
    if (!metadataFetcher_(url, kMetadataLimit, body, result.error)) {
        result.ok = false;
        return result;
    }

    if (!parseRelease(body, result.release, result.error)) {
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
    const ProgressCallback& progress) const {
    const std::string tempZip = autorunWineRoot(sdRoot_) + "/.runtime_tmp.zip";
    makeDirectories(autorunWineRoot(sdRoot_));

    if (!assetFetcher_(release.archiveUrl, tempZip, kRuntimeArchiveLimit, error)) {
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
    return deployAutorunGame(sdRoot_, gameFolder, title, profile, error);
}

} // namespace pipensx
