#include "app/retro_catalog.hpp"
#include "app/catalog_urls.hpp"
#include <borealis/extern/nlohmann/json.hpp>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>

using namespace pipensx;
using Json = nlohmann::json;

int main(int argc, char** argv) {
    const auto sources = gbaCatalogSources(kCatalogGitVerseBaseUrl);
    assert(sources.size() == 3);
    assert(sources[0].dataBaseUrl == std::string(kCatalogGitVerseBaseUrl) + "retro/data/");
    assert(gbaCatalogSources("https://untrusted.example/")[0].imageBaseUrl == kCatalogGitLabBaseUrl);
    const Json row = {
        {"title", "Игра GBA"}, {"topic_id", "6916190"},
        {"magnet", "magnet:?xt=urn:btih:B8FBEEF7F49E8E565C3E3E8482DDC519C06853C0&tr=http%3A%2F%2Fbt2.t-ru.org%2Fann%3Fmagnet"},
        {"cover", "retro/covers/gba/6916190.jpg"},
        {"title_id", "AGB-TEST"}, {"is_romset", true}, {"content_type", "game"},
        {"screenshots", {"https://images.example/game.jpg", "retro/screens/gba/game.jpg"}},
        {"interface_lang", "RUS"}, {"description", "Описание"}
    };
    std::vector<RetroRelease> releases;
    std::string error;
    assert(parseGbaCatalog(Json::array({row}).dump(), sources[0], releases, error));
    assert(error.empty() && releases.size() == 1);
    assert(releases[0].key() == "gba:6916190");
    assert(releases[0].serial == "AGB-TEST" && releases[0].romset);
    assert(releases[0].coverUrl == sources[0].imageBaseUrl + "retro/covers/gba/6916190.jpg");
    assert(releases[0].screenshots[1] == sources[0].imageBaseUrl + "retro/screens/gba/game.jpg");
    auto other = row;
    other["topic_id"] = "6916191";
    assert(parseGbaCatalog(Json::array({row, other}).dump(), sources[0], releases, error));
    assert(releases.size() == 2); // Separate topics may share a torrent hash.
    auto crossPlatform = releases[0];
    crossPlatform.platform = "gbc";
    assert(crossPlatform.key() != releases[0].key());
    const auto unchanged = releases[0].key();
    auto reject = [&](const Json& bad) {
        assert(!parseGbaCatalog(bad.dump(), sources[0], releases, error));
        assert(!error.empty() && releases.size() == 2 && releases[0].key() == unchanged);
    };
    reject(Json::array({row, row}));
    reject(Json::array());
    reject(Json::object());
    for (const char* path : {"../cover.jpg", "/cover.jpg", "retro/../cover.jpg", "//evil/cover.jpg",
                             "retro/%2e%2e/a.jpg", "javascript:alert(1)", "retro\\a.jpg"}) {
        auto bad = row; bad["cover"] = path; reject(Json::array({row, bad}));
    }
    for (const char* key : {"topic_id", "title", "magnet", "title_id", "is_romset", "screenshots"}) {
        auto bad = row; bad[key] = 17; reject(Json::array({bad}));
    }
    other["title_id"] = nullptr;
    assert(parseGbaCatalog(Json::array({other}).dump(), sources[0], releases, error));
    assert(releases[0].serial.empty());
    char temporary[] = "/tmp/pipensx-retro-XXXXXX";
    assert(mkdtemp(temporary));
    const std::string appRoot = temporary;
    std::atomic<bool> cancelled{false};
    const std::string games = Json::array({row}).dump();
    const std::string stats = Json({{"6916190", {{"seeds", 10}, {"leeches", 2}, {"downloads", 100}}}}).dump();
    const std::string emulators = Json::array({{
        {"id", "mgba"}, {"name", "mGBA"}, {"version", "0.10.5"},
        {"download_url", "https://example.com/mgba.7z"}, {"filename", "mgba.7z"},
        {"file_size", 2211306}, {"is_archive", true}, {"supported_console_ids", {"gba", "gbc"}},
        {"install_path", "../../untrusted"}
    }}).dump();
    bool offline = false, badStats = false, cancelDuringFetch = false, oversized = false;
    std::vector<std::string> requests;
    auto fetch = [&](const std::string& url, const RetroCatalog::Sink& sink,
                     const std::atomic<bool>&, std::string& fetchError) {
        requests.push_back(url);
        if (oversized) return sink(nullptr, 8 * 1024 * 1024 + 1);
        if (cancelDuringFetch) { cancelled = true; return false; }
        if (offline) { fetchError = "offline"; return false; }
        const auto& body = url.find("gba_games") != std::string::npos ? games :
                           url.find("stats") != std::string::npos ? stats : emulators;
        if ((url.find("gitlab.com") != std::string::npos || badStats) && url.find("stats") != std::string::npos)
            return sink("{}", 2);
        return sink(body.data(), body.size());
    };
    RetroCatalog catalog(appRoot, fetch);
    RetroCatalogSnapshot snapshot;
    assert(!catalog.load(snapshot, error));
    assert(catalog.refresh(snapshot, "", cancelled, error));
    assert(error.empty() && requests.size() == 6);
    assert(snapshot.source.imageBaseUrl == kCatalogGitVerseBaseUrl);
    assert(snapshot.releases[0].coverUrl == std::string(kCatalogGitVerseBaseUrl) + "retro/covers/gba/6916190.jpg");
    assert(snapshot.stats.at("6916190").seeds == 10 && snapshot.emulators[0].id == "mgba");
    offline = true;
    const auto cached = catalog.refreshAsync("", {}).get();
    assert(cached.cached && !cached.refreshed && !cached.error.empty());
    assert(cached.snapshot.releases[0].key() == "gba:6916190");
    assert(!catalog.refresh(snapshot, "", cancelled, error));
    RetroCatalog restarted(appRoot, fetch);
    assert(restarted.load(snapshot, error) && error.empty());
    offline = false;
    assert(catalog.refresh(snapshot, "", cancelled, error));
    std::ofstream(appRoot + "/retro/consoles/gba.json") << "corrupt";
    assert(restarted.load(snapshot, error));
    badStats = true;
    assert(!catalog.refresh(snapshot, "", cancelled, error));
    assert(restarted.load(snapshot, error));
    badStats = false;
    oversized = true;
    assert(!catalog.refresh(snapshot, "", cancelled, error));
    assert(restarted.load(snapshot, error));
    oversized = false;
    cancelDuringFetch = true;
    requests.clear();
    assert(!catalog.refresh(snapshot, "", cancelled, error));
    assert(error == "Cancelled." && requests.size() == 1);
    assert(restarted.load(snapshot, error));
    for (const char* file : {"consoles/gba.json", "consoles/gba.json.previous",
                             "shared.json", "shared.json.previous",
                             "catalog.json", "previous.json"})
        unlink((appRoot + "/retro/" + file).c_str());
    rmdir((appRoot + "/retro/consoles").c_str());
    rmdir((appRoot + "/retro").c_str());
    rmdir(appRoot.c_str());

    // Multi-console + long >1024-char interface_lang + full 20-console registry check
    assert(retroConsoles().size() == 20);
    assert(extractRetroYear("2001") == "2001");
    assert(extractRetroYear("2018 г.") == "2018");
    assert(extractRetroYear("22 февраля 2012") == "2012");
    assert(extractRetroYear("~1980-2015") == "1980");
    assert(extractRetroYear("1987 - 2008") == "1987");
    assert(extractRetroYear("Unknown").empty());
    assert(extractRetroYear("201122").empty());
    auto ps2Row = row;
    ps2Row["topic_id"] = "7770001";
    ps2Row["interface_lang"] = std::string(1500, 'L');
    std::vector<RetroRelease> ps2Releases;
    assert(parseRetroCatalog("ps2", Json::array({ps2Row}).dump(), sources[0], ps2Releases, error));
    assert(ps2Releases.size() == 1 && ps2Releases[0].platform == "ps2" &&
           ps2Releases[0].key() == "ps2:7770001" && ps2Releases[0].interfaceLang.size() == 1500);
    assert(!parseRetroCatalog("unknown_console", Json::array({ps2Row}).dump(), sources[0], ps2Releases, error));

    if (argc > 1) {
        const std::string arg = argv[1];
        struct stat st {};
        if (stat(arg.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            for (const auto& console : retroConsoles()) {
                std::ifstream input(arg + "/" + console.jsonFile);
                assert(input.good());
                const std::string body{std::istreambuf_iterator<char>(input), {}};
                std::vector<RetroRelease> parsed;
                if (!parseRetroCatalog(console.id, body, sources[0], parsed, error)) {
                    std::fprintf(stderr, "%s: %s\n", console.jsonFile.c_str(), error.c_str());
                    return 1;
                }
                std::printf("Verified %s: %zu releases\n", console.id.c_str(), parsed.size());
            }
        } else {
            std::string detectedPlat = "gba";
            for (const auto& console : retroConsoles()) {
                if (arg.size() >= console.jsonFile.size() &&
                    arg.compare(arg.size() - console.jsonFile.size(), console.jsonFile.size(),
                                console.jsonFile) == 0) {
                    detectedPlat = console.id;
                    break;
                }
            }
            std::ifstream input(arg);
            assert(input.good());
            const std::string body{std::istreambuf_iterator<char>(input), {}};
            if (!parseRetroCatalog(detectedPlat, body, sources[0], releases, error)) {
                std::fprintf(stderr, "%s\n", error.c_str()); return 1;
            }
            std::printf("Parsed %zu %s releases\n", releases.size(), detectedPlat.c_str());
            if (detectedPlat == "gba") {
                char realTemporary[] = "/tmp/pipensx-retro-real-XXXXXX";
                assert(mkdtemp(realTemporary));
                const std::string directory = arg.substr(0, arg.find_last_of('/') + 1);
                RetroCatalog realCatalog(realTemporary, [&](const std::string& url, const RetroCatalog::Sink& sink,
                                                            const std::atomic<bool>&, std::string&) {
                    std::ifstream file(directory + url.substr(url.find_last_of('/') + 1));
                    if (!file) return false;
                    const std::string payload{std::istreambuf_iterator<char>(file), {}};
                    return sink(payload.data(), payload.size());
                });
                std::atomic<bool> stop{false};
                RetroCatalogSnapshot realSnapshot;
                assert(realCatalog.refresh(realSnapshot, "", stop, error));
                assert(realSnapshot.releases.size() == releases.size());
                std::printf("Parsed %zu topic statistics and %zu emulator packages\n",
                            realSnapshot.stats.size(), realSnapshot.emulators.size());
                unlink((std::string(realTemporary) + "/retro/consoles/gba.json").c_str());
                rmdir((std::string(realTemporary) + "/retro/consoles").c_str());
                unlink((std::string(realTemporary) + "/retro/shared.json").c_str());
                rmdir((std::string(realTemporary) + "/retro").c_str());
                rmdir(realTemporary);
            }
        }
    }
    std::puts("retro catalog: OK");
}
