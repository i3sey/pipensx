#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx {

struct RdFile {
    int id = 0;
    std::string path;
    uint64_t bytes = 0;
    bool selected = false;
};

struct RdInfo {
    std::string id;
    std::string filename;
    std::string hash;
    std::string status;
    uint64_t bytes = 0;
    double progress = 0.0;
    std::vector<RdFile> files;
    std::vector<std::string> links;
};

struct RdHttpRequest {
    std::string method;
    std::string url;
    std::string token;
    std::string body;
    std::string uploadFilePath;
};

struct RdHttpResponse {
    long status = 0;
    std::string body;
};

using RdTransport = std::function<bool(const RdHttpRequest&,
    RdHttpResponse&, std::string&)>;

class RealDebridClient {
public:
    explicit RealDebridClient(std::string token, RdTransport transport = {});

    bool validate(std::string& error);
    bool addMagnet(const std::string& magnet, std::string& id,
                   std::string& error);
    bool addTorrent(const std::string& torrentPath, std::string& id,
                    std::string& error);
    bool info(const std::string& id, RdInfo& out, std::string& error);
    bool selectFiles(const std::string& id, const std::string& fileIdsCsv,
                     std::string& error);
    bool unrestrict(const std::string& link, std::string& downloadUrl,
                    std::string& error);
    bool remove(const std::string& id, std::string& error);

    static bool parseAdd(const std::string& json, std::string& id,
                         std::string& error);
    static bool parseInfo(const std::string& json, RdInfo& out,
                          std::string& error);
    static bool parseUnrestrict(const std::string& json, std::string& url,
                                std::string& error);

private:
    std::string token_;
    RdTransport transport_;
};

} // namespace pipensx
