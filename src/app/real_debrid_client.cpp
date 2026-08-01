#include "real_debrid_client.hpp"
#include "curl_https.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

using Json = nlohmann::json;

constexpr const char* kBase = "https://api.real-debrid.com/rest/1.0";

std::string formEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

size_t writeBody(char* data, size_t size, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(data, size * count);
    return size * count;
}

bool curlTransport(const RdHttpRequest& request,
                   RdHttpResponse& response, std::string& error) {
    std::string endpoint = request.url;
    size_t baseLen = std::strlen(kBase);
    if (endpoint.compare(0, baseLen, kBase) == 0)
        endpoint = endpoint.substr(baseLen);
    log_msg("[realdebrid] %s %s\n", request.method.c_str(), endpoint.c_str());
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialize HTTP.";
        return false;
    }
    curl_slist* headers = nullptr;
    std::string auth = "Authorization: Bearer " + request.token;
    headers = curl_slist_append(headers, auth.c_str());

    if (!request.uploadFilePath.empty()) {
        struct curl_slist* putHeaders = nullptr;
        putHeaders = curl_slist_append(putHeaders, auth.c_str());
        curl_slist_free_all(headers);
        headers = putHeaders;
        FILE* f = std::fopen(request.uploadFilePath.c_str(), "rb");
        if (!f) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            error = "Unable to open .torrent file for upload.";
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        long fsize = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READDATA, f);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                         static_cast<curl_off_t>(fsize));
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
        std::fclose(f);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        curl_slist_free_all(headers);
        if (result != CURLE_OK)
            error = std::string("RealDebrid request failed: ") +
                    curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        return result == CURLE_OK;
    }

    if (!request.body.empty()) {
        headers = curl_slist_append(headers,
            "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    } else if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    if (request.method == "PUT")
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (request.method == "DELETE")
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

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
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
        error = std::string("RealDebrid request failed: ") +
                curl_easy_strerror(result);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

} // namespace

RealDebridClient::RealDebridClient(std::string token, RdTransport transport)
    : token_(std::move(token)),
      transport_(transport ? std::move(transport) : curlTransport) {}

bool RealDebridClient::validate(std::string& error) {
    RdHttpRequest r;
    r.method = "GET";
    r.url = std::string(kBase) + "/user";
    r.token = token_;
    RdHttpResponse res;
    std::string terr;
    if (!transport_(r, res, terr)) {
        error = terr.empty() ? "Unable to reach RealDebrid." : terr;
        return false;
    }
    if (res.status == 401) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    if (res.status == 403) {
        error = "RealDebrid account is not premium.";
        return false;
    }
    return res.status >= 200 && res.status < 300;
}

bool RealDebridClient::addMagnet(const std::string& magnet,
                                 std::string& id, std::string& error) {
    RdHttpRequest r;
    r.method = "POST";
    r.url = std::string(kBase) + "/torrents/addMagnet";
    r.token = token_;
    r.body = "magnet=" + formEncode(magnet);
    RdHttpResponse res;
    std::string terr;
    if (!transport_(r, res, terr)) {
        error = terr;
        return false;
    }
    if (res.status == 401 || res.status == 403) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        error = "RealDebrid rejected the magnet.";
        return false;
    }
    return parseAdd(res.body, id, error);
}

bool RealDebridClient::addTorrent(const std::string& torrentPath,
                                  std::string& id, std::string& error) {
    RdHttpRequest hostReq;
    hostReq.method = "GET";
    hostReq.url = std::string(kBase) + "/torrents/availableHosts";
    hostReq.token = token_;
    RdHttpResponse hostRes;
    std::string terr;
    if (!transport_(hostReq, hostRes, terr)) {
        error = terr.empty() ? "Unable to reach RealDebrid." : terr;
        return false;
    }
    if (hostRes.status < 200 || hostRes.status >= 300) {
        error = "RealDebrid hosts request failed.";
        return false;
    }
    std::string host;
    {
        auto j = Json::parse(hostRes.body, nullptr, false);
        if (!j.is_discarded() && j.is_array() && !j.empty()) {
            const Json& first = j[0];
            if (first.is_object() && first.contains("host") &&
                first["host"].is_string())
                host = first["host"].get<std::string>();
        }
    }
    if (host.empty()) {
        error = "RealDebrid: no available host for torrent upload.";
        return false;
    }

    RdHttpRequest r;
    r.method = "PUT";
    r.url = std::string(kBase) + "/torrents/addTorrent?host=" + host;
    r.token = token_;
    r.uploadFilePath = torrentPath;
    RdHttpResponse res;
    if (!transport_(r, res, terr)) {
        error = terr;
        return false;
    }
    if (res.status == 401 || res.status == 403) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        error = "RealDebrid rejected the torrent file.";
        return false;
    }
    return parseAdd(res.body, id, error);
}

bool RealDebridClient::info(const std::string& id, RdInfo& out,
                            std::string& error) {
    RdHttpRequest r;
    r.method = "GET";
    r.url = std::string(kBase) + "/torrents/info/" + id;
    r.token = token_;
    RdHttpResponse res;
    std::string terr;
    if (!transport_(r, res, terr)) {
        error = terr;
        return false;
    }
    if (res.status == 401 || res.status == 403) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        error = "RealDebrid info request failed.";
        return false;
    }
    return parseInfo(res.body, out, error);
}

bool RealDebridClient::selectFiles(const std::string& id,
                                   const std::string& fileIdsCsv,
                                   std::string& error) {
    RdHttpRequest r;
    r.method = "POST";
    r.url = std::string(kBase) + "/torrents/selectFiles/" + id;
    r.token = token_;
    r.body = "files=" + fileIdsCsv;
    RdHttpResponse res;
    std::string terr;
    if (!transport_(r, res, terr)) {
        error = terr;
        return false;
    }
    if (res.status == 401 || res.status == 403) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    return res.status >= 200 && res.status < 300;
}

bool RealDebridClient::unrestrict(const std::string& link,
                                  std::string& downloadUrl,
                                  std::string& error) {
    RdHttpRequest r;
    r.method = "POST";
    r.url = std::string(kBase) + "/unrestrict/link";
    r.token = token_;
    r.body = "link=" + formEncode(link);
    RdHttpResponse res;
    std::string terr;
    if (!transport_(r, res, terr)) {
        error = terr;
        return false;
    }
    if (res.status == 401 || res.status == 403) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        error = "RealDebrid unrestrict failed.";
        return false;
    }
    return parseUnrestrict(res.body, downloadUrl, error);
}

bool RealDebridClient::remove(const std::string& id, std::string& error) {
    RdHttpRequest r;
    r.method = "DELETE";
    r.url = std::string(kBase) + "/torrents/delete/" + id;
    r.token = token_;
    RdHttpResponse res;
    std::string terr;
    if (!transport_(r, res, terr)) {
        error = terr;
        return false;
    }
    if (res.status == 401 || res.status == 403) {
        error = "RealDebrid rejected the token.";
        return false;
    }
    return res.status >= 200 && res.status < 300;
}

bool RealDebridClient::parseAdd(const std::string& json, std::string& id,
                                std::string& error) {
    auto j = Json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("id") ||
        !j["id"].is_string()) {
        error = "RealDebrid returned an unexpected response.";
        return false;
    }
    id = j["id"].get<std::string>();
    return true;
}

bool RealDebridClient::parseInfo(const std::string& json, RdInfo& out,
                                 std::string& error) {
    auto j = Json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "Bad RealDebrid info response.";
        return false;
    }
    auto str = [&](const char* k) {
        return j.contains(k) && j[k].is_string()
                   ? j[k].get<std::string>()
                   : std::string();
    };
    out.id = str("id");
    out.filename = str("filename");
    out.hash = str("hash");
    out.status = str("status");
    if (j.contains("bytes") && j["bytes"].is_number_integer())
        out.bytes = j["bytes"].get<uint64_t>();
    if (j.contains("progress") && j["progress"].is_number())
        out.progress = j["progress"].get<double>() / 100.0;
    if (j.contains("files") && j["files"].is_array()) {
        for (auto& f : j["files"]) {
            if (!f.is_object())
                continue;
            RdFile rf;
            if (f.contains("id") && f["id"].is_number_integer())
                rf.id = f["id"].get<int>();
            if (f.contains("path") && f["path"].is_string())
                rf.path = f["path"].get<std::string>();
            if (f.contains("bytes") && f["bytes"].is_number_integer())
                rf.bytes = f["bytes"].get<uint64_t>();
            if (f.contains("selected") && f["selected"].is_number_integer())
                rf.selected = f["selected"].get<int>() != 0;
            out.files.push_back(std::move(rf));
        }
    }
    if (j.contains("links") && j["links"].is_array()) {
        for (auto& l : j["links"])
            if (l.is_string())
                out.links.push_back(l.get<std::string>());
    }
    return true;
}

bool RealDebridClient::parseUnrestrict(const std::string& json,
                                       std::string& url,
                                       std::string& error) {
    auto j = Json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("download") ||
        !j["download"].is_string()) {
        error = "RealDebrid could not unrestrict the link.";
        return false;
    }
    url = j["download"].get<std::string>();
    return true;
}

} // namespace pipensx
