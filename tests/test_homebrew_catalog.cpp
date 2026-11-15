#include "app/homebrew_catalog.hpp"
#include "app/catalog_urls.hpp"
#include "core/sha256.h"
#include "zip_fixture.hpp"
#include <borealis/extern/nlohmann/json.hpp>
#include <cassert>
#include <cerrno>
#include <cstdio>
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

    if (argc > 1) {
        assert(HomebrewCatalog::parse(read(argv[1]), "ru", kCatalogGitLabBaseUrl, packages, error));
        std::printf("Local homebrew catalog: %zu visible packages\n", packages.size());
    }
    assert(std::system(("rm -rf " + root).c_str()) == 0);
    std::puts("homebrew catalog tests passed");
}
