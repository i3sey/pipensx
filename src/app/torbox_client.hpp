#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx {

struct TorboxFile {
    uint64_t id;
    std::string name;
    uint64_t size;
};

struct TorboxTorrentInfo {
    uint64_t id = 0;
    std::string hash;              // lowercase hex
    std::string name;
    uint64_t size = 0;
    double progress = 0.0;         // 0..1 fetch fraction (API % normalized)
    std::string state;             // raw download_state
    bool ready = false;            // download_finished && download_present
    std::vector<TorboxFile> files;
};

struct TorboxHttpRequest {
    std::string method;            // "GET" or "POST"
    std::string url;
    std::string apiKey;            // sent as Authorization: Bearer
    std::string body;              // JSON body when non-empty (POST)
    std::string magnet;            // multipart field when non-empty
    std::string uploadFilePath;    // multipart "file" part when non-empty
};

struct TorboxHttpResponse {
    long status = 0;
    std::string body;
};

using TorboxTransport = std::function<bool(const TorboxHttpRequest&,
    TorboxHttpResponse&, std::string&)>;

class TorboxClient {
public:
    explicit TorboxClient(std::string apiKey, TorboxTransport transport = {});
    bool validateKey(std::string& error);
    bool createFromMagnet(const std::string& magnet, uint64_t& torboxId,
                          std::string& error);
    bool createFromFile(const std::string& torrentPath, uint64_t& torboxId,
                        std::string& error);
    bool fetchInfo(uint64_t torboxId, TorboxTorrentInfo& info,
                   std::string& error);
    bool requestDownloadLink(uint64_t torboxId, uint64_t fileId,
                             std::string& url, std::string& error);
    bool remove(uint64_t torboxId, std::string& error);
    const std::string& apiKey() const;

    // Pure parsers (unit-tested directly):
    static bool parseSuccess(const std::string& json, std::string& error);
    static bool parseCreate(const std::string& json, uint64_t& torboxId,
                            std::string& error);
    static bool parseInfo(const std::string& json, uint64_t torboxId,
                          TorboxTorrentInfo& info, std::string& error);
    static bool parseDownloadLink(const std::string& json, std::string& url,
                                  std::string& error);

private:
    std::string apiKey_;
    TorboxTransport transport_;
};

} // namespace pipensx
