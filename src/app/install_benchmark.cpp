#include "install_benchmark.hpp"
#include "task_files.hpp"
#include "../install/install_backend.hpp"
#include "../install/package_stream.hpp"
extern "C" {
#include "../platform/storage.h"
#include "../core/util.h"
}
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <borealis/extern/nlohmann/json.hpp>

namespace pipensx {
namespace {
constexpr size_t kBalanced = 1u << 20;
constexpr size_t kProfiles[] = {256u << 10, kBalanced, 4u << 20};
uint64_t median(std::vector<uint64_t> times) {
    if (times.size() != 3) return UINT64_MAX;
    std::sort(times.begin(), times.end());
    return times[1];
}
}

size_t chooseInstallBatch(const std::vector<InstallBenchmarkSample>& samples) {
    size_t chosen = kBalanced;
    uint64_t baseline = UINT64_MAX, fastest = UINT64_MAX;
    for (size_t bytes : kProfiles) {
        std::vector<uint64_t> times;
        for (const auto& sample : samples)
            if (sample.batchBytes == bytes && sample.durationUs) times.push_back(sample.durationUs);
        const uint64_t time = median(times);
        if (bytes == kBalanced) baseline = time;
        if (time < fastest) { fastest = time; chosen = bytes; }
    }
    if (baseline == UINT64_MAX || fastest == UINT64_MAX) return kBalanced;
    return fastest < baseline - baseline / 20 ? chosen : kBalanced;
}

bool InstallBenchmarkService::start(const std::string& taskId, std::string& error) {
    if (running_) { error = "Benchmark already active."; return false; }
    if (manager_.hasActiveTransfer()) { error = "Finish active transfers before benchmarking."; return false; }
    auto lease = manager_.beginExternalDeploy(taskId, error);
    if (!lease) return false;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskId_ = taskId; phase_ = "warming"; error_.clear();
        samples_.clear(); currentPass_ = 0; currentBatch_ = kBalanced;
    }
    cancelled_ = false;
    running_ = true;
    try {
        worker_ = std::thread([this, owned = std::move(*lease)]() mutable { run(std::move(owned)); });
    } catch (const std::exception& exception) {
        running_ = false;
        error = exception.what();
        return false;
    }
    return true;
}

void InstallBenchmarkService::stop() {
    cancelled_ = true;
    if (worker_.joinable()) worker_.join();
}

std::string InstallBenchmarkService::statusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json j = {{"running", running_.load()}, {"phase", phase_},
        {"taskId", taskId_}, {"error", error_}, {"pass", currentPass_},
        {"batchBytes", currentBatch_}, {"chosenBatchBytes", chosenBatch_},
        {"registration", false}, {"samples", nlohmann::json::array()}};
    for (const auto& sample : samples_)
        j["samples"].push_back({{"batchBytes", sample.batchBytes},
            {"durationUs", sample.durationUs}, {"sourceBytes", sample.sourceBytes},
            {"installedBytes", sample.installedBytes}});
    return j.dump();
}

void InstallBenchmarkService::run(DownloadManager::ExternalDeployLease lease) {
    std::string error;
    try {
        TaskFileInventory inventory;
        if (!buildTaskFileInventory(manager_.rootPath(), lease.task(), inventory, error))
            throw std::runtime_error(error);
        std::vector<TaskFileInfo> packages;
        for (const auto& file : inventory.files) {
            if (!file.package || file.action == TaskFileAction::Skip) continue;
            if (file.state != TaskFileState::Present || file.cartridge)
                throw std::runtime_error("Benchmark requires complete cached NSP/NSZ files.");
            packages.push_back(file);
        }
        if (packages.empty()) throw std::runtime_error("No cached packages available for benchmark.");
        const std::string runId = "benchmark-" + std::to_string(now_ms());
        // Warm-up + three interleaved repetitions. Every pass writes fresh
        // placeholders even if the same game is already installed.
        for (uint32_t pass = 0; pass < 10 && !cancelled_; ++pass) {
            const uint32_t round = pass ? (pass - 1) / 3 : 0;
            const uint32_t position = pass ? (pass - 1) % 3 : 1;
            const size_t bytes = kProfiles[round % 2 ? 2 - position : position];
            {
                std::lock_guard<std::mutex> lock(mutex_);
                currentPass_ = pass; currentBatch_ = bytes;
                phase_ = pass ? "measuring" : "warming";
            }
            InstallBenchmarkSample sample;
            sample.batchBytes = bytes;
            const uint64_t start = now_us();
            size_t number = 0;
            for (const auto& file : packages) {
                auto backend = install::createInstallBackend(manager_.rootPath());
                if (!backend->setBenchmarkMode(true)) throw std::runtime_error("Backend does not support isolated benchmarking.");
                const std::string id = runId + "-" + std::to_string(pass) + "-" + std::to_string(number++);
                if (!backend->beginPackage(id, file.logicalPath)) throw std::runtime_error(backend->error());
                install::PackageCallbacks callbacks;
                callbacks.beginFile = [&](const auto& name, uint64_t size) { return backend->beginFile(name, size); };
                callbacks.setFileSize = [&](uint64_t size) { return backend->setFileSize(size); };
                callbacks.writeFile = [&](const uint8_t* data, size_t size) { return !cancelled_ && backend->writeFile(data, size); };
                callbacks.endFile = [&] { return backend->endFile(); };
                install::PackageStream stream(file.compressed, std::move(callbacks), id);
                const auto slash = file.absolutePath.find_last_of('/');
                if (slash == std::string::npos || file.absolutePath.size() - slash > 512)
                    throw std::runtime_error("Invalid cached package path.");
                mi_file_t source{};
                const std::string name = file.absolutePath.substr(slash + 1);
                if (name.size() >= sizeof(source.path)) throw std::runtime_error("Cached package filename too long.");
                std::snprintf(source.path, sizeof(source.path), "%s", name.c_str());
                source.length = static_cast<int64_t>(file.size);
                metainfo_t meta{};
                meta.files = &source; meta.num_files = 1;
                meta.total_length = source.length; meta.piece_length = 1u << 20;
                std::snprintf(meta.name, sizeof(meta.name), "%s", name.c_str());
                std::unique_ptr<storage_t, decltype(&storage_close)> input(
                    storage_open(&meta, file.absolutePath.substr(0, slash).c_str()), storage_close);
                if (!input) throw std::runtime_error("Unable to read cached package.");
                std::vector<uint8_t> buffer(bytes);
                uint64_t offset = 0;
                while (offset < file.size && !cancelled_) {
                    const size_t count = std::min<uint64_t>(bytes, file.size - offset);
                    if (storage_read(input.get(), offset, buffer.data(), count) != static_cast<int>(count))
                        throw std::runtime_error("Cached package read failed.");
                    if (!stream.write(buffer.data(), count))
                        throw std::runtime_error(stream.error().empty() ? backend->error() : stream.error());
                    offset += count;
                }
                if (cancelled_) throw std::runtime_error("Benchmark cancelled.");
                if (!stream.finish()) throw std::runtime_error(stream.error());
                sample.sourceBytes += file.size;
                sample.installedBytes += backend->installedBytes();
                bool existing = false;
                if (!backend->commitPackage(existing)) throw std::runtime_error(backend->error());
            }
            sample.durationUs = now_us() - start;
            telemetry_log("benchmark", taskId_.c_str(),
                "event=pass pass=%u warmup=%d batch_bytes=%zu duration_us=%llu source_bytes=%llu installed_bytes=%llu registered=0",
                pass, pass == 0, bytes, (unsigned long long)sample.durationUs,
                (unsigned long long)sample.sourceBytes, (unsigned long long)sample.installedBytes);
            if (pass) { std::lock_guard<std::mutex> lock(mutex_); samples_.push_back(sample); }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_) { phase_ = "cancelled"; }
        else {
            chosenBatch_ = chooseInstallBatch(samples_);
            manager_.setInstallBatchBytes(chosenBatch_);
            phase_ = "completed";
        }
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = exception.what(); phase_ = cancelled_ ? "cancelled" : "error";
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Benchmark worker failed."; phase_ = "error";
    }
    running_ = false;
}
} // namespace pipensx
