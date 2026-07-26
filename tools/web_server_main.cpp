// Standalone PC driver for the web companion: real DownloadManager +
// MagnetResolver + WebServer serving resources/web/ on localhost, so the SPA
// can be developed and the whole add flow exercised in a desktop browser.
//
//   make -f Makefile.pc webserver
//   ./tools/webserver [port] [--root DIR] [--catalog FILE.json] [--pin PIN]
//
// Default root is ./web-test-root (created); --catalog loads a Langegen
// switch_games.json (or any CatalogService-parsable file) into /api/catalog.

#include "app/catalog_service.hpp"
#include "app/web_server.hpp"

#include <curl/curl.h>

#include <csignal>
#include <cstdio>
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
    WebServer server(manager, "resources/web", "dev");
    if (!pin.empty()) server.setPin(pin);

    if (!catalogPath.empty()) {
        std::ifstream in(catalogPath, std::ios::binary);
        if (!in) {
            fprintf(stderr, "cannot open catalog %s\n", catalogPath.c_str());
            return 1;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::vector<CatalogEntry> entries;
        std::string error;
        if (!CatalogService::parseJson(buffer.str(), entries, error)) {
            fprintf(stderr, "catalog parse failed: %s\n", error.c_str());
            return 1;
        }
        printf("catalog: %zu entries\n", entries.size());
        server.updateCatalog(std::make_shared<const std::vector<CatalogEntry>>(
            std::move(entries)));
    }

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
