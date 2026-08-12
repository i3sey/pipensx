#pragma once

#include "debrid_provider.hpp"
#include "torbox_client.hpp"

namespace pipensx {

class TorboxProvider : public DebridProvider {
public:
    explicit TorboxProvider(std::string apiKey,
                            TorboxTransport transport = {})
        : client_(std::move(apiKey), std::move(transport)) {}

    bool validate(std::string& e) override { return client_.validateKey(e); }
    bool createFromMagnet(const std::string& m, std::string& id,
                          std::string& e) override;
    bool createFromFile(const std::string& p, std::string& id,
                        std::string& e) override;
    bool fetchInfo(const std::string& id, DebridInfo& out,
                   std::string& e) override;
    bool selectFiles(const std::string&, const std::vector<std::string>&,
                     std::string&) override {
        return true;
    }
    bool resolveDownloadUrl(const std::string& id, const DebridInfo& info,
                            size_t kthSelected,
                            const DebridFile& file, std::string& url,
                            std::string& e) override;
    bool remove(const std::string& id, std::string& e) override;
    const char* name() const override { return "torbox"; }

private:
    TorboxClient client_;
};

} // namespace pipensx
