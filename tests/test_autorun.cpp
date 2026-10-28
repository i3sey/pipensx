#include "app/autorun_service.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/nacp_language.hpp"
#include "app/task_files.hpp"
#include "core/metainfo.h"
#include "core/sha256.h"
#include "zip_fixture.hpp"
#include "install/install_backend.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pipensx;

namespace {

std::string createTempDir(const std::string& prefix) {
    const std::string path = "/tmp/" + prefix + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(path);
    return path;
}

void removeTempDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

void writeFile(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void test_path_helpers() {
    const std::string sd = "sdmc:";
    assert(autorunWineRoot(sd) == "sdmc:/switch/wine");
    assert(autorunRuntimeNroPath(sd) == "sdmc:/switch/wine/wine-nx-runtime.nro");
    assert(autorunGamesPath(sd) == "sdmc:/switch/wine/drive_c/Games");
    assert(autorunDesktopPath(sd) == "sdmc:/switch/wine/drive_c/users/wine/Desktop");
    assert(autorunDocumentsPath(sd) == "sdmc:/switch/wine/drive_c/users/wine/Documents");
    assert(autorunRegistryPath(sd) == "sdmc:/switch/wine/registry/user.reg");
    assert(autorunSystemRegistryPath(sd) == "sdmc:/switch/wine/registry/system.reg");
    assert(autorunVersionPath(sd) == "sdmc:/switch/wine/version.json");

    const std::string custom = "/tmp/switch_sd";
    assert(autorunWineRoot(custom) == "/tmp/switch_sd/switch/wine");
    assert(autorunGamesPath(custom) == "/tmp/switch_sd/switch/wine/drive_c/Games");
}

void test_environment_bootstrap_and_preflight() {
    const std::string sd = createTempDir("autorun_boot_test");

    // 1. Pre-flight when not installed
    assert(!isAutorunInstalled(sd));
    assert(installedAutorunVersion(sd).empty());

    // 2. Prepare environment
    std::string error;
    assert(prepareAutorunEnvironment(sd, error));
    assert(error.empty());

    assert(fs::is_directory(autorunGamesPath(sd)));
    assert(fs::is_directory(autorunDesktopPath(sd)));
    assert(fs::is_directory(autorunDocumentsPath(sd)));
    assert(fs::is_regular_file(autorunRegistryPath(sd)));
    assert(fs::is_regular_file(autorunSystemRegistryPath(sd)));

    const std::string ureg = readFile(autorunRegistryPath(sd));
    assert(ureg.find("WINE REGISTRY Version 2") != std::string::npos);
    assert(ureg.find("relative to \\\\User") != std::string::npos);

    const std::string sreg = readFile(autorunSystemRegistryPath(sd));
    assert(sreg.find("WINE REGISTRY Version 2") != std::string::npos);
    assert(sreg.find("relative to \\\\System") != std::string::npos);

    // 3. Fake runtime NRO
    writeFile(autorunRuntimeNroPath(sd), "fake-nro-content");
    assert(isAutorunInstalled(sd));
    assert(installedAutorunVersion(sd) == "custom");

    // 4. Version file
    writeFile(autorunVersionPath(sd), "{\"version\": \"1.0.4\"}");
    assert(installedAutorunVersion(sd) == "1.0.4");

    removeTempDir(sd);
}

void test_release_parsing_and_updates() {
    const std::string sd = createTempDir("autorun_update_test");
    writeFile(autorunRuntimeNroPath(sd), "nro");
    writeFile(autorunVersionPath(sd), "{\"version\": \"1.0.0\"}");

    const std::string releaseJson = R"({
        "tag_name": "v1.1.0",
        "draft": false,
        "prerelease": false,
        "assets": [
            {
                "name": "wine-nx-runtime.zip",
                "browser_download_url": "https://github.com/Pipen-dev/wine-nx/releases/download/v1.1.0/wine-nx-runtime.zip",
                "size": 10485760
            },
            {
                "name": "wine-nx-runtime.zip.sha256",
                "browser_download_url": "https://github.com/Pipen-dev/wine-nx/releases/download/v1.1.0/wine-nx-runtime.zip.sha256",
                "size": 64
            }
        ]
    })";

    AutorunReleaseInfo release;
    std::string error;
    assert(AutorunService::parseRelease(releaseJson, release, error));
    assert(release.version == "v1.1.0");
    assert(release.archiveUrl == "https://github.com/Pipen-dev/wine-nx/releases/download/v1.1.0/wine-nx-runtime.zip");
    assert(release.size == 10485760);

    // Mock fetcher returning the releaseJson
    AutorunService svc(sd, [releaseJson](const std::string&, size_t, std::string& body, std::string&) {
        body = releaseJson;
        return true;
    });

    AutorunUpdateCheck check = svc.checkForUpdates();
    assert(check.ok);
    assert(check.updateAvailable);
    assert(check.installedVersion == "1.0.0");
    assert(check.release.version == "v1.1.0");

    // Rejection on draft release
    const std::string draftJson = R"({"tag_name": "v1.2.0", "draft": true, "assets": []})";
    assert(!AutorunService::parseRelease(draftJson, release, error));

    removeTempDir(sd);
}

void test_runtime_download_cancellation() {
    const auto sd = createTempDir("runtime_cancel");
    std::atomic<bool> cancelled{true};
    unsigned calls = 0;
    AutorunService service(sd, {}, [&](const std::string&, const std::string& path,
                                      size_t, std::string&) {
        ++calls;
        writeFile(path, "partial archive");
        cancelled.store(true);
        return true;
    });
    AutorunReleaseInfo release;
    release.archiveUrl = "https://example.invalid/runtime.zip";
    std::string error;
    assert(!service.downloadAndInstallRuntime(release, error, cancelled));
    assert(calls == 0);
    cancelled.store(false);
    assert(!service.downloadAndInstallRuntime(release, error, cancelled));
    assert(calls == 1);
    assert(error == "Cancelled.");
    assert(!fs::exists(autorunWineRoot(sd) + "/.runtime_tmp.zip"));
    assert(!fs::exists(autorunVersionPath(sd)));
    removeTempDir(sd);
}

void test_validation() {
    // 1. Clean portable release with main executable
    std::vector<std::string> portable = {
        "FlatOut2/FlatOut2.exe",
        "FlatOut2/Savegame/options.dat",
        "FlatOut2/data/sounds.bfs",
        "FlatOut2/dxvk.conf"
    };
    AutorunValidation v1 = validateAutorunFilePaths(portable, "FlatOut2.exe");
    assert(v1.isPortable());
    assert(v1.result == AutorunValidationResult::ValidPortable);
    assert(v1.mainExecutable == "FlatOut2.exe");
    assert(v1.warning.empty());

    // 2. Installer detected (setup.exe)
    std::vector<std::string> installer = {
        "GameSetup/setup.exe",
        "GameSetup/data1.bin",
        "GameSetup/data2.bin"
    };
    AutorunValidation v2 = validateAutorunFilePaths(installer);
    assert(!v2.isPortable());
    assert(v2.result == AutorunValidationResult::InstallerDetected);
    assert(!v2.warning.empty());
    assert(v2.warning.find("installer") != std::string::npos);

    // 3. Disk image detected (.iso)
    std::vector<std::string> isoRelease = {
        "Game.iso",
        "Crack/keygen.exe",
        "instructions.txt"
    };
    AutorunValidation v3 = validateAutorunFilePaths(isoRelease);
    assert(!v3.isPortable());
    assert(v3.result == AutorunValidationResult::DiskImageDetected);
    assert(!v3.warning.empty());
    assert(v3.warning.find("disk image") != std::string::npos);

    // 4. Missing executable
    std::vector<std::string> noExe = {
        "game_data/sound.ogg",
        "game_data/textures.dat"
    };
    AutorunValidation v4 = validateAutorunFilePaths(noExe);
    assert(!v4.isPortable());
    assert(v4.result == AutorunValidationResult::NoExecutable);
}

void test_registry_merge_idempotent() {
    const std::string dir = createTempDir("reg_merge_test");
    const std::string regPath = dir + "/user.reg";

    const std::string tweak1 = R"(
[Software\\Bugbear\\FlatOut2]
"DisplayWidth"=dword:00000500
"DisplayHeight"=dword:000002d0
"Fullscreen"=dword:00000001
)";

    std::string error;
    // 1. Initial merge on non-existent file
    assert(mergeUserRegistry(regPath, tweak1, error));
    std::string content1 = readFile(regPath);
    assert(content1.find(R"([Software\\Bugbear\\FlatOut2])") != std::string::npos);
    assert(content1.find("DisplayWidth") != std::string::npos);

    // 2. Idempotent merge with same tweak - should skip duplicate
    assert(mergeUserRegistry(regPath, tweak1, error));
    std::string content2 = readFile(regPath);
    assert(content2 == content1);

    // 3. Merge second distinct tweak
    const std::string tweak2 = R"(
[Software\\THQ\\Barnyard]
"EnableGamepad"=dword:00000001
)";
    assert(mergeUserRegistry(regPath, tweak2, error));
    std::string content3 = readFile(regPath);
    assert(content3.find(R"([Software\\Bugbear\\FlatOut2])") != std::string::npos);
    assert(content3.find(R"([Software\\THQ\\Barnyard])") != std::string::npos);

    removeTempDir(dir);
}

void test_shell_link_generation() {
    const std::string target = "C:\\Games\\FlatOut2\\FlatOut2.exe";
    const std::string workdir = "C:\\Games\\FlatOut2";

    std::vector<uint8_t> link = generateShellLink(target, workdir);
    assert(link.size() > 76);

    // Header size (first 4 bytes)
    uint32_t headerSize = static_cast<uint32_t>(link[0]) |
                         (static_cast<uint32_t>(link[1]) << 8) |
                         (static_cast<uint32_t>(link[2]) << 16) |
                         (static_cast<uint32_t>(link[3]) << 24);
    assert(headerSize == 76);

    // LinkCLSID (offset 4, 16 bytes): 00021401-0000-0000-C000-000000000046
    const uint8_t expectedClsid[16] = {
        0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    assert(std::memcmp(link.data() + 4, expectedClsid, 16) == 0);

    // LinkFlags (offset 20): HasLinkInfo (0x02) | HasWorkingDir (0x10) | IsUnicode (0x80) = 0x92
    uint32_t flags = static_cast<uint32_t>(link[20]) |
                    (static_cast<uint32_t>(link[21]) << 8) |
                    (static_cast<uint32_t>(link[22]) << 16) |
                    (static_cast<uint32_t>(link[23]) << 24);
    assert((flags & 0x02) != 0); // HasLinkInfo
    assert((flags & 0x10) != 0); // HasWorkingDir
    assert((flags & 0x80) != 0); // IsUnicode

    // LinkInfo starts at offset 76
    uint32_t linkInfoSize = static_cast<uint32_t>(link[76]) |
                           (static_cast<uint32_t>(link[77]) << 8) |
                           (static_cast<uint32_t>(link[78]) << 16) |
                           (static_cast<uint32_t>(link[79]) << 24);
    assert(linkInfoSize > 28);

    // LocalBasePath inside LinkInfo
    const std::string linkStr(reinterpret_cast<char*>(link.data()), link.size());
    assert(linkStr.find("C:\\Games\\FlatOut2\\FlatOut2.exe") != std::string::npos);

    // Create desktop shortcut file
    const std::string dir = createTempDir("shortcut_test");
    std::string error;
    assert(createWineDesktopShortcut(dir, "FlatOut 2", "FlatOut2", "FlatOut2.exe", error));
    const std::string lnkPath = dir + "/FlatOut 2.lnk";
    assert(fs::is_regular_file(lnkPath));
    assert(fs::file_size(lnkPath) == link.size());

    removeTempDir(dir);
}

void test_full_game_deploy() {
    const std::string sd = createTempDir("full_deploy_test");
    std::string prepError;
    assert(prepareAutorunEnvironment(sd, prepError));

    AutorunProfile profile;
    profile.title = "Deus Ex: Human Revolution";
    profile.executable = "DXHR.exe";
    profile.targetFolder = "DeusExHR";
    profile.dxvkConf = "dxvk.enableAsync = true\nd3d9.maxFrameLatency = 1\n";
    profile.box64Conf = "BOX64_DYNAREC_BIGBLOCK=0\n";
    profile.registry = R"(
[Software\\Eidos\\Deus Ex: HR]
"ResolutionWidth"=dword:000003c0
"ResolutionHeight"=dword:0000021c
)";

    fs::create_directories(autorunGamesPath(sd) + "/" + profile.targetFolder);
    writeFile(autorunGamesPath(sd) + "/" + profile.targetFolder + "/" + profile.executable, "test executable");
    std::string error;
    assert(deployAutorunGame(sd, profile.targetFolder, profile.title, profile, error));
    assert(error.empty());

    // Check dxvk.conf
    const std::string dxvkPath = autorunGamesPath(sd) + "/DeusExHR/dxvk.conf";
    assert(fs::is_regular_file(dxvkPath));
    const std::string dxvkContent = readFile(dxvkPath);
    assert(dxvkContent.find("dxvk.enableAsync = true") != std::string::npos);

    // Check user.reg
    const std::string regContent = readFile(autorunRegistryPath(sd));
    assert(regContent.find(R"([Software\\Eidos\\Deus Ex: HR])") != std::string::npos);
    assert(regContent.find("ResolutionWidth") != std::string::npos);

    // Check desktop shortcut
    const std::string shortcut = autorunDesktopPath(sd) + "/Deus Ex: Human Revolution.lnk";
    assert(fs::is_regular_file(shortcut));

    // Profile save / load roundtrip
    const std::string appRoot = sd + "/pipensx";
    fs::create_directories(appRoot);
    const std::string taskId = "aabbccdd11223344556677889900aabbccddeeff";
    assert(saveAutorunProfile(appRoot, taskId, profile, error));

    AutorunProfile loaded;
    assert(loadAutorunProfile(appRoot, taskId, loaded, error));
    assert(loaded.title == profile.title);
    assert(loaded.executable == profile.executable);
    assert(loaded.targetFolder == profile.targetFolder);
    assert(loaded.dxvkConf == profile.dxvkConf);
    assert(loaded.box64Conf == profile.box64Conf);

    removeAutorunProfile(appRoot, taskId);
    assert(!loadAutorunProfile(appRoot, taskId, loaded, error));

    removeTempDir(sd);
}

void test_autorun_task_queue_and_paths() {
    const std::string root = createTempDir("dm_autorun_test");
    DownloadManager dm(root + "/pipensx", false);

    const std::string wineGames = root + "/wine/drive_c/Games";
    dm.setAutorunGamesRoot(wineGames);
    assert(dm.autorunGamesRoot() == wineGames);

    // TransferMode persistence
    assert(persistedMode(TransferMode::AutoRunInstall) == std::string("autorun"));
    assert(persistedMode("autorun") == TransferMode::AutoRunInstall);

    DownloadTask task;
    task.id = "10b83ef3082fa6cf29d81d227c3dafcfe5165c01";
    task.name = "FlatOut 2";
    task.status = DownloadStatus::Completed;
    task.mode = TransferMode::AutoRunInstall;
    task.dataPath = wineGames + "/FlatOut2";
    assert(taskReadyForSwitchDeploy(task));

    removeTempDir(root);
}

void test_catalog_autorun_parsing() {
    const std::string entryJson = R"({
        "title": "FlatOut 2",
        "type": "autorun",
        "category": "PC Ports",
        "info_hash": "10B83EF3082FA6CF29D81D227C3DAFCFE5165C01",
        "magnet": "magnet:?xt=urn:btih:10B83EF3082FA6CF29D81D227C3DAFCFE5165C01",
        "size": 2847592812,
        "autorun": {
            "executable": "FlatOut2.exe",
            "target_folder": "FlatOut2",
            "status": "playable",
            "fps": "60",
            "overclock": "1785/768/1900",
            "translator": "Box64",
            "renderer": "DXVK",
            "notes": "Stable 60 FPS",
            "dxvk_conf": "dxvk.enableAsync = true\n",
            "registry": "[Software\\Bugbear\\FlatOut2]\n\"Fullscreen\"=dword:1\n"
        }
    })";

    const auto root = nlohmann::json::parse(entryJson);
    CatalogEntry entry;
    entry.title = root.value("title", "");
    entry.type = root.value("type", "");
    if (root.contains("autorun") && root["autorun"].is_object()) {
        const auto& ar = root["autorun"];
        AutorunProfile profile;
        profile.title = entry.title;
        profile.executable = ar.value("executable", "");
        profile.targetFolder = ar.value("target_folder", "");
        profile.status = ar.value("status", "");
        profile.fps = ar.value("fps", "");
        profile.overclock = ar.value("overclock", "");
        profile.translator = ar.value("translator", "");
        profile.renderer = ar.value("renderer", "");
        profile.notes = ar.value("notes", "");
        profile.dxvkConf = ar.value("dxvk_conf", "");
        profile.registry = ar.value("registry", "");
        if (profile.valid()) {
            entry.autorun = std::move(profile);
        }
    }

    assert(entry.isAutorun());
    assert(entry.autorun.has_value());
    assert(entry.autorun->executable == "FlatOut2.exe");
    assert(entry.autorun->targetFolder == "FlatOut2");
    assert(entry.autorun->fps == "60");
    assert(entry.autorun->overclock == "1785/768/1900");
    assert(entry.autorun->translator == "Box64");
}

void test_title_id_and_forwarder_config() {
    // 1. Title ID generation
    uint64_t id1 = generateAutorunTitleId("FlatOut 2");
    uint64_t id2 = generateAutorunTitleId("flatout 2");
    assert(id1 == id2);
    assert((id1 >> 32) == 0x0100BEEF);
    assert((id1 & 0xFFFFULL) == 0x0000ULL);

    std::string hex1 = autorunTitleIdToString(id1);
    assert(hex1.size() == 16);
    assert(hex1.substr(0, 8) == "0100BEEF");
    assert(hex1.substr(12, 4) == "0000");

    uint64_t idDeus = generateAutorunTitleId("Deus Ex: Human Revolution");
    assert(idDeus != id1);
    assert((idDeus >> 32) == 0x0100BEEF);

    // 2. Forwarder INI Config
    const std::string sd = createTempDir("forwarder_cfg_test");
    std::string error;
    assert(createAutorunForwarderConfig(sd, id1, "FlatOut 2", "FlatOut2", "FlatOut2.exe", error));
    assert(error.empty());

    std::string cfgPath = autorunForwarderConfigPath(sd, id1);
    assert(fs::is_regular_file(cfgPath));

    std::string title, folder, exe, nro, argv;
    assert(readAutorunForwarderConfig(sd, id1, title, folder, exe, nro, argv, error));
    assert(title == "FlatOut 2");
    assert(folder == "FlatOut2");
    assert(exe == "FlatOut2.exe");
    assert(nro == autorunRuntimeNroPath(sd));
    assert(argv.find(autorunGamesPath(sd) + "/FlatOut2/FlatOut2.exe") != std::string::npos);

    assert(removeAutorunForwarderConfig(sd, id1, error));
    assert(!fs::exists(cfgPath));

    removeTempDir(sd);
}

void test_sys_clk_profile_parsing_and_config() {
    // 1. Parsing
    SysClkProfile p1;
    assert(parseOverclockProfile("1785/768/1900", p1));
    assert(p1.cpu == 1785);
    assert(p1.gpu == 768);
    assert(p1.mem == 1900);

    SysClkProfile p2;
    assert(parseOverclockProfile("1963 / 1152 / 2133", p2));
    assert(p2.cpu == 1963);
    assert(p2.gpu == 1152);
    assert(p2.mem == 2133);

    SysClkProfile p3;
    assert(parseOverclockProfile("CPU 1785, GPU 614, RAM 1900", p3));
    assert(p3.cpu == 1785);
    assert(p3.gpu == 614);
    assert(p3.mem == 1900);

    SysClkProfile p4;
    assert(parseOverclockProfile("Safe Max (CPU 1785, GPU 614, RAM 1900)", p4));
    assert(p4.cpu == 1785);
    assert(p4.gpu == 614);
    assert(p4.mem == 1900);

    SysClkProfile p5;
    assert(parseOverclockProfile("cpu=1900, gpu=844, mem=2000", p5));
    assert(p5.cpu == 1900);
    assert(p5.gpu == 844);
    assert(p5.mem == 2000);

    SysClkProfile pDef;
    assert(parseOverclockProfile("Safe Max", pDef));
    assert(pDef.cpu == 1785);
    assert(pDef.gpu == 768);
    assert(pDef.mem == 1900);

    // 2. sys-clk config.ini manipulation
    const std::string sd = createTempDir("sys_clk_cfg_test");
    uint64_t tid = generateAutorunTitleId("FlatOut 2");

    assert(!isSysClkInstalled(sd));
    assert(!hasSysClkProfile(sd, tid));

    std::string error;
    // Apply profile
    assert(applySysClkProfile(sd, tid, p1, error));
    assert(isSysClkInstalled(sd));
    assert(hasSysClkProfile(sd, tid));

    const std::string iniContent = readFile(autorunSysClkConfigPath(sd));
    assert(iniContent.find("[" + autorunTitleIdToString(tid) + "]") != std::string::npos);
    assert(iniContent.find("handheld_cpu=1785") != std::string::npos);
    assert(iniContent.find("docked_gpu=768") != std::string::npos);

    // Update profile idempotently
    SysClkProfile pUpdate{1963, 1152, 2133};
    assert(applySysClkProfile(sd, tid, pUpdate, error));
    const std::string iniUpdated = readFile(autorunSysClkConfigPath(sd));
    assert(iniUpdated.find("handheld_cpu=1963") != std::string::npos);
    assert(iniUpdated.find("handheld_cpu=1785") == std::string::npos);

    // Remove profile
    assert(removeSysClkProfile(sd, tid, error));
    assert(!hasSysClkProfile(sd, tid));
    const std::string iniRemoved = readFile(autorunSysClkConfigPath(sd));
    assert(iniRemoved.find("[" + autorunTitleIdToString(tid) + "]") == std::string::npos);

    removeTempDir(sd);
}

void test_nacp_and_pfs0() {
    // 1. NACP generation
    const std::string gameTitle = "Barnyard";
    const std::string author = "AutoRun / pipensx";
    std::vector<uint8_t> nacp = generateNacp(gameTitle, author, "1.0.0");
    assert(nacp.size() == 0x4000);

    std::string parsedName, parsedAuthor;
    assert(nacpReadLanguage(nacp.data(), nacp.size(), 0, parsedName, parsedAuthor));
    assert(parsedName == gameTitle);
    assert(parsedAuthor == author);

    // 2. PFS0 build and parse
    std::vector<uint8_t> data1 = {'H', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> data2 = {'W', 'o', 'r', 'l', 'd', '!'};
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
        { "first.txt", data1 },
        { "sub/second.bin", data2 }
    };
    std::vector<uint8_t> pfs0 = buildPfs0(files);
    assert(pfs0.size() > 16);
    assert(pfs0[0] == 'P' && pfs0[1] == 'F' && pfs0[2] == 'S' && pfs0[3] == '0');

    std::vector<std::pair<std::string, std::vector<uint8_t>>> parsed;
    assert(parsePfs0(pfs0.data(), pfs0.size(), parsed));
    assert(parsed.size() == 2);
    assert(parsed[0].first == "first.txt");
    assert(parsed[0].second == data1);
    assert(parsed[1].first == "sub/second.bin");
    assert(parsed[1].second == data2);
}

void test_missing_and_nested_executable() {
    const auto sd = createTempDir("autorun_executable");
    AutorunProfile profile;
    profile.targetFolder = "FTL";
    profile.executable = "FTLGame.exe";
    profile.translator = "Box64";
    profile.renderer = "DXVK";
    std::string error;
    assert(!deployAutorunGame(sd, "FTL", "FTL", profile, error));
    assert(error.find("FTLGame.exe") != std::string::npos);
    assert(!fs::exists(autorunWineRoot(sd) + "/target.txt"));
    assert(!fs::exists(autorunWineRoot(sd) + "/launcher-library-v2.ini"));
    // A directory named like the expected executable is not a game binary.
    fs::create_directories(autorunGamesPath(sd) + "/FTL/FTLGame.exe");
    assert(resolveAutorunExecutable(sd, "FTL", "FTLGame.exe").empty());
    fs::remove_all(autorunGamesPath(sd) + "/FTL/FTLGame.exe");
    fs::create_directories(autorunGamesPath(sd) + "/FTL/release/bin");
    writeFile(autorunGamesPath(sd) + "/FTL/release/bin/ftlgame.EXE", "test executable");
    assert(resolveAutorunExecutable(sd, "FTL", "FTLGame.exe") == "release/bin/ftlgame.EXE");
    error.clear();
    assert(deployAutorunGame(sd, "FTL", "FTL", profile, error));
    assert(readFile(autorunWineRoot(sd) + "/target.txt") ==
        autorunGamesPath(sd) + "/FTL/release/bin/ftlgame.EXE\n");
    const auto settings = autorunGamesPath(sd) + "/FTL/release/bin/ftlgame.wine-nx.txt";
    assert(readFile(settings) == "cpu=box64\nd3d=dxvk\n");
    writeFile(settings, "# user settings\n CPU =fex\nd3d9=wine\ntitle=My FTL\n");
    assert(deployAutorunGame(sd, "FTL", "FTL", profile, error));
    assert(readFile(settings) == "# user settings\n CPU =fex\nd3d9=wine\ntitle=My FTL\n");
    const auto expected = generateShellLink("C:\\Games\\FTL\\release/bin/ftlgame.EXE",
        "C:\\Games\\FTL\\release/bin");
    const auto shortcut = readFile(autorunDesktopPath(sd) + "/FTL.lnk");
    assert(shortcut.size() == expected.size());
    assert(!std::memcmp(shortcut.data(), expected.data(), expected.size()));
    removeTempDir(sd);
}

void test_box64_profile_deployment() {
    const auto sd = createTempDir("autorun_box64_profile");
    const auto folder = autorunGamesPath(sd) + "/FTL/release";
    writeFile(folder + "/FTLGame.exe", "test executable");
    AutorunProfile profile;
    profile.executable = "FTLGame.exe";
    profile.targetFolder = "FTL";
    profile.box64Conf = "# packed DLLs\r\n BOX64_DYNAREC_BIGBLOCK = 0\r\n"
        "BOX64_DYNAREC_CALLRET=0\nBOX64_DYNAREC_SAFEFLAGS=2\n";
    std::string error;
    assert(deployAutorunGame(sd, "FTL", "FTL", profile, error));
    const auto path = folder + "/FTLGame.box64.txt";
    assert(readFile(path) == "BOX64_DYNAREC_BIGBLOCK=0\nBOX64_DYNAREC_CALLRET=0\nBOX64_DYNAREC_SAFEFLAGS=2\n");
    writeFile(path, "# user override\n BOX64_DYNAREC_BIGBLOCK = 1\n");
    assert(deployAutorunGame(sd, "FTL", "FTL", profile, error));
    const auto expected = "# user override\n BOX64_DYNAREC_BIGBLOCK = 1\n"
        "BOX64_DYNAREC_CALLRET=0\nBOX64_DYNAREC_SAFEFLAGS=2\n";
    assert(readFile(path) == expected);
    assert(deployAutorunGame(sd, "FTL", "FTL", profile, error));
    assert(readFile(path) == expected);
    for (const auto* invalid : { "BOX64_DYNAREC_CALLRET=\r\n", "BOX64_DYNAREC_CALLRET=abc",
                                "BOX64_DYNAREC_CALLRET=-1", "bad=value", "no-equals" }) {
        profile.box64Conf = invalid;
        error.clear();
        assert(!deployAutorunGame(sd, "FTL", "FTL", profile, error));
        assert(error.find("Box64") != std::string::npos);
        assert(readFile(path) == expected);
    }
    // Old saved profiles lack the new field and retain all runtime settings.
    const auto appRoot = sd + "/pipensx";
    writeFile(appRoot + "/autorun-old.json", "{\"executable\":\"FTLGame.exe\",\"target_folder\":\"FTL\"}");
    assert(loadAutorunProfile(appRoot, "old", profile, error));
    assert(profile.box64Conf.empty());
    assert(deployAutorunGame(sd, "FTL", "FTL", profile, error));
    assert(readFile(path) == expected);
    removeTempDir(sd);
}

std::string dependencyHash(const std::string& data) {
    uint8_t digest[32];
    sha256(data.data(), data.size(), digest);
    char hex[65];
    for (size_t i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

void test_verified_dependency_updates() {
    const auto root = createTempDir("autorun_dependencies");
    const std::string original = "old packed x86 library", replacement = "new x86 library";
    const auto archive = zip_fixture::build({{"x64/bass.dll", "wrong architecture"},
        {"bass.dll", replacement, true}, {"../outside.txt", "must not be extracted"}}, false, 0, false);
    AutorunDependencyFix fix{"bass.dll", dependencyHash(original), dependencyHash(replacement),
        "https://publisher.invalid/bass24.zip", dependencyHash(archive)};
    unsigned fetches = 0;
    auto fetch = [&](const std::string& url, const std::string& path, size_t limit, std::string&) {
        ++fetches;
        assert(url == fix.archiveUrl && limit >= archive.size());
        writeFile(path, archive);
        return true;
    };
    std::string error;
    writeFile(root + "/bass.dll", original);
    assert(installAutorunDependencyFixes(root, {fix}, fetch, error));
    assert(readFile(root + "/bass.dll") == replacement);
    assert(readFile(root + "/bass.dll.pipensx-original") == original);
    assert(!fs::exists(root + "/outside.txt") && !fs::exists(root + "/x64"));
    assert(fetches == 1);
    assert(installAutorunDependencyFixes(root, {fix}, fetch, error));
    assert(fetches == 1); // already fixed, no download
    writeFile(root + "/bass.dll", original);
    assert(installAutorunDependencyFixes(root, {fix}, {}, error)); // verified cache, offline
    assert(readFile(root + "/bass.dll") == replacement);
    writeFile(root + "/bass.dll", "user custom library");
    assert(installAutorunDependencyFixes(root, {fix}, fetch, error));
    assert(readFile(root + "/bass.dll") == "user custom library" && fetches == 1);
    fs::remove(root + "/bass.dll"); // interrupted FAT replacement, recover using backup
    assert(installAutorunDependencyFixes(root, {fix}, {}, error));
    assert(readFile(root + "/bass.dll") == replacement);

    // Corrupt download, selected DLL, or original backup must not replace a DLL.
    for (int corrupt = 0; corrupt < 3; ++corrupt) {
        const auto dir = root + "/corrupt" + std::to_string(corrupt);
        writeFile(dir + "/bass.dll", original);
        auto rule = fix;
        if (corrupt == 0) rule.archiveSha256 = std::string(64, '0');
        if (corrupt == 1) rule.replacementSha256 = std::string(64, '0');
        if (corrupt == 2) writeFile(dir + "/bass.dll.pipensx-original", "unrelated backup");
        assert(!installAutorunDependencyFixes(dir, {rule}, fetch, error));
        assert(readFile(dir + "/bass.dll") == original);
        assert(!fs::exists(dir + "/bass.dll.pipensx-new"));
    }
    const auto pair = root + "/pair";
    writeFile(pair + "/bass.dll", original);
    writeFile(pair + "/bassmix.dll", original);
    auto mixer = fix; mixer.fileName = "bassmix.dll";
    // First DLL is prepared successfully; failure on the second leaves both old.
    assert(!installAutorunDependencyFixes(pair, {fix, mixer}, fetch, error));
    assert(readFile(pair + "/bass.dll") == original);
    assert(readFile(pair + "/bassmix.dll") == original);
    assert(!fs::exists(pair + "/bass.dll.pipensx-new"));
    assert(!fs::exists(pair + "/bassmix.dll.pipensx-new"));
    removeTempDir(root);
}

void test_stub_nsp_generation_and_install() {
    const std::string sd = createTempDir("stub_nsp_test");
    const std::string gameTitle = "FlatOut 2";
    uint64_t tid = generateAutorunTitleId(gameTitle);

    // PC cannot derive the console's NCA header key; it must never report a
    // fabricated, uninstallable package as a successful forwarder.
    assert(generateAutorunStubNsp(gameTitle, tid).empty());
    fs::create_directories(autorunGamesPath(sd) + "/FlatOut2");
    writeFile(autorunGamesPath(sd) + "/FlatOut2/FlatOut2.exe", "test executable");

    // 2. Install forwarder via InstallBackend
    auto backend = install::createInstallBackend(sd, install::InstallStorageTarget::SdCard);
    assert(backend != nullptr);

    std::string error;
    assert(!installAutorunForwarder(sd, *backend, gameTitle, "FlatOut2", "FlatOut2.exe", {}, error));
    assert(!error.empty());

    // Verify forwarder config was created
    assert(!fs::exists(autorunForwarderConfigPath(sd, tid)));

    removeTempDir(sd);
}

} // namespace

int main() {
    test_path_helpers();
    test_environment_bootstrap_and_preflight();
    test_release_parsing_and_updates();
    test_runtime_download_cancellation();
    test_validation();
    test_registry_merge_idempotent();
    test_shell_link_generation();
    test_full_game_deploy();
    test_autorun_task_queue_and_paths();
    test_catalog_autorun_parsing();
    test_title_id_and_forwarder_config();
    test_sys_clk_profile_parsing_and_config();
    test_nacp_and_pfs0();
    test_missing_and_nested_executable();
    test_box64_profile_deployment();
    test_verified_dependency_updates();
    test_stub_nsp_generation_and_install();
    std::printf("autorun tests passed\n");
    return 0;
}
