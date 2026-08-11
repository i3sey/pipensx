#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx {

struct RdFile {
    std::string id;
    std::string path;
    uint64_t bytes = 0;
};

struct RdTorrentInfo {
    std::string id;
    std::string hash;
    std::string filename;
    uint64_t bytes = 0;
    double progress = 0.0;
    std::string status;
    std::vector<RdFile> files;
    std::vector<std::string> links;
};

struct RdHttpRequest {
    std::string method;
    std::string url;
    std::string apiKey;
    std::string body;
    std::string uploadFilePath;
};

struct RdHttpResponse {
    long status = 0;
    std::string body;
};

using RdTransport = std::function<bool(const RdHttpRequest&,
    RdHttpResponse&, std::string&)>;

class RdClient {
public:
    explicit RdClient(std::string apiKey, RdTransport transport = {});
    bool validateKey(std::string& error);
    bool createFromMagnet(const std::string& magnet, std::string& torrentId,
                          std::string& error);
    bool createFromFile(const std::string& torrentPath, std::string& torrentId,
                        std::string& error);
    bool fetchInfo(const std::string& torrentId, RdTorrentInfo& info,
                   std::string& error);
    bool selectFiles(const std::string& torrentId,
                     const std::vector<std::string>& fileIds,
                     std::string& error);
    bool unrestrictLink(const std::string& link, std::string& url,
                        std::string& error);
    bool remove(const std::string& torrentId, std::string& error);
    const std::string& apiKey() const;

    static bool parseUserResponse(const std::string& json, std::string& error);
    static bool parseAddMagnetResponse(const std::string& json,
                                       std::string& torrentId,
                                       std::string& error);
    static bool parseAddTorrentResponse(const std::string& json,
                                        std::string& torrentId,
                                        std::string& error);
    static bool parseInfo(const std::string& json, RdTorrentInfo& info,
                          std::string& error);
    static bool parseUnrestrict(const std::string& json, std::string& url,
                                std::string& error);

private:
    std::string apiKey_;
    RdTransport transport_;
};

} // namespace pipensx
