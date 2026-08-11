#include "realdebrid_client.hpp"
#include "curl_https.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

using Json = nlohmann::json;

constexpr const char* kBaseUrl = "https://api.real-debrid.com/rest/1.0";

size_t writeBody(char* data, size_t size, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(data, size * count);
    return size * count;
}

bool curlTransport(const RdHttpRequest& request,
                   RdHttpResponse& response, std::string& error) {
    std::string endpoint = request.url;
    size_t baseLen = std::strlen(kBaseUrl);
    if (endpoint.compare(0, baseLen, kBaseUrl) == 0)
        endpoint = endpoint.substr(baseLen);
    log_msg("[realdebrid] %s %s\n", request.method.c_str(), endpoint.c_str());
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    curl_slist* headers = nullptr;
    std::string auth = "Authorization: Bearer " + request.apiKey;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curlPinHttpsOnly(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    if (request.method == "POST") {
        if (!request.body.empty()) {
            headers = curl_slist_append(headers,
                "Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    } else if (request.method == "PUT") {
        if (!request.uploadFilePath.empty()) {
            FILE* file = fopen(request.uploadFilePath.c_str(), "rb");
            if (!file) {
                error = "Unable to open torrent file for upload.";
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return false;
            }
            fseek(file, 0, SEEK_END);
            long fileSize = ftell(file);
            rewind(file);
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl, CURLOPT_READDATA, file);
            curl_easy_setopt(curl, CURLOPT_INFILESIZE, fileSize);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            CURLcode result = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
            fclose(file);
            curl_slist_free_all(headers);
            if (result != CURLE_OK)
                error = std::string("Real-Debrid request failed: ") +
                        curl_easy_strerror(result);
            curl_easy_cleanup(curl);
            return result == CURLE_OK;
        }
    } else if (request.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
        error = std::string("Real-Debrid request failed: ") +
                curl_easy_strerror(result);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool parseJson(const std::string& text, Json& root, std::string& error) {
    root = Json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        error = "Real-Debrid returned an invalid response.";
        return false;
    }
    return true;
}

bool checkAuthError(const std::string& body, long status,
                    std::string& error) {
    if (status == 401 || status == 403) {
        error = "Real-Debrid key rejected - relink in Settings.";
        return false;
    }
    if (status == 204)
        return true;
    if (status < 200 || status >= 300) {
        Json root;
        if (parseJson(body, root, error)) {
            std::string detail;
            if (root.is_object() && root.contains("error") &&
                root["error"].is_string())
                detail = root["error"].get<std::string>();
            else if (root.is_object() && root.contains("error_code"))
                detail = std::to_string(root["error_code"].get<int>());
            error = detail.empty()
                ? "Real-Debrid request failed (HTTP " +
                  std::to_string(status) + ")."
                : detail;
        }
        return false;
    }
    return true;
}

bool readStringField(const Json& obj, const char* key, std::string& value) {
    if (obj.contains(key) && obj[key].is_string()) {
        value = obj[key].get<std::string>();
        return true;
    }
    return false;
}

bool readNumberField(const Json& obj, const char* key, uint64_t& value) {
    if (obj.contains(key) && obj[key].is_number()) {
        value = obj[key].get<uint64_t>();
        return true;
    }
    return false;
}

} // namespace

RdClient::RdClient(std::string apiKey, RdTransport transport)
    : apiKey_(std::move(apiKey)),
      transport_(transport ? std::move(transport) : curlTransport) {}

const std::string& RdClient::apiKey() const { return apiKey_; }

bool RdClient::parseUserResponse(const std::string& json, std::string& error) {
    Json root;
    if (!parseJson(json, root, error))
        return false;
    if (!root.is_object() || !root.contains("id")) {
        error = "Real-Debrid key is not valid.";
        return false;
    }
    return true;
}

bool RdClient::parseAddMagnetResponse(const std::string& json,
                                      std::string& torrentId,
                                      std::string& error) {
    Json root;
    if (!parseJson(json, root, error))
        return false;
    if (!root.is_object() || !root.contains("id")) {
        error = "Real-Debrid did not return a torrent id.";
        return false;
    }
    torrentId = root["id"].get<std::string>();
    return true;
}

bool RdClient::parseAddTorrentResponse(const std::string& json,
                                       std::string& torrentId,
                                       std::string& error) {
    return parseAddMagnetResponse(json, torrentId, error);
}

bool RdClient::parseInfo(const std::string& json, RdTorrentInfo& info,
                         std::string& error) {
    Json root;
    if (!parseJson(json, root, error))
        return false;
    if (!root.is_object() || !root.contains("id")) {
        error = "Real-Debrid returned an invalid torrent entry.";
        return false;
    }
    info = RdTorrentInfo{};
    info.id = root["id"].get<std::string>();
    readStringField(root, "hash", info.hash);
    readStringField(root, "filename", info.filename);
    readNumberField(root, "bytes", info.bytes);
    if (root.contains("progress") && root["progress"].is_number()) {
        info.progress = root["progress"].get<double>();
        if (info.progress > 1.0)
            info.progress /= 100.0;
        if (info.progress < 0.0)
            info.progress = 0.0;
        if (info.progress > 1.0)
            info.progress = 1.0;
    }
    readStringField(root, "status", info.status);
    if (root.contains("files") && root["files"].is_array()) {
        for (const Json& file : root["files"]) {
            if (!file.is_object())
                continue;
            RdFile entry;
            readStringField(file, "path", entry.path);
            if (file.contains("id")) {
                if (file["id"].is_number())
                    entry.id = std::to_string(file["id"].get<uint64_t>());
                else if (file["id"].is_string())
                    entry.id = file["id"].get<std::string>();
            }
            readNumberField(file, "bytes", entry.bytes);
            info.files.push_back(std::move(entry));
        }
    }
    if (root.contains("links") && root["links"].is_array()) {
        for (const Json& link : root["links"])
            if (link.is_string())
                info.links.push_back(link.get<std::string>());
    }
    return true;
}

bool RdClient::parseUnrestrict(const std::string& json, std::string& url,
                               std::string& error) {
    Json root;
    if (!parseJson(json, root, error))
        return false;
    if (!root.is_object()) {
        error = "Real-Debrid returned an invalid unrestrict response.";
        return false;
    }
    if (root.contains("download") && root["download"].is_string()) {
        url = root["download"].get<std::string>();
        return true;
    }
    if (root.contains("link") && root["link"].is_string()) {
        url = root["link"].get<std::string>();
        return true;
    }
    error = "Real-Debrid did not return a download link.";
    return false;
}

bool RdClient::validateKey(std::string& error) {
    RdHttpRequest request;
    request.method = "GET";
    request.url = std::string(kBaseUrl) + "/user";
    request.apiKey = apiKey_;
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return parseUserResponse(response.body, error);
}

bool RdClient::createFromMagnet(const std::string& magnet,
                                std::string& torrentId,
                                std::string& error) {
    char* escaped = curl_easy_escape(nullptr, magnet.c_str(),
                                     static_cast<int>(magnet.size()));
    if (!escaped) {
        error = "Unable to encode magnet link.";
        return false;
    }
    RdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/torrents/addMagnet";
    request.apiKey = apiKey_;
    request.body = std::string("magnet=") + escaped;
    curl_free(escaped);
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return parseAddMagnetResponse(response.body, torrentId, error);
}

bool RdClient::createFromFile(const std::string& torrentPath,
                              std::string& torrentId,
                              std::string& error) {
    RdHttpRequest request;
    request.method = "PUT";
    request.url = std::string(kBaseUrl) + "/torrents/addTorrent";
    request.apiKey = apiKey_;
    request.uploadFilePath = torrentPath;
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return parseAddTorrentResponse(response.body, torrentId, error);
}

bool RdClient::fetchInfo(const std::string& torrentId, RdTorrentInfo& info,
                         std::string& error) {
    RdHttpRequest request;
    request.method = "GET";
    request.url = std::string(kBaseUrl) + "/torrents/info/" + torrentId;
    request.apiKey = apiKey_;
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return parseInfo(response.body, info, error);
}

bool RdClient::selectFiles(const std::string& torrentId,
                           const std::vector<std::string>& fileIds,
                           std::string& error) {
    std::string files;
    for (size_t i = 0; i < fileIds.size(); ++i) {
        if (i > 0)
            files += ",";
        files += fileIds[i];
    }
    RdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/torrents/selectFiles/" + torrentId;
    request.apiKey = apiKey_;
    request.body = "files=" + files;
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return true;
}

bool RdClient::unrestrictLink(const std::string& link, std::string& url,
                              std::string& error) {
    char* escaped = curl_easy_escape(nullptr, link.c_str(),
                                     static_cast<int>(link.size()));
    if (!escaped) {
        error = "Unable to encode download link.";
        return false;
    }
    RdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/unrestrict/link";
    request.apiKey = apiKey_;
    request.body = std::string("link=") + escaped;
    curl_free(escaped);
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return parseUnrestrict(response.body, url, error);
}

bool RdClient::remove(const std::string& torrentId, std::string& error) {
    RdHttpRequest request;
    request.method = "DELETE";
    request.url = std::string(kBaseUrl) + "/torrents/delete/" + torrentId;
    request.apiKey = apiKey_;
    RdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkAuthError(response.body, response.status, error))
        return false;
    return true;
}

} // namespace pipensx
