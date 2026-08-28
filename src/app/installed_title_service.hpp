#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
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
    bool uninstall(const std::string& titleId, std::string& error);
    bool uninstall(const std::string& titleId, std::string& error,
                   std::string& refreshError);
    bool contains(const std::string& titleId) const;
    bool containsDlc(const std::string& titleId) const;

    std::vector<InstalledTitle> titles() const;
    // Normalized title-id snapshot copied under one lock for bulk lookups.
    std::unordered_set<std::string> titleIds() const;
    std::vector<std::string> dlcTitleIds() const;
    size_t dlcCountForBase(const std::string& titleId) const;
    uint64_t generation() const;
    const std::string& rootPath() const { return rootPath_; }
    // Golden-runner seam: the PC shim reports an empty library, but the
    // installed-populated screen needs rows to pin the update chips. Replaces
    // the enumerated set like a refresh would (generation bumps).
    void injectTitles(std::vector<InstalledTitle> titles);
    void injectDlcTitleIds(std::vector<std::string> dlcTitleIds);

    // Updates set bit 11 (…800); DLC lives in the low 12 bits from …1000.
    // Masking those bits maps every variant onto the base application id.
    static uint64_t nxBaseApplicationId(uint64_t applicationId) {
        return applicationId & ~0x1FFFULL;
    }

    static std::string formatTitleId(uint64_t applicationId) {
        char text[17];
        std::snprintf(text, sizeof(text), "%016llX",
                      static_cast<unsigned long long>(applicationId));
        return text;
    }
    static bool parseTitleId(const std::string& titleId,
                             uint64_t& applicationId) {
        if (titleId.size() != 16)
            return false;
        uint64_t value = 0;
        for (char c : titleId) {
            int digit = -1;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            if (digit < 0)
                return false;
            value = (value << 4) | static_cast<uint64_t>(digit);
        }
        applicationId = value;
        return true;
    }

private:
    std::string rootPath_;
    std::string iconRoot_;
    std::mutex refreshMutex_;
    mutable std::mutex mutex_;
    std::vector<InstalledTitle> titles_;
    std::unordered_set<std::string> titleIds_;
    std::unordered_set<std::string> dlcTitleIds_;
    uint64_t generation_ = 0;
};

// Maps an application id onto the Patch content-meta version string used by
// game-update checks. Empty when the ncm scan was incomplete (CheckError);
// "0" when the scan succeeded and no patch is installed.
inline std::string installedPatchVersionString(
    uint64_t applicationId,
    const std::unordered_map<uint64_t, uint32_t>& patchVersions,
    bool patchMetaComplete) {
    if (!patchMetaComplete)
        return {};
    const auto patch = patchVersions.find(applicationId | 0x800ULL);
    return patch == patchVersions.end() ? "0"
                                        : std::to_string(patch->second);
}

// Nintendo title version as eShop x.y.z: (major<<16)|(minor<<8)|micro.
// Empty or non-decimal input yields an empty string so the caller can hide
// the row. "0" is a real version (base game, no patch) and formats as 0.0.0.
inline bool parseTitleVersionDecimal(const std::string& text, uint64_t& out) {
    if (text.empty())
        return false;
    uint64_t value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

inline std::string formatTitleVersion(const std::string& decimal) {
    uint64_t value = 0;
    if (!parseTitleVersionDecimal(decimal, value))
        return {};
    const unsigned major = static_cast<unsigned>(value >> 16);
    const unsigned minor = static_cast<unsigned>((value >> 8) & 0xff);
    const unsigned micro = static_cast<unsigned>(value & 0xff);
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(micro);
}

inline bool titleVersionIsNewer(const std::string& latest,
                                const std::string& installed) {
    uint64_t latestValue = 0;
    uint64_t installedValue = 0;
    return parseTitleVersionDecimal(latest, latestValue) &&
           parseTitleVersionDecimal(installed, installedValue) &&
           latestValue > installedValue;
}

} // namespace pipensx
