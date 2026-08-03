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
    // Installed title version as a decimal string: the title version of the
    // installed Patch content meta read from ncm (0 when no patch is
    // installed). Compared against the metadata index's latestVersion by the
    // game-update check; empty when the ncm read is unavailable.
    std::string version;
    std::string iconPath;
};

class InstalledTitleService {
public:
    explicit InstalledTitleService(std::string rootPath);

    bool refresh(std::string& error);
    bool contains(const std::string& titleId) const;

    std::vector<InstalledTitle> titles() const;
    uint64_t generation() const;
    const std::string& rootPath() const { return rootPath_; }
    // Golden-runner seam: the PC shim reports an empty library, but the
    // installed-populated screen needs rows to pin the update chips. Replaces
    // the enumerated set like a refresh would (generation bumps).
    void injectTitles(std::vector<InstalledTitle> titles);

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
