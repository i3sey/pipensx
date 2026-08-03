#include "install_pacer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace pipensx {
namespace {

uint64_t systemNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void systemSleepMs(uint64_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

uint64_t scaledRate(uint64_t rate, double multiplier) {
    const long double result = static_cast<long double>(rate) * multiplier;
    return result >= std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(result);
}

} // namespace

InstallPacer::InstallPacer(size_t maximumBufferedBytes, Clock clock)
    : clock_(std::move(clock)), maximumBufferedBytes_(maximumBufferedBytes) {
    if (!clock_.nowMs)
        clock_.nowMs = systemNowMs;
    if (!clock_.sleepMs)
        clock_.sleepMs = systemSleepMs;
}

void InstallPacer::setMaximumBufferedBytes(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maximumBufferedBytes_ = bytes;
}

void InstallPacer::beginPackage(bool compressed) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now = clock_.nowMs();
    packageActive_ = true;
    compressed_ = compressed;
    ratioReady_ = !compressed;
    sourceComplete_ = false;
    expansionRatio_ = 1.0;
    ratioBaseConsumed_ = 0;
    ratioBaseInstalled_ = 0;
    ratioSampleConsumed_ = 0;
    ratioSampleInstalled_ = 0;
    prebufferStartedMs_ = now;
    tokenUpdatedMs_ = now;
    tokens_ = 0.0;
    tokensInitialized_ = false;
    limitBps_ = 0;
    state_ = State::Prebuffering;
}

void InstallPacer::observeSource(uint64_t bytes, uint64_t nowMs) {
    if (!bytes)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sourceMeasurementEnabled_)
        return;
    if (lastSourceMs_ && nowMs > lastSourceMs_ &&
        nowMs - lastSourceMs_ > 15000) {
        sourceRateBps_ = 0;
        sourceWindowBytes_ = 0;
        sourceWindowStartedMs_ = 0;
    }
    lastSourceMs_ = nowMs;
    if (!sourceWindowStartedMs_) {
        sourceWindowStartedMs_ = nowMs;
        sourceWindowBytes_ = bytes;
        return;
    }
    sourceWindowBytes_ += bytes;
    if (nowMs <= sourceWindowStartedMs_ ||
        nowMs - sourceWindowStartedMs_ < kSourceSampleMs)
        return;
    const uint64_t sample = sourceWindowBytes_ * 1000 /
                            (nowMs - sourceWindowStartedMs_);
    if (sample) {
        sourceRateBps_ = sourceRateBps_
            ? (sourceRateBps_ * 4 + sample) / 5
            : sample;
    }
    sourceWindowBytes_ = 0;
    sourceWindowStartedMs_ = nowMs;
    if (packageActive_ && state_ != State::Prebuffering)
        updateControlLocked(nowMs);
}

void InstallPacer::setSourceMeasurementEnabled(bool enabled,
                                                uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sourceMeasurementEnabled_ == enabled)
        return;
    sourceMeasurementEnabled_ = enabled;
    sourceWindowBytes_ = 0;
    sourceWindowStartedMs_ = 0;
    if (enabled)
        lastSourceMs_ = nowMs;
}

void InstallPacer::observeConsumed(uint64_t sourceConsumed,
                                   uint64_t installedBytes,
                                   uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!packageActive_)
        return;
    if (sourceConsumed < ratioBaseConsumed_ ||
        installedBytes < ratioBaseInstalled_) {
        ratioBaseConsumed_ = sourceConsumed;
        ratioBaseInstalled_ = installedBytes;
        ratioSampleConsumed_ = 0;
        ratioSampleInstalled_ = 0;
        return;
    }
    ratioSampleConsumed_ += sourceConsumed - ratioBaseConsumed_;
    ratioSampleInstalled_ += installedBytes - ratioBaseInstalled_;
    ratioBaseConsumed_ = sourceConsumed;
    ratioBaseInstalled_ = installedBytes;
    if (!compressed_) {
        expansionRatio_ = 1.0;
        ratioReady_ = true;
    } else if (ratioSampleConsumed_ >= kRatioSampleBytes ||
               (sourceComplete_ && ratioSampleConsumed_ > 0)) {
        const double sample = static_cast<double>(ratioSampleInstalled_) /
                              static_cast<double>(ratioSampleConsumed_);
        if (sample > 0.0) {
            expansionRatio_ = ratioReady_
                ? expansionRatio_ * 0.8 + sample * 0.2
                : sample;
            ratioReady_ = true;
        }
        ratioSampleConsumed_ = 0;
        ratioSampleInstalled_ = 0;
    }
    updateControlLocked(nowMs);
}

void InstallPacer::setBufferedBytes(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    bufferedBytes_ = bytes;
    if (packageActive_ && state_ != State::Prebuffering)
        updateControlLocked(clock_.nowMs());
}

void InstallPacer::setSourceComplete(bool complete) {
    std::lock_guard<std::mutex> lock(mutex_);
    sourceComplete_ = complete;
    if (complete && packageActive_)
        state_ = State::Disabled;
}

uint64_t InstallPacer::targetLocked() const {
    if (!sourceRateBps_ || !maximumBufferedBytes_)
        return 0;
    const uint64_t desired = scaledRate(sourceRateBps_, 6.0);
    const uint64_t maximumTarget = maximumBufferedBytes_ / 5 * 3;
    const uint64_t minimumTarget = std::min<uint64_t>(8 * kMiB,
                                                       maximumTarget);
    return std::clamp(desired, minimumTarget, maximumTarget);
}

void InstallPacer::refillLocked(uint64_t nowMs) {
    if (!tokenUpdatedMs_) {
        tokenUpdatedMs_ = nowMs;
        return;
    }
    if (nowMs <= tokenUpdatedMs_)
        return;
    const double added = static_cast<double>(limitBps_) *
                         static_cast<double>(nowMs - tokenUpdatedMs_) / 1000.0;
    tokens_ += added;
    const double capacity = static_cast<double>(limitBps_) * 0.2;
    if (tokens_ > capacity)
        tokens_ = capacity;
    tokenUpdatedMs_ = nowMs;
}

void InstallPacer::updateControlLocked(uint64_t nowMs) {
    refillLocked(nowMs);
    if (!packageActive_ || sourceComplete_) {
        state_ = State::Disabled;
        limitBps_ = 0;
        return;
    }
    const uint64_t target = targetLocked();
    if (!target || !ratioReady_) {
        limitBps_ = 0;
        return;
    }
    if (!bufferedBytes_) {
        limitBps_ = 0;
        state_ = State::Recovering;
        return;
    }
    const double occupancyError =
        (static_cast<double>(bufferedBytes_) - static_cast<double>(target)) /
        static_cast<double>(target);
    const double multiplier = std::clamp(1.0 + occupancyError * 0.5,
                                         0.70, 1.50);
    limitBps_ = scaledRate(scaledRate(sourceRateBps_, expansionRatio_),
                           multiplier);
    if (tokensInitialized_)
        tokens_ = std::min(tokens_, static_cast<double>(limitBps_) * 0.2);

    const uint64_t low = target / 3;
    const uint64_t high = target / 3 * 5;
    if (bufferedBytes_ < low)
        state_ = State::Recovering;
    else if (bufferedBytes_ > high)
        state_ = State::Draining;
    else
        state_ = State::Running;
}

bool InstallPacer::waitForWrite(
    size_t bytes, const std::function<bool()>& cancelled) {
    if (!bytes)
        return true;
    bool charged = false;
    while (true) {
        if (cancelled && cancelled())
            return false;
        uint64_t sleepMs = kWaitSliceMs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t now = clock_.nowMs();
            if (!packageActive_ || sourceComplete_ ||
                state_ == State::Disabled)
                return true;

            const uint64_t target = targetLocked();
            if (state_ == State::Prebuffering) {
                const bool timedOut = now >= prebufferStartedMs_ &&
                    now - prebufferStartedMs_ >= kPrebufferTimeoutMs;
                if (!timedOut && (!target || bufferedBytes_ < target)) {
                    sleepMs = kWaitSliceMs;
                } else {
                    state_ = State::Running;
                    updateControlLocked(now);
                    continue;
                }
            } else if (!ratioReady_) {
                // The first compressed sample is deliberately unpaced.
                return true;
            } else {
                updateControlLocked(now);
                if (!limitBps_) {
                    if (!bufferedBytes_)
                        sleepMs = kWaitSliceMs;
                    else
                        return true;
                } else {
                    if (!charged) {
                        if (!tokensInitialized_) {
                            tokens_ = static_cast<double>(limitBps_) * 0.2;
                            tokensInitialized_ = true;
                        }
                        tokens_ -= static_cast<double>(bytes);
                        charged = true;
                    }
                    if (tokens_ >= 0.0)
                        return true;
                    const uint64_t deficitMs = static_cast<uint64_t>(std::ceil(
                        -tokens_ * 1000.0 / static_cast<double>(limitBps_)));
                    sleepMs = std::max<uint64_t>(1,
                        std::min<uint64_t>(kWaitSliceMs, deficitMs));
                }
            }
        }
        clock_.sleepMs(sleepMs);
    }
}

void InstallPacer::endPackage() {
    std::lock_guard<std::mutex> lock(mutex_);
    packageActive_ = false;
    sourceComplete_ = false;
    limitBps_ = 0;
    tokens_ = 0.0;
    tokensInitialized_ = false;
    state_ = State::Disabled;
}

uint64_t InstallPacer::sourceRateBytesPerSecond() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sourceRateBps_;
}

uint64_t InstallPacer::limitBytesPerSecond() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return limitBps_;
}

uint64_t InstallPacer::targetBufferedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return targetLocked();
}

double InstallPacer::expansionRatio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expansionRatio_;
}

InstallPacer::State InstallPacer::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

} // namespace pipensx
