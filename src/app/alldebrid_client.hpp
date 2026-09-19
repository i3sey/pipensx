#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx {

struct AdFile {
    std::string id;
    std::string path;
    std::string link;
    uint64_t bytes = 0;
};

struct AdTorrentInfo {
    std::string id;
    std::string filename;
    uint64_t bytes = 0;
    uint64_t downloaded = 0;
    double progress = 0.0;
    std::string status;
    int statusCode = -1;
    std::vector<AdFile> files;
};

struct AdHttpRequest {
    std::string method;
    std::string url;
    std::string apiKey;
    std::string body;
    std::string uploadFilePath;
};

struct AdHttpResponse {
    long status = 0;
    std::string body;
};

struct AdUnrestrict {
    std::string url;
    std::string filename;
    uint64_t filesize = 0;
};

using AdTransport = std::function<bool(const AdHttpRequest&,
                                       AdHttpResponse&, std::string&)>;

class AdClient {
public:
    explicit AdClient(std::string apiKey, AdTransport transport = {});
    bool validateKey(std::string& error);
    bool createFromMagnet(const std::string& magnet, std::string& torrentId,
                          std::string& error);
    bool createFromFile(const std::string& torrentPath, std::string& torrentId,
                        std::string& error);
    bool fetchInfo(const std::string& torrentId, AdTorrentInfo& info,
                   std::string& error);
    bool unrestrictLink(const std::string& link, AdUnrestrict& out,
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
    static bool parseStatus(const std::string& json, const std::string& id,
                            AdTorrentInfo& info, std::string& error);
    static bool parseFiles(const std::string& json, const std::string& id,
                           std::vector<AdFile>& files, std::string& error);
    static bool parseUnrestrict(const std::string& json, AdUnrestrict& out,
                                std::string& error);

private:
    std::string apiKey_;
    AdTransport transport_;
};

} // namespace pipensx
