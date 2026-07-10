#pragma once

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pipensx {

struct ReleaseInfo {
    std::string version;
    std::string nroUrl;
    std::string checksumUrl;
};

struct UpdateCheckResult {
    bool ok = false;
    bool updateAvailable = false;
    ReleaseInfo release;
    std::string error;
};

// Retrieves published releases from the canonical GitHub repository and
// atomically replaces the homebrew NRO only after its published SHA-256
// checksum matches. The process must be restarted to load the new NRO.
class UpdateService {
public:
    using MetadataFetcher = std::function<bool(
        const std::string&, size_t, std::string&, std::string&)>;
    using AssetFetcher = std::function<bool(
        const std::string&, const std::string&, size_t, std::string&)>;
    using CheckCallback = std::function<void(UpdateCheckResult)>;
    using InstallCallback = std::function<void(bool, std::string)>;

    explicit UpdateService(
        std::string targetPath = "sdmc:/switch/pipensx/pipensx.nro",
        MetadataFetcher metadataFetcher = {}, AssetFetcher assetFetcher = {});
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    UpdateCheckResult check() const;
    bool install(const ReleaseInfo& release, std::string& error) const;
    void checkAsync(CheckCallback callback);
    void installAsync(ReleaseInfo release, InstallCallback callback);

    static bool isNewerVersion(const std::string& candidate,
                               const std::string& current);
    static bool parseRelease(const std::string& json, ReleaseInfo& release,
                             std::string& error);

private:
    std::string targetPath_;
    MetadataFetcher metadataFetcher_;
    AssetFetcher assetFetcher_;
    std::vector<std::thread> workers_;
};

} // namespace pipensx
