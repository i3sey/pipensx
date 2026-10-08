#include "app/ui_thread_call.hpp"
#include <cassert>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    int calls = 0;
    std::function<void()> delayed;
    assert(!pipensx::dispatchUiCall(
        [&](std::function<void()> callback) { delayed = std::move(callback); },
        [&] { ++calls; }, 1ms));
    delayed();
    assert(calls == 0); // Cancelled work cannot apply an update later.

    std::promise<void> started;
    auto startedFuture = started.get_future().share();
    std::promise<void> release;
    auto releaseFuture = release.get_future();
    std::thread worker;
    auto result = std::async(std::launch::async, [&] {
        return pipensx::dispatchUiCall(
            [&](std::function<void()> callback) {
                worker = std::thread(std::move(callback));
                startedFuture.wait();
            },
            [&] { started.set_value(); releaseFuture.wait(); ++calls; }, 5ms);
    });
    startedFuture.wait();
    assert(result.wait_for(30ms) == std::future_status::timeout);
    release.set_value();
    assert(result.get()); // Started work returns its completed result.
    worker.join();
    assert(calls == 1);

    bool caught = false;
    try {
        pipensx::dispatchUiCall(
            [](std::function<void()> callback) { callback(); },
            [] { throw std::runtime_error("UI failure"); }, 0ms);
    } catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    std::puts("UI thread call tests passed");
}
