// Standalone PC driver for the web companion: real DownloadManager +
// MagnetResolver + WebServer serving resources/web/ on localhost, so the SPA
// can be developed and the whole add flow exercised in a desktop browser.
//
//   make -f Makefile.pc webserver
//   ./tools/webserver [port] [--root DIR] [--catalog FILE.json] [--pin PIN]
//
// Default root is ./web-test-root (created); --catalog loads a catalog JSON
// (or any CatalogService-parsable file) into /api/catalog.

#include "app/app_settings.hpp"
#include "app/archive_service.hpp"
#include "app/autorun_service.hpp"
#include "app/catalog_service.hpp"
#include "app/companion_settings.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/web_server.hpp"

#include <borealis/extern/nlohmann/json.hpp>
#include <curl/curl.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

using namespace pipensx;

static volatile sig_atomic_t gStop = 0;
static void onSignal(int) { gStop = 1; }

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    uint16_t port = 8080;
    std::string root = "./web-test-root";
    std::string catalogPath;
    std::string pin;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            root = argv[++i];
        } else if (!strcmp(argv[i], "--catalog") && i + 1 < argc) {
            catalogPath = argv[++i];
        } else if (!strcmp(argv[i], "--pin") && i + 1 < argc) {
            pin = argv[++i];
        } else {
            port = (uint16_t)atoi(argv[i]);
        }
    }

    mkdir(root.c_str(), 0755);
    DownloadManager manager(root);
    AppSettings settings(root + "/settings.json");
    std::string settingsError;
    settings.load(settingsError);
    manager.setTorrentingEnabled(true);
    InstalledTitleService installed(root);
    CatalogService catalog(root);
    GameMetadataService metadata(root);
    std::vector<CatalogEntry> loadedCatalog;
    if (!catalogPath.empty()) {
        std::ifstream in(catalogPath, std::ios::binary);
        if (!in) {
            fprintf(stderr, "cannot open catalog %s\n", catalogPath.c_str());
            return 1;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string error;
        if (!CatalogService::parseJson(buffer.str(), loadedCatalog, error)) {
            fprintf(stderr, "catalog parse failed: %s\n", error.c_str());
            return 1;
        }
        printf("catalog: %zu entries\n", loadedCatalog.size());
        CatalogSnapshot snap;
        snap.entries = loadedCatalog;
        catalog.adopt(std::move(snap), catalogPath);
    }

    ArchiveService archive(
        root, &installed, &manager, &catalog, &metadata, &settings, {},
        "sdmc:/switch/pipensx/pipensx.nro",
        [](uint64_t tid, const std::string& titleName,
           const std::string& publisher, const std::string& displayVersion,
           const std::string& nroPath, const std::string& args,
           const std::vector<uint8_t>& badgedIcon,
           std::vector<uint8_t>& outNsp, std::string& error) {
            (void)tid;
            (void)titleName;
            (void)publisher;
            (void)displayVersion;
            (void)nroPath;
            (void)args;
            outNsp = buildPfs0({{"stub.nca", badgedIcon}});
            if (outNsp.empty()) {
                error = "Failed to build stub PFS0.";
                return false;
            }
            return true;
        });

    if (const char* seed = std::getenv("PIPENSX_WEB_SEED_LIBRARY");
        seed && std::strcmp(seed, "1") == 0) {
        std::vector<InstalledTitle> seeded;
        InstalledTitle t1;
        t1.applicationId = 0x0100ABCDEF000000ULL;
        t1.titleId = "0100ABCDEF000000";
        t1.name = "Metroid Prime Remastered";
        t1.publisher = "Nintendo";
        t1.version = "0";
        t1.displayVersion = "1.0.0";
        t1.iconPath = root + "/installed-icons/" + t1.titleId + ".jpg";
        seeded.push_back(t1);

        InstalledTitle t2;
        t2.applicationId = 0x0100FEDCBA000000ULL;
        t2.titleId = "0100FEDCBA000000";
        t2.name = "Cartridge Dump Game";
        t2.publisher = "Publisher";
        t2.version = "0";
        t2.displayVersion = "1.0.0";
        t2.iconPath = root + "/installed-icons/" + t2.titleId + ".jpg";
        seeded.push_back(t2);
        installed.injectTitles(std::move(seeded));
    }

    WebServer server(manager, "resources/web", "dev");
    if (!pin.empty()) server.setPin(pin);
    server.setStreamSelection(settings.get().streamSelection);
    if (!loadedCatalog.empty()) {
        server.updateCatalog(std::make_shared<const std::vector<CatalogEntry>>(
            loadedCatalog));
    }
    server.setLibraryHandlers(
        [&installed, &archive](std::string& jsonOut, std::string& error) {
            (void)error;
            const auto titles = installed.titles();
            const auto archMap = archive.inspectLibrary(titles);
            nlohmann::json rows = nlohmann::json::array();
            for (const auto& t : titles) {
                const auto it = archMap.find(t.titleId);
                const ArchiveInspection insp =
                    it != archMap.end() ? it->second : ArchiveInspection{};
                rows.push_back({
                    {"titleId", t.titleId},
                    {"name", t.name},
                    {"isPort", false},
                    {"isArchived", t.isArchived},
                    {"canArchive", insp.canArchive},
                    {"archiveBlockReason", archiveBlockReasonCode(insp.blockReason)},
                    {"installedBytes", insp.installedBytes},
                    {"freedBytes", insp.estimatedFreedBytes},
                    {"publisher", t.publisher},
                    {"version", t.version},
                    {"displayVersion", t.displayVersion},
                    {"latestVersion", ""},
                    {"updateState", "up_to_date"},
                    {"updateHash", ""},
                    {"hasLayeredFsMods", t.hasLayeredFsMods},
                    {"dlcCount", 0},
                });
            }
            jsonOut = nlohmann::json({{"titles", rows},
                                      {"generation", installed.generation()}})
                          .dump();
            return true;
        },
        [&installed, &archive](const std::string& action,
                               const std::string& titleId,
                               std::string& error) {
            if (action == "refresh")
                return true;
            if (action == "archive") {
                auto list = installed.titles();
                ArchiveActionResult res;
                if (!archive.archiveTitle(titleId, res, error)) {
                    installed.injectTitles(std::move(list));
                    return false;
                }
                for (auto& t : list) {
                    if (t.titleId == res.titleId)
                        t.isArchived = true;
                }
                installed.injectTitles(std::move(list));
                return true;
            }
            if (action == "uninstall") {
                auto list = installed.titles();
                if (installed.isArchived(titleId)) {
                    if (!archive.removeArchivedStub(titleId, error)) {
                        installed.injectTitles(std::move(list));
                        return false;
                    }
                }
                list.erase(std::remove_if(list.begin(), list.end(),
                    [&](const InstalledTitle& t) { return t.titleId == titleId; }),
                    list.end());
                installed.injectTitles(std::move(list));
                return true;
            }
            error = "unsupported action";
            return false;
        });
    server.setLibraryRestoreHandler(
        [&archive](const std::string& tid, RestoreStartResult& out,
                   std::string& err) {
            std::atomic<bool> cancelled{false};
            return archive.restoreTitle(tid, cancelled, out, err, false);
        });
    server.setSettingsHandlers(
        [&settings](std::string& json, std::string& error) {
            (void)error;
            json = companionSettingsJson(settings.get());
            return true;
        },
        [&settings, &manager, &server](const std::string& body,
                                       std::string& error, std::string& json) {
            AppSettingsData next = settings.get();
            if (!applyCompanionSettingsPatch(next, body, error))
                return false;
            if (!settings.update(next, error))
                return false;
            applyCompanionSettingsRuntime(settings.get(), manager);
            server.setStreamSelection(settings.get().streamSelection);
            json = companionSettingsJson(settings.get());
            return true;
        });

    if (!server.start(port)) {
        fprintf(stderr, "failed to start on port %u\n", (unsigned)port);
        return 1;
    }
    printf("web companion at http://localhost:%u  (root: %s)\n",
           (unsigned)server.boundPort(), root.c_str());
    while (!gStop) sleep(1);

    printf("shutting down\n");
    server.shutdown();
    manager.shutdown();
    curl_global_cleanup();
    return 0;
}
