#pragma once

#include "debrid_provider.hpp"

#include <functional>
#include <string>

namespace pipensx {

struct TsHttpRequest {
    std::string method;            // "GET" or "POST"
    std::string url;
    std::string body;              // JSON body when non-empty
    std::string uploadFilePath;    // multipart "file" part when non-empty
};

struct TsHttpResponse {
    long status = 0;
    std::string body;
};

using TsTransport = std::function<bool(const TsHttpRequest&, TsHttpResponse&,
    std::string&)>;

// A self-hosted TorrServer (github.com/YouROK/TorrServer) on the LAN. It keeps
// the swarm off the console the same way a debrid service does, so it rides the
// same seam — but the "key" is the server's base URL, and any credentials ride
// in it as http://user:pass@host:8090.
//
// The torrent id is its infohash: TorrServer keys everything on it, and
// re-posting `add` with magnet:?xt=urn:btih:<hash> both loads a torrent the
// server has unloaded and returns its current status, so one call covers
// create and poll.
class TorrserverProvider : public DebridProvider {
public:
    explicit TorrserverProvider(std::string baseUrl,
                                TsTransport transport = {});

    bool validate(std::string& error) override;
    bool createFromMagnet(const std::string& magnet, std::string& id,
                          std::string& error) override;
    bool createFromFile(const std::string& torrentPath, std::string& id,
                        std::string& error) override;
    bool fetchInfo(const std::string& id, DebridInfo& info,
                   std::string& error) override;
    // Nothing to select: TorrServer streams whatever file is asked for and
    // fetches only the pieces that file needs.
    bool selectFiles(const std::string&, const std::vector<std::string>&,
                     std::string&) override {
        return true;
    }
    bool resolveDownloadUrl(const std::string& id, const DebridInfo& info,
                            size_t kthSelected, const DebridFile& file,
                            std::string& url, std::string& error) override;
    bool remove(const std::string& id, std::string& error) override;
    const char* name() const override { return "torrserver"; }
    // The user typed this address; a LAN TorrServer speaks plain HTTP.
    bool allowsPlaintextLinks() const override { return true; }

    const std::string& baseUrl() const { return base_; }

    // "192.168.1.10:8090" -> "http://192.168.1.10:8090"; trailing '/' dropped.
    static std::string normalizeBaseUrl(const std::string& raw);
    // Parses a state.TorrentStatus into the provider-neutral shape.
    static bool parseStatus(const std::string& json, DebridInfo& info,
                            std::string& hash, std::string& error);

private:
    bool torrents(const std::string& body, TsHttpResponse& response,
                  std::string& error);

    std::string base_;
    TsTransport transport_;
};

} // namespace pipensx
