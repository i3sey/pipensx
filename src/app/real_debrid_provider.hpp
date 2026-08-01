#pragma once

#include "debrid_provider.hpp"
#include "real_debrid_client.hpp"

namespace pipensx {

class RealDebridProvider : public DebridProvider {
public:
    explicit RealDebridProvider(std::string token,
                                RdTransport transport = {})
        : client_(std::move(token), std::move(transport)) {}

    bool validate(std::string& e) override { return client_.validate(e); }
    bool createFromMagnet(const std::string& m, std::string& id,
                          std::string& e) override {
        return client_.addMagnet(m, id, e);
    }
    bool createFromFile(const std::string& p, std::string& id,
                        std::string& e) override {
        return client_.addTorrent(p, id, e);
    }
    bool fetchInfo(const std::string& id, DebridInfo& out,
                   std::string& e) override;
    bool selectFiles(const std::string& id,
                     const std::vector<std::string>& ids,
                     std::string& e) override;
    bool resolveDownloadUrl(const std::string& id, const DebridInfo& info,
                            size_t kthSelected,
                            const DebridFile& file, std::string& url,
                            std::string& e) override;
    bool remove(const std::string& id, std::string& e) override {
        return client_.remove(id, e);
    }
    const char* name() const override { return "realdebrid"; }

private:
    RealDebridClient client_;
};

} // namespace pipensx
