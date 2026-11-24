/// What the HUB75 engine tests share: waits on a stub backend's submits and the walls the PIO and GPIO engines' tests open them with.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "platform/Hub75Gpio.h"
#include "platform/Hub75Pio.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_engine.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_pio_engine.h"
#include "platform/rpi/hub75_pio_pins.h"

namespace mm::test {

/// Wait up to `ms` for the stub to have submitted a frame whose word `index` has `bit` set, or all of `expect` when `bit` is -1; `copyLast` copies the stub's last frame out and returns how many words it holds.
template <class CopyLastFrame>
bool waitForWord(CopyLastFrame copyLast, size_t index, int bit, uint32_t expect, int ms = 2000) {
    std::vector<uint32_t> frame(index + 1);   // the word waited on is the last one the copy needs
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (copyLast(frame.data(), frame.size()) > index) {
            if (bit >= 0 ? (frame[index] & (1u << bit)) != 0 : frame[index] == expect) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// Wait up to `ms` for `done()` to hold, and say whether it did.
template <class Done>
bool waitUntil(Done done, int ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return done();
}

/// Wait up to two seconds for the engine to have submitted its first frame, and say whether it did.
template <class Engine>
bool waitFirstSubmit(const Engine& engine) {
    return waitUntil([&] { return engine.framesSubmitted() > 0; });
}

/// Lets a hung submit go and waits for `engine` to finish shutting down, so a refresh thread stuck in the stub is back before its engine is destroyed.
template <class Engine>
void releaseHang(Engine& engine) {
    mm::platform::stubBackendSetFaults({});
    waitUntil([&] { return engine.shutdown(); });
}

/// Releases the hang of `engine` when it goes out of scope, however the case ends: a case that fails while the engine is wedged then lets the thread go before the engine is destroyed, which would read freed buffers.
template <class Engine>
struct HangRelease {
    Engine& engine;   ///< the engine whose refresh thread may be hung
    /// Guard `e`, declared after it so the guard goes first.
    explicit HangRelease(Engine& e) : engine(e) {}
    /// Let the hang go and wait for the shutdown.
    ~HangRelease() { releaseHang(engine); }
    /// A guard releases once, so it is not copyable.
    HangRelease(const HangRelease&) = delete;
    /// A guard releases once, so it is not assignable.
    HangRelease& operator=(const HangRelease&) = delete;
};

/// The PIO engine opened on the stub backend with its records and faults cleared and its queue model set, shut down however the case ends.
struct PioRun {
    mm::platform::Hub75PioEngine engine;   ///< the engine under test
    std::string error;                     ///< what a refused open said
    /// Clear the stub, set the queue model `model` and open the engine on `o`; the open must succeed.
    explicit PioRun(const mm::platform::Hub75PioOptions& o,
                    mm::platform::StubQueueModel model = mm::platform::StubQueueModel::Blocking) {
        mm::platform::stubBackendReset();
        mm::platform::stubBackendSetQueueModel(model);
        REQUIRE(engine.open(o, error, true));
    }
    /// Release a hung submit and stop the engine, which a second shutdown leaves as it is, and clear the stub's faults.
    ~PioRun() {
        releaseHang(engine);
        mm::platform::stubBackendReset();
    }
};

/// The GPIO engine opened on the stub backend with its recorders cleared, shut down however the case ends.
struct GpioRun {
    mm::platform::Hub75GpioEngine engine;   ///< the engine under test
    std::string error;                      ///< what a refused open said
    /// Clear the stub's recorders and open the engine on `o`; the open must succeed.
    explicit GpioRun(const mm::platform::Hub75GpioOptions& o) {
        mm::platform::gpioStubResetCapture();
        REQUIRE(engine.open(o, error, true));
    }
    /// Stop the engine; a second shutdown is a no-op.
    ~GpioRun() { engine.shutdown(); }
};

/// Call `body` once under each queue model the stub has, with the model's name captured for the report.
template <class Body>
void forBothQueueModels(Body body) {
    for (const mm::platform::StubQueueModel model : {mm::platform::StubQueueModel::Blocking, mm::platform::StubQueueModel::Ring}) {
        CAPTURE(model == mm::platform::StubQueueModel::Blocking ? "blocking" : "ring");
        body(model);
    }
}

/// The checksums of the frames the stub was handed, collected over polls: the stub keeps the last 64 identities, so a poll shorter than 64 frames misses none.
struct IdWatcher {
    std::set<uint64_t> checksums;   ///< every distinct frame checksum seen
    uint64_t newestIndex = 0;       ///< the highest frame index seen
    /// Read the stub's identities and keep what they show.
    void poll() {
        mm::platform::StubFrameId ids[mm::platform::kStubFrameIdRing];
        const size_t n = mm::platform::stubBackendFrameIds(ids, mm::platform::kStubFrameIdRing);
        for (size_t i = 0; i < n; i++) {
            checksums.insert(ids[i].checksum);
            if (ids[i].index > newestIndex) newestIndex = ids[i].index;
        }
    }
};

/// The checksum of the newest frame the stub was handed, 0 before the first.
inline uint64_t newestChecksum() {
    mm::platform::StubFrameId id{};
    return mm::platform::stubBackendFrameIds(&id, 1) == 1 ? id.checksum : 0;
}

/// Publish `frame`, wait for the engine to encode it and the stub to take three frames after that, and return the checksum the frame settles at.
template <class Engine>
uint64_t settledChecksum(Engine& engine, const std::vector<uint8_t>& frame) {
    const uint64_t encoded = engine.framesEncoded();
    engine.publishRgb(frame.data());
    waitUntil([&] { return engine.framesEncoded() > encoded; });
    const uint64_t submitted = mm::platform::stubBackendSubmittedFrames();
    waitUntil([&] { return mm::platform::stubBackendSubmittedFrames() >= submitted + 3; });
    return newestChecksum();
}

/// The words of the stub's last frame with the OE pin clear, which are the lit ones.
inline size_t litWords(const std::vector<uint32_t>& frame) {
    size_t lit = 0;
    for (const uint32_t w : frame) lit += (w & (1u << mm::platform::kWideBitOe)) == 0;
    return lit;
}

/// What a one-panel 64x32 engine opened on the stub reads back: running, on the stub, one lane, 2,048 slots a frame.
template <class Engine>
void checkSmallEngine(const Engine& engine) {
    CHECK(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "stub (no hardware)");
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.lanes() == 1);
    CHECK(engine.frameSlots() == 2048);
}

/// Publish one black frame to the small engine and wait for its threads: submitted and encoded, one frame published, no failed submit.
template <class Engine>
void publishBlackFrame(Engine& engine) {
    const std::vector<uint8_t> frame(64 * 32 * 3, 0);
    engine.publishRgb(frame.data());
    CHECK(waitFirstSubmit(engine));
    CHECK(waitUntil([&] { return engine.framesEncoded() > 0; }));   // the encode thread encodes the published frame beside the refresh thread
    CHECK(engine.framesPublished() == 1);
    CHECK(engine.submitErrors() == 0);
}

/// Call `check` with the slots of every color combination on each of the three lanes, the other lanes dark.
template <class Check>
void forEveryLaneColor(Check check) {
    for (int lane = 0; lane < 3; lane++) {
        for (uint16_t v = 0; v < 64; v++) {
            uint16_t slot[3] = {0, 0, 0};
            slot[lane] = v;
            check(slot);
        }
    }
}

/// Every submit from now on hangs, and the refresh thread has entered one: it returns once a test clears the fault.
inline void hangSubmits() {
    mm::platform::StubFaults faults;
    faults.hangSubmits = true;
    mm::platform::stubBackendSetFaults(faults);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

/// One 32x64 panel at 1/16 scan, 2-bit, a 10 MHz clock: 2 planes x 16 rows x 64 columns, 2,048 slots a frame.
inline mm::platform::Hub75PioOptions pioSmall() {
    mm::platform::Hub75PioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pixelClockHz = 10000000;
    return o;
}

/// One 32x64 panel at 1/16 scan, 2-bit: 2 planes x 16 rows x 64 columns, 2,048 slots a frame.
inline mm::platform::Hub75GpioOptions gpioSmall() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pwmLsbNs = 130;
    o.slowdown = -1;   // auto: the stub resolves it to 1
    return o;
}

/// The sign's options (BENCH-V6): 3 parallel lanes of one 32x64 panel, Stripe, 6-bit, 160 ns LSB, true slowdown 2, a 64x96 frame of 6,144 words; the calibration is in hub75_gpio_backend.h (Calibration).
inline mm::platform::Hub75GpioOptions gpioSignOptions() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 3;
    o.scanRate = 8;
    o.multiplexing = mm::platform::kHub75MultiplexStripe;
    o.bitDepth = 6;
    o.pwmLsbNs = 160;
    o.slowdown = 2;
    return o;
}

}  // namespace mm::test
