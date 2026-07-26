#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app_settings.hpp"
#include "catalog_service.hpp"
#include "download_manager.hpp"
#include "http_server.hpp"
#include "install_space.hpp"
#include "web_add_queue.hpp"

namespace pipensx {

// The web companion: REST + SSE API and the static SPA, served over the LAN
// so a phone/PC can watch progress, control the queue, browse the catalog and
// add downloads. Owns the HttpServer thread and the WebAddQueue resolve
// worker; talks to the rest of the app ONLY through the thread-safe
// DownloadManager API and its own copies of catalog/settings state
// (CatalogService and AppSettings are UI-thread-only — never pass them in).
//
// Lifetime: construct after DownloadManager, shutdown() BEFORE
// DownloadManager::shutdown() so no server thread can touch a dead manager.
class WebServer {
public:
    static constexpr uint16_t kDefaultPort = 8080;

    // staticRoot: directory with the SPA files ("romfs:/web" on Switch,
    // "resources/web" on PC). resolver: test seam for WebAddQueue.
    WebServer(DownloadManager& manager, std::string staticRoot,
              std::string version, WebAddQueue::Resolver resolver = {});
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    bool start(uint16_t port = kDefaultPort);
    void stop();
    void shutdown();  // stop + join the add-queue worker
    bool running() const { return http_.running(); }
    uint16_t boundPort() const { return http_.boundPort(); }

    // Thread-safe setters, called from the UI thread on settings changes.
    // PIN: empty disables auth; otherwise required on mutating endpoints.
    void setPin(std::string pin);
    void setStreamSelection(StreamSelection selection);

    // UI-thread callers (startup + after every CatalogService::adopt()):
    // copies the entries so the server thread never touches CatalogService.
    void updateCatalog(const std::vector<CatalogEntry>& entries);

private:
    HttpResponse route(const HttpRequest& req);
    HttpResponse routeApi(const HttpRequest& req);
    HttpResponse routeStatic(const HttpRequest& req);
    HttpResponse handleTaskCommand(const std::string& id,
                                   const std::string& command,
                                   const HttpRequest& req);
    HttpResponse handleAddMagnet(const HttpRequest& req);
    HttpResponse handleAddCatalog(const HttpRequest& req);
    HttpResponse handleAddTorrent(const HttpRequest& req);
    HttpResponse handleCatalog(const HttpRequest& req);
    bool authorized(const HttpRequest& req) const;
    std::string buildStateJson();
    void onTick();

    DownloadManager& manager_;
    WebAddQueue addQueue_;
    HttpServer http_;
    std::string staticRoot_;
    std::string version_;

    mutable std::mutex configMutex_;
    std::string pin_;
    StreamSelection streamSelection_ = StreamSelection::AllFiles;

    std::mutex catalogMutex_;
    std::vector<CatalogEntry> catalog_;
    std::unordered_map<std::string, size_t> catalogIndex_;  // lower hash → idx
    uint64_t catalogGeneration_ = 0;
    // Built lazily on the server thread; invalidated by updateCatalog().
    std::shared_ptr<const std::string> catalogGzip_;
    uint64_t catalogGzipGeneration_ = 0;

    // Server-thread only.
    std::map<std::string, std::shared_ptr<const std::string>> staticCache_;
    StorageSpaceSnapshot storageCache_;
    uint64_t storageCacheAtMs_ = 0;
    // SSE change detection: last broadcast state frame + when it was sent, so
    // idle ticks push a tiny keepalive comment instead of an identical JSON
    // frame every second.
    std::string lastStateJson_;
    uint64_t lastSseSentMs_ = 0;
};

// gzip (RFC 1952) compression for Content-Encoding — note bug_report's
// compress2 produces zlib framing, which browsers reject here.
std::string gzipCompress(const std::string& input);

}  // namespace pipensx
