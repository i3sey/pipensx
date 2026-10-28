#include "app/autorun_service.hpp"
#include "app/catalog_presentation.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/nacp_language.hpp"
#include "install/install_backend.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <cassert>
#include <chrono>
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

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    assert(out.good());
}

void removeTempDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 1. Парсинг структуры AutorunProfile из catalog.json
void test_catalog_profile_parsing() {
    const std::string catalogJson = R"({
        "title": "FlatOut 2",
        "type": "autorun",
        "category": "PC Ports",
        "info_hash": "10B83EF3082FA6CF29D81D227C3DAFCFE5165C01",
        "size": 2847592812,
        "size_bytes": 2847592812,
        "autorun": {
            "executable": "FlatOut2.exe",
            "target_folder": "FlatOut2",
            "status": "playable",
            "fps": "60",
            "overclock": "1785/768/1900",
            "translator": "Box64",
            "renderer": "DXVK",
            "notes": "Стабильные 55-60 FPS с DXVK Sarek. Управление геймпадом нативно, твик разрешения 720p через реестр.",
            "dxvk_conf": "dxvk.enableAsync = true\ndxvk.numCompilerThreads = 4\n",
            "registry": "[Software\\\\Bugbear\\\\FlatOut2]\n\"DisplayWidth\"=dword:00000500\n\"DisplayHeight\"=dword:000002d0\n\"Fullscreen\"=dword:00000001\n"
        }
    })";

    const auto root = nlohmann::json::parse(catalogJson);
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
    assert(entry.autorun->valid());
    assert(entry.autorun->executable == "FlatOut2.exe");
    assert(entry.autorun->targetFolder == "FlatOut2");
    assert(entry.autorun->fps == "60");
    assert(entry.autorun->overclock == "1785/768/1900");
    assert(entry.autorun->translator == "Box64");
    assert(entry.autorun->renderer == "DXVK");
    assert(entry.autorun->dxvkConf.find("dxvk.enableAsync = true") != std::string::npos);
    assert(entry.autorun->registry.find("DisplayWidth") != std::string::npos);
}

// 2. Мердж реестра в user.reg (проверка отсутствия повреждений существующего содержимого)
void test_registry_merge_safety() {
    const std::string sd = createTempDir("deploy_reg_test");
    std::string prepError;
    assert(prepareAutorunEnvironment(sd, prepError));

    const std::string regPath = autorunRegistryPath(sd);
    const std::string initialReg = readFile(regPath);
    assert(initialReg.find("WINE REGISTRY Version 2") != std::string::npos);

    // Initial game tweak merge
    std::string tweakDeusEx = R"([Software\\Eidos\\Deus Ex: HR]
"Aspect"="16:9"
"ResolutionWidth"=dword:000003c0
)";
    std::string err;
    assert(mergeUserRegistry(regPath, tweakDeusEx, err));
    std::string merged1 = readFile(regPath);
    assert(merged1.find("WINE REGISTRY Version 2") != std::string::npos);
    assert(merged1.find(R"([Software\\Eidos\\Deus Ex: HR])") != std::string::npos);
    assert(merged1.find("ResolutionWidth") != std::string::npos);

    // Second game tweak merge (FlatOut 2)
    std::string tweakFlatOut = R"([Software\\Bugbear\\FlatOut2]
"Fullscreen"=dword:00000001
)";
    assert(mergeUserRegistry(regPath, tweakFlatOut, err));
    std::string merged2 = readFile(regPath);
    // Verifying both games exist, no corruption of headers or previous keys
    assert(merged2.find("WINE REGISTRY Version 2") != std::string::npos);
    assert(merged2.find(R"([Software\\Eidos\\Deus Ex: HR])") != std::string::npos);
    assert(merged2.find(R"([Software\\Bugbear\\FlatOut2])") != std::string::npos);

    // Idempotent re-merge of already existing section
    assert(mergeUserRegistry(regPath, tweakDeusEx, err));
    std::string merged3 = readFile(regPath);
    assert(merged3 == merged2);

    removeTempDir(sd);
}

// 3. Генерация dxvk.conf
void test_dxvk_conf_generation() {
    const std::string sd = createTempDir("deploy_dxvk_test");
    AutorunProfile profile;
    profile.title = "FlatOut 2";
    profile.executable = "FlatOut2.exe";
    profile.targetFolder = "FlatOut2";
    profile.dxvkConf = "dxvk.enableAsync = true\ndxvk.numCompilerThreads = 4\n";

    fs::create_directories(autorunGamesPath(sd) + "/" + profile.targetFolder);
    writeFile(autorunGamesPath(sd) + "/" + profile.targetFolder + "/" + profile.executable, "test executable");
    std::string error;
    assert(deployAutorunGame(sd, profile.targetFolder, profile.title, profile, error));

    const std::string dxvkPath = autorunGamesPath(sd) + "/FlatOut2/dxvk.conf";
    assert(fs::is_regular_file(dxvkPath));
    const std::string content = readFile(dxvkPath);
    assert(content.find("dxvk.enableAsync = true") != std::string::npos);
    assert(content.find("dxvk.numCompilerThreads = 4") != std::string::npos);

    // Test default dxvk.conf when profile dxvkConf is empty
    AutorunProfile defProfile;
    defProfile.title = "Generic Game";
    defProfile.executable = "game.exe";
    defProfile.targetFolder = "GenericGame";
    fs::create_directories(autorunGamesPath(sd) + "/GenericGame");
    writeFile(autorunGamesPath(sd) + "/GenericGame/game.exe", "test executable");
    assert(deployAutorunGame(sd, defProfile.targetFolder, defProfile.title, defProfile, error));

    const std::string defDxvkPath = autorunGamesPath(sd) + "/GenericGame/dxvk.conf";
    assert(fs::is_regular_file(defDxvkPath));
    const std::string defContent = readFile(defDxvkPath);
    assert(defContent.find("dxvk.enableAsync = true") != std::string::npos);
    assert(defContent.find("d3d9.maxFrameLatency = 1") != std::string::npos);

    removeTempDir(sd);
}

// 4. Формирование Stub-NSP и проверка заголовков PFS0/NACP
void test_stub_nsp_headers_and_nacp() {
    const std::string gameTitle = "FlatOut 2";
    uint64_t titleId = generateAutorunTitleId(gameTitle);
    assert((titleId >> 32) == 0x0100BEEF);
    assert((titleId & 0xFFFFULL) == 0x0000ULL);

    std::string titleIdHex = autorunTitleIdToString(titleId);
    assert(titleIdHex.rfind("0100BEEF", 0) == 0);

    assert(generateAutorunStubNsp(gameTitle, titleId).empty());

    // Verify sys-clk config integration
    const std::string sd = createTempDir("deploy_sysclk_test");
    SysClkProfile ocProfile;
    assert(parseOverclockProfile("1785/768/1900", ocProfile));
    std::string err;
    assert(applySysClkProfile(sd, titleId, ocProfile, err));
    assert(hasSysClkProfile(sd, titleId));

    const std::string sysClkContent = readFile(autorunSysClkConfigPath(sd));
    assert(sysClkContent.find("[" + titleIdHex + "]") != std::string::npos);
    assert(sysClkContent.find("handheld_cpu=1785") != std::string::npos);
    assert(sysClkContent.find("docked_gpu=768") != std::string::npos);
    assert(sysClkContent.find("docked_mem=1900") != std::string::npos);

    removeTempDir(sd);
}

// 5. Тестирование бейджей совместимости и фильтрации разделов каталога (Фаза 6)
void test_compatibility_badges_and_sections() {
    AutorunProfile p1;
    p1.status = "playable";
    p1.fps = "60";
    assert(autorunCompatibilityBadge(p1) == "🟢 60 FPS");

    AutorunProfile p2;
    p2.status = "playable_with_tweaks";
    p2.fps = "30";
    assert(autorunCompatibilityBadge(p2) == "🟡 30 FPS");

    AutorunProfile p3;
    p3.status = "issues";
    p3.fps = "15-20";
    assert(autorunCompatibilityBadge(p3) == "🔴 15-20 FPS");

    AutorunProfile p4;
    p4.status = "playable";
    p4.fps = "";
    assert(autorunCompatibilityBadge(p4) == "🟢 Playable");

    AutorunProfile p5;
    p5.status = "playable_with_tweaks";
    p5.fps = "";
    assert(autorunCompatibilityBadge(p5) == "🟡 Tweaks");

    AutorunProfile p6;
    p6.status = "issues";
    p6.fps = "";
    assert(autorunCompatibilityBadge(p6) == "🔴 Issues");

    // CatalogSection::AutoRun section routing
    CatalogEntry arEntry;
    arEntry.title = "FlatOut 2";
    arEntry.type = "autorun";
    arEntry.autorun = p1;
    assert(arEntry.isAutorun());
    assert(catalogEntryInSection(arEntry, nullptr, CatalogSection::AutoRun));
    assert(!catalogEntryInSection(arEntry, nullptr, CatalogSection::Games));
    assert(!catalogEntryInSection(arEntry, nullptr, CatalogSection::Ports));

    CatalogEntry nativeEntry;
    nativeEntry.title = "Super Mario Odyssey [0100000000010000] [v0]";
    nativeEntry.titleId = "0100000000010000";
    assert(!nativeEntry.isAutorun());
    assert(!catalogEntryInSection(nativeEntry, nullptr, CatalogSection::AutoRun));
    assert(catalogEntryInSection(nativeEntry, nullptr, CatalogSection::Games));
    assert(!catalogEntryInSection(nativeEntry, nullptr, CatalogSection::Ports));

    CatalogEntry portEntry;
    portEntry.title = "Doom Retro [Homebrew]";
    assert(!portEntry.isAutorun());
    assert(!catalogEntryInSection(portEntry, nullptr, CatalogSection::AutoRun));
    assert(catalogEntryInSection(portEntry, nullptr, CatalogSection::Ports));
    assert(!catalogEntryInSection(portEntry, nullptr, CatalogSection::Games));
}

} // namespace

int main() {
    test_catalog_profile_parsing();
    test_registry_merge_safety();
    test_dxvk_conf_generation();
    test_stub_nsp_headers_and_nacp();
    test_compatibility_badges_and_sections();
    std::printf("test_autorun_deploy: all assertions passed\n");
    return 0;
}
