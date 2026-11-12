#include "web_server.hpp"
#include "catalog_descriptions.hpp"
#include "catalog_section.hpp"
#include "installed_title_service.hpp"
#include "game_metadata_service.hpp"
#include "add_release.hpp"
#include "companion_settings.hpp"
#include "task_actions.hpp"
#include "task_files.hpp"
#include "port_selection.hpp"

extern "C" {
#include "../core/util.h"
#include "../core/sha256.h"
}

#include <borealis/extern/nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <unistd.h>

namespace pipensx {

using Json = nlohmann::json;

namespace {

std::atomic<uint64_t> gUploadSerial{1};

constexpr uint64_t kStorageCacheTtlMs = 10 * 1000;
constexpr size_t kWebLogTailBytes = 512 * 1024;
constexpr size_t kMaxTorrentUploadBytes = 2 * 1024 * 1024;
constexpr size_t kMaxUpdateNroBytes = 64 * 1024 * 1024;

HttpServer::Options webHttpOptions() {
    HttpServer::Options options;
    options.bodyCap = kMaxUpdateNroBytes;
    return options;
}

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

// Catalog titles/descriptions come from a RuTracker dump and torrent names
// from arbitrary metainfo — both can carry broken UTF-8 (e.g. Cyrillic cut
// mid-sequence). Plain dump() throws type_error.316 on those, turning the
// whole endpoint into a 500; replace bad bytes with U+FFFD instead.
std::string dumpJson(const Json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

HttpResponse jsonError(int status, const std::string& message) {
    Json j;
    j["error"] = message;
    return HttpResponse::text(status, dumpJson(j));
}

const char* catalogHealthName(CatalogHealth health) {
    switch (health) {
        case CatalogHealth::Ok: return "ok";
        case CatalogHealth::NoPeers: return "noPeers";
        case CatalogHealth::MetadataTimeout: return "metadataTimeout";
        case CatalogHealth::TrackerNotRegistered: return "notRegistered";
        case CatalogHealth::Replaced: return "replaced";
        case CatalogHealth::Dead: return "dead";
        case CatalogHealth::Unknown: return "unknown";
    }
    return "unknown";
}

const char* modeName(TransferMode mode) {
    if (mode == TransferMode::PortInstall)
        return "port";
    if (mode == TransferMode::AutoRunInstall)
        return "autorun";
    return mode == TransferMode::StreamInstall ? "install" : "download";
}

bool parseMode(const std::string& value, TransferMode& out) {
    if (value == "install") {
        out = TransferMode::StreamInstall;
        return true;
    }
    if (value == "port") {
        out = TransferMode::PortInstall;
        return true;
    }
    if (value == "autorun") {
        out = TransferMode::AutoRunInstall;
        return true;
    }
    if (value == "download") {
        out = TransferMode::DownloadOnly;
        return true;
    }
    return false;
}

const char* magnetStageName(MagnetProgress::Stage stage) {
    switch (stage) {
        case MagnetProgress::Stage::FindingPeers: return "findingPeers";
        case MagnetProgress::Stage::Connecting: return "connecting";
        case MagnetProgress::Stage::FetchingMetadata: return "fetchingMetadata";
        case MagnetProgress::Stage::Validating: return "validating";
    }
    return "unknown";
}

// Path pieces after "/api/", e.g. "/api/tasks/abc/pause" → {"tasks","abc","pause"}
std::vector<std::string> splitApiPath(const std::string& path) {
    std::vector<std::string> parts;
    size_t pos = 5;  // strlen("/api/")
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        if (slash > pos) parts.push_back(path.substr(pos, slash - pos));
        pos = slash + 1;
    }
    return parts;
}

struct StaticEntry {
    const char* file;
    const char* contentType;
    const char* cacheControl;
};

const std::map<std::string, StaticEntry>& staticTable() {
    static const std::map<std::string, StaticEntry> table = {
        {"/", {"index.html", "text/html; charset=utf-8", "no-cache"}},
        {"/index.html",
         {"index.html", "text/html; charset=utf-8", "no-cache"}},
        {"/app.js",
         {"app.js", "application/javascript; charset=utf-8",
          "no-store"}},
        {"/app.css", {"app.css", "text/css; charset=utf-8", "no-store"}},
        {"/i18n.json", {"i18n.json", "application/json; charset=utf-8", "no-store"}},
        {"/i18n.js", {"i18n.js", "application/javascript; charset=utf-8", "no-store"}},
        {"/manifest.webmanifest",
         {"manifest.webmanifest", "application/manifest+json", "no-cache"}},
        {"/icon-192.png", {"icon-192.png", "image/png", "max-age=3600"}},
        {"/icon-512.png", {"icon-512.png", "image/png", "max-age=3600"}},
    };
    return table;
}

bool readFileBinary(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[16384];
    out.clear();
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

}  // namespace

std::string gzipCompress(const std::string& input) {
    z_stream stream{};
    // 15 + 16 selects gzip framing (RFC 1952) instead of zlib.
    // Default level, not Z_BEST_COMPRESSION: on a multi-MB catalog level 9
    // costs several times the CPU of level 6 for a couple of percent of size,
    // and this runs on the Switch's server thread.
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return "";
    std::string out;
    out.resize(deflateBound(&stream, (uLong)input.size()));
    stream.next_in = (Bytef*)input.data();
    stream.avail_in = (uInt)input.size();
    stream.next_out = (Bytef*)out.data();
    stream.avail_out = (uInt)out.size();
    int r = deflate(&stream, Z_FINISH);
    if (r != Z_STREAM_END) {
        deflateEnd(&stream);
        return "";
    }
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

WebServer::WebServer(DownloadManager& manager, std::string staticRoot,
                     std::string version, WebAddQueue::Resolver resolver)
    : manager_(manager),
      benchmark_(manager),
      addQueue_(manager, std::move(resolver)),
      http_(webHttpOptions()),
      staticRoot_(std::move(staticRoot)),
      version_(std::move(version)) {
    instanceId_ = nowMs();
    http_.setTickCallback([this] { onTick(); });
}

WebServer::~WebServer() {
    shutdown();
    for (const auto& item : preparedTorrents_) unlink(item.second.path.c_str());
}

bool WebServer::start(uint16_t port) {
    if (http_.running()) return true;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        rebuildCatalogIndexLocked();
    }
    std::string error;
    bool ok = http_.start(
        port, [this](const HttpRequest& req) { return route(req); }, error);
    if (!ok)
        log_msg("[web] start on port %u failed: %s\n", (unsigned)port,
                error.c_str());
    else
        log_msg("[web] serving on port %u\n", (unsigned)http_.boundPort());
    return ok;
}

void WebServer::stop() { http_.stop(); }

void WebServer::shutdown() {
    http_.stop();
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        libraryStopping_ = true;
    }
    libraryCondition_.notify_all();
    if (libraryWorker_.joinable()) libraryWorker_.join();
    benchmark_.stop();
    addQueue_.shutdown();
}

void WebServer::setPin(std::string pin) {
    std::lock_guard<std::mutex> lock(configMutex_);
    pin_ = std::move(pin);
}

void WebServer::setLibraryHandlers(LibraryGetter getter, LibraryAction action) {
    std::lock_guard<std::mutex> lock(configMutex_);
    libraryGetter_ = std::move(getter);
    libraryAction_ = std::move(action);
}

void WebServer::setPortHandler(PortHandler handler) {
    std::lock_guard<std::mutex> lock(configMutex_);
    portHandler_ = std::move(handler);
}

HttpResponse WebServer::handlePortRequest(const HttpRequest& req, const std::string& action,
                                         const std::string& taskId, const std::string& body) {
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");
    PortHandler handler;
    { std::lock_guard<std::mutex> lock(configMutex_); handler = portHandler_; }
    if (!handler) return jsonError(503, "Port editor unavailable");
    std::string json, error;
    if (!handler(action, taskId, body, json, error)) return jsonError(409, error);
    return HttpResponse::text(action == "start" ? 202 : 200, std::move(json));
}

void WebServer::updateMetadata(std::shared_ptr<const GameMetadataIndexSnapshot> metadata) {
    std::lock_guard<std::mutex> lock(catalogMutex_);
    metadata_ = std::move(metadata);
    ++catalogGeneration_;
    catalogGzip_.reset();
}

void WebServer::libraryWorkerMain() {
    for (;;) {
        LibraryJob job;
        LibraryAction action;
        MaintenanceHandler maintenance;
        {
            std::unique_lock<std::mutex> lock(configMutex_);
            libraryCondition_.wait(lock, [this] {
                return libraryStopping_ || std::any_of(libraryJobs_.begin(), libraryJobs_.end(),
                    [](const LibraryJob& j) { return j.state == "queued"; });
            });
            if (libraryStopping_) return;
            auto found = std::find_if(libraryJobs_.begin(), libraryJobs_.end(),
                [](const LibraryJob& j) { return j.state == "queued"; });
            found->state = "working";
            job = *found;
            action = libraryAction_;
            maintenance = maintenanceHandler_;
        }
        std::string error, result;
        log_msg("[web_library] action=%s title=%s started\n", job.action.c_str(), job.titleId.c_str());
        bool ok = false;
        try {
            ok = job.action.compare(0, 8, "storage-") == 0
                ? maintenance && maintenance(job.action.substr(8), result, error)
                : action && action(job.action, job.titleId, error);
        } catch (const std::exception& e) { error = e.what(); }
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            if (ok && !result.empty()) maintenanceJson_ = std::move(result);
            for (auto& item : libraryJobs_) if (item.id == job.id) {
                item.state = ok ? "done" : "error";
                item.error = error;
                break;
            }
        }
        log_msg("[web_library] action=%s title=%s result=%s error=%s\n",
                job.action.c_str(), job.titleId.c_str(), ok ? "done" : "error", error.c_str());
    }
}

HttpResponse WebServer::handleLibrary(const HttpRequest& req) {
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");
    LibraryGetter getter;
    { std::lock_guard<std::mutex> lock(configMutex_); getter = libraryGetter_; }
    if (!getter) return jsonError(501, "installed library unavailable");
    std::string data, error;
    if (!getter(data, error)) return jsonError(503, error);
    Json response = Json::parse(data, nullptr, false);
    if (response.is_discarded() || !response.is_object())
        return jsonError(500, "invalid library snapshot");
    response["jobs"] = Json::array();
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        for (const auto& job : libraryJobs_)
            if (job.action.compare(0, 8, "storage-") != 0)
            response["jobs"].push_back({{"jobId", job.id}, {"action", job.action},
                {"titleId", job.titleId}, {"state", job.state}, {"error", job.error}});
    }
    return HttpResponse::text(200, dumpJson(response));
}

void WebServer::setCatalogUploader(CatalogUploader uploader) {
    std::lock_guard<std::mutex> lock(configMutex_);
    catalogUploader_ = std::move(uploader);
}

HttpResponse WebServer::handleLibraryCommand(const HttpRequest& req,
    const std::string& requestedId, const std::string& action) {
    const Json body = req.body.empty() ? Json::object() : Json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return jsonError(400, "invalid JSON body");
    const bool storageAction = action == "storage-scan" || action == "storage-images" ||
        action == "storage-torrents" || action == "storage-downloads" ||
        action == "storage-temporary" || action == "storage-system" || action == "storage-tickets";
    uint64_t parsedId = 0;
    if (!storageAction && action != "refresh" && action != "catalog-refresh" && (!InstalledTitleService::parseTitleId(requestedId, parsedId) || !parsedId))
        return jsonError(400, "invalid title id");
    const std::string titleId = storageAction || action == "refresh" || action == "catalog-refresh" ? "" : InstalledTitleService::formatTitleId(parsedId);
    if ((action == "uninstall" || (storageAction && action != "storage-scan")) &&
        (!body.contains("confirm") || !body["confirm"].is_boolean() ||
                                  !body["confirm"].get<bool>()))
        return jsonError(400, "confirmation required");
    if (action == "update") {
        LibraryGetter getter;
        { std::lock_guard<std::mutex> lock(configMutex_); getter = libraryGetter_; }
        std::string data, error;
        if (!getter || !getter(data, error)) return jsonError(503, "installed library unavailable");
        const Json library = Json::parse(data, nullptr, false);
        if (library.is_discarded() || !library.is_object() || !library.contains("titles") ||
            !library["titles"].is_array()) return jsonError(500, "invalid library snapshot");
        std::string hash;
        for (const auto& row : library["titles"]) if (row.value("titleId", "") == titleId) {
            if (row.value("updateState", "") != "update_available")
                return jsonError(409, "no verified update available");
            if (row.value("hasLayeredFsMods", false) &&
                (!body.contains("acknowledgeMods") || !body["acknowledgeMods"].is_boolean() ||
                 !body["acknowledgeMods"].get<bool>()))
                return jsonError(409, "review installed mods before updating");
            hash = lowerAscii(row.value("updateHash", ""));
            break;
        }
        if (hash.empty()) return jsonError(409, "no matching update bundle");
        CatalogEntry entry;
        {
            std::lock_guard<std::mutex> lock(catalogMutex_);
            auto found = catalogIndex_.find(hash);
            if (found == catalogIndex_.end()) return jsonError(404, "update bundle missing");
            entry = (*catalog_)[found->second];
        }
        const std::string jobId = addQueue_.enqueue(entry.title, entry.magnetUri, hash,
            entry.infoDict, TransferMode::StreamInstall, StreamSelection::AllFiles, error,
            AddInputKind::Catalog, titleId, true);
        if (jobId.empty()) return jsonError(409, error);
        return HttpResponse::text(202, dumpJson({{"jobId", jobId}}));
    }
    if (!storageAction && action != "refresh" && action != "uninstall" && action != "catalog-refresh") return jsonError(404, "unknown library action");
    std::lock_guard<std::mutex> lock(configMutex_);
    if (!(storageAction ? bool(maintenanceHandler_) : bool(libraryAction_)) || libraryStopping_)
        return jsonError(503, "service unavailable");
    for (const auto& job : libraryJobs_)
        if ((job.state == "queued" || job.state == "working") &&
            job.action == action && job.titleId == titleId)
            return HttpResponse::text(202, dumpJson({{"jobId", job.id}}));
    if (libraryJobs_.size() >= 16) {
        libraryJobs_.erase(std::remove_if(libraryJobs_.begin(), libraryJobs_.end(),
            [](const LibraryJob& j) { return j.state == "done" || j.state == "error"; }), libraryJobs_.end());
        if (libraryJobs_.size() >= 16) return jsonError(409, "library queue full");
    }
    LibraryJob job;
    job.id = "library-" + std::to_string(++librarySerial_);
    job.action = action; job.titleId = titleId;
    libraryJobs_.push_back(job);
    if (!libraryWorker_.joinable()) libraryWorker_ = std::thread([this] { libraryWorkerMain(); });
    libraryCondition_.notify_one();
    return HttpResponse::text(202, dumpJson({{"jobId", job.id}}));
}

void WebServer::setDebridProviderFactory(DebridProviderFactory factory) {
    std::lock_guard<std::mutex> lock(configMutex_);
    debridFactory_ = factory;
    addQueue_.setDebridProviderFactory(std::move(factory));
}

void WebServer::setOneTapContextLookup(
    std::function<OneTapContext(const std::string& infoHash,
                                const std::string& titleId)> lookup) {
    addQueue_.setOneTapContextLookup(std::move(lookup));
}

void WebServer::setStreamSelection(StreamSelection selection) {
    std::lock_guard<std::mutex> lock(configMutex_);
    streamSelection_ = selection;
}

void WebServer::setSettingsHandlers(SettingsGetter getter,
                                    SettingsPatcher patcher) {
    std::lock_guard<std::mutex> lock(configMutex_);
    settingsGetter_ = std::move(getter);
    settingsPatcher_ = std::move(patcher);
}

void WebServer::setUpdateHandlers(UpdateStatusGetter statusGetter,
                                  UpdateUploader uploader,
                                  UpdateApplier applier) {
    std::lock_guard<std::mutex> lock(configMutex_);
    updateStatusGetter_ = std::move(statusGetter);
    updateUploader_ = std::move(uploader);
    updateApplier_ = std::move(applier);
}

void WebServer::updateCatalog(
    std::shared_ptr<const std::vector<CatalogEntry>> entries) {
    if (!entries)
        entries = std::make_shared<const std::vector<CatalogEntry>>();
    std::lock_guard<std::mutex> lock(catalogMutex_);
    catalog_ = std::move(entries);
    ++catalogGeneration_;
    catalogGzip_ = nullptr;
    if (http_.running())
        rebuildCatalogIndexLocked();
    else
        catalogIndex_.clear();
}

void WebServer::rebuildCatalogIndexLocked() {
    catalogIndex_.clear();
    catalogIndex_.reserve(catalog_->size());
    for (size_t i = 0; i < catalog_->size(); ++i)
        catalogIndex_[lowerAscii((*catalog_)[i].infoHash)] = i;
}

// A cross-site request carries an Origin; a same-origin one names the host
// the client already dialled, which is exactly what Host says. No Origin at
// all means it is not a browser (curl, a script on the LAN) — allowed, the
// companion is a LAN service by design. "null" (sandboxed iframe, file://)
// has no "://" and is rejected.
bool WebServer::sameOrigin(const HttpRequest& req) {
    std::string origin = req.header("origin");
    if (origin.empty()) return true;
    size_t scheme = origin.find("://");
    if (scheme == std::string::npos) return false;
    return origin.substr(scheme + 3) == req.header("host");
}

bool WebServer::authorized(const HttpRequest& req) const {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (pin_.empty()) return false;
    // Header only: a PIN in the query string lands in browser history and
    // logs, and would let a plain cross-site link carry it.
    std::string provided = req.header("x-pipensx-pin");
    if (provided.size() != pin_.size()) return false;
    // constant-time compare; a LAN PIN hardly merits it but it costs nothing
    unsigned char diff = 0;
    for (size_t i = 0; i < pin_.size(); ++i)
        diff |= (unsigned char)(provided[i] ^ pin_[i]);
    return diff == 0;
}

static Json taskActionJson(const TaskAction& action) {
    Json j;
    j["allowed"] = action.allowed;
    j["reason"] = taskActionReasonName(action.reason);
    return j;
}

std::string WebServer::buildStateJson() {
    Json state;
    state["version"] = version_;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        state["catalogGeneration"] = catalogGeneration_;
    }
    std::vector<DownloadTask> snapshot = manager_.snapshotUi();
    const std::string leasedId = manager_.externalDeployTaskId();
    Json tasks = Json::array();
    uint64_t now = nowMs();
    for (const DownloadTask& t : snapshot) {
        Json j;
        j["id"] = t.id;
        j["name"] = t.name;
        j["status"] = statusName(t.status);
        j["mode"] = modeName(t.mode);
        j["error"] = t.error;
        j["totalBytes"] = t.totalBytes;
        j["completedBytes"] = t.completedBytes;
        const auto wanted = downloadProgressBytes(t);
        j["wantedTotalBytes"] = wanted.second;
        j["wantedCompletedBytes"] = wanted.first;
        j["fetchProgress"] = t.fetchProgress;
        j["speedBps"] = t.speedBytesPerSecond;
        j["uploadedBytes"] = t.uploadedBytes;
        j["uploadSpeedBps"] = t.uploadSpeedBytesPerSecond;
        j["seedCacheBytes"] = t.seedCacheBytes;
        j["seeding"] = t.seeding;
        j["peers"] = t.peers;
        j["dhtGood"] = t.dhtGood;
        j["dhtDubious"] = t.dhtDubious;
        j["piecesDone"] = t.piecesDone;
        j["piecesTotal"] = t.piecesTotal;
        j["piecesVerified"] = t.piecesVerified;
        j["packageCount"] = t.packageCount;
        j["packagesInstalled"] = t.packagesInstalled;
        j["installedBytes"] = t.installedBytes;
        j["installTotalBytes"] = t.installTotalBytes;
        j["installSpeedBps"] = currentInstallSpeed(t, now);
        const auto eta = taskEtaSeconds(t, now);
        j["etaSeconds"] = eta ? *eta : 0;
        j["currentPackage"] = t.currentPackage;
        const RecoveryAccount recovery = recoveryAccountOf(t);
        Json recoveryJson;
        recoveryJson["work"] = recoveryWorkName(recovery.work);
        recoveryJson["downloadedBytes"] = recovery.downloadedBytes;
        recoveryJson["downloadTotalBytes"] = recovery.downloadTotalBytes;
        recoveryJson["packagesInstalled"] = recovery.packagesInstalled;
        recoveryJson["packageCount"] = recovery.packageCount;
        recoveryJson["savedBytes"] = recovery.savedBytes;
        recoveryJson["reworkBytes"] = recovery.reworkBytes;
        recoveryJson["byteExact"] = recovery.byteExact;
        recoveryJson["verifyingSaved"] = recovery.verifyingSaved;
        recoveryJson["showDownloaded"] = recovery.showDownloaded;
        recoveryJson["showInstalled"] = recovery.showInstalled;
        j["recovery"] = std::move(recoveryJson);
        const TaskCapabilities caps =
            taskCapabilities(t, t.id == leasedId);
        Json capabilities;
        capabilities["pause"] = taskActionJson(caps.pause);
        capabilities["resume"] = taskActionJson(caps.resume);
        capabilities["verify"] = taskActionJson(caps.verify);
        capabilities["remove"] = taskActionJson(caps.remove);
        capabilities["move"] = taskActionJson(caps.move);
        j["capabilities"] = std::move(capabilities);
        tasks.push_back(std::move(j));
    }
    state["tasks"] = std::move(tasks);

    Json jobs = Json::array();
    for (const WebAddJob& job : addQueue_.snapshot()) {
        Json j;
        j["jobId"] = job.jobId;
        j["title"] = job.title;
        j["infoHash"] = job.infoHashHex;
        j["mode"] = modeName(job.requestedMode);
        j["state"] = webAddJobStateName(job.state);
        j["stage"] = magnetStageName(job.progress.stage);
        j["peerIndex"] = job.progress.peerIndex;
        j["peerCount"] = job.progress.peerCount;
        j["metaPieces"] = job.progress.completedPieces;
        j["metaPiecesTotal"] = job.progress.totalPieces;
        j["error"] = job.error;
        j["taskId"] = job.taskId;
        jobs.push_back(std::move(j));
    }
    state["jobs"] = std::move(jobs);

    if (!storageCacheAtMs_ || now - storageCacheAtMs_ > kStorageCacheTtlMs) {
        storageCache_ = queryStorageSpace(manager_.downloadRoot());
        storageCacheAtMs_ = now;
    }
    Json storage;
    storage["totalBytes"] = storageCache_.totalBytes;
    storage["freeBytes"] = storageCache_.freeBytes;
    storage["available"] = storageCache_.available;
    state["storage"] = std::move(storage);

    // Queue summary for the downloads dashboard: counts per status plus
    // aggregate throughput and drain ETA. Computed from the same snapshot
    // so the header never disagrees with the card list.
    const QueueSummary summary = summarizeQueue(snapshot, now);
    Json q;
    q["downloading"] = summary.downloading;
    q["queued"] = summary.queued;
    q["installing"] = summary.installing;
    q["paused"] = summary.paused;
    q["completed"] = summary.completed;
    q["errors"] = summary.errors;
    q["downloadSpeedBps"] = summary.downloadSpeedBps;
    q["installSpeedBps"] = summary.installSpeedBps;
    q["remainingBytes"] = summary.totalRemainingBytes;
    q["etaSeconds"] = summary.etaSeconds;
    state["summary"] = std::move(q);

    // Download source for the companion sidebar: direct swarm vs the
    // active debrid provider, plus whether its key/URL is set. Secrets
    // stay flags, not values — same rule as companionSettingsJson — and
    // the frame stays on the open read path so the sidebar needs no PIN.
    Json source;
    source["direct"] = manager_.torrentingEnabled();
    source["provider"] = debridProviderJson(manager_.activeDebridProvider());
    source["keyConfigured"] = !manager_.activeDebridCredential().empty();
    state["source"] = std::move(source);
    return dumpJson(state);
}

void WebServer::onTick() {
    if (http_.sseClientCount() == 0) {
        lastStateJson_.clear();
        return;
    }
    constexpr uint64_t kSseKeepaliveMs = 15000;
    std::string state = buildStateJson();
    uint64_t now = nowMs();
    if (state == lastStateJson_) {
        // Nothing changed — keep the connection visibly alive (and let dead
        // clients surface as send failures) without re-sending the frame.
        if (now - lastSseSentMs_ >= kSseKeepaliveMs) {
            lastSseSentMs_ = now;
            http_.broadcastSse(": keepalive\n\n");
        }
        return;
    }
    lastSseSentMs_ = now;
    http_.broadcastSse("event: state\ndata: " + state + "\n\n");
    lastStateJson_ = std::move(state);
}

HttpResponse WebServer::route(const HttpRequest& req) {
    if (req.path.compare(0, 5, "/api/") == 0) return routeApi(req);
    return routeStatic(req);
}

HttpResponse WebServer::routeStatic(const HttpRequest& req) {
    if (req.method != "GET" && req.method != "HEAD")
        return jsonError(405, "method not allowed");
    auto it = staticTable().find(req.path);
    if (it == staticTable().end()) return jsonError(404, "not found");
    const StaticEntry& entry = it->second;
    auto cached = staticCache_.find(entry.file);
    std::shared_ptr<const std::string> body;
    if (cached != staticCache_.end()) {
        body = cached->second;
    } else {
        std::string data;
        if (!readFileBinary(staticRoot_ + "/" + entry.file, data))
            return jsonError(404, "not found");
        body = std::make_shared<const std::string>(std::move(data));
        staticCache_[entry.file] = body;
    }
    HttpResponse resp;
    resp.status = 200;
    resp.contentType = entry.contentType;
    resp.body = body;
    resp.extraHeaders.push_back({"Cache-Control", entry.cacheControl});
    return resp;
}

HttpResponse WebServer::routeApi(const HttpRequest& req) {
    std::vector<std::string> parts = splitApiPath(req.path);
    if (parts.empty()) return jsonError(404, "not found");

    if (req.method == "GET" || req.method == "HEAD") {
        if (parts[0] == "maintenance" && parts.size() == 1) {
            if (!sameOrigin(req)) return jsonError(403, "cross-origin");
            if (!authorized(req)) return jsonError(401, "pin");
            std::lock_guard<std::mutex> lock(configMutex_);
            Json jobs = Json::array();
            for (const auto& job : libraryJobs_) if (job.action.compare(0, 8, "storage-") == 0)
                jobs.push_back({{"jobId", job.id}, {"state", job.state}, {"error", job.error}});
            return HttpResponse::text(200, dumpJson({{"storage", Json::parse(maintenanceJson_)}, {"jobs", jobs}}));
        }
        if (parts[0] == "favorites" && parts.size() == 1)
            return handleFavorites(req, false);
        if (parts[0] == "tasks" && parts.size() == 3 && parts[2] == "files")
            return handleTaskFiles(req, parts[1]);
        if (parts[0] == "ports" && parts.size() == 2) {
            if (parts[1] == "folders")
                return handlePortRequest(req, "folders", "", Json{{"path", req.queryParam("path")}}.dump());
            return handlePortRequest(req, "get", parts[1], "");
        }
        if (parts[0] == "info" && parts.size() == 1) {
            Json j;
            j["name"] = "pipensx";
            j["version"] = version_;
            j["instanceId"] = instanceId_;
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                j["authRequired"] = true;  // mutations always require a PIN
            }
            j["maxTorrentBytes"] = kMaxTorrentUploadBytes;
            j["maxUpdateNroBytes"] = kMaxUpdateNroBytes;
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                j["features"] = {{"favorites", bool(favoritesHandler_)},
                    {"library", bool(libraryGetter_)}, {"ports", bool(portHandler_)},
                    {"update", bool(updateUploader_)}, {"catalogRefresh", bool(libraryAction_)},
                    {"maintenance", bool(maintenanceHandler_)}, {"catalogUpload", bool(catalogUploader_)}};
            }
            return HttpResponse::text(200, dumpJson(j));
        }
        if (parts[0] == "tasks" && parts.size() == 1)
            return HttpResponse::text(200, buildStateJson());
        if (parts[0] == "library" && parts.size() == 1) return handleLibrary(req);
        if (parts[0] == "events" && parts.size() == 1) {
            Json hello;
            hello["version"] = version_;
            HttpResponse r = HttpResponse::text(
                200,
                "retry: 5000\n\nevent: hello\ndata: " + dumpJson(hello) +
                    "\n\nevent: state\ndata: " + buildStateJson() + "\n\n",
                "text/event-stream");
            r.sse = true;
            return r;
        }
        if (parts[0] == "catalog" && parts.size() == 1)
            return handleCatalog(req);
        if (parts[0] == "catalog" && parts.size() == 2 && parts[1] == "description")
            return handleCatalogDescription(req);
        if (parts[0] == "catalog" && parts.size() == 2 && parts[1] == "coverage") {
            const std::string locale = descriptionLocale(req.queryParam("lang"));
            if (locale.empty()) return jsonError(400, "unsupported language");
            std::shared_ptr<const std::vector<CatalogEntry>> snapshot;
            { std::lock_guard<std::mutex> lock(catalogMutex_); snapshot = catalog_; }
            size_t available = 0;
            Json missing = Json::array();
            for (const auto& entry : *snapshot) {
                if (hasOfflineCatalogDescription(entry.description, locale)) ++available;
                else if (missing.size() < 10) missing.push_back(entry.infoHash);
            }
            return HttpResponse::text(200, dumpJson({{"locale", locale},
                {"total", snapshot->size()}, {"available", available}, {"missing", missing}}));
        }
        if (parts[0] == "storage" && parts.size() == 1) {
            StorageSpaceSnapshot snap =
                queryStorageSpace(manager_.downloadRoot());
            Json j;
            j["totalBytes"] = snap.totalBytes;
            j["freeBytes"] = snap.freeBytes;
            j["available"] = snap.available;
            return HttpResponse::text(200, dumpJson(j));
        }
        if (parts[0] == "settings" && parts.size() == 1)
            return handleGetSettings(req);
        if (parts[0] == "logs" && parts.size() == 1)
            return handleLogs(req);
        if (parts[0] == "benchmark" && parts.size() == 1) {
            if (!authorized(req)) return jsonError(401, "pin");
            return HttpResponse::text(200, benchmark_.statusJson());
        }
        if (parts[0] == "update" && parts.size() == 1)
            return handleUpdateStatus(req);
        return jsonError(404, "not found");
    }

    if (req.method == "PATCH") {
        if (!sameOrigin(req)) return jsonError(403, "cross-origin");
        if (!authorized(req)) return jsonError(401, "pin");
        if (parts[0] == "settings" && parts.size() == 1)
            return handlePatchSettings(req);
        if (parts[0] == "ports" && parts.size() == 2)
            return handlePortRequest(req, "save", parts[1], req.body);
        return jsonError(404, "not found");
    }

    // Everything below mutates → a page on another origin must not be able
    // to drive the console just because the browser can reach it.
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    // PIN check (auth/check exists so the UI can validate a remembered PIN
    // explicitly).
    if (!authorized(req)) return jsonError(401, "pin");
    if (req.method == "POST" && req.path == "/api/catalog/upload") {
        if (req.body.empty() || req.body.size() > 48 * 1024 * 1024)
            return jsonError(413, "catalog must be between 1 byte and 48 MiB");
        const auto expected = req.header("x-pipensx-sha256");
        if (!expected.empty()) {
            uint8_t digest[32];
            sha256(req.body.data(), req.body.size(), digest);
            char hex[65];
            for (size_t i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (expected != hex) return jsonError(400, "catalog checksum mismatch");
        }
        CatalogUploader upload;
        { std::lock_guard<std::mutex> lock(configMutex_); upload = catalogUploader_; }
        if (!upload) return jsonError(501, "catalog upload unavailable");
        std::string error;
        if (!upload(req.body, error)) return jsonError(409, error);
        return HttpResponse::text(200, "{}");
    }
    if (req.method == "POST" && parts[0] == "favorites" && parts.size() == 1)
        return handleFavorites(req, true);
    if (req.method == "POST" && parts[0] == "maintenance" && parts.size() == 2)
        return handleLibraryCommand(req, "", "storage-" + parts[1]);
    if (req.method == "POST" && parts[0] == "catalog" && parts.size() == 2 && parts[1] == "refresh")
        return handleLibraryCommand(req, "", "catalog-refresh");
    if (req.method == "POST" && parts[0] == "preview" && parts.size() == 2 &&
        (parts[1] == "catalog" || parts[1] == "torrent"))
        return handleFilePreview(req, parts[1] == "catalog");
    if (req.method == "POST" && parts[0] == "preview" && parts.size() == 2 && parts[1] == "discard") {
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("token") || !body["token"].is_string())
            return jsonError(400, "token required");
        auto found = preparedTorrents_.find(body["token"].get<std::string>());
        if (found != preparedTorrents_.end()) {
            unlink(found->second.path.c_str());
            preparedTorrents_.erase(found);
        }
        return HttpResponse::empty(204);
    }
    if (req.method == "POST" && parts[0] == "ports" && parts.size() == 3 &&
        (parts[2] == "preview" || parts[2] == "start"))
        return handlePortRequest(req, parts[2], parts[1], req.body);
    if (req.method == "POST" && parts[0] == "library") {
        if (parts.size() == 2 && parts[1] == "refresh")
            return handleLibraryCommand(req, "", "refresh");
        if (parts.size() == 3)
            return handleLibraryCommand(req, parts[1], parts[2]);
        return jsonError(404, "unknown library action");
    }
    if (req.method == "POST" && parts.size() == 2 && parts[0] == "benchmark" && parts[1] == "cancel") {
        benchmark_.stop();
        return HttpResponse::text(200, benchmark_.statusJson());
    }
    if (req.method == "POST" && parts[0] == "benchmark" && parts.size() == 1) {
        const Json request = Json::parse(req.body, nullptr, false);
        if (request.is_discarded() || !request.is_object() ||
            !request.contains("taskId") || !request["taskId"].is_string())
            return jsonError(400, "taskId required");
        std::string error;
        if (!benchmark_.start(request["taskId"].get<std::string>(), error))
            return jsonError(409, error);
        return HttpResponse::text(202, benchmark_.statusJson());
    }
    if (parts[0] == "auth" && parts.size() == 2 && parts[1] == "check")
        return HttpResponse::empty(204);
    if (parts[0] == "tasks" && parts.size() == 3)
        return handleTaskCommand(parts[1], parts[2], req);
    if (parts[0] == "queue" && parts.size() == 2) {
        if (parts[1] == "pause-all") {
            manager_.pauseAll();
            return HttpResponse::empty(204);
        }
        if (parts[1] == "resume-all") {
            manager_.resumeAll();
            return HttpResponse::empty(204);
        }
        if (parts[1] == "clear-completed") {
            bool deleteData = false;
            if (!req.body.empty()) {
                Json body = Json::parse(req.body, nullptr, false);
                if (body.is_discarded())
                    return jsonError(400, "invalid JSON body");
                deleteData = body.value("deleteData", false);
            }
            std::string error;
            ClearCompletedResult result;
            if (!manager_.clearCompleted(deleteData, error, &result))
                return jsonError(409, error.empty() ? "rejected" : error);
            Json body;
            body["cleared"] = result.cleared;
            body["skippedBusy"] = result.skippedBusy;
            return HttpResponse::text(200, dumpJson(body));
        }
    }
    if (parts[0] == "jobs" && parts.size() == 3 && parts[2] == "cancel")
        return addQueue_.cancel(parts[1]) ? HttpResponse::empty(204)
                                          : jsonError(404, "unknown job");
    if (parts[0] == "add" && parts.size() == 2) {
        if (parts[1] == "prepared") return handlePreparedAdd(req);
        if (parts[1] == "magnet") return handleAddMagnet(req);
        if (parts[1] == "catalog") return handleAddCatalog(req);
        if (parts[1] == "torrent") return handleAddTorrent(req);
    }
    if (parts[0] == "update" && parts.size() == 2) {
        if (parts[1] == "discard") {
            std::function<void()> discard;
            { std::lock_guard<std::mutex> lock(configMutex_); discard = updateDiscard_; }
            if (!discard) return jsonError(501, "update service unavailable");
            discard();
            return handleUpdateStatus(req);
        }
        if (parts[1] == "upload") return handleUpdateUpload(req);
        if (parts[1] == "apply") return handleUpdateApply(req);
    }
    return jsonError(404, "not found");
}

HttpResponse WebServer::handleFilePreview(const HttpRequest& req, bool fromCatalog) {
    const uint64_t now = nowMs();
    for (auto it = preparedTorrents_.begin(); it != preparedTorrents_.end();) {
        if (it->second.expiresAtMs < now) {
            unlink(it->second.path.c_str());
            it = preparedTorrents_.erase(it);
        } else ++it;
    }
    if (preparedTorrents_.size() >= 4) return jsonError(409, "Too many file previews. Try again in a few minutes.");
    const std::string token = std::to_string(gUploadSerial.fetch_add(1));
    const std::string path = manager_.rootPath() + "/_web_preview_" + token + ".torrent";
    std::string error;
    if (fromCatalog) {
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("infoHash") || !body["infoHash"].is_string())
            return jsonError(400, "infoHash required");
        CatalogEntry entry;
        {
            std::lock_guard<std::mutex> lock(catalogMutex_);
            auto found = catalogIndex_.find(lowerAscii(body["infoHash"].get<std::string>()));
            if (found == catalogIndex_.end()) return jsonError(404, "game not found");
            entry = (*catalog_)[found->second];
        }
        if (entry.infoDict.empty()) return jsonError(409, "File metadata is not available for this release. Upload its .torrent file to choose files.");
        if (!writeTorrentFromInfoDict(entry.magnetUri, entry.infoDict, path, error)) {
            unlink(path.c_str());
            return jsonError(400, error);
        }
    } else {
        if (req.body.empty() || req.body.size() > kMaxTorrentUploadBytes)
            return jsonError(400, "invalid torrent upload size");
        FILE* file = fopen(path.c_str(), "wb");
        if (!file) return jsonError(500, "cannot write temp file");
        const size_t written = fwrite(req.body.data(), 1, req.body.size(), file);
        const int closed = fclose(file);
        if (written != req.body.size() || closed != 0) {
            unlink(path.c_str());
            return jsonError(500, "cannot write temp file");
        }
    }
    TorrentPreview preview;
    if (!DownloadManager::previewTorrent(path, preview, error)) {
        unlink(path.c_str());
        return jsonError(400, error);
    }
    preparedTorrents_[token] = {path, now + 10 * 60 * 1000};
    Json files = Json::array();
    for (size_t i = 0; i < preview.files.size(); ++i)
        files.push_back({{"path", preview.files[i].path}, {"size", preview.files[i].length},
            {"installable", isTorrentFileInstallable(preview, i)}});
    return HttpResponse::text(200, dumpJson({{"token", token}, {"name", preview.name},
        {"infoHash", preview.infoHash}, {"files", files}}));
}

HttpResponse WebServer::handlePreparedAdd(const HttpRequest& req) {
    const auto body = Json::parse(req.body, nullptr, false);
    if (!body.is_object() || !body.contains("token") || !body["token"].is_string() ||
        !body.contains("files") || !body["files"].is_array())
        return jsonError(400, "token and files required");
    auto found = preparedTorrents_.find(body["token"].get<std::string>());
    if (found == preparedTorrents_.end() || found->second.expiresAtMs < nowMs())
        return jsonError(409, "File preview expired. Open the file list again.");
    AddRequest request;
    for (const auto& file : body["files"]) {
        if (!file.is_object() || !file.contains("path") || !file["path"].is_string() ||
            !file.contains("action") || !file["action"].is_number_unsigned() ||
            file["action"].get<uint64_t>() > 2)
            return jsonError(400, "invalid file action");
        request.fileChoices.emplace_back(file["path"].get<std::string>(), file["action"].get<uint8_t>());
    }
    if (request.fileChoices.empty()) return jsonError(400, "No files selected.");
    TorrentPreview preview;
    std::string error;
    if (!DownloadManager::previewTorrent(found->second.path, preview, error))
        return jsonError(400, error);
    const auto jobId = addQueue_.enqueue(preview.name, "", preview.infoHash, {},
        TransferMode::StreamInstall, StreamSelection::AllFiles, error, AddInputKind::TorrentFile,
        {}, false, found->second.path, std::move(request.fileChoices));
    if (jobId.empty()) return jsonError(409, error);
    preparedTorrents_.erase(found);
    return HttpResponse::text(202, dumpJson({{"jobId", jobId}}));
}

HttpResponse WebServer::handleUpdateStatus(const HttpRequest& req) {
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");
    UpdateStatusGetter getter;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        getter = updateStatusGetter_;
    }
    if (!getter) return jsonError(501, "update service unavailable");
    bool stagedReady = false;
    uint64_t stagedBytes = 0;
    std::string error;
    if (!getter(stagedReady, stagedBytes, error))
        return jsonError(500, error.empty() ? "update status unavailable" : error);
    Json j;
    j["currentVersion"] = version_;
    j["stagedReady"] = stagedReady;
    j["stagedBytes"] = stagedBytes;
    j["maxUploadBytes"] = kMaxUpdateNroBytes;
    j["source"] = "remoteUpload";
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        j["targetPath"] = updateTargetPath_;
        j["canApply"] = updateCanApply_;
    }
    return HttpResponse::text(200, dumpJson(j));
}

void WebServer::setUpdateContext(std::string targetPath, bool canApply,
                                  std::function<void()> discard) {
    std::lock_guard<std::mutex> lock(configMutex_);
    updateTargetPath_ = std::move(targetPath);
    updateCanApply_ = canApply;
    updateDiscard_ = std::move(discard);
}

void WebServer::setFavoritesHandler(FavoritesHandler handler) {
    std::lock_guard<std::mutex> lock(configMutex_);
    favoritesHandler_ = std::move(handler);
}

void WebServer::setMaintenanceHandler(MaintenanceHandler handler) {
    std::lock_guard<std::mutex> lock(configMutex_);
    maintenanceHandler_ = std::move(handler);
}

HttpResponse WebServer::handleFavorites(const HttpRequest& req, bool toggle) {
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");
    FavoritesHandler handler;
    { std::lock_guard<std::mutex> lock(configMutex_); handler = favoritesHandler_; }
    if (!handler) return jsonError(501, "favorites unavailable");
    std::string hash;
    if (toggle) {
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("infoHash") || !body["infoHash"].is_string())
            return jsonError(400, "infoHash required");
        hash = lowerAscii(body["infoHash"].get<std::string>());
        if (hash.size() != 40 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            return jsonError(400, "invalid infoHash");
    }
    std::string json, error;
    if (!handler(hash, toggle, json, error)) return jsonError(409, error);
    return HttpResponse::text(200, std::move(json));
}

HttpResponse WebServer::handleTaskFiles(const HttpRequest& req, const std::string& id) {
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");
    const auto task = manager_.snapshot(id);
    if (!task) return jsonError(404, "task not found");
    TaskFileInventory inventory;
    std::string error;
    if (!buildTaskFileInventory(manager_.rootPath(), *task, inventory, error))
        return jsonError(409, error);
    annotateTaskFileDestinations(inventory);
    Json files = Json::array();
    const char* states[] = {"pending", "present", "installed", "skipped", "missing", "unsafe"};
    const char* actions[] = {"skip", "download", "install"};
    for (const auto& file : inventory.files) {
        files.push_back({{"path", file.logicalPath}, {"size", file.size},
            {"state", states[static_cast<size_t>(file.state)]},
            {"action", actions[static_cast<size_t>(file.action)]},
            {"destination", file.destinationExample}, {"localPath", file.localPath}});
    }
    return HttpResponse::text(200, dumpJson({{"taskId", id}, {"rootPath", inventory.rootPath},
        {"complete", inventory.completeManifest}, {"files", files}}));
}

HttpResponse WebServer::handleUpdateUpload(const HttpRequest& req) {
    const std::string expected = req.header("x-pipensx-sha256");
    if (!expected.empty()) {
        uint8_t digest[32];
        sha256(req.body.data(), req.body.size(), digest);
        char hex[65];
        for (size_t i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (expected != hex) return jsonError(400, "update checksum mismatch");
    }
    if (req.body.empty()) return jsonError(400, "empty NRO upload");
    if (req.body.size() > kMaxUpdateNroBytes)
        return jsonError(413, "NRO upload too large");
    UpdateUploader uploader;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        uploader = updateUploader_;
    }
    if (!uploader) return jsonError(501, "update service unavailable");
    std::string error;
    if (!uploader(req.body, error)) {
        const int status = error.find("Unable to") == 0 ? 500 : 400;
        return jsonError(status, error.empty() ? "invalid NRO upload" : error);
    }
    return handleUpdateStatus(req);
}

HttpResponse WebServer::handleUpdateApply(const HttpRequest&) {
    UpdateApplier applier;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        applier = updateApplier_;
    }
    if (!applier) return jsonError(501, "update service unavailable");
    std::string error;
    if (!applier(error))
        return jsonError(409, error.empty() ? "update cannot be applied" : error);
    Json j;
    j["applying"] = true;
    return HttpResponse::text(202, dumpJson(j));
}

HttpResponse WebServer::handleGetSettings(const HttpRequest& req) {
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");
    SettingsGetter getter;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        getter = settingsGetter_;
    }
    if (!getter) return jsonError(501, "settings unavailable");
    std::string json;
    std::string error;
    if (!getter(json, error))
        return jsonError(error == "timed out" ? 503 : 500,
                         error.empty() ? "settings unavailable" : error);
    return HttpResponse::text(200, json);
}

HttpResponse WebServer::handleLogs(const HttpRequest& req) {
    // Logs can contain filenames, torrent/debrid ids and diagnostic details,
    // so unlike the public task snapshot they require the companion PIN.
    if (!sameOrigin(req)) return jsonError(403, "cross-origin");
    if (!authorized(req)) return jsonError(401, "pin");

    std::string tail(kWebLogTailBytes, '\0');
    tail.resize(log_read_tail(tail.data(), tail.size()));

    // A bounded tail can begin in the middle of a UTF-8 character or log
    // record. Dropping the first partial record keeps browser rendering sane.
    if (tail.size() == kWebLogTailBytes) {
        const size_t newline = tail.find('\n');
        if (newline != std::string::npos)
            tail.erase(0, newline + 1);
    }

    HttpResponse response =
        HttpResponse::text(200, tail, "text/plain; charset=utf-8");
    response.extraHeaders.push_back({"Cache-Control", "no-store"});
    return response;
}

HttpResponse WebServer::handlePatchSettings(const HttpRequest& req) {
    SettingsPatcher patcher;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        patcher = settingsPatcher_;
    }
    if (!patcher) return jsonError(501, "settings unavailable");
    std::string json;
    std::string error;
    if (!patcher(req.body, error, json)) {
        int status = 400;
        if (error == "timed out")
            status = 503;
        else if (error.find("Unable to") != std::string::npos)
            status = 500;
        return jsonError(status, error.empty() ? "rejected" : error);
    }
    return HttpResponse::text(200, json);
}

HttpResponse WebServer::handleCatalogDescription(const HttpRequest& req) {
    const std::string locale = descriptionLocale(req.queryParam("lang"));
    if (locale.empty()) return jsonError(400, "unsupported language");
    std::string source;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        const auto found = catalogIndex_.find(lowerAscii(req.queryParam("hash")));
        if (found == catalogIndex_.end()) return jsonError(404, "game not found");
        source = (*catalog_)[found->second].description;
    }
    std::string text;
    const bool localized = offlineCatalogDescription(source, locale, text);
    Json j;
    j["description"] = localized ? text : source;
    j["locale"] = locale;
    j["localized"] = localized;
    HttpResponse response = HttpResponse::text(200, dumpJson(j));
    response.extraHeaders.push_back({"Cache-Control", "no-store"});
    return response;
}

HttpResponse WebServer::handleCatalog(const HttpRequest& req) {
    std::shared_ptr<const GameMetadataIndexSnapshot> metadata;
    // Grab the state under the lock, but do the expensive JSON build + gzip
    // outside it: holding catalogMutex_ for the multi-MB encode blocked the
    // UI thread's updateCatalog for the whole duration.
    std::shared_ptr<const std::vector<CatalogEntry>> snapshot;
    std::shared_ptr<const std::string> gz;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        generation = catalogGeneration_;
        metadata = metadata_;
        std::string etag = "\"cat-" + std::to_string(generation) + "\"";
        if (req.header("if-none-match") == etag) {
            HttpResponse r = HttpResponse::empty(304);
            r.extraHeaders.push_back({"ETag", etag});
            return r;
        }
        if (catalogGzip_ && catalogGzipGeneration_ == generation)
            gz = catalogGzip_;
        else
            snapshot = catalog_;
    }
    if (!gz) {
        Json list = Json::array();
        for (const CatalogEntry& e : *snapshot) {
            Json j;
            j["infoHash"] = lowerAscii(e.infoHash);
            j["title"] = e.title;
            j["size"] = e.size;
            j["year"] = e.year;
            j["genre"] = e.genre;
            j["developer"] = e.developer;
            j["publisher"] = e.publisher;
            j["posterUrl"] = e.posterUrl;
            j["description"] = e.description;
            // Rich detail-card fields for the game-store UI. All optional
            // on the client — old cached app.js simply ignores the extras.
            j["titleId"] = e.titleId;
            std::string metadataId;
            if (metadata) {
                const auto found = metadata->byInfoHash.find(lowerAscii(e.infoHash));
                if (found != metadata->byInfoHash.end() && found->second < metadata->items.size())
                    metadataId = metadata->items[found->second].titleId;
            }
            j["section"] = catalogTitleIsGame(e.title, e.titleId, metadataId, e.packageType) ? "games" : "ports";
            j["performance"] = e.performance;
            j["multiplayer"] = e.multiplayer;
            j["interfaceLang"] = e.interfaceLang;
            j["voiceLang"] = e.voiceLang;
            j["topicId"] = e.topicId;
            j["publishedAt"] = e.publishedAt;
            j["peerCount"] = e.peerCount;
            j["health"] = catalogHealthName(e.health);
            j["trackerUrl"] = e.trackerUrl;
            Json shots = Json::array();
            for (size_t i = 0; i < e.screenshots.size() && i < 6; ++i)
                shots.push_back(e.screenshots[i]);
            j["screenshots"] = std::move(shots);
            list.push_back(std::move(j));
        }
        std::string compressed = gzipCompress(dumpJson(list));
        if (compressed.empty() && !snapshot->empty())
            return jsonError(500, "catalog compression failed");
        gz = std::make_shared<const std::string>(std::move(compressed));
        std::lock_guard<std::mutex> lock(catalogMutex_);
        // Cache only if the catalog was not replaced mid-build; the response
        // itself is still consistent with the generation it advertises.
        if (catalogGeneration_ == generation) {
            catalogGzip_ = gz;
            catalogGzipGeneration_ = generation;
        }
    }
    HttpResponse resp;
    resp.status = 200;
    resp.contentType = "application/json";
    resp.body = std::move(gz);
    resp.extraHeaders.push_back({"Content-Encoding", "gzip"});
    resp.extraHeaders.push_back(
        {"ETag", "\"cat-" + std::to_string(generation) + "\""});
    resp.extraHeaders.push_back({"Cache-Control", "no-cache"});
    return resp;
}

HttpResponse WebServer::handleTaskCommand(const std::string& id,
                                          const std::string& command,
                                          const HttpRequest& req) {
    if (!manager_.hasTask(id)) return jsonError(404, "unknown task");

    std::string error;
    bool ok = false;
    if (command == "pause") {
        ok = manager_.pause(id, error);
    } else if (command == "resume" || command == "retry") {
        ok = manager_.resume(id, error);
    } else if (command == "verify") {
        ok = manager_.verify(id, error);
    } else if (command == "move-front") {
        ok = manager_.moveToFront(id, error);
    } else if (command == "move-up") {
        ok = manager_.moveTask(id, true, error);
        if (ok) error.clear();
    } else if (command == "move-down") {
        ok = manager_.moveTask(id, false, error);
        if (ok) error.clear();
    } else if (command == "remove") {
        bool deleteData = false;
        if (!req.body.empty()) {
            Json body = Json::parse(req.body, nullptr, false);
            if (body.is_discarded())
                return jsonError(400, "invalid JSON body");
            deleteData = body.value("deleteData", false);
        }
        ok = manager_.remove(id, deleteData, error);
    } else {
        return jsonError(404, "unknown command");
    }
    if (!ok) return jsonError(409, error.empty() ? "rejected" : error);
    return HttpResponse::empty(204);
}

HttpResponse WebServer::handleAddMagnet(const HttpRequest& req) {
    Json body = Json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        return jsonError(400, "invalid JSON body");
    std::string magnet = body.value("magnet", "");
    TransferMode mode;
    if (!parseMode(body.value("mode", ""), mode))
        return jsonError(400, "mode must be \"install\" or \"download\"");
    MagnetSpec spec;
    std::string error;
    if (!MagnetResolver::parse(magnet, spec, error))
        return jsonError(400, error.empty() ? "invalid magnet URI" : error);

    const auto existing = manager_.snapshot(spec.infoHashHex);
    if (mode == TransferMode::StreamInstall && existing &&
        existing->mode == TransferMode::DownloadOnly && existing->status == DownloadStatus::Completed) {
        std::string error;
        if (!manager_.installDownloaded(spec.infoHashHex, error)) return jsonError(409, error);
        return HttpResponse::text(200, dumpJson({{"taskId", spec.infoHashHex}, {"reusedTask", true}}));
    }
    StreamSelection selection;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        selection = streamSelection_;
    }
    std::string jobId = addQueue_.enqueue("", magnet, spec.infoHashHex, {},
                                          mode, selection, error,
                                          AddInputKind::Magnet);
    if (jobId.empty()) return jsonError(409, error);
    Json j;
    j["jobId"] = jobId;
    return HttpResponse::text(202, dumpJson(j));
}

HttpResponse WebServer::handleAddCatalog(const HttpRequest& req) {
    Json body = Json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        return jsonError(400, "invalid JSON body");
    std::string hash = lowerAscii(body.value("infoHash", ""));
    TransferMode mode;
    if (!parseMode(body.value("mode", ""), mode))
        return jsonError(400, "mode must be \"install\" or \"download\"");

    std::string title;
    std::string magnet;
    std::string titleId;
    std::vector<uint8_t> infoDict;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        auto it = catalogIndex_.find(hash);
        if (it == catalogIndex_.end())
            return jsonError(404, "unknown catalog entry");
        const CatalogEntry& entry = (*catalog_)[it->second];
        title = entry.title;
        magnet = entry.magnetUri;
        infoDict = entry.infoDict;
        titleId = entry.titleId;
    }
    const auto existing = manager_.snapshot(hash);
    if (mode == TransferMode::StreamInstall && existing &&
        existing->mode == TransferMode::DownloadOnly && existing->status == DownloadStatus::Completed) {
        std::string error;
        if (!manager_.installDownloaded(hash, error)) return jsonError(409, error);
        return HttpResponse::text(200, dumpJson({{"taskId", hash}, {"reusedTask", true}}));
    }
    StreamSelection selection;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        selection = streamSelection_;
    }
    std::string error;
    std::string jobId =
        addQueue_.enqueue(std::move(title), std::move(magnet), hash,
                          std::move(infoDict), mode, selection, error,
                          AddInputKind::Catalog, std::move(titleId));
    if (jobId.empty()) return jsonError(409, error);
    Json j;
    j["jobId"] = jobId;
    return HttpResponse::text(202, dumpJson(j));
}

HttpResponse WebServer::handleAddTorrent(const HttpRequest& req) {
    if (req.body.empty()) return jsonError(400, "empty body");
    if (req.body.size() > kMaxTorrentUploadBytes)
        return jsonError(413, "torrent upload too large");
    TransferMode mode;
    if (!parseMode(req.queryParam("mode"), mode))
        return jsonError(400, "mode must be \"install\" or \"download\"");

    // Refuse before writing the upload when the saved source cannot take a
    // .torrent. Direct stays off; a configured debrid provider accepts it.
    DebridProviderFactory factory;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        factory = debridFactory_;
    }
    ReleaseAdder adder(manager_, std::move(factory));
    const AddSourceChoice source = adder.choose(AddInputKind::TorrentFile);
    if (!source.accepted)
        return jsonError(409, source.refusal);

    const std::string path =
        manager_.rootPath() + "/_web_upload_" +
        std::to_string(gUploadSerial.fetch_add(1)) + ".torrent";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return jsonError(500, "cannot write temp file");
    size_t written = fwrite(req.body.data(), 1, req.body.size(), f);
    fclose(f);
    if (written != req.body.size()) {
        ::unlink(path.c_str());
        return jsonError(500, "cannot write temp file");
    }

    std::string error;
    TorrentPreview preview;
    if (!DownloadManager::previewTorrent(path, preview, error)) {
        ::unlink(path.c_str());
        return jsonError(400, error.empty() ? "invalid torrent" : error);
    }
    StreamSelection selection;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        selection = streamSelection_;
    }
    AddRequest request;
    request.input = AddInputKind::TorrentFile;
    request.title = preview.name;
    request.infoHashHex = preview.infoHash;
    request.requestedMode = mode;
    request.selection = selection;
    std::atomic<bool> cancelled{false};
    const AddOutcome outcome = adder.addTorrentFile(request, path, cancelled);
    ::unlink(path.c_str());
    if (!outcome.ok)
        return jsonError(409, outcome.error.empty() ? "import failed"
                                                    : outcome.error);
    Json j;
    j["taskId"] = outcome.taskId;
    return HttpResponse::text(200, dumpJson(j));
}

}  // namespace pipensx
