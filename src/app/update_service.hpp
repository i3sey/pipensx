#pragma once

#include <cstdint>
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

// Retrieves published releases from the canonical GitHub repository. A
// verified release is staged beside the active NRO, then a chain-loaded copy
// finalizes the replacement after the original process has exited.
class UpdateService {
public:
    using MetadataFetcher = std::function<bool(
        const std::string&, size_t, std::string&, std::string&)>;
    using AssetFetcher = std::function<bool(
        const std::string&, const std::string&, size_t, std::string&)>;
    using CheckCallback = std::function<void(UpdateCheckResult)>;
    using InstallCallback = std::function<void(bool, std::string)>;
    using ProgressCallback = std::function<void(uint64_t, uint64_t)>;

    explicit UpdateService(
        std::string targetPath = "sdmc:/switch/pipensx/pipensx.nro",
        MetadataFetcher metadataFetcher = {}, AssetFetcher assetFetcher = {});
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    UpdateCheckResult check() const;
    bool install(const ReleaseInfo& release, std::string& error) const;
    bool finalizeStaged(std::string& error) const;
    bool stagedReady() const;
    void discardStaged() const;
    bool isStagedLaunch(const std::vector<std::string>& arguments) const;
    const std::string& targetPath() const { return targetPath_; }
    std::string stagedPath() const { return targetPath_ + ".update"; }
    std::string helperPath() const { return targetPath_ + ".updater"; }
    void checkAsync(CheckCallback callback);
    void installAsync(ReleaseInfo release, InstallCallback callback);
    // Reports (received, total) from the built-in NRO downloader on a worker
    // thread. Set it before installAsync; the callback must marshal to the UI
    // thread itself.
    void onInstallProgress(ProgressCallback callback) {
        progress_ = std::move(callback);
    }

    static bool isNewerVersion(const std::string& candidate,
                               const std::string& current);
    static bool parseRelease(const std::string& json, ReleaseInfo& release,
                             std::string& error);

private:
    std::string targetPath_;
    MetadataFetcher metadataFetcher_;
    AssetFetcher assetFetcher_;
    ProgressCallback progress_;
    std::vector<std::thread> workers_;
};

} // namespace pipensx
