#include "alldebrid_client.hpp"
#include "curl_https.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <curl/curl.h>

#include <cstring>
#include <cstdint>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

using Json = nlohmann::json;

constexpr const char* kBaseUrl = "https://api.alldebrid.com/v4";
constexpr const char* kBaseUrlV41 = "https://api.alldebrid.com/v4.1";

size_t writeBody(char* data, size_t size, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(data, size * count);
    return size * count;
}

std::string logEndpoint(const std::string& url) {
    const char* host = "https://api.alldebrid.com";
    size_t hostLen = std::strlen(host);
    if (url.compare(0, hostLen, host) == 0)
        return url.substr(hostLen);
    return url;
}

bool curlTransport(const AdHttpRequest& request,
                   AdHttpResponse& response, std::string& error) {
    const std::string endpoint = logEndpoint(request.url);
    log_msg("[alldebrid] %s %s\n", request.method.c_str(), endpoint.c_str());
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    curl_slist* headers = nullptr;
    curl_mime* mime = nullptr;
    std::string auth = "Authorization: Bearer " + request.apiKey;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curlPinHttpsOnly(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curlApplyTrustedSsl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    if (!request.uploadFilePath.empty()) {
        mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "files[0]");
        curl_mime_filedata(part, request.uploadFilePath.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (request.method == "POST") {
        if (!request.body.empty()) {
            headers = curl_slist_append(headers,
                "Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (mime)
        curl_mime_free(mime);
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
        error = std::string("AllDebrid request failed: ") +
                curl_easy_strerror(result);
    log_msg("[alldebrid] %s %s -> HTTP %ld (%s)\n",
            request.method.c_str(), endpoint.c_str(), response.status,
            result == CURLE_OK ? "ok" : curl_easy_strerror(result));
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool parseJson(const std::string& text, Json& root, std::string& error) {
    root = Json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        error = "AllDebrid returned an invalid response.";
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

bool readIntField(const Json& obj, const char* key, int& value) {
    if (obj.contains(key) && obj[key].is_number_integer()) {
        value = obj[key].get<int>();
        return true;
    }
    return false;
}

bool readIdField(const Json& obj, const char* key, std::string& value) {
    if (!obj.contains(key))
        return false;
    if (obj[key].is_string()) {
        value = obj[key].get<std::string>();
        return !value.empty();
    }
    if (obj[key].is_number()) {
        value = std::to_string(obj[key].get<uint64_t>());
        return true;
    }
    return false;
}

std::string envelopeError(const Json& root) {
    if (root.contains("error") && root["error"].is_object()) {
        const Json& err = root["error"];
        std::string code;
        std::string message;
        readStringField(err, "code", code);
        readStringField(err, "message", message);
        if (code == "AUTH_BAD_APIKEY" || code == "AUTH_MISSING_APIKEY")
            return "AllDebrid key rejected - relink in Settings.";
        if (code == "AUTH_BLOCKED")
            return "AllDebrid blocked this login — confirm the email, then retry.";
        if (code == "AUTH_USER_BANNED")
            return "AllDebrid account is banned.";
        if (code == "MAGNET_MUST_BE_PREMIUM" || code == "MUST_BE_PREMIUM")
            return "AllDebrid premium is required — free accounts cannot add torrents.";
        if (!message.empty())
            return message;
        if (!code.empty())
            return code;
    }
    return {};
}

bool unwrapData(const std::string& json, Json& data, std::string& error) {
    Json root;
    if (!parseJson(json, root, error))
        return false;
    if (!root.is_object()) {
        error = "AllDebrid returned an invalid response.";
        return false;
    }
    std::string status;
    readStringField(root, "status", status);
    if (status == "error") {
        error = envelopeError(root);
        if (error.empty())
            error = "AllDebrid request failed.";
        return false;
    }
    if (status != "success" || !root.contains("data") ||
        !root["data"].is_object()) {
        error = "AllDebrid returned an invalid response.";
        return false;
    }
    data = root["data"];
    return true;
}

bool checkTransportError(const AdHttpResponse& response, std::string& error) {
    Json root;
    std::string parseError;
    const bool parsed = parseJson(response.body, root, parseError);
    if (parsed && root.is_object()) {
        std::string status;
        readStringField(root, "status", status);
        if (status == "error") {
            error = envelopeError(root);
            if (error.empty())
                error = "AllDebrid request failed.";
            log_msg("[alldebrid] error HTTP %ld: %s\n", response.status,
                    error.c_str());
            return false;
        }
    }
    if (response.status == 401 || response.status == 403) {
        error = "AllDebrid key rejected - relink in Settings.";
        log_msg("[alldebrid] error HTTP %ld: %s\n", response.status,
                error.c_str());
        return false;
    }
    if (response.status == 429) {
        error = "AllDebrid rate limit — retry shortly.";
        return false;
    }
    if (response.status != 0 &&
        (response.status < 200 || response.status >= 300)) {
        error = parsed ? envelopeError(root) : std::string();
        if (error.empty())
            error = "AllDebrid request failed (HTTP " +
                    std::to_string(response.status) + ").";
        log_msg("[alldebrid] error HTTP %ld: %s\n", response.status,
                error.c_str());
        return false;
    }
    return true;
}

std::string itemError(const Json& obj) {
    if (!obj.contains("error") || !obj["error"].is_object())
        return {};
    std::string message;
    readStringField(obj["error"], "message", message);
    if (!message.empty())
        return message;
    readStringField(obj["error"], "code", message);
    return message;
}

const Json* magnetById(const Json& data, const std::string& id) {
    if (!data.contains("magnets"))
        return nullptr;
    const Json& magnets = data["magnets"];
    if (magnets.is_object())
        return &magnets;
    if (!magnets.is_array())
        return nullptr;
    for (const Json& item : magnets) {
        if (!item.is_object())
            continue;
        std::string itemId;
        if (readIdField(item, "id", itemId) && itemId == id)
            return &item;
    }
    if (!magnets.empty() && magnets[0].is_object())
        return &magnets[0];
    return nullptr;
}

void flattenFiles(const Json& node, const std::string& prefix,
                  std::vector<AdFile>& out) {
    if (node.is_array()) {
        for (const Json& child : node)
            flattenFiles(child, prefix, out);
        return;
    }
    if (!node.is_object())
        return;
    std::string name;
    readStringField(node, "n", name);
    if (node.contains("e") && node["e"].is_array()) {
        std::string next = prefix;
        if (!name.empty())
            next = prefix.empty() ? name : prefix + "/" + name;
        for (const Json& child : node["e"])
            flattenFiles(child, next, out);
        return;
    }
    AdFile file;
    file.path = prefix.empty() ? name : prefix + "/" + name;
    if (file.path.empty())
        file.path = "noname";
    readNumberField(node, "s", file.bytes);
    readStringField(node, "l", file.link);
    file.id = file.link.empty() ? std::to_string(out.size()) : file.link;
    out.push_back(std::move(file));
}

void forceHttps(std::string& url) {
    if (url.compare(0, 7, "http://") == 0)
        url.replace(0, 7, "https://");
}

} // namespace

AdClient::AdClient(std::string apiKey, AdTransport transport)
    : apiKey_(std::move(apiKey)),
      transport_(transport ? std::move(transport) : curlTransport) {}

const std::string& AdClient::apiKey() const { return apiKey_; }

bool AdClient::parseUserResponse(const std::string& json, std::string& error) {
    Json data;
    if (!unwrapData(json, data, error))
        return false;
    if (!data.contains("user") || !data["user"].is_object()) {
        error = "AllDebrid key is not valid.";
        return false;
    }
    const Json& user = data["user"];
    if (user.contains("isPremium") && user["isPremium"].is_boolean() &&
        !user["isPremium"].get<bool>()) {
        error = "AllDebrid premium is required — free accounts cannot add torrents.";
        return false;
    }
    return true;
}

bool AdClient::parseAddMagnetResponse(const std::string& json,
                                      std::string& torrentId,
                                      std::string& error) {
    Json data;
    if (!unwrapData(json, data, error))
        return false;
    if (!data.contains("magnets") || !data["magnets"].is_array() ||
        data["magnets"].empty() || !data["magnets"][0].is_object()) {
        error = "AllDebrid did not return a torrent id.";
        return false;
    }
    const Json& magnet = data["magnets"][0];
    const std::string detail = itemError(magnet);
    if (!detail.empty()) {
        error = detail;
        return false;
    }
    if (!readIdField(magnet, "id", torrentId)) {
        error = "AllDebrid did not return a torrent id.";
        return false;
    }
    return true;
}

bool AdClient::parseAddTorrentResponse(const std::string& json,
                                       std::string& torrentId,
                                       std::string& error) {
    Json data;
    if (!unwrapData(json, data, error))
        return false;
    if (!data.contains("files") || !data["files"].is_array() ||
        data["files"].empty() || !data["files"][0].is_object()) {
        error = "AllDebrid did not return a torrent id.";
        return false;
    }
    const Json& file = data["files"][0];
    const std::string detail = itemError(file);
    if (!detail.empty()) {
        error = detail;
        return false;
    }
    if (!readIdField(file, "id", torrentId)) {
        error = "AllDebrid did not return a torrent id.";
        return false;
    }
    return true;
}

bool AdClient::parseStatus(const std::string& json, const std::string& id,
                           AdTorrentInfo& info, std::string& error) {
    Json data;
    if (!unwrapData(json, data, error))
        return false;
    const Json* magnet = magnetById(data, id);
    if (!magnet) {
        error = "AllDebrid returned an invalid torrent entry.";
        return false;
    }
    const std::string detail = itemError(*magnet);
    if (!detail.empty()) {
        error = detail;
        return false;
    }
    info = AdTorrentInfo{};
    if (!readIdField(*magnet, "id", info.id)) {
        error = "AllDebrid returned an invalid torrent entry.";
        return false;
    }
    readStringField(*magnet, "filename", info.filename);
    readNumberField(*magnet, "size", info.bytes);
    readNumberField(*magnet, "downloaded", info.downloaded);
    readStringField(*magnet, "status", info.status);
    readIntField(*magnet, "statusCode", info.statusCode);
    if (info.statusCode == 4)
        info.progress = 1.0;
    else if (info.bytes > 0)
        info.progress = static_cast<double>(info.downloaded) /
                        static_cast<double>(info.bytes);
    if (info.progress < 0.0)
        info.progress = 0.0;
    if (info.progress > 1.0)
        info.progress = 1.0;
    if (magnet->contains("files"))
        flattenFiles((*magnet)["files"], "", info.files);
    return true;
}

bool AdClient::parseFiles(const std::string& json, const std::string& id,
                          std::vector<AdFile>& files, std::string& error) {
    Json data;
    if (!unwrapData(json, data, error))
        return false;
    const Json* magnet = magnetById(data, id);
    if (!magnet) {
        error = "AllDebrid did not return files for this torrent.";
        return false;
    }
    const std::string detail = itemError(*magnet);
    if (!detail.empty()) {
        error = detail;
        return false;
    }
    files.clear();
    if (magnet->contains("files"))
        flattenFiles((*magnet)["files"], "", files);
    return true;
}

bool AdClient::parseUnrestrict(const std::string& json, AdUnrestrict& out,
                               std::string& error) {
    Json data;
    if (!unwrapData(json, data, error))
        return false;
    out = AdUnrestrict{};
    readStringField(data, "link", out.url);
    if (out.url.empty()) {
        error = "AllDebrid did not return a download link.";
        return false;
    }
    forceHttps(out.url);
    readStringField(data, "filename", out.filename);
    readNumberField(data, "filesize", out.filesize);
    return true;
}

bool AdClient::validateKey(std::string& error) {
    AdHttpRequest request;
    request.method = "GET";
    request.url = std::string(kBaseUrl) + "/user";
    request.apiKey = apiKey_;
    AdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkTransportError(response, error))
        return false;
    return parseUserResponse(response.body, error);
}

bool AdClient::createFromMagnet(const std::string& magnet,
                                std::string& torrentId,
                                std::string& error) {
    char* escaped = curl_easy_escape(nullptr, magnet.c_str(),
                                     static_cast<int>(magnet.size()));
    if (!escaped) {
        error = "Unable to encode magnet link.";
        return false;
    }
    AdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/magnet/upload";
    request.apiKey = apiKey_;
    request.body = std::string("magnets[]=") + escaped;
    curl_free(escaped);
    AdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkTransportError(response, error))
        return false;
    return parseAddMagnetResponse(response.body, torrentId, error);
}

bool AdClient::createFromFile(const std::string& torrentPath,
                              std::string& torrentId,
                              std::string& error) {
    AdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/magnet/upload/file";
    request.apiKey = apiKey_;
    request.uploadFilePath = torrentPath;
    AdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkTransportError(response, error))
        return false;
    return parseAddTorrentResponse(response.body, torrentId, error);
}

bool AdClient::fetchInfo(const std::string& torrentId, AdTorrentInfo& info,
                         std::string& error) {
    AdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrlV41) + "/magnet/status";
    request.apiKey = apiKey_;
    request.body = "id=" + torrentId;
    AdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkTransportError(response, error))
        return false;
    if (!parseStatus(response.body, torrentId, info, error))
        return false;
    const bool ready = info.statusCode == 4 || info.status == "Ready";
    if (!ready)
        return true;
    AdHttpRequest filesRequest;
    filesRequest.method = "POST";
    filesRequest.url = std::string(kBaseUrl) + "/magnet/files";
    filesRequest.apiKey = apiKey_;
    filesRequest.body = "id[]=" + torrentId;
    AdHttpResponse filesResponse;
    if (!transport_(filesRequest, filesResponse, error))
        return false;
    if (!checkTransportError(filesResponse, error))
        return false;
    std::vector<AdFile> files;
    if (!parseFiles(filesResponse.body, torrentId, files, error))
        return false;
    if (!files.empty())
        info.files = std::move(files);
    return true;
}

bool AdClient::unrestrictLink(const std::string& link, AdUnrestrict& out,
                              std::string& error) {
    char* escaped = curl_easy_escape(nullptr, link.c_str(),
                                     static_cast<int>(link.size()));
    if (!escaped) {
        error = "Unable to encode download link.";
        return false;
    }
    AdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/link/unlock";
    request.apiKey = apiKey_;
    request.body = std::string("link=") + escaped;
    curl_free(escaped);
    AdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkTransportError(response, error))
        return false;
    return parseUnrestrict(response.body, out, error);
}

bool AdClient::remove(const std::string& torrentId, std::string& error) {
    AdHttpRequest request;
    request.method = "POST";
    request.url = std::string(kBaseUrl) + "/magnet/delete";
    request.apiKey = apiKey_;
    request.body = "id=" + torrentId;
    AdHttpResponse response;
    if (!transport_(request, response, error))
        return false;
    if (!checkTransportError(response, error))
        return false;
    Json data;
    return unwrapData(response.body, data, error);
}

} // namespace pipensx
