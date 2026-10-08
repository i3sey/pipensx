#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace pipensx {

// A timed-out queued callback must never access its caller's expired locals.
// Once execution starts, wait for completion so the result is authoritative.
template <class Dispatch>
bool dispatchUiCall(Dispatch dispatch, const std::function<void()>& fn,
                    std::chrono::milliseconds timeout) {
    struct Call {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool completed = false;
        bool cancelled = false;
        std::exception_ptr error;
    };
    const auto call = std::make_shared<Call>();
    dispatch([call, fn] {
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            if (call->cancelled) return;
            call->started = true;
        }
        std::exception_ptr error;
        try { fn(); } catch (...) { error = std::current_exception(); }
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->error = error;
            call->completed = true;
        }
        call->cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(call->mutex);
    if (!call->cv.wait_for(lock, timeout, [&] { return call->completed; })) {
        if (!call->started) {
            call->cancelled = true;
            return false;
        }
        call->cv.wait(lock, [&] { return call->completed; });
    }
    if (call->error) std::rethrow_exception(call->error);
    return true;
}

} // namespace pipensx
