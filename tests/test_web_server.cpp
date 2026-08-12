// End-to-end host test of the web companion API against a real HttpServer +
// DownloadManager (worker disabled) in a temp root: state shape, PIN auth,
// task commands, torrent upload with default actions, magnet add through a
// fake resolver, catalog gzip/ETag and the static whitelist.

#include "app/web_server.hpp"
#include "app/app_settings.hpp"
#include "app/companion_settings.hpp"

extern "C" {
#include "core/sha1.h"
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace pipensx;

namespace {

std::string gTorrentBytes;

std::string makeTorrentBytes(const std::string& name,
                             const std::string& payload) {
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
         digest);
    std::string torrent = "d8:announce14:http://tracker4:infod6:lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e4:name" + std::to_string(name.size()) + ":" + name;
    torrent += "12:piece lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e6:pieces20:";
    torrent.append(reinterpret_cast<const char*>(digest), 20);
    torrent += "ee";
    return torrent;
}

int clientConnect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (sockaddr*)&a, sizeof(a));
    assert(r == 0);
    return fd;
}

// One request per connection; returns the full raw response.
std::string request(uint16_t port, const std::string& method,
                    const std::string& target, const std::string& body = "",
                    const std::string& extraHeaders = "") {
    int fd = clientConnect(port);
    std::string req = method + " " + target + " HTTP/1.1\r\n" + extraHeaders +
                      "Connection: close\r\n";
    if (method == "POST" || method == "PATCH")
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    size_t off = 0;
    while (off < req.size()) {
        ssize_t n = send(fd, req.data() + off, req.size() - off, 0);
        assert(n > 0);
        off += (size_t)n;
    }
    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, (size_t)n);
    close(fd);
    return resp;
}

std::string responseBody(const std::string& resp) {
    size_t pos = resp.find("\r\n\r\n");
    assert(pos != std::string::npos);
    return resp.substr(pos + 4);
}

std::string gunzip(const std::string& in) {
    z_stream stream{};
    assert(inflateInit2(&stream, 15 + 16) == Z_OK);
    std::string out;
    out.resize(4 * 1024 * 1024);
    stream.next_in = (Bytef*)in.data();
    stream.avail_in = (uInt)in.size();
    stream.next_out = (Bytef*)out.data();
    stream.avail_out = (uInt)out.size();
    int r = inflate(&stream, Z_FINISH);
    assert(r == Z_STREAM_END);
    out.resize(stream.total_out);
    inflateEnd(&stream);
    return out;
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);

    char rootTemplate[] = "/tmp/pipensx-web-XXXXXX";
    char* root = mkdtemp(rootTemplate);
    assert(root);
    std::string rootStr = root;
    std::string webRoot = rootStr + "/webui";
    mkdir(webRoot.c_str(), 0755);
    {
        std::ofstream out(webRoot + "/index.html");
        out << "<html>pipensx web</html>";
    }

    gTorrentBytes = makeTorrentBytes("package.nsp", "test payload");
    std::string torrentHash;
    {
        std::string tmp = rootStr + "/probe.torrent";
        std::ofstream out(tmp, std::ios::binary);
        out.write(gTorrentBytes.data(), (std::streamsize)gTorrentBytes.size());
        out.close();
        TorrentPreview preview;
        std::string error;
        assert(DownloadManager::previewTorrent(tmp, preview, error));
        torrentHash = preview.infoHash;
        ::unlink(tmp.c_str());
    }

    DownloadManager manager(rootStr, /*startWorker=*/false);
    // The add endpoints are gated on torrenting, which is off by default; the
    // gate itself is asserted separately below.
    manager.setTorrentingEnabled(true);
    auto fakeResolver = [](const WebAddJob&, const std::string& path,
                           std::atomic<bool>&,
                           const MagnetResolver::ProgressCallback&,
                           std::vector<uint8_t>&, std::string&) {
        std::ofstream out(path, std::ios::binary);
        out.write(gTorrentBytes.data(), (std::streamsize)gTorrentBytes.size());
        return out.good();
    };
    WebServer server(manager, webRoot, "test-1.0", fakeResolver);
    AppSettings companionSettings(rootStr + "/settings.json");
    std::string settingsError;
    assert(companionSettings.load(settingsError));
    server.setSettingsHandlers(
        [&companionSettings](std::string& json, std::string& error) {
            (void)error;
            json = companionSettingsJson(companionSettings.get());
            return true;
        },
        [&companionSettings, &manager, &server](const std::string& body,
                                                std::string& error,
                                                std::string& json) {
            AppSettingsData next = companionSettings.get();
            if (!applyCompanionSettingsPatch(next, body, error))
                return false;
            if (!companionSettings.update(next, error))
                return false;
            applyCompanionSettingsRuntime(companionSettings.get(), manager);
            server.setStreamSelection(
                companionSettings.get().streamSelection);
            json = companionSettingsJson(companionSettings.get());
            return true;
        });
    assert(server.start(0));
    uint16_t port = server.boundPort();
    const std::string pinHeader = "X-Pipensx-Pin: 1234\r\n";

    // info + static + storage — mutations always require a PIN
    {
        std::string resp = request(port, "GET", "/api/info");
        assert(resp.find("200 OK") != std::string::npos);
        assert(resp.find("\"authRequired\":true") != std::string::npos);
        assert(resp.find("test-1.0") != std::string::npos);

        resp = request(port, "GET", "/");
        assert(resp.find("pipensx web") != std::string::npos);
        resp = request(port, "GET", "/secret.txt");
        assert(resp.find("404") != std::string::npos);

        resp = request(port, "GET", "/api/storage");
        assert(resp.find("totalBytes") != std::string::npos);
    }

    // Empty PIN fails closed for mutations (GHSA-v2p6-jqrh-3wq4)
    {
        server.setPin("");
        std::string resp = request(port, "POST", "/api/auth/check");
        assert(resp.find("401") != std::string::npos);
        resp = request(port, "POST", "/api/add/torrent?mode=download",
                       gTorrentBytes);
        assert(resp.find("401") != std::string::npos);
        resp = request(port, "GET", "/api/tasks");
        assert(resp.find("200 OK") != std::string::npos);
    }

    // PIN auth
    {
        server.setPin("1234");
        std::string resp = request(port, "GET", "/api/info");
        assert(resp.find("\"authRequired\":true") != std::string::npos);
        resp = request(port, "POST", "/api/auth/check");
        assert(resp.find("401") != std::string::npos);
        resp = request(port, "POST", "/api/auth/check", "", pinHeader);
        assert(resp.find("204") != std::string::npos);
        // header only: a PIN in the query string would ride along on any
        // cross-site link and sit in browser history
        resp = request(port, "POST", "/api/auth/check?pin=1234");
        assert(resp.find("401") != std::string::npos);
        // reads stay open
        resp = request(port, "GET", "/api/tasks");
        assert(resp.find("200 OK") != std::string::npos);
    }

    // CSRF: a page served from somewhere else must not drive the console
    {
        const std::string host = "Host: 192.168.1.50:8080\r\n";
        std::string resp = request(port, "POST", "/api/auth/check", "",
                                   host + "Origin: http://evil.example\r\n" +
                                       pinHeader);
        assert(resp.find("403") != std::string::npos);

        resp = request(port, "POST", "/api/auth/check", "",
                       host + "Origin: http://192.168.1.50:8080\r\n" +
                           pinHeader);
        assert(resp.find("204") != std::string::npos);

        // "null" origin (sandboxed iframe, file://) is not the host either
        resp = request(port, "POST", "/api/auth/check", "",
                       host + "Origin: null\r\n" + pinHeader);
        assert(resp.find("403") != std::string::npos);

        // no Origin at all: not a browser (curl, a script on the LAN) — PIN
        // still required
        resp = request(port, "POST", "/api/auth/check");
        assert(resp.find("401") != std::string::npos);
        resp = request(port, "POST", "/api/auth/check", "", pinHeader);
        assert(resp.find("204") != std::string::npos);

        // reads are untouched — they mutate nothing
        resp = request(port, "GET", "/api/tasks", "",
                       host + "Origin: http://evil.example\r\n");
        assert(resp.find("200 OK") != std::string::npos);
    }

    // upload torrent (download mode) → task appears; duplicate → 409
    {
        std::string resp = request(port, "POST",
                                   "/api/add/torrent?mode=download",
                                   gTorrentBytes, pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find(torrentHash) != std::string::npos);

        resp = request(port, "GET", "/api/tasks");
        std::string body = responseBody(resp);
        assert(body.find(torrentHash) != std::string::npos);
        assert(body.find("package.nsp") != std::string::npos);
        assert(body.find("\"installSpeedBps\":0") != std::string::npos);
        assert(body.find("\"etaSeconds\":0") != std::string::npos);
        // Selection-aware progress fields ride along from the first snapshot:
        // wanted fields fall back to the raw range until the engine reports.
        assert(body.find("\"wantedTotalBytes\":12") != std::string::npos);
        assert(body.find("\"wantedCompletedBytes\":0") != std::string::npos);
        assert(body.find("\"fetchProgress\":0.0") != std::string::npos);

        resp = request(port, "POST", "/api/add/torrent?mode=download",
                       gTorrentBytes, pinHeader);
        assert(resp.find("409") != std::string::npos);

        resp = request(port, "POST", "/api/add/torrent?mode=bogus",
                       gTorrentBytes, pinHeader);
        assert(resp.find("400") != std::string::npos);
    }

    // Torrenting gate: with it off the companion must refuse before it can
    // put the console on the torrent network, and reads must stay open.
    {
        manager.setTorrentingEnabled(false);
        std::string resp = request(port, "POST",
                                   "/api/add/torrent?mode=download",
                                   gTorrentBytes, pinHeader);
        assert(resp.find("409") != std::string::npos);
        assert(responseBody(resp).find("torrenting is disabled") !=
               std::string::npos);

        resp = request(port, "GET", "/api/tasks");
        assert(resp.find("200 OK") != std::string::npos);
        manager.setTorrentingEnabled(true);
    }

    // task commands
    {
        std::string resp =
            request(port, "POST", "/api/tasks/" + torrentHash + "/move-front",
                    "", pinHeader);
        assert(resp.find("204") != std::string::npos);
        resp = request(port, "POST", "/api/tasks/nope/pause", "", pinHeader);
        assert(resp.find("404") != std::string::npos);
        resp = request(port, "POST", "/api/tasks/" + torrentHash + "/remove",
                       "{\"deleteData\":true}", pinHeader);
        assert(resp.find("204") != std::string::npos);
        resp = request(port, "GET", "/api/tasks");
        assert(responseBody(resp).find(torrentHash) == std::string::npos);
    }

    // magnet add through the fake resolver → job runs → task imported
    {
        std::string magnet = "magnet:?xt=urn:btih:" + torrentHash +
                             "&tr=http://bt.t-ru.org/ann?magnet";
        std::string resp = request(port, "POST", "/api/add/magnet",
                                   "{\"magnet\":\"" + magnet +
                                       "\",\"mode\":\"download\"}",
                                   pinHeader);
        assert(resp.find("202") != std::string::npos);
        assert(responseBody(resp).find("jobId") != std::string::npos);

        bool imported = waitFor([&] {
            std::string body =
                responseBody(request(port, "GET", "/api/tasks"));
            return body.find("\"state\":\"done\"") != std::string::npos &&
                   body.find(torrentHash) != std::string::npos;
        });
        assert(imported);

        // duplicate of an existing task → 409
        resp = request(port, "POST", "/api/add/magnet",
                       "{\"magnet\":\"" + magnet +
                           "\",\"mode\":\"download\"}",
                       pinHeader);
        assert(resp.find("409") != std::string::npos);

        resp = request(port, "POST", "/api/add/magnet",
                       "{\"magnet\":\"magnet:?xt=urn:btih:zz\",\"mode\":"
                       "\"download\"}",
                       pinHeader);
        assert(resp.find("400") != std::string::npos);
    }

    // catalog gzip + ETag + add-by-hash 404 for unknown entries
    {
        CatalogEntry entry;
        entry.infoHash = torrentHash;
        entry.title = "Test Game";
        entry.magnetUri = "magnet:?xt=urn:btih:" + torrentHash +
                          "&tr=http://bt.t-ru.org/ann?magnet";
        entry.posterUrl = "https://example.com/cover.jpg";
        entry.size = 12;
        server.updateCatalog(
            std::make_shared<const std::vector<CatalogEntry>>(
                std::vector<CatalogEntry>{entry}));

        std::string resp = request(port, "GET", "/api/catalog");
        assert(resp.find("Content-Encoding: gzip") != std::string::npos);
        size_t etagPos = resp.find("ETag: ");
        assert(etagPos != std::string::npos);
        std::string etag =
            resp.substr(etagPos + 6, resp.find("\r\n", etagPos) - etagPos - 6);
        std::string json = gunzip(responseBody(resp));
        assert(json.find("Test Game") != std::string::npos);
        assert(json.find("cover.jpg") != std::string::npos);

        resp = request(port, "GET", "/api/catalog", "",
                       "If-None-Match: " + etag + "\r\n");
        assert(resp.find("304") != std::string::npos);

        resp = request(port, "POST", "/api/add/catalog",
                       "{\"infoHash\":\"ffffffffffffffffffffffffffffffffffffff"
                       "ff\",\"mode\":\"install\"}",
                       pinHeader);
        assert(resp.find("404") != std::string::npos);

        // Broken UTF-8 from the RuTracker dump (Cyrillic cut mid-sequence)
        // must degrade to U+FFFD, not 500 the whole endpoint (seen live:
        // json type_error.316).
        CatalogEntry broken = entry;
        broken.infoHash = "ffffffffffffffffffffffffffffffffffffffff";
        broken.title = "Half cyrillic \xD0";
        broken.description = "bad \xD0";
        server.updateCatalog(
            std::make_shared<const std::vector<CatalogEntry>>(
                std::vector<CatalogEntry>{entry, broken}));
        resp = request(port, "GET", "/api/catalog");
        assert(resp.find("200 OK") != std::string::npos);
        json = gunzip(responseBody(resp));
        assert(json.find("Half cyrillic") != std::string::npos);
    }

    // Companion settings: PIN + same-origin, whitelist, secrets stay flags
    {
        std::string resp = request(port, "GET", "/api/settings");
        assert(resp.find("401") != std::string::npos);

        resp = request(port, "GET", "/api/settings", "", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        std::string body = responseBody(resp);
        assert(body.find("torboxApiKey") == std::string::npos);
        assert(body.find("realdebridApiKey") == std::string::npos);
        assert(body.find("\"torboxConfigured\":false") != std::string::npos);
        assert(body.find("webServerPin") == std::string::npos);

        const std::string host = "Host: 192.168.1.50:8080\r\n";
        resp = request(port, "PATCH", "/api/settings",
                       "{\"maxActiveDownloads\":2}",
                       host + "Origin: http://evil.example\r\n" + pinHeader);
        assert(resp.find("403") != std::string::npos);

        resp = request(port, "PATCH", "/api/settings",
                       "{\"language\":\"ru\"}", pinHeader);
        assert(resp.find("400") != std::string::npos);

        resp = request(port, "PATCH", "/api/settings",
                       "{\"maxActiveDownloads\":3,\"torboxApiKey\":\"tb-secret\","
                       "\"streamSelection\":\"packagesOnly\"}",
                       pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        body = responseBody(resp);
        assert(body.find("\"maxActiveDownloads\":3") != std::string::npos);
        assert(body.find("\"torboxConfigured\":true") != std::string::npos);
        assert(body.find("tb-secret") == std::string::npos);
        assert(body.find("packagesOnly") != std::string::npos);
        assert(companionSettings.get().torboxApiKey == "tb-secret");
        assert(companionSettings.get().maxActiveDownloads == 3);

        resp = request(port, "PATCH", "/api/settings",
                       "{\"torboxApiKey\":\"\"}", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find("\"torboxConfigured\":false") !=
               std::string::npos);
        assert(companionSettings.get().torboxApiKey.empty());
    }

    server.shutdown();
    manager.shutdown();
    printf("test_web_server: all ok\n");
    return 0;
}
