#include "web_server.hpp"

extern "C" {
#include "../core/util.h"
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

const char* modeName(TransferMode mode) {
    return mode == TransferMode::StreamInstall ? "install" : "download";
}

bool parseMode(const std::string& value, TransferMode& out) {
    if (value == "install") {
        out = TransferMode::StreamInstall;
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
          "max-age=3600"}},
        {"/app.css", {"app.css", "text/css; charset=utf-8", "max-age=3600"}},
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
    // 15 + 16 selects gzip framing (RFC 1952) instead of zlib
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
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
      addQueue_(manager, std::move(resolver)),
      staticRoot_(std::move(staticRoot)),
      version_(std::move(version)) {
    http_.setTickCallback([this] { onTick(); });
}

WebServer::~WebServer() { shutdown(); }

bool WebServer::start(uint16_t port) {
    if (http_.running()) return true;
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
    addQueue_.shutdown();
}

void WebServer::setPin(std::string pin) {
    std::lock_guard<std::mutex> lock(configMutex_);
    pin_ = std::move(pin);
}

void WebServer::setStreamSelection(StreamSelection selection) {
    std::lock_guard<std::mutex> lock(configMutex_);
    streamSelection_ = selection;
}

void WebServer::updateCatalog(const std::vector<CatalogEntry>& entries) {
    std::lock_guard<std::mutex> lock(catalogMutex_);
    catalog_ = entries;
    catalogIndex_.clear();
    catalogIndex_.reserve(catalog_.size());
    for (size_t i = 0; i < catalog_.size(); ++i)
        catalogIndex_[lowerAscii(catalog_[i].infoHash)] = i;
    ++catalogGeneration_;
    catalogGzip_ = nullptr;
}

bool WebServer::authorized(const HttpRequest& req) const {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (pin_.empty()) return true;
    std::string provided = req.header("x-pipensx-pin");
    if (provided.empty()) provided = req.queryParam("pin");
    if (provided.size() != pin_.size()) return false;
    // constant-time compare; a LAN PIN hardly merits it but it costs nothing
    unsigned char diff = 0;
    for (size_t i = 0; i < pin_.size(); ++i)
        diff |= (unsigned char)(provided[i] ^ pin_[i]);
    return diff == 0;
}

std::string WebServer::buildStateJson() {
    Json state;
    Json tasks = Json::array();
    for (const DownloadTask& t : manager_.snapshot()) {
        Json j;
        j["id"] = t.id;
        j["name"] = t.name;
        j["status"] = statusName(t.status);
        j["mode"] = modeName(t.mode);
        j["error"] = t.error;
        j["totalBytes"] = t.totalBytes;
        j["completedBytes"] = t.completedBytes;
        j["speedBps"] = t.speedBytesPerSecond;
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
        j["currentPackage"] = t.currentPackage;
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

    uint64_t now = nowMs();
    if (!storageCacheAtMs_ || now - storageCacheAtMs_ > kStorageCacheTtlMs) {
        storageCache_ = queryStorageSpace(manager_.downloadRoot());
        storageCacheAtMs_ = now;
    }
    Json storage;
    storage["totalBytes"] = storageCache_.totalBytes;
    storage["freeBytes"] = storageCache_.freeBytes;
    storage["available"] = storageCache_.available;
    state["storage"] = std::move(storage);
    return dumpJson(state);
}

void WebServer::onTick() {
    if (http_.sseClientCount() == 0) return;
    http_.broadcastSse("event: state\ndata: " + buildStateJson() + "\n\n");
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
        if (parts[0] == "info" && parts.size() == 1) {
            Json j;
            j["name"] = "pipensx";
            j["version"] = version_;
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                j["authRequired"] = !pin_.empty();
            }
            j["maxTorrentBytes"] = 2 * 1024 * 1024;
            return HttpResponse::text(200, dumpJson(j));
        }
        if (parts[0] == "tasks" && parts.size() == 1)
            return HttpResponse::text(200, buildStateJson());
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
        if (parts[0] == "storage" && parts.size() == 1) {
            StorageSpaceSnapshot snap =
                queryStorageSpace(manager_.downloadRoot());
            Json j;
            j["totalBytes"] = snap.totalBytes;
            j["freeBytes"] = snap.freeBytes;
            j["available"] = snap.available;
            return HttpResponse::text(200, dumpJson(j));
        }
        return jsonError(404, "not found");
    }

    // Everything below mutates → PIN check (auth/check exists so the UI can
    // validate a remembered PIN explicitly).
    if (!authorized(req)) return jsonError(401, "pin");
    if (parts[0] == "auth" && parts.size() == 2 && parts[1] == "check")
        return HttpResponse::empty(204);
    if (parts[0] == "tasks" && parts.size() == 3)
        return handleTaskCommand(parts[1], parts[2], req);
    if (parts[0] == "jobs" && parts.size() == 3 && parts[2] == "cancel")
        return addQueue_.cancel(parts[1]) ? HttpResponse::empty(204)
                                          : jsonError(404, "unknown job");
    if (parts[0] == "add" && parts.size() == 2) {
        if (parts[1] == "magnet") return handleAddMagnet(req);
        if (parts[1] == "catalog") return handleAddCatalog(req);
        if (parts[1] == "torrent") return handleAddTorrent(req);
    }
    return jsonError(404, "not found");
}

HttpResponse WebServer::handleCatalog(const HttpRequest& req) {
    std::lock_guard<std::mutex> lock(catalogMutex_);
    std::string etag = "\"cat-" + std::to_string(catalogGeneration_) + "\"";
    if (req.header("if-none-match") == etag) {
        HttpResponse r = HttpResponse::empty(304);
        r.extraHeaders.push_back({"ETag", etag});
        return r;
    }
    if (!catalogGzip_ || catalogGzipGeneration_ != catalogGeneration_) {
        Json list = Json::array();
        for (const CatalogEntry& e : catalog_) {
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
            list.push_back(std::move(j));
        }
        catalogGzip_ =
            std::make_shared<const std::string>(gzipCompress(dumpJson(list)));
        catalogGzipGeneration_ = catalogGeneration_;
    }
    HttpResponse resp;
    resp.status = 200;
    resp.contentType = "application/json";
    resp.body = catalogGzip_;
    resp.extraHeaders.push_back({"Content-Encoding", "gzip"});
    resp.extraHeaders.push_back({"ETag", etag});
    resp.extraHeaders.push_back({"Cache-Control", "no-cache"});
    return resp;
}

HttpResponse WebServer::handleTaskCommand(const std::string& id,
                                          const std::string& command,
                                          const HttpRequest& req) {
    bool known = false;
    for (const DownloadTask& t : manager_.snapshot()) {
        if (t.id == id) {
            known = true;
            break;
        }
    }
    if (!known) return jsonError(404, "unknown task");

    std::string error;
    bool ok = false;
    if (command == "pause") {
        ok = manager_.pause(id);
        error = "task is not pausable right now";
    } else if (command == "resume") {
        ok = manager_.resume(id);
        error = "task is not paused";
    } else if (command == "retry") {
        ok = manager_.retry(id);
        error = "task is not in an error state";
    } else if (command == "verify") {
        ok = manager_.verify(id);
        error = "task cannot be verified right now";
    } else if (command == "move-front") {
        ok = manager_.moveToFront(id, error);
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

    StreamSelection selection;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        selection = streamSelection_;
    }
    std::string jobId = addQueue_.enqueue("", magnet, spec.infoHashHex, {},
                                          mode, selection, error);
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
    std::vector<uint8_t> infoDict;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        auto it = catalogIndex_.find(hash);
        if (it == catalogIndex_.end())
            return jsonError(404, "unknown catalog entry");
        const CatalogEntry& entry = catalog_[it->second];
        title = entry.title;
        magnet = entry.magnetUri;
        infoDict = entry.infoDict;
    }
    StreamSelection selection;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        selection = streamSelection_;
    }
    std::string error;
    std::string jobId =
        addQueue_.enqueue(std::move(title), std::move(magnet), hash,
                          std::move(infoDict), mode, selection, error);
    if (jobId.empty()) return jsonError(409, error);
    Json j;
    j["jobId"] = jobId;
    return HttpResponse::text(202, dumpJson(j));
}

HttpResponse WebServer::handleAddTorrent(const HttpRequest& req) {
    if (req.body.empty()) return jsonError(400, "empty body");
    TransferMode mode;
    if (!parseMode(req.queryParam("mode"), mode))
        return jsonError(400, "mode must be \"install\" or \"download\"");

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
    std::vector<uint8_t> mask;
    if (mode == TransferMode::StreamInstall) {
        StreamSelection selection;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            selection = streamSelection_;
        }
        mask = defaultInstallSelection(preview, mode, selection);
        InstallSpaceEstimate space = estimateInstallSpace(preview, mask, mode);
        if (space.packageFiles == 0) mode = TransferMode::DownloadOnly;
    }
    std::string taskId;
    bool ok = manager_.importTorrent(path, mode, mask, taskId, error);
    ::unlink(path.c_str());
    if (!ok) return jsonError(409, error.empty() ? "import failed" : error);
    Json j;
    j["taskId"] = taskId;
    return HttpResponse::text(200, dumpJson(j));
}

}  // namespace pipensx
