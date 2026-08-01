#include "torbox_client.hpp"
#include "curl_https.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

using Json = nlohmann::json;

constexpr const char* kBaseUrl = "https://api.torbox.app/v1/api";

size_t writeBody(char* data, size_t size, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(data, size * count);
    return size * count;
}

bool curlTransport(const TorboxHttpRequest& request,
                   TorboxHttpResponse& response, std::string& error) {
    std::string endpoint = request.url;
    size_t baseLen = std::strlen(kBaseUrl);
    if (endpoint.compare(0, baseLen, kBaseUrl) == 0)
        endpoint = endpoint.substr(baseLen);
    log_msg("[torbox] %s %s\n", request.method.c_str(), endpoint.c_str());
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    curl_slist* headers = nullptr;
    std::string auth = "Authorization: Bearer " + request.apiKey;
    headers = curl_slist_append(headers, auth.c_str());
    curl_mime* mime = nullptr;
    if (!request.magnet.empty() || !request.uploadFilePath.empty()) {
        mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        if (!request.magnet.empty()) {
            curl_mime_name(part, "magnet");
            curl_mime_data(part, request.magnet.c_str(), CURL_ZERO_TERMINATED);
        } else {
            curl_mime_name(part, "file");
            curl_mime_filedata(part, request.uploadFilePath.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (!request.body.empty()) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    } else if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curlPinHttpsOnly(curl);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (mime)
        curl_mime_free(mime);
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
        error = std::string("TorBox request failed: ") +
                curl_easy_strerror(result);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool rootObject(const std::string& text, Json& root, std::string& error) {
    root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "TorBox returned an invalid response.";
        return false;
    }
    return true;
}

bool checkSuccess(const Json& root, std::string& error) {
    if (root.contains("success") && root["success"].is_boolean() &&
        root["success"].get<bool>())
        return true;
    std::string code;
    if (root.contains("error") && root["error"].is_string())
        code = root["error"].get<std::string>();
    if (code == "BAD_TOKEN" || code == "AUTH_ERROR") {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    if (root.contains("detail") && root["detail"].is_string() &&
        !root["detail"].get<std::string>().empty())
        error = root["detail"].get<std::string>();
    else
        error = "TorBox request failed.";
    return false;
}

bool readInfoObject(const Json& item, TorboxTorrentInfo& info,
                    std::string& error) {
    if (!item.is_object() || !item.contains("id") ||
        !item["id"].is_number()) {
        error = "TorBox returned an invalid torrent entry.";
        return false;
    }
    info = TorboxTorrentInfo{};
    info.id = item["id"].get<uint64_t>();
    info.hash = (item.contains("hash") && item["hash"].is_string())
                    ? item["hash"].get<std::string>() : "";
    info.name = (item.contains("name") && item["name"].is_string())
                    ? item["name"].get<std::string>() : "";
    info.size = (item.contains("size") && item["size"].is_number())
                    ? item["size"].get<uint64_t>() : 0;
    info.progress = (item.contains("progress") &&
                     item["progress"].is_number())
                        ? item["progress"].get<double>() : 0.0;
    info.state = (item.contains("download_state") &&
                  item["download_state"].is_string())
                     ? item["download_state"].get<std::string>() : "";
    {
        bool finished = item.contains("download_finished") &&
                        item["download_finished"].is_boolean() &&
                        item["download_finished"].get<bool>();
        bool present  = item.contains("download_present") &&
                        item["download_present"].is_boolean() &&
                        item["download_present"].get<bool>();
        info.ready = finished && present;
    }
    if (item.contains("files") && item["files"].is_array()) {
        for (const Json& file : item["files"]) {
            if (!file.is_object())
                continue;
            TorboxFile entry;
            entry.id = (file.contains("id") && file["id"].is_number())
                           ? file["id"].get<uint64_t>() : 0;
            entry.name = (file.contains("name") && file["name"].is_string())
                             ? file["name"].get<std::string>() : "";
            entry.size = (file.contains("size") && file["size"].is_number())
                             ? file["size"].get<uint64_t>() : 0;
            info.files.push_back(std::move(entry));
        }
    }
    return true;
}

bool finishParse(bool ok, long status, std::string& error) {
    if (!ok && error == "TorBox request failed." && status != 0) {
        error = "TorBox request failed (HTTP " + std::to_string(status) + ").";
    }
    return ok;
}

} // namespace

TorboxClient::TorboxClient(std::string apiKey, TorboxTransport transport)
    : apiKey_(std::move(apiKey)),
      transport_(transport ? std::move(transport) : curlTransport) {}

const std::string& TorboxClient::apiKey() const { return apiKey_; }

bool TorboxClient::parseSuccess(const std::string& json, std::string& error) {
    Json root;
    return rootObject(json, root, error) && checkSuccess(root, error);
}

bool TorboxClient::parseCreate(const std::string& json, uint64_t& torboxId,
                               std::string& error) {
    Json root;
    if (!rootObject(json, root, error) || !checkSuccess(root, error))
        return false;
    if (!root.contains("data") || !root["data"].is_object() ||
        !root["data"].contains("torrent_id") ||
        !root["data"]["torrent_id"].is_number()) {
        error = "TorBox did not return a torrent id.";
        return false;
    }
    torboxId = root["data"]["torrent_id"].get<uint64_t>();
    return true;
}

bool TorboxClient::parseInfo(const std::string& json, uint64_t torboxId,
                             TorboxTorrentInfo& info, std::string& error) {
    Json root;
    if (!rootObject(json, root, error) || !checkSuccess(root, error))
        return false;
    if (!root.contains("data")) {
        error = "TorBox returned no torrent data.";
        return false;
    }
    const Json& data = root["data"];
    if (data.is_object())
        return readInfoObject(data, info, error);
    if (data.is_array()) {
        for (const Json& item : data) {
            TorboxTorrentInfo candidate;
            std::string itemError;
            if (readInfoObject(item, candidate, itemError) &&
                candidate.id == torboxId) {
                info = std::move(candidate);
                return true;
            }
        }
    }
    error = "TorBox torrent not found.";
    return false;
}

bool TorboxClient::parseDownloadLink(const std::string& json,
                                     std::string& url, std::string& error) {
    Json root;
    if (!rootObject(json, root, error) || !checkSuccess(root, error))
        return false;
    if (!root.contains("data") || !root["data"].is_string()) {
        error = "TorBox did not return a download link.";
        return false;
    }
    url = root["data"].get<std::string>();
    return true;
}

bool TorboxClient::validateKey(std::string& error) {
    TorboxHttpRequest request;
    request.method = "GET";
    request.url = std::string(kBaseUrl) + "/user/me";
    request.apiKey = apiKey_;
    TorboxHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (response.status == 401 || response.status == 403) {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    return finishParse(parseSuccess(response.body, error),
                       response.status, error);
}

bool TorboxClient::createFromMagnet(const std::string& magnet,
                                    uint64_t& torboxId, std::string& error) {
    TorboxHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/torrents/createtorrent";
    request.apiKey = apiKey_;
    request.magnet = magnet;
    TorboxHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (response.status == 401 || response.status == 403) {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    return finishParse(parseCreate(response.body, torboxId, error),
                       response.status, error);
}

bool TorboxClient::createFromFile(const std::string& torrentPath,
                                  uint64_t& torboxId, std::string& error) {
    TorboxHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/torrents/createtorrent";
    request.apiKey = apiKey_;
    request.uploadFilePath = torrentPath;
    TorboxHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (response.status == 401 || response.status == 403) {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    return finishParse(parseCreate(response.body, torboxId, error),
                       response.status, error);
}

bool TorboxClient::fetchInfo(uint64_t torboxId, TorboxTorrentInfo& info,
                             std::string& error) {
    TorboxHttpRequest request;
    request.method = "GET";
    request.url = std::string(kBaseUrl) +
        "/torrents/mylist?bypass_cache=true&id=" + std::to_string(torboxId);
    request.apiKey = apiKey_;
    TorboxHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (response.status == 401 || response.status == 403) {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    return finishParse(parseInfo(response.body, torboxId, info, error),
                       response.status, error);
}

bool TorboxClient::requestDownloadLink(uint64_t torboxId, uint64_t fileId,
                                       std::string& url, std::string& error) {
    TorboxHttpRequest request;
    request.method = "GET";
    // TorBox's requestdl endpoint authenticates via a `token` query parameter,
    // not the Authorization header. Without it the API returns HTTP 422
    // ({"detail":[{"loc":["query","token"],"msg":"Field required"}]}).
    request.url = std::string(kBaseUrl) + "/torrents/requestdl?token=" +
        apiKey_ + "&torrent_id=" + std::to_string(torboxId) +
        "&file_id=" + std::to_string(fileId);
    request.apiKey = apiKey_;
    TorboxHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (response.status == 401 || response.status == 403) {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    return finishParse(parseDownloadLink(response.body, url, error),
                       response.status, error);
}

bool TorboxClient::remove(uint64_t torboxId, std::string& error) {
    TorboxHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/torrents/controltorrent";
    request.apiKey = apiKey_;
    request.body = std::string("{\"torrent_id\":") +
        std::to_string(torboxId) + ",\"operation\":\"delete\"}";
    TorboxHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (response.status == 401 || response.status == 403) {
        error = "TorBox key rejected - relink in Settings.";
        return false;
    }
    return finishParse(parseSuccess(response.body, error),
                       response.status, error);
}

} // namespace pipensx
