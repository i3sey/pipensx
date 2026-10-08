#pragma once
#include "download_manager.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace pipensx {
struct InstallBenchmarkSample {
    size_t batchBytes = 0;
    uint64_t durationUs = 0, sourceBytes = 0, installedBytes = 0;
};
// Keep the balanced profile when differences are within a 5% noise band.
size_t chooseInstallBatch(const std::vector<InstallBenchmarkSample>& samples);

class InstallBenchmarkService {
public:
    explicit InstallBenchmarkService(DownloadManager& manager) : manager_(manager) {}
    ~InstallBenchmarkService() { stop(); }
    bool start(const std::string& taskId, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    std::string statusJson() const;
private:
    void run(DownloadManager::ExternalDeployLease lease);
    DownloadManager& manager_;
    std::atomic<bool> running_{false}, cancelled_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::string taskId_, phase_ = "idle", error_;
    size_t currentBatch_ = 0, chosenBatch_ = 1u << 20;
    uint32_t currentPass_ = 0;
    std::vector<InstallBenchmarkSample> samples_;
};
} // namespace pipensx
