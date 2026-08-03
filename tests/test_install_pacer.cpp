#include "../src/app/install_pacer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint64_t MiB = 1024 * 1024;

struct FakeClock {
    uint64_t now = 1000;
    uint64_t slept = 0;
    bool cancelOnSleep = false;
    bool cancelled = false;

    pipensx::InstallPacer::Clock callbacks() {
        return {
            [this] { return now; },
            [this](uint64_t milliseconds) {
                now += milliseconds;
                slept += milliseconds;
                if (cancelOnSleep)
                    cancelled = true;
            },
        };
    }
};

void establishTenMiBSource(pipensx::InstallPacer& pacer, FakeClock& clock) {
    pacer.observeSource(5 * MiB / 2, clock.now);
    clock.now += 500;
    pacer.observeSource(5 * MiB / 2, clock.now);
    assert(pacer.sourceRateBytesPerSecond() == 10 * MiB);
}

void testNspPacesInInstalledBytes() {
    FakeClock clock;
    pipensx::InstallPacer pacer(256 * MiB, clock.callbacks());
    establishTenMiBSource(pacer, clock);
    pacer.beginPackage(false);
    pacer.setBufferedBytes(60 * MiB);

    assert(pacer.waitForWrite(4 * MiB, [] { return false; }));
    assert(clock.slept == 200);
    assert(pacer.limitBytesPerSecond() == 10 * MiB);
    assert(pacer.state() == pipensx::InstallPacer::State::Running);
}

void testOccupancyChangesLimitAndState() {
    FakeClock clock;
    pipensx::InstallPacer pacer(256 * MiB, clock.callbacks());
    establishTenMiBSource(pacer, clock);
    pacer.beginPackage(false);
    pacer.setBufferedBytes(60 * MiB);
    assert(pacer.waitForWrite(1, [] { return false; }));

    pacer.setBufferedBytes(10 * MiB);
    assert(pacer.waitForWrite(1, [] { return false; }));
    assert(pacer.limitBytesPerSecond() >= 7 * MiB - 1);
    assert(pacer.limitBytesPerSecond() <= 7 * MiB + 1);
    assert(pacer.state() == pipensx::InstallPacer::State::Recovering);

    pacer.setBufferedBytes(120 * MiB);
    assert(pacer.waitForWrite(1, [] { return false; }));
    assert(pacer.limitBytesPerSecond() >= 15 * MiB - 1);
    assert(pacer.limitBytesPerSecond() <= 15 * MiB + 1);
    assert(pacer.state() == pipensx::InstallPacer::State::Draining);
}

void testNszExpansionRaisesSustainableRate() {
    FakeClock clock;
    pipensx::InstallPacer pacer(256 * MiB, clock.callbacks());
    establishTenMiBSource(pacer, clock);
    pacer.beginPackage(true);
    pacer.setBufferedBytes(60 * MiB);
    pacer.observeConsumed(2 * MiB, 4 * MiB, clock.now);

    assert(std::fabs(pacer.expansionRatio() - 2.0) < 0.001);
    assert(pacer.waitForWrite(1, [] { return false; }));
    assert(pacer.limitBytesPerSecond() == 20 * MiB);
}

void testCompletePackageBypassesPacing() {
    FakeClock clock;
    pipensx::InstallPacer pacer(64 * MiB, clock.callbacks());
    pacer.beginPackage(true);
    pacer.setSourceComplete(true);
    assert(pacer.waitForWrite(32 * MiB, [] { return false; }));
    assert(clock.slept == 0);
    assert(pacer.state() == pipensx::InstallPacer::State::Disabled);
}

void testSourceMeasurementFreezesDuringBackpressure() {
    FakeClock clock;
    pipensx::InstallPacer pacer(256 * MiB, clock.callbacks());
    establishTenMiBSource(pacer, clock);
    pacer.setSourceMeasurementEnabled(false, clock.now);
    clock.now += 10000;
    pacer.setSourceMeasurementEnabled(true, clock.now);
    establishTenMiBSource(pacer, clock);
    assert(pacer.sourceRateBytesPerSecond() == 10 * MiB);
}

void testPrebufferWaitIsCancellable() {
    FakeClock clock;
    clock.cancelOnSleep = true;
    pipensx::InstallPacer pacer(64 * MiB, clock.callbacks());
    pacer.beginPackage(false);
    assert(!pacer.waitForWrite(1, [&clock] { return clock.cancelled; }));
    assert(clock.slept == 50);
}

} // namespace

int main() {
    testNspPacesInInstalledBytes();
    testOccupancyChangesLimitAndState();
    testNszExpansionRaisesSustainableRate();
    testCompletePackageBypassesPacing();
    testSourceMeasurementFreezesDuringBackpressure();
    testPrebufferWaitIsCancellable();
    std::printf("test_install_pacer: all tests passed\n");
    return 0;
}
