#pragma once

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

#include "app_settings.hpp"
#include "catalog_service.hpp"
#include "download_manager.hpp"
#include "http_server.hpp"
#include "install_space.hpp"
#include "install_benchmark.hpp"
#include "web_add_queue.hpp"

namespace pipensx {
struct GameMetadataIndexSnapshot;

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
    // PIN: required on mutating endpoints; empty fails closed.
    void setPin(std::string pin);
    void setStreamSelection(StreamSelection selection);
    // Test seam. Empty uses the real provider clients.
    void setDebridProviderFactory(DebridProviderFactory factory);
    // Console one-tap context for catalog Install. Called on the add worker.
    // The Switch wiring hops to the UI thread; tests omit it.
    void setOneTapContextLookup(
        std::function<OneTapContext(const std::string& infoHash,
                                    const std::string& titleId)> lookup);

    // Companion settings. AppSettings is UI-thread-only — never pass it in.
    // Getter/patcher run on the HTTP thread; Switch wiring hops to the UI
    // thread. Getter: fill json. Patcher: persist, apply runtime, fill json.
    // Both return false on failure (error is a message for the client).
    using SettingsGetter =
        std::function<bool(std::string& json, std::string& error)>;
    using SettingsPatcher = std::function<bool(
        const std::string& body, std::string& error, std::string& json)>;
    void setSettingsHandlers(SettingsGetter getter, SettingsPatcher patcher);

    using UpdateStatusGetter = std::function<bool(
        bool& stagedReady, uint64_t& stagedBytes, std::string& error)>;
    using UpdateUploader = std::function<bool(
        const std::string& nroBytes, std::string& error)>;
    using UpdateApplier = std::function<bool(std::string& error)>;
    void setUpdateHandlers(UpdateStatusGetter statusGetter,
                           UpdateUploader uploader,
                           UpdateApplier applier);
    using LibraryGetter = std::function<bool(std::string&, std::string&)>;
    using LibraryAction = std::function<bool(const std::string& action,
                                             const std::string& titleId,
                                             std::string& error)>;
    void setLibraryHandlers(LibraryGetter getter, LibraryAction action);
    using PortHandler = std::function<bool(const std::string& action,
        const std::string& taskId, const std::string& body, std::string& json, std::string& error)>;
    void setPortHandler(PortHandler handler);

    // UI-thread callers (startup + after every CatalogService::adopt()):
    // copies the entries so the server thread never touches CatalogService.
    // Takes a shared immutable snapshot (see CatalogService::sharedEntries)
    // instead of deep-copying the ~10 MB catalogue into the server.
    void updateCatalog(std::shared_ptr<const std::vector<CatalogEntry>> entries);
    void updateMetadata(std::shared_ptr<const GameMetadataIndexSnapshot> metadata);
    bool benchmarkActive() const { return benchmark_.running(); }

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
    HttpResponse handleCatalogDescription(const HttpRequest& req);
    HttpResponse handleLogs(const HttpRequest& req);
    void rebuildCatalogIndexLocked();
    HttpResponse handleGetSettings(const HttpRequest& req);
    HttpResponse handlePatchSettings(const HttpRequest& req);
    HttpResponse handleUpdateStatus(const HttpRequest& req);
    HttpResponse handleUpdateUpload(const HttpRequest& req);
    HttpResponse handleUpdateApply(const HttpRequest& req);
    HttpResponse handleLibrary(const HttpRequest& req);
    HttpResponse handleLibraryCommand(const HttpRequest& req,
                                      const std::string& titleId,
                                      const std::string& action);
    void libraryWorkerMain();
    HttpResponse handlePortRequest(const HttpRequest& req, const std::string& action,
                                   const std::string& taskId, const std::string& body);
    bool authorized(const HttpRequest& req) const;
    // False when the request names an origin other than the host it reached.
    static bool sameOrigin(const HttpRequest& req);
    std::string buildStateJson();
    void onTick();

    DownloadManager& manager_;
    InstallBenchmarkService benchmark_;
    WebAddQueue addQueue_;
    HttpServer http_;
    std::string staticRoot_;
    std::string version_;

    mutable std::mutex configMutex_;
    std::string pin_;
    StreamSelection streamSelection_ = StreamSelection::AllFiles;
    DebridProviderFactory debridFactory_;
    SettingsGetter settingsGetter_;
    SettingsPatcher settingsPatcher_;
    UpdateStatusGetter updateStatusGetter_;
    UpdateUploader updateUploader_;
    UpdateApplier updateApplier_;
    LibraryGetter libraryGetter_;
    LibraryAction libraryAction_;
    PortHandler portHandler_;
    struct LibraryJob {
        std::string id, action, titleId, state = "queued", error;
    };
    std::vector<LibraryJob> libraryJobs_;
    std::condition_variable libraryCondition_;
    std::thread libraryWorker_;
    bool libraryStopping_ = false;
    uint64_t librarySerial_ = 0;

    std::mutex catalogMutex_;
    // Shared immutable snapshot published by CatalogService — never null.
    std::shared_ptr<const std::vector<CatalogEntry>> catalog_ =
        std::make_shared<const std::vector<CatalogEntry>>();
    std::unordered_map<std::string, size_t> catalogIndex_;  // lower hash → idx
    uint64_t catalogGeneration_ = 0;
    std::shared_ptr<const GameMetadataIndexSnapshot> metadata_;
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
