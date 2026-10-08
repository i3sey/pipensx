#pragma once

#include "debrid_provider.hpp"
#include "realdebrid_client.hpp"

namespace pipensx {

class RealdebridProvider : public DebridProvider {
public:
    explicit RealdebridProvider(std::string apiKey,
                                RdTransport transport = {})
        : client_(std::move(apiKey), std::move(transport)) {}

    bool validate(std::string& e) override { return client_.validateKey(e); }
    bool createFromMagnet(const std::string& m, std::string& id,
                          std::string& e) override;
    bool createFromFile(const std::string& p, std::string& id,
                        std::string& e) override;
    bool fetchInfo(const std::string& id, DebridInfo& out,
                   std::string& e) override;
    bool selectFiles(const std::string& id,
                     const std::vector<std::string>& fileIds,
                     std::string& e) override;
    bool selectionAlreadyFinalized() const override {
        return selectionAlreadyFinalized_;
    }
    bool resolveDownloadUrl(const std::string& id, const DebridInfo& info,
                            size_t kthSelected,
                            const DebridFile& file, std::string& url,
                            std::string& e) override;
    bool remove(const std::string& id, std::string& e) override;
    bool resolveGroupedDownloadUrl(const DebridInfo& info, size_t index,
                                   std::string& url, uint64_t& bytes,
                                   std::string& e) override;
    bool createSelectionReplacement(const std::string& id, std::string& newId,
                                    std::string& e) override;
    const char* name() const override { return "realdebrid"; }

private:
    RdClient client_;
    bool selectionAlreadyFinalized_ = false;
};

} // namespace pipensx
