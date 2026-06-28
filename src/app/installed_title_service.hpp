#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace pipensx {

struct InstalledTitle {
    uint64_t applicationId = 0;
    std::string titleId;
    std::string name;
    std::string publisher;
    std::string iconPath;
};

class InstalledTitleService {
public:
    explicit InstalledTitleService(std::string rootPath);

    bool refresh(std::string& error);
    bool contains(const std::string& titleId) const;

    std::vector<InstalledTitle> titles() const;
    uint64_t generation() const;

    static std::string formatTitleId(uint64_t applicationId);

private:
    std::string rootPath_;
    std::string iconRoot_;
    std::mutex refreshMutex_;
    mutable std::mutex mutex_;
    std::vector<InstalledTitle> titles_;
    std::unordered_set<std::string> titleIds_;
    uint64_t generation_ = 0;
};

} // namespace pipensx
