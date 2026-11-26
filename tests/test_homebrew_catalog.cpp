#include "app/homebrew_catalog.hpp"
#include "app/catalog_urls.hpp"
#include "core/sha256.h"
#include "zip_fixture.hpp"
#include <borealis/extern/nlohmann/json.hpp>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace pipensx;
using Json = nlohmann::json;

extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* source, const char* destination) {
    struct stat st{};
    if (lstat(destination, &st) == 0) { errno = EEXIST; return -1; }
    return __real_rename(source, destination);
}

static std::string hashOf(const std::string& body) {
    uint8_t digest[32];
    sha256(body.data(), body.size(), digest);
    std::string result;
    for (auto byte : digest) { char hex[3]; std::snprintf(hex, sizeof(hex), "%02x", byte); result += hex; }
    return result;
}

static Json manifest(const std::string& body, bool zip = false) {
    return {{"format", 1}, {"hidden", {"hidden-app"}}, {"packages", Json::array({{
        {"name", "test-app"}, {"title", "Test App"}, {"author", "Developer"},
        {"version", "1.2"}, {"category", "tool"}, {"description", "English"},
        {"description_i18n", {{"ru", "Описание"}}},
        {"download_url", "https://example.com/app"}, {"download_type", zip ? "zip" : "file"},
        {"sha256", hashOf(body)}, {"filesize", body.size()}, {"extracted", zip ? 128 : body.size()},
        {"icon_url", "homebrew/icons/test-app.jpg"},
        {"files", zip ? Json({"U: switch/test-app/test.nro", "G: config/test-app/config.ini"})
                      : Json({"U: switch/test-app/test.nro"})}
    }})}};
}

static std::string read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

static void writeFixture(const std::string& path, const std::string& data) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), data.size());
    assert(out.good());
}

static std::string makeSyntheticNro(const std::string& title,
                                    const std::string& author,
                                    const std::string& version,
                                    const std::string& iconBytes = {}) {
    constexpr size_t kNroSize = 0x80;
    constexpr size_t kAsetHeader = 0x38;
    constexpr size_t kNacpSize = 0x4000;
    const size_t iconOffset = kAsetHeader + kNacpSize;
    std::string buf(kNroSize + iconOffset + iconBytes.size(), '\0');
    buf.replace(0x10, 4, "NRO0");
    buf[0x18] = static_cast<char>(kNroSize);
    buf.replace(kNroSize, 4, "ASET");
    if (!iconBytes.empty()) {
        const uint64_t rel = iconOffset;
        const uint64_t sz = iconBytes.size();
        for (int i = 0; i < 8; ++i) {
            buf[kNroSize + 0x08 + i] = static_cast<char>((rel >> (8 * i)) & 0xFF);
            buf[kNroSize + 0x10 + i] = static_cast<char>((sz >> (8 * i)) & 0xFF);
        }
        buf.replace(kNroSize + iconOffset, iconBytes.size(), iconBytes);
    }
    buf[kNroSize + 0x18] = kAsetHeader;
    buf[kNroSize + 0x21] = kNacpSize >> 8;
    const size_t nacp = kNroSize + kAsetHeader;
    buf.replace(nacp, title.size(), title);
    buf.replace(nacp + 0x200, author.size(), author);
    buf.replace(nacp + 0x3060, version.size(), version);
    return buf;
}

static const HomebrewPackage* findByInstalledPath(const std::vector<HomebrewPackage>& pkgs,
                                                  const std::string& relPath) {
    for (const auto& pkg : pkgs) {
        if (pkg.installedPath == relPath) return &pkg;
    }
    return nullptr;
}

static void testNroDiscovery(const std::string& root) {
    const std::string sdRoot = root + "/sdroot";
    const std::string appRoot = root + "/approot";
    const std::string jpegV1 = "\xFF\xD8\xFF\xE0" "FAKEJPEG1";
    const std::string jpegV2 = "\xFF\xD8\xFF\xE0" "FAKEJPEG_UPDATED";

    writeFixture(sdRoot + "/switch/raw/plain.nro", "not an nro");
    writeFixture(sdRoot + "/switch/deep/sub/local_tool.NRO",
              makeSyntheticNro("Local Tool", "Indie Dev", "3.4.5", jpegV1));
    writeFixture(sdRoot + "/switch/custom/moved_app.NRO",
              makeSyntheticNro("Test App", "Developer", "0.9", jpegV1));
    writeFixture(sdRoot + "/switch/ambig/dup.nro",
              makeSyntheticNro("Ambiguous App", "Unknown", "1.0", "NOT_AN_IMAGE"));
    assert(symlink((sdRoot + "/switch/deep/sub/local_tool.NRO").c_str(),
                   (sdRoot + "/switch/linked.nro").c_str()) == 0);
    assert(symlink((sdRoot + "/switch/deep").c_str(),
                   (sdRoot + "/switch/linked_dir").c_str()) == 0);

    HomebrewCatalog catalog(appRoot, sdRoot);
    std::vector<HomebrewPackage> installed;
    std::string error;
    assert(catalog.loadInstalled(installed, "en", error));
    assert(installed.size() == 4);
    for (const auto& package : installed) assert(package.downloadUrl.empty());
    const auto* local = findByInstalledPath(installed, "switch/deep/sub/local_tool.NRO");
    assert(local && local->title == "Local Tool" && local->author == "Indie Dev");
    assert(local->installedVersion == "3.4.5");
    assert(!local->iconUrl.empty() && read(local->iconUrl) == jpegV1);
    const auto* raw = findByInstalledPath(installed, "switch/raw/plain.nro");
    assert(raw && !raw->title.empty() && raw->installedVersion.empty() && raw->iconUrl.empty());
    const auto* invalidImg = findByInstalledPath(installed, "switch/ambig/dup.nro");
    assert(invalidImg && invalidImg->iconUrl.empty());

    // Updating the NRO icon on disk refreshes the cached icon even when destination exists.
    writeFixture(sdRoot + "/switch/deep/sub/local_tool.NRO",
                 makeSyntheticNro("Local Tool", "Indie Dev", "3.4.6", jpegV2));
    assert(catalog.loadInstalled(installed, "en", error));
    local = findByInstalledPath(installed, "switch/deep/sub/local_tool.NRO");
    assert(local && local->installedVersion == "3.4.6");
    assert(!local->iconUrl.empty() && read(local->iconUrl) == jpegV2);

    Json cache = manifest("test NRO payload");
    for (const std::string id : {"ambig-a", "ambig-b"}) {
        auto entry = cache["packages"][0];
        entry["name"] = id;
        entry["title"] = "Ambiguous App";
        entry["author"] = "Unknown";
        entry["files"] = Json::array({"U: switch/" + id + "/app.nro"});
        cache["packages"].push_back(std::move(entry));
    }
    writeFixture(appRoot + "/homebrew/catalog.json", cache.dump());
    assert(catalog.loadInstalled(installed, "en", error));
    assert(installed.size() == 4);
    const auto* matched = findByInstalledPath(installed, "switch/custom/moved_app.NRO");
    assert(matched && matched->name == "test-app" && matched->version == "1.2");
    assert(matched->installedVersion == "0.9" && matched->downloadUrl == "https://example.com/app");
    assert(matched->iconUrl == std::string(kCatalogGitLabBaseUrl) + "homebrew/icons/test-app.jpg");
    const auto* ambiguous = findByInstalledPath(installed, "switch/ambig/dup.nro");
    assert(ambiguous && ambiguous->downloadUrl.empty() && ambiguous->title == "Ambiguous App");

    // Monogram normalization and update version comparison.
    assert(homebrewMonogram("mGBA") == "M");
    assert(homebrewMonogram("  daybreak") == "D");
    assert(homebrewMonogram("тест") == "Т");
    assert(homebrewMonogram("ёжик") == "Ё");
    assert(homebrewMonogram("") == "?");
    assert(homebrewMonogram("   ") == "?");

    assert(isHomebrewUpdateAvailable("1.5.4", "1.5.5"));
    assert(isHomebrewUpdateAvailable("v2.9.0", "2.10.0"));
    assert(isHomebrewUpdateAvailable("921", "925"));
    assert(isHomebrewUpdateAvailable("1.0.0-rc1", "1.0.0"));
    assert(!isHomebrewUpdateAvailable("1.5.4", "1.5.4"));
    assert(!isHomebrewUpdateAvailable("v1.5.4", "1.5.4"));
    assert(!isHomebrewUpdateAvailable("2.23.3", "2.23.2"));
    assert(!isHomebrewUpdateAvailable("921", "921"));
    assert(!isHomebrewUpdateAvailable("1.0", "1.0.0"));
    assert(!isHomebrewUpdateAvailable("", "1.0.0"));
    assert(!isHomebrewUpdateAvailable("nightly", "1.0.0"));
}

int main(int argc, char** argv) {
    const std::string payload = "test NRO payload";
    const Json good = manifest(payload);
    std::vector<HomebrewPackage> packages;
    std::string error;
    assert(HomebrewCatalog::parse(good.dump(), "ru", kCatalogGitLabBaseUrl, packages, error));
    assert(packages.size() == 1 && packages[0].description == "Описание");
    assert(packages[0].iconUrl == std::string(kCatalogGitLabBaseUrl) + "homebrew/icons/test-app.jpg");
    for (const std::string invalid : {"../test", "/test", "switch/../test.nro", "switch/test./test.nro",
                                      "switch/pipensx/config.json", "switch/pipensx.nro", "switch/test\\app.nro"}) {
        auto bad = good;
        bad["packages"][0]["files"] = {"U: " + invalid};
        assert(!HomebrewCatalog::parse(bad.dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    }
    for (const std::string key : {"sha256", "filesize", "files", "download_type"}) {
        auto bad = good; bad["packages"][0].erase(key);
        assert(!HomebrewCatalog::parse(bad.dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    }
    auto hidden = good;
    hidden["packages"].push_back({{"name", "hidden-app"}});
    assert(HomebrewCatalog::parse(hidden.dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    assert(packages.size() == 1);
    auto duplicate = good;
    duplicate["packages"].push_back(duplicate["packages"][0]);
    assert(!HomebrewCatalog::parse(duplicate.dump(), "en", kCatalogGitLabBaseUrl, packages, error));

    char temporary[] = "/tmp/pipensx-homebrew-XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    testNroDiscovery(root);
    assert(mkdir((root + "/sd").c_str(), 0777) == 0);
    std::string metadata = good.dump(), download = payload;
    bool offline = false, cancelOnProgress = false;
    std::atomic<bool> cancelled{false};
    std::vector<std::string> requests;
    HomebrewCatalog service(root + "/app", root + "/sd",
        [&](const std::string& url, uint64_t, const HomebrewCatalog::Sink& sink,
            const std::atomic<bool>&, const HomebrewCatalog::Progress& progress, std::string& e) {
            requests.push_back(url);
            if (offline) { e = "offline"; return false; }
            const std::string& body = url == "https://example.com/app" ? download : metadata;
            if (url.find("homebrew.json") != std::string::npos && url.rfind(kCatalogGitVerseBaseUrl, 0) != 0) {
                const std::string invalid = "<html>upstream error</html>";
                return sink(reinterpret_cast<const uint8_t*>(invalid.data()), invalid.size());
            }
            if (progress) progress(body.size(), body.size());
            if (cancelOnProgress) cancelled = true;
            return sink(reinterpret_cast<const uint8_t*>(body.data()), body.size());
        });
    assert(service.refresh(packages, "ru", kCatalogGitLabBaseUrl, cancelled, error));
    assert(requests.front() == std::string(kCatalogGitLabBaseUrl) + "homebrew.json");
    assert(packages[0].iconUrl.rfind(kCatalogGitVerseBaseUrl, 0) == 0);
    offline = true;
    assert(!service.refresh(packages, "en", {}, cancelled, error));
    assert(service.load(packages, "ru", error));
    assert(packages[0].iconUrl.rfind(kCatalogGitVerseBaseUrl, 0) == 0);
    offline = false;
    const std::string target = root + "/sd/switch/test-app/test.nro";
    assert(service.install(packages[0], cancelled, {}, error));
    assert(read(target) == payload);
    assert(service.load(packages, "en", error));
    assert(packages[0].installedVersion == "1.2");
    download = "corrupted";
    assert(!service.install(packages[0], cancelled, {}, error));
    assert(read(target) == payload);
    download = payload;
    cancelOnProgress = true;
    assert(!service.install(packages[0], cancelled, [](uint64_t, uint64_t) {}, error));
    assert(read(target) == payload);
    cancelled = false;
    cancelOnProgress = false;

    download = zip_fixture::build({{"switch/test-app/test.nro", "new NRO", true},
                                  {"config/test-app/config.ini", "default", true}});
    assert(HomebrewCatalog::parse(manifest(download, true).dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    assert(service.install(packages[0], cancelled, {}, error));
    assert(read(target) == "new NRO");
    const std::string config = root + "/sd/config/test-app/config.ini";
    std::ofstream(config) << "user config";
    assert(service.install(packages[0], cancelled, {}, error));
    assert(read(config) == "user config");
    download = zip_fixture::build({{"release/v1/switch/test-app/test.nro", "wrapped NRO", true},
                                  {"release/v1/config/test-app/config.ini", "default", true}});
    assert(HomebrewCatalog::parse(manifest(download, true).dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    assert(service.install(packages[0], cancelled, {}, error));
    assert(read(target) == "wrapped NRO" && read(config) == "user config");
    download = zip_fixture::build({{"../switch/test-app/test.nro", "unsafe", true},
                                  {"../config/test-app/config.ini", "unsafe", true}});
    assert(HomebrewCatalog::parse(manifest(download, true).dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    assert(!service.install(packages[0], cancelled, {}, error));
    assert(read(target) == "wrapped NRO" && read(config) == "user config");
    download = zip_fixture::build({{"switch/test-app/test.nro", "bad replacement", true}});
    assert(HomebrewCatalog::parse(manifest(download, true).dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    assert(!service.install(packages[0], cancelled, {}, error));
    assert(read(target) == "wrapped NRO" && read(config) == "user config");

    download = payload;
    assert(HomebrewCatalog::parse(good.dump(), "en", kCatalogGitLabBaseUrl, packages, error));
    assert(unlink(target.c_str()) == 0);
    assert(symlink(config.c_str(), target.c_str()) == 0);
    assert(!service.install(packages[0], cancelled, {}, error));
    assert(read(config) == "user config");
    assert(unlink(target.c_str()) == 0);

    assert(mkdir((root + "/app/homebrew/pending").c_str(), 0777) == 0);
    std::ofstream(root + "/app/homebrew/pending/backup-0") << "old NRO";
    std::ofstream(target) << "interrupted replacement";
    std::ofstream(root + "/app/homebrew/pending/journal.json") << Json({{"committed", false},
        {"files", Json::array({{{"path", "switch/test-app/test.nro"}, {"existed", true}}})}}).dump();
    assert(service.load(packages, "en", error));
    assert(read(target) == "old NRO");
    assert(access((root + "/app/homebrew/pending").c_str(), F_OK) != 0);

    assert(mkdir((root + "/app/homebrew/pending").c_str(), 0777) == 0);
    std::ofstream(root + "/app/homebrew/pending/journal.json") << Json({{"committed", true},
        {"files", Json::array()}, {"name", "test-app"}, {"version", "2.0"}, {"sha256", hashOf(payload)}}).dump();
    assert(service.load(packages, "en", error));
    assert(packages[0].installedVersion == "2.0");
    assert(access((root + "/app/homebrew/pending").c_str(), F_OK) != 0);
    assert(rename((root + "/app/homebrew/installed/test-app.json").c_str(),
                  (root + "/app/homebrew/installed/test-app.json.previous").c_str()) == 0);
    assert(service.load(packages, "en", error));
    assert(packages[0].installedVersion == "2.0");
    assert(unlink(target.c_str()) == 0);
    assert(service.load(packages, "en", error));
    assert(packages[0].installedVersion.empty());

    // Uninstall catalog package: deletes NRO & receipt, preserves preserve=true config and saves.
    writeFixture(root + "/sd/switch/test-app/test.nro", "app binary");
    writeFixture(root + "/sd/config/test-app/config.ini", "keep me");
    writeFixture(root + "/app/homebrew/installed/test-app.json",
                 Json({{"version", "1.2"}, {"sha256", "abc"}}).dump());
    HomebrewPackage catPkg = packages[0];
    catPkg.installedVersion = "1.2";
    catPkg.files = {{"switch/test-app/test.nro", false}, {"config/test-app/config.ini", true}};
    assert(service.uninstall(catPkg, error));
    assert(access((root + "/sd/switch/test-app/test.nro").c_str(), F_OK) != 0);
    assert(read(root + "/sd/config/test-app/config.ini") == "keep me");
    assert(access((root + "/app/homebrew/installed/test-app.json").c_str(), F_OK) != 0);

    // Uninstall local folder app: removes NRO & assets, preserves saves & ini configs.
    writeFixture(root + "/sd/switch/my-tool/tool.nro", "local nro");
    writeFixture(root + "/sd/switch/my-tool/assets/data.bin", "asset");
    writeFixture(root + "/sd/switch/my-tool/saves/slot1.sav", "SAVE_DATA");
    writeFixture(root + "/sd/switch/my-tool/settings.ini", "INI_DATA");
    HomebrewPackage localFolder;
    localFolder.name = "switch/my-tool/tool.nro";
    localFolder.installedPath = "switch/my-tool/tool.nro";
    assert(service.uninstall(localFolder, error));
    assert(access((root + "/sd/switch/my-tool/tool.nro").c_str(), F_OK) != 0);
    assert(access((root + "/sd/switch/my-tool/assets/data.bin").c_str(), F_OK) != 0);
    assert(read(root + "/sd/switch/my-tool/saves/slot1.sav") == "SAVE_DATA");
    assert(read(root + "/sd/switch/my-tool/settings.ini") == "INI_DATA");

    // Uninstall standalone top-level NRO in switch/
    writeFixture(root + "/sd/switch/solo.nro", "solo nro");
    HomebrewPackage localSolo;
    localSolo.name = "switch/solo.nro";
    localSolo.installedPath = "switch/solo.nro";
    assert(service.uninstall(localSolo, error));
    assert(access((root + "/sd/switch/solo.nro").c_str(), F_OK) != 0);

    // Self-protection: refuse to uninstall pipensx itself.
    HomebrewPackage selfPkg;
    selfPkg.name = "pipensx";
    selfPkg.installedPath = "switch/pipensx/pipensx.nro";
    assert(!service.uninstall(selfPkg, error));

    if (argc > 1) {
        assert(HomebrewCatalog::parse(read(argv[1]), "ru", kCatalogGitLabBaseUrl, packages, error));
        std::printf("Local homebrew catalog: %zu visible packages\n", packages.size());
    }
    assert(std::system(("rm -rf " + root).c_str()) == 0);
    std::puts("homebrew catalog tests passed");
}
