#include "torrserver_provider.hpp"
#include "curl_https.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

using Json = nlohmann::json;

size_t writeBody(char* data, size_t size, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(data, size * count);
    return size * count;
}

// The URL can carry basic-auth credentials, so only the path reaches the log.
std::string logPath(const std::string& url) {
    size_t scheme = url.find("://");
    size_t host = scheme == std::string::npos ? 0 : scheme + 3;
    size_t path = url.find('/', host);
    return path == std::string::npos ? "/" : url.substr(path);
}

bool curlTransport(const TsHttpRequest& request, TsHttpResponse& response,
                   std::string& error) {
    log_msg("[torrserver] %s %s\n", request.method.c_str(),
            logPath(request.url).c_str());
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    curl_slist* headers = nullptr;
    curl_mime* mime = nullptr;
    if (!request.uploadFilePath.empty()) {
        mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filedata(part, request.uploadFilePath.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (!request.body.empty()) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    } else if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curlPinScheme(curl, request.url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    log_msg("[torrserver] %s %s -> rc=%d http=%ld\n", request.method.c_str(),
            logPath(request.url).c_str(), (int)result, response.status);
    if (mime)
        curl_mime_free(mime);
    if (headers)
        curl_slist_free_all(headers);
    if (result != CURLE_OK)
        error = std::string("TorrServer request failed: ") +
                curl_easy_strerror(result);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool statusOk(const TsHttpResponse& response, std::string& error) {
    if (response.status == 401 || response.status == 403) {
        error = "TorrServer rejected the credentials in the address.";
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = "TorrServer answered HTTP " +
                std::to_string(response.status) + ".";
        return false;
    }
    return true;
}

} // namespace

TorrserverProvider::TorrserverProvider(std::string baseUrl,
                                       TsTransport transport)
    : base_(normalizeBaseUrl(baseUrl)),
      transport_(transport ? std::move(transport) : curlTransport) {}

std::string TorrserverProvider::normalizeBaseUrl(const std::string& raw) {
    size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    size_t last = raw.find_last_not_of(" \t\r\n/");
    if (last == std::string::npos || last < first)
        return {};
    std::string url = raw.substr(first, last - first + 1);
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
        url = "http://" + url;
    return url;
}

bool TorrserverProvider::torrents(const std::string& body,
                                  TsHttpResponse& response,
                                  std::string& error) {
    TsHttpRequest request;
    request.method = "POST";
    request.url = base_ + "/torrents";
    request.body = body;
    std::string transportError;
    if (!transport_(request, response, transportError)) {
        error = transportError.empty()
            ? "Unable to reach TorrServer at " + base_ + "." : transportError;
        return false;
    }
    return statusOk(response, error);
}

bool TorrserverProvider::validate(std::string& error) {
    if (base_.empty()) {
        error = "No TorrServer address set.";
        return false;
    }
    TsHttpRequest request;
    request.method = "GET";
    request.url = base_ + "/echo";
    TsHttpResponse response;
    std::string transportError;
    if (!transport_(request, response, transportError)) {
        error = transportError.empty()
            ? "Unable to reach TorrServer at " + base_ + "." : transportError;
        return false;
    }
    if (!statusOk(response, error))
        return false;
    // /echo answers with the build name ("MatriX.141"). An empty body means
    // something else is listening on that port.
    if (response.body.find_first_not_of(" \t\r\n") == std::string::npos) {
        error = "That address answered, but it is not a TorrServer.";
        return false;
    }
    return true;
}

bool TorrserverProvider::createFromMagnet(const std::string& magnet,
                                          std::string& id,
                                          std::string& error) {
    Json body;
    body["action"] = "add";
    body["link"] = magnet;
    body["save_to_db"] = true;
    TsHttpResponse response;
    if (!torrents(body.dump(), response, error))
        return false;
    DebridInfo ignored;
    return parseStatus(response.body, ignored, id, error);
}

bool TorrserverProvider::createFromFile(const std::string& torrentPath,
                                        std::string& id,
                                        std::string& error) {
    TsHttpRequest request;
    request.method = "POST";
    request.url = base_ + "/torrent/upload";
    request.uploadFilePath = torrentPath;
    TsHttpResponse response;
    std::string transportError;
    if (!transport_(request, response, transportError)) {
        error = transportError.empty()
            ? "Unable to reach TorrServer at " + base_ + "." : transportError;
        return false;
    }
    if (!statusOk(response, error))
        return false;
    DebridInfo ignored;
    return parseStatus(response.body, ignored, id, error);
}

bool TorrserverProvider::fetchInfo(const std::string& id, DebridInfo& info,
                                   std::string& error) {
    // `add` rather than `get`: it is idempotent on the hash, returns the same
    // status object, and loads a torrent the server has dropped from memory —
    // `get` would answer "in db" with no file list forever.
    Json body;
    body["action"] = "add";
    body["link"] = "magnet:?xt=urn:btih:" + id;
    body["save_to_db"] = true;
    TsHttpResponse response;
    if (!torrents(body.dump(), response, error))
        return false;
    std::string hash;
    return parseStatus(response.body, info, hash, error);
}

bool TorrserverProvider::resolveDownloadUrl(const std::string& id,
                                            const DebridInfo& /*info*/,
                                            size_t /*kthSelected*/,
                                            const DebridFile& file,
                                            std::string& url,
                                            std::string& error) {
    if (id.empty() || file.id.empty()) {
        error = "TorrServer file index missing.";
        return false;
    }
    url = base_ + "/play/" + id + "/" + file.id;
    return true;
}

bool TorrserverProvider::remove(const std::string& id, std::string& error) {
    Json body;
    body["action"] = "rem";
    body["hash"] = id;
    TsHttpResponse response;
    return torrents(body.dump(), response, error);
}

bool TorrserverProvider::parseStatus(const std::string& json, DebridInfo& info,
                                     std::string& hash, std::string& error) {
    Json root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("hash") ||
        !root["hash"].is_string()) {
        error = "TorrServer returned an unexpected response.";
        return false;
    }
    hash = root["hash"].get<std::string>();
    info = DebridInfo{};
    if (root.contains("name") && root["name"].is_string())
        info.name = root["name"].get<std::string>();
    if (root.contains("torrent_size") && root["torrent_size"].is_number())
        info.bytes = root["torrent_size"].get<uint64_t>();
    if (root.contains("stat_string") && root["stat_string"].is_string())
        info.rawState = root["stat_string"].get<std::string>();
    if (root.contains("file_stats") && root["file_stats"].is_array()) {
        for (const Json& item : root["file_stats"]) {
            if (!item.is_object() || !item.contains("id") ||
                !item["id"].is_number())
                continue;
            DebridFile file;
            // TorrServer indexes files from 1, and that index is the one
            // /play/<hash>/<id> takes.
            file.id = std::to_string(item["id"].get<uint64_t>());
            if (item.contains("path") && item["path"].is_string())
                file.path = item["path"].get<std::string>();
            if (item.contains("length") && item["length"].is_number())
                file.bytes = item["length"].get<uint64_t>();
            info.files.push_back(std::move(file));
        }
    }
    // The file list appears once the metadata is in, and from that moment
    // every byte is streamable — TorrServer pulls pieces on demand, so there
    // is no server-side "downloading" phase to wait through.
    info.phase = info.files.empty() ? DebridInfo::Phase::Creating
                                    : DebridInfo::Phase::Ready;
    return true;
}

} // namespace pipensx
