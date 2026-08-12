#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace pipensx {

class InstallPacer {
public:
    enum class State { Prebuffering, Running, Recovering, Draining, Disabled };

    struct Clock {
        std::function<uint64_t()> nowMs;
        std::function<void(uint64_t)> sleepMs;
    };

    explicit InstallPacer(size_t maximumBufferedBytes,
                          Clock clock = {});

    void setMaximumBufferedBytes(size_t bytes);
    void beginPackage(bool compressed);
    void observeSource(uint64_t bytes, uint64_t nowMs);
    void setSourceMeasurementEnabled(bool enabled, uint64_t nowMs);
    void observeConsumed(uint64_t sourceConsumed,
                         uint64_t installedBytes,
                         uint64_t nowMs);
    void setBufferedBytes(uint64_t bytes);
    void setSourceComplete(bool complete);
    bool waitForWrite(size_t bytes,
                      const std::function<bool()>& cancelled);
    void endPackage();

    uint64_t sourceRateBytesPerSecond() const;
    uint64_t limitBytesPerSecond() const;
    uint64_t targetBufferedBytes() const;
    double expansionRatio() const;
    State state() const;

private:
    void refillLocked(uint64_t nowMs);
    void updateControlLocked(uint64_t nowMs);
    uint64_t targetLocked() const;

    static constexpr uint64_t kMiB = 1024 * 1024;
    static constexpr uint64_t kSourceSampleMs = 500;
    static constexpr uint64_t kRatioSampleBytes = 1 * kMiB;
    static constexpr uint64_t kPrebufferTimeoutMs = 10000;
    static constexpr uint64_t kWaitSliceMs = 50;

    Clock clock_;
    mutable std::mutex mutex_;
    size_t maximumBufferedBytes_ = 0;
    uint64_t bufferedBytes_ = 0;
    uint64_t sourceRateBps_ = 0;
    uint64_t sourceWindowBytes_ = 0;
    uint64_t sourceWindowStartedMs_ = 0;
    uint64_t lastSourceMs_ = 0;
    uint64_t ratioBaseConsumed_ = 0;
    uint64_t ratioBaseInstalled_ = 0;
    uint64_t ratioSampleConsumed_ = 0;
    uint64_t ratioSampleInstalled_ = 0;
    uint64_t prebufferStartedMs_ = 0;
    uint64_t tokenUpdatedMs_ = 0;
    uint64_t limitBps_ = 0;
    double expansionRatio_ = 1.0;
    double tokens_ = 0.0;
    bool packageActive_ = false;
    bool sourceMeasurementEnabled_ = true;
    bool compressed_ = false;
    bool ratioReady_ = true;
    bool tokensInitialized_ = false;
    bool sourceComplete_ = false;
    State state_ = State::Disabled;
};

} // namespace pipensx
