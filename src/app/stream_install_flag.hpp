#pragma once

#include <atomic>

namespace pipensx {

inline std::atomic<int> gStreamInstallWorkers{0};

inline bool streamInstallActive() {
    return gStreamInstallWorkers.load(std::memory_order_acquire) > 0;
}

struct StreamInstallWorkerGuard {
    StreamInstallWorkerGuard() {
        gStreamInstallWorkers.fetch_add(1, std::memory_order_acq_rel);
    }
    ~StreamInstallWorkerGuard() {
        gStreamInstallWorkers.fetch_sub(1, std::memory_order_acq_rel);
    }
};

}  // namespace pipensx
