// End-to-end host test of the web companion API against a real HttpServer +
// DownloadManager (worker disabled) in a temp root: state shape, PIN auth,
// task commands, torrent upload with default actions, magnet add through a
// fake resolver, catalog gzip/ETag and the static whitelist.

#include "app/web_server.hpp"
#include "app/app_settings.hpp"
#include "app/companion_settings.hpp"
#include "app/task_files.hpp"
#include "app/game_metadata_service.hpp"
#include "install/install_backend.hpp"

extern "C" {
#include "core/sha1.h"
#include "core/util.h"
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace pipensx;

namespace {

class ReadyProvider : public DebridProvider {
public:
    bool validate(std::string&) override { return true; }
    bool createFromMagnet(const std::string&, std::string& id,
                          std::string&) override {
        id = "web-1";
        return true;
    }
    bool createFromFile(const std::string&, std::string& id,
                        std::string&) override {
        id = "web-file";
        return true;
    }
    bool fetchInfo(const std::string&, DebridInfo& info,
                   std::string&) override {
        info = DebridInfo{};
        info.phase = DebridInfo::Phase::Ready;
        info.name = "Phone Game";
        info.bytes = 100;
        info.files.push_back({"1", "game.nsp", 100});
        return true;
    }
    bool selectFiles(const std::string&, const std::vector<std::string>&,
                     std::string&) override {
        return true;
    }
    bool resolveDownloadUrl(const std::string&, const DebridInfo&, size_t,
                            const DebridFile&, std::string&,
                            std::string&) override {
        return false;
    }
    bool remove(const std::string&, std::string&) override { return true; }
    const char* name() const override { return "stub"; }
};

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
    const std::string logPath = rootStr + "/pipensx.log";
    log_init(logPath.c_str());
    log_msg("[realdebrid] web diagnostics marker\n");
    log_flush();
    std::string webRoot = rootStr + "/webui";
    mkdir(webRoot.c_str(), 0755);
    {
        std::ofstream out(webRoot + "/index.html");
        out << "<html>pipensx web</html>";
    }
    {
        std::ofstream out(webRoot + "/app.js");
        out << "window.pipensxWebTest = true;";
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
    auto resolveCalls = std::make_shared<std::atomic<int>>(0);
    auto fakeResolver = [resolveCalls](const WebAddJob&, const std::string& path,
                           std::atomic<bool>&,
                           const MagnetResolver::ProgressCallback&,
                           std::vector<uint8_t>&, std::string&) {
        resolveCalls->fetch_add(1);
        std::ofstream out(path, std::ios::binary);
        out.write(gTorrentBytes.data(), (std::streamsize)gTorrentBytes.size());
        return out.good();
    };
    WebServer server(manager, webRoot, "test-1.0", fakeResolver);
    bool updateStaged = false;
    uint64_t updateStagedBytes = 0;
    int updateApplyCalls = 0;
    server.setUpdateHandlers(
        [&updateStaged, &updateStagedBytes](bool& stagedReady,
                                           uint64_t& stagedBytes,
                                           std::string&) {
            stagedReady = updateStaged;
            stagedBytes = updateStagedBytes;
            return true;
        },
        [&updateStaged, &updateStagedBytes](const std::string& bytes,
                                            std::string& error) {
            if (bytes.size() < 0x14 ||
                std::memcmp(bytes.data() + 0x10, "NRO0", 4) != 0) {
                error = "bad NRO";
                return false;
            }
            updateStaged = true;
            updateStagedBytes = bytes.size();
            return true;
        },
        [&updateStaged, &updateApplyCalls](std::string& error) {
            if (!updateStaged) {
                error = "no staged update";
                return false;
            }
            ++updateApplyCalls;
            return true;
        });
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
        resp = request(port, "GET", "/app.js?v=logs-2");
        assert(resp.find("200 OK") != std::string::npos);
        assert(resp.find("Cache-Control: no-store") != std::string::npos);
        assert(resp.find("pipensxWebTest") != std::string::npos);
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

        // Logs are a sensitive read: unlike the task snapshot, they require
        // the companion PIN and expose only a bounded live tail.
        resp = request(port, "GET", "/api/logs");
        assert(resp.find("401") != std::string::npos);
        resp = request(port, "GET", "/api/benchmark");
        assert(resp.find("401") != std::string::npos);
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
        {
            int portCalls = 0;
            server.setPortHandler([&portCalls](const std::string& action, const std::string& id,
                                               const std::string& body, std::string& json, std::string& error) {
                ++portCalls;
                if (action != "folders" && id != "port-test") { error = "Port task required."; return false; }
                if (action == "folders") assert(body.find("/switch") != std::string::npos);
                json = "{\"files\":[],\"manual\":true}"; return true;
            });
            assert(request(port, "GET", "/api/ports/port-test").find("401") != std::string::npos);
            assert(request(port, "GET", "/api/ports/port-test", "", pinHeader +
                "Host: localhost\r\nOrigin: http://evil.example\r\n").find("403") != std::string::npos);
            assert(portCalls == 0);
            assert(request(port, "GET", "/api/ports/port-test", "", pinHeader).find("200 OK") != std::string::npos);
            assert(request(port, "GET", "/api/ports/folders?path=%2Fswitch", "", pinHeader).find("200 OK") != std::string::npos);
            assert(request(port, "PATCH", "/api/ports/port-test", "{}", pinHeader).find("200 OK") != std::string::npos);
            assert(request(port, "POST", "/api/ports/port-test/preview", "{}", pinHeader).find("200 OK") != std::string::npos);
            assert(request(port, "POST", "/api/ports/port-test/start", "{}", pinHeader).find("202") != std::string::npos);
            assert(request(port, "POST", "/api/ports/retail-task/start", "{}", pinHeader).find("409") != std::string::npos);
            server.setPortHandler({});
        }
        resp = request(port, "GET", "/api/tasks");
        assert(resp.find("200 OK") != std::string::npos);
        resp = request(port, "GET", "/api/logs", "", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find("[realdebrid] web diagnostics marker") !=
               std::string::npos);
        resp = request(port, "PATCH", "/api/settings", "{\"extendedTelemetry\":true}", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(telemetry_enabled());
        assert(responseBody(resp).find("\"extendedTelemetry\":true") != std::string::npos);
        resp = request(port, "PATCH", "/api/settings", "{\"extendedTelemetry\":false}", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(!telemetry_enabled());

        // Remote updater is a protected read/write surface. A valid NRO is
        // uploaded once, appears as staged, then can be applied from the panel.
        resp = request(port, "GET", "/api/update");
        assert(resp.find("401") != std::string::npos);
        resp = request(port, "GET", "/api/update", "", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find("\"stagedReady\":false") !=
               std::string::npos);
        assert(responseBody(resp).find("\"currentVersion\":\"test-1.0\"") !=
               std::string::npos);

        std::string nro(0x90, '\0');
        std::memcpy(nro.data() + 0x10, "NRO0", 4);
        nro[0x18] = static_cast<char>(0x80);
        resp = request(port, "POST", "/api/update/upload", nro, pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find("\"stagedReady\":true") !=
               std::string::npos);
        assert(updateStagedBytes == nro.size());

        resp = request(port, "POST", "/api/update/apply", "", pinHeader);
        assert(resp.find("202 Accepted") != std::string::npos);
        assert(updateApplyCalls == 1);

        resp = request(port, "POST", "/api/queue/clear-completed",
                       "{\"deleteData\":false}", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find("\"cleared\":0") != std::string::npos);
        assert(responseBody(resp).find("\"skippedBusy\":0") !=
               std::string::npos);
        resp = request(port, "POST", "/api/queue/clear-completed",
                       "{\"deleteData\":true}", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(responseBody(resp).find("\"cleared\":0") != std::string::npos);
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
        resp = request(port, "GET", "/api/logs", "",
                       host + "Origin: http://evil.example\r\n" + pinHeader);
        assert(resp.find("403") != std::string::npos);
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
        // Queued torrent: pause and move are offered, verify is not.
        assert(body.find("\"pause\":{\"allowed\":true,\"reason\":\"\"}") !=
               std::string::npos);
        assert(body.find("\"move\":{\"allowed\":true,\"reason\":\"\"}") !=
               std::string::npos);
        assert(body.find(
                   "\"verify\":{\"allowed\":false,\"reason\":\"not_completed\"}") !=
               std::string::npos);
        assert(body.find(
                   "\"resume\":{\"allowed\":false,\"reason\":\"not_resumable\"}") !=
               std::string::npos);

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
        assert(responseBody(resp).find("not configured") != std::string::npos);
        assert(responseBody(resp).find("Direct BitTorrent stays off") !=
               std::string::npos);
        assert(manager.torrentingEnabled() == false);

        resp = request(
            port, "POST", "/api/add/magnet",
            "{\"magnet\":\"magnet:?xt=urn:btih:"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "&tr=http://bt.t-ru.org/ann?magnet\",\"mode\":\"install\"}",
            pinHeader);
        assert(resp.find("409") != std::string::npos);
        assert(responseBody(resp).find("Direct BitTorrent stays off") !=
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
        resp = request(port, "POST", "/api/tasks/" + torrentHash + "/verify",
                       "", pinHeader);
        assert(resp.find("409") != std::string::npos);
        assert(responseBody(resp).find("\"error\":\"not_completed\"") !=
               std::string::npos);
        resp = request(port, "GET", "/api/tasks");
        assert(responseBody(resp).find("\"status\":\"Queued\"") !=
               std::string::npos);
        resp = request(port, "POST", "/api/tasks/" + torrentHash + "/pause",
                       "", pinHeader);
        assert(resp.find("204") != std::string::npos);
        resp = request(port, "POST", "/api/tasks/" + torrentHash + "/pause",
                       "", pinHeader);
        assert(resp.find("409") != std::string::npos);
        assert(responseBody(resp).find("\"error\":\"not_pausable\"") !=
               std::string::npos);
        resp = request(port, "GET", "/api/tasks");
        assert(responseBody(resp).find(
                   "\"pause\":{\"allowed\":false,\"reason\":\"not_pausable\"}") !=
               std::string::npos);
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

    // Installed library actions are authenticated, asynchronous and confirmed.
    // Mock callbacks only: these tests never touch the console's applications.
    {
        auto calls = std::make_shared<std::atomic<int>>(0);
        auto release = std::make_shared<std::atomic<bool>>(false);
        auto removed = std::make_shared<std::atomic<bool>>(false);
        server.setLibraryHandlers(
            [removed](std::string& json, std::string&) {
                json = removed->load() ? "{\"titles\":[]}" :
                    "{\"titles\":[{\"titleId\":\"0100000000001000\",\"name\":\"Fixture\",\"updateState\":\"up_to_date\"}]}";
                return true;
            },
            [calls, release, removed](const std::string& action, const std::string& id, std::string&) {
                ++*calls;
                while (!release->load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (action == "uninstall") { assert(id == "0100000000001000"); removed->store(true); }
                return true;
            });
        assert(request(port, "GET", "/api/library").find("401") != std::string::npos);
        assert(request(port, "GET", "/api/library", "", pinHeader +
            "Host: localhost\r\nOrigin: http://evil.example\r\n").find("403") != std::string::npos);
        assert(request(port, "POST", "/api/library/refresh", "{}").find("401") != std::string::npos);
        assert(request(port, "POST", "/api/library/not-an-id/uninstall", "{\"confirm\":true}", pinHeader).find("400") != std::string::npos);
        for (const auto* body : {"{}", "{\"confirm\":false}", "{\"confirm\":\"true\"}"})
            assert(request(port, "POST", "/api/library/0100000000001000/uninstall", body, pinHeader).find("400") != std::string::npos);
        assert(calls->load() == 0);
        assert(request(port, "POST", "/api/library/0100000000001000/update", "{}", pinHeader).find("409") != std::string::npos);
        assert(request(port, "POST", "/api/library/0100000000002000/update", "{}", pinHeader).find("409") != std::string::npos);
        const auto first = request(port, "POST", "/api/library/refresh", "{}", pinHeader);
        assert(first.find("202") != std::string::npos);
        assert(waitFor([&] { return calls->load() == 1; }));
        const auto duplicate = request(port, "POST", "/api/library/refresh", "{}", pinHeader);
        assert(responseBody(first) == responseBody(duplicate));
        assert(request(port, "GET", "/api/tasks").find("200 OK") != std::string::npos);
        assert(request(port, "GET", "/api/library", "", pinHeader).find("working") != std::string::npos);
        const auto deletion = request(port, "POST", "/api/library/0100000000001000/uninstall", "{\"confirm\":true}", pinHeader);
        assert(deletion.find("202") != std::string::npos);
        assert(calls->load() == 1); // serialized behind refresh
        release->store(true);
        assert(waitFor([&] { return removed->load(); }));
        assert(waitFor([&] { return request(port, "GET", "/api/library", "", pinHeader).find("\"state\":\"done\"") != std::string::npos; }));
        assert(calls->load() == 2);
        assert(request(port, "GET", "/api/library", "", pinHeader).find("\"titles\":[]") != std::string::npos);
        CatalogEntry update;
        update.infoHash = std::string(40, 'a');
        update.title = "Fixture update [NSP]";
        update.magnetUri = "magnet:?xt=urn:btih:" + update.infoHash;
        server.updateCatalog(std::make_shared<const std::vector<CatalogEntry>>(std::vector<CatalogEntry>{update}));
        server.setLibraryHandlers([](std::string& json, std::string&) {
            json = "{\"titles\":[{\"titleId\":\"0100000000001000\",\"updateState\":\"update_available\",\"hasLayeredFsMods\":true,\"updateHash\":\"" + std::string(40, 'a') + "\"}]}";
            return true;
        }, {});
        const auto resolverBefore = resolveCalls->load();
        assert(request(port, "POST", "/api/library/0100000000001000/update", "{}", pinHeader).find("409") != std::string::npos);
        assert(request(port, "POST", "/api/library/0100000000001000/update", "{\"acknowledgeMods\":true}", pinHeader).find("202") != std::string::npos);
        // Base disappeared before the worker ran: refuse before contacting a
        // torrent resolver/provider, even though the UI snapshot offered it.
        assert(waitFor([&] { return request(port, "GET", "/api/tasks").find("installed base could not be verified") != std::string::npos; }));
        assert(resolveCalls->load() == resolverBefore);
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
        entry.description = "Original fixture prose";
        server.updateCatalog(
            std::make_shared<const std::vector<CatalogEntry>>(
                std::vector<CatalogEntry>{entry}));

        std::string resp = request(port, "GET", "/api/catalog");
        auto description = request(port, "GET", "/api/catalog/description?hash=" + torrentHash + "&lang=pt-BR");
        assert(description.find("200 OK") != std::string::npos);
        assert(responseBody(description).find("Original fixture prose") != std::string::npos);
        assert(responseBody(description).find("\"localized\":false") != std::string::npos);
        description = request(port, "GET", "/api/catalog/description?hash=" + torrentHash + "&lang=../../pt-BR");
        assert(description.find("400") != std::string::npos);
        assert(resp.find("Content-Encoding: gzip") != std::string::npos);
        size_t etagPos = resp.find("ETag: ");
        assert(etagPos != std::string::npos);
        std::string etag =
            resp.substr(etagPos + 6, resp.find("\r\n", etagPos) - etagPos - 6);
        std::string json = gunzip(responseBody(resp));
        assert(json.find("Test Game") != std::string::npos);
        assert(json.find("cover.jpg") != std::string::npos);
        assert(json.find("\"section\":\"ports\"") != std::string::npos);
        auto joined = std::make_shared<GameMetadataIndexSnapshot>();
        GameMetadata metadata;
        metadata.titleId = "0100000000001000";
        joined->items.push_back(metadata);
        joined->byInfoHash[torrentHash] = 0;
        server.updateMetadata(joined);
        resp = request(port, "GET", "/api/catalog", "", "If-None-Match: " + etag + "\r\n");
        assert(resp.find("200 OK") != std::string::npos);
        assert(gunzip(responseBody(resp)).find("\"section\":\"games\"") != std::string::npos);
        etagPos = resp.find("ETag: ");
        etag = resp.substr(etagPos + 6, resp.find("\r\n", etagPos) - etagPos - 6);

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
        assert(body.find("alldebridApiKey") == std::string::npos);
        assert(body.find("\"torboxConfigured\":false") != std::string::npos);
        assert(body.find("webServerPin") == std::string::npos);

        const std::string host = "Host: 192.168.1.50:8080\r\n";
        resp = request(port, "PATCH", "/api/settings",
                       "{\"maxActiveDownloads\":2}",
                       host + "Origin: http://evil.example\r\n" + pinHeader);
        assert(resp.find("403") != std::string::npos);

        resp = request(port, "PATCH", "/api/settings",
                       "{\"language\":\"de\"}", pinHeader);
        assert(resp.find("400") != std::string::npos);
        resp = request(port, "PATCH", "/api/settings",
                       "{\"language\":\"pt-BR\"}", pinHeader);
        assert(resp.find("200 OK") != std::string::npos);
        assert(companionSettings.get().language == "pt-BR");

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

    // Torrenting stays off. A configured provider adds the catalog release
    // through debrid, with the same one-tap file plan the console uses, and
    // without resolving the magnet on the swarm.
    {
        const std::string hash(40, 'a');
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(DebridProviderKind::TorBox);
        manager.setTorboxApiKey("tb-test");
        auto providerCalls = std::make_shared<std::atomic<int>>(0);
        server.setDebridProviderFactory(
            [providerCalls](DebridProviderKind, const std::string&) {
                ++*providerCalls;
                return std::unique_ptr<DebridProvider>(new ReadyProvider());
            });
        CatalogEntry entry;
        entry.infoHash = hash;
        entry.title = "Phone Game";
        entry.magnetUri = "magnet:?xt=urn:btih:" + hash;
        server.updateCatalog(
            std::make_shared<const std::vector<CatalogEntry>>(
                std::vector<CatalogEntry>{entry}));
        const int resolvesBefore = resolveCalls->load();
        for (const char* endpoint : {"/api/catalog", "/api/tasks", "/api/events", "/api/logs"}) {
            const auto read = request(port, "GET", endpoint, "", pinHeader);
            assert(read.find("200 OK") != std::string::npos);
        }
        assert(providerCalls->load() == 0);
        std::string resp = request(port, "POST", "/api/add/catalog",
                                   "{\"infoHash\":\"" + hash +
                                       "\",\"mode\":\"install\"}",
                                   pinHeader);
        assert(resp.find("202") != std::string::npos);
        bool imported = waitFor([&] {
            for (const DownloadTask& task : manager.snapshot()) {
                if (task.id == hash && task.source == TaskSource::Debrid &&
                    task.debridProvider == DebridProviderKind::TorBox &&
                    task.mode == TransferMode::StreamInstall &&
                    task.fileSelection.size() == 1 &&
                    task.fileSelection[0] ==
                        static_cast<uint8_t>(FileAction::Install))
                    return true;
            }
            return false;
        });
        assert(imported);
        assert(manager.torrentingEnabled() == false);
        assert(resolveCalls->load() == resolvesBefore);
        assert(providerCalls->load() == 1);
        manager.setTorboxApiKey("");
        std::string removeError;
        assert(manager.remove(hash, true, removeError));
    }

    server.shutdown();
    // Three repetitions: a single unusually quick pass must not win.
    std::vector<InstallBenchmarkSample> samples;
    for (uint64_t time : {1u, 120u, 121u}) samples.push_back({256u << 10, time, 1, 1});
    for (uint64_t time : {100u, 101u, 102u}) samples.push_back({1u << 20, time, 1, 1});
    for (uint64_t time : {80u, 81u, 82u}) samples.push_back({4u << 20, time, 1, 1});
    assert(chooseInstallBatch(samples) == (4u << 20));
    for (auto& sample : samples) if (sample.batchBytes == (4u << 20)) sample.durationUs = 99;
    assert(chooseInstallBatch(samples) == (1u << 20)); // noise band keeps baseline.
    assert(chooseInstallBatch({}) == (1u << 20));
    assert(!manager.setInstallBatchBytes(7));
    assert(manager.setInstallBatchBytes(4u << 20));
    assert(manager.installBatchBytes() == (4u << 20));
    {
        const std::string keep = rootStr + "/keep-original.bin";
        std::ofstream(keep) << "original";
        auto scratch = install::createInstallBackend(rootStr);
        assert(scratch->setBenchmarkMode(true));
        assert(scratch->beginPackage("benchmark-owned-test", "fixture"));
        assert(!scratch->setBenchmarkMode(false));
        const uint8_t bytes[] = {1, 2, 3};
        assert(scratch->beginFile("content.nca", sizeof(bytes)));
        assert(scratch->writeFile(bytes, sizeof(bytes)));
        assert(scratch->endFile());
        const std::string owned = rootStr + "/install-sim/benchmark-owned-test-fixture/content.nca";
        assert(access(owned.c_str(), F_OK) == 0);
        bool existing = true;
        assert(scratch->commitPackage(existing));
        assert(!existing && access(owned.c_str(), F_OK) != 0);
        assert(access(keep.c_str(), F_OK) == 0);
        assert(scratch->beginPackage("benchmark-owned-test", "fixture"));
        assert(scratch->beginFile("content.nca", sizeof(bytes)));
        scratch->rollbackPackage();
        assert(access(owned.c_str(), F_OK) != 0 && access(keep.c_str(), F_OK) == 0);
    }
    // A completed debrid download can be installed without submitting a new
    // cloud torrent or invoking the swarm resolver. Its lease still wins.
    {
        const std::string cachedRoot = rootStr + "/cached-upgrade";
        mkdir(cachedRoot.c_str(), 0755);
        mkdir((cachedRoot + "/torrents").c_str(), 0755);
        mkdir((cachedRoot + "/downloads").c_str(), 0755);
        const std::string cachedMeta = cachedRoot + "/torrents/original.torrent";
        { std::ofstream out(cachedMeta, std::ios::binary); out << gTorrentBytes; }
        const auto bs = [](const std::string& value) { return std::to_string(value.size()) + ":" + value; };
        const std::string originalData = cachedRoot + "/downloads/retained-data";
        std::string state = "d5:tasksl";
        state += "d4:data" + bs(originalData) + "9:debrid-id" + bs("original-account-torrent");
        state += "5:error0:2:id" + bs(torrentHash) + "8:metainfo" + bs(cachedMeta);
        state += "4:mode8:download4:name11:package.nsp13:package-counti0e13:packages-donei0e";
        state += "8:provider10:realdebrid9:selection0:6:source6:debrid6:status9:completed5:totali12ee";
        state += "e7:versioni6ee";
        { std::ofstream out(cachedRoot + "/queue.bencode", std::ios::binary); out << state; }
        DownloadManager cached(cachedRoot, false);
        const auto before = cached.snapshot(torrentHash);
        assert(before && before->status == DownloadStatus::Completed && before->source == TaskSource::Debrid);
        auto providerCalls = std::make_shared<std::atomic<int>>(0);
        WebServer cachedServer(cached, webRoot, "cached-test", fakeResolver);
        cachedServer.setDebridProviderFactory([providerCalls](DebridProviderKind, const std::string&) {
            providerCalls->fetch_add(1);
            return std::unique_ptr<DebridProvider>();
        });
        cachedServer.setPin("1234");
        CatalogEntry entry;
        entry.infoHash = torrentHash; entry.title = "Cached game";
        cachedServer.updateCatalog(std::make_shared<const std::vector<CatalogEntry>>(std::vector<CatalogEntry>{entry}));
        assert(cachedServer.start(0));
        const auto cachedPort = cachedServer.boundPort();
        const auto body = "{\"infoHash\":\"" + torrentHash + "\",\"mode\":\"install\"}";
        std::string error;
        auto lease = cached.beginExternalDeploy(torrentHash, error);
        assert(lease);
        assert(request(cachedPort, "POST", "/api/add/catalog", body, pinHeader).find("409") != std::string::npos);
        assert(cached.snapshot(torrentHash)->mode == TransferMode::DownloadOnly);
        lease.reset();
        const int resolvedBefore = resolveCalls->load();
        auto response = request(cachedPort, "POST", "/api/add/catalog", body, pinHeader);
        assert(response.find("200 OK") != std::string::npos && response.find("\"reusedTask\":true") != std::string::npos);
        const auto after = cached.snapshot(torrentHash);
        assert(after && after->status == DownloadStatus::Queued && after->mode == TransferMode::StreamInstall);
        assert(after->packageCount == 1 && after->fileSelection == std::vector<uint8_t>{2});
        assert(after->source == before->source && after->debridId == before->debridId && after->dataPath == before->dataPath);
        assert(resolveCalls->load() == resolvedBefore && providerCalls->load() == 0);
        assert(request(cachedPort, "POST", "/api/add/catalog", body, pinHeader).find("409") != std::string::npos);
        cachedServer.shutdown();
        cached.shutdown();
        // Catalog imports have no .torrent. The saved manifest must preserve
        // nested updates/DLC, skipped files and the original disk paths.
        const std::string manifestRoot = cachedRoot + "/manifest";
        mkdir(manifestRoot.c_str(), 0755);
        mkdir((manifestRoot + "/downloads").c_str(), 0755);
        std::string manifestState = state;
        const auto oldMeta = "8:metainfo" + bs(cachedMeta);
        manifestState.replace(manifestState.find(oldMeta), oldMeta.size(), "8:metainfo0:");
        manifestState.replace(manifestState.find(bs(originalData)), bs(originalData).size(), bs(manifestRoot + "/downloads/retained"));
        { std::ofstream out(manifestRoot + "/queue.bencode", std::ios::binary); out << manifestState; }
        TaskFileManifest manifest;
        manifest.taskId = torrentHash;
        for (const auto& path : {"base.nsz", "Updates/update.nsp", "DLC/nested/dlc.nsp", "unselected/other.nsp"}) {
            TaskFileRecord file;
            file.logicalPath = path;
            file.localPath = path;
            file.size = 12;
            file.package = true;
            file.action = manifest.files.size() == 3 ? TaskFileAction::Skip : TaskFileAction::Download;
            manifest.files.push_back(file);
        }
        assert(saveTaskFileManifest(manifestRoot, manifest, error));
        DownloadManager manifestManager(manifestRoot, false);
        auto manifestLease = manifestManager.beginExternalDeploy(torrentHash, error);
        assert(manifestLease && !manifestManager.installDownloaded(torrentHash, error));
        assert(manifestManager.snapshot(torrentHash)->mode == TransferMode::DownloadOnly);
        manifestLease.reset();
        WebServer manifestServer(manifestManager, webRoot, "manifest-test", fakeResolver);
        manifestServer.setPin("1234");
        manifestServer.updateCatalog(std::make_shared<const std::vector<CatalogEntry>>(std::vector<CatalogEntry>{entry}));
        manifestServer.setDebridProviderFactory([providerCalls](DebridProviderKind, const std::string&) {
            providerCalls->fetch_add(1);
            return std::unique_ptr<DebridProvider>();
        });
        assert(manifestServer.start(0));
        const auto manifestResponse = request(manifestServer.boundPort(), "POST", "/api/add/catalog", body, pinHeader);
        assert(manifestResponse.find("\"reusedTask\":true") != std::string::npos);
        const auto promoted = manifestManager.snapshot(torrentHash);
        assert(promoted && promoted->mode == TransferMode::StreamInstall && promoted->packageCount == 3);
        assert(promoted->fileSelection.empty() && promoted->debridId == before->debridId);
        TaskFileManifest updated;
        assert(loadTaskFileManifest(manifestRoot, torrentHash, updated, error));
        assert(updated.files.size() == 4 && updated.files[3].action == TaskFileAction::Skip);
        for (size_t i = 0; i < 3; ++i) {
            assert(updated.files[i].action == TaskFileAction::Install);
            assert(updated.files[i].localPath == manifest.files[i].localPath);
        }
        assert(resolveCalls->load() == resolvedBefore && providerCalls->load() == 0);
        manifestServer.shutdown();
        manifestManager.shutdown();
        // For a downloaded NRO, deployment changes the mode even though the
        // retained download actions are unchanged. A lease must prevent it.
        const std::string portRoot = cachedRoot + "/port";
        mkdir(portRoot.c_str(), 0755);
        mkdir((portRoot + "/torrents").c_str(), 0755);
        mkdir((portRoot + "/downloads").c_str(), 0755);
        const std::string portMeta = portRoot + "/torrents/port.torrent";
        { std::ofstream out(portMeta, std::ios::binary); out << makeTorrentBytes("port.nro", "test payload"); }
        TorrentPreview preview;
        assert(DownloadManager::previewTorrent(portMeta, preview, error));
        std::string portState = "d5:tasksl";
        portState += "d4:data" + bs(portRoot + "/downloads/retained") + "2:id" + bs(preview.infoHash) + "8:metainfo" + bs(portMeta);
        portState += "4:mode8:download4:name8:port.nro6:status9:completed5:totali12eee7:versioni6ee";
        { std::ofstream out(portRoot + "/queue.bencode", std::ios::binary); out << portState; }
        DownloadManager downloadedPort(portRoot, false);
        auto portLease = downloadedPort.beginExternalDeploy(preview.infoHash, error);
        assert(portLease && !downloadedPort.installDownloaded(preview.infoHash, error));
        assert(downloadedPort.snapshot(preview.infoHash)->mode == TransferMode::DownloadOnly);
        portLease.reset();
        assert(downloadedPort.installDownloaded(preview.infoHash, error));
        assert(downloadedPort.snapshot(preview.infoHash)->mode == TransferMode::PortInstall);
        assert(downloadedPort.snapshot(preview.infoHash)->status == DownloadStatus::Queued);
        downloadedPort.shutdown();
    }
    manager.shutdown();
    log_close();
    printf("test_web_server: all ok\n");
    return 0;
}
