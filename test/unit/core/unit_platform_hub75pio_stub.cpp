/// @module platform

/// Pins the stub PIO backend the engine tests stand on: its fault hooks, its records, and the two queue models that pace a submit, the blocking one of a 6.12 kernel and the ring of a kernel that queues. The stub is driven directly here, with no engine, so a timing case reads the backend's own wait.
///
/// The timing cases use a slow pixel clock, so a 2,048-word frame takes 20 ms on the wire at 100 kHz and 200 ms at 10 kHz. A lower bound, a wait that lasted at least a frame, is one the stub guarantees, since a sleep lasts at least its duration, and those cases use 100 kHz.
///
/// An upper bound, no wait at all, depends on the host keeping up, so those cases use 10 kHz and leave a margin of a whole frame, 200 ms, to a loaded sanitizer runner.

#include "doctest.h"
#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_pio_pins.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using mm::platform::StubFaults;
using mm::platform::StubQueueModel;

// 100 kHz: a divider of 400 (200 MHz / (5 x 100 kHz)).
constexpr uint16_t kSlowDivider = 400;
constexpr uint32_t kFrameWords = 2048;
constexpr uint32_t kFrameBytes = kFrameWords * 4;
constexpr auto kFrameWire = std::chrono::milliseconds(20);   // 2,048 words x 10 us
// 10 kHz: a divider of 4000, and a frame of 204.8 ms, for the cases that bound a wait from above.
constexpr uint16_t kSlowerDivider = 4000;
constexpr auto kSlowerWire = std::chrono::milliseconds(200);

// A started stub backend on the slow clock, its ring `count` buffers of `bufBytes`, with the stub's state cleared first and after.
struct Started {
    std::unique_ptr<mm::platform::PioBackend> backend;
    std::string error;
    std::vector<uint16_t> program{1, 2, 3};
    std::vector<uint32_t> frame = std::vector<uint32_t>(kFrameWords, 0);

    Started(uint32_t bufBytes, uint32_t count, uint16_t divider = kSlowDivider) {
        mm::platform::stubBackendReset();
        backend.reset(mm::platform::createStubBackend());
        REQUIRE(backend->open(error));
        mm::platform::PioStartConfig config;
        config.programInstructions = program.data();
        config.programLength = static_cast<int>(program.size());
        config.wrap = 2;
        config.clkdivInt = divider;
        config.xferBufSize = bufBytes;
        config.xferBufCount = count;
        REQUIRE(backend->start(config, error));
    }
    ~Started() {
        backend->close();
        mm::platform::stubBackendReset();
    }
    int submit() { return backend->submitFrame(frame.data(), kFrameBytes); }
};

// The time `n` submits take.
std::chrono::nanoseconds submitsTake(Started& s, int n) {
    const auto t0 = Clock::now();
    for (int i = 0; i < n; i++) CHECK(s.submit() == 0);
    return Clock::now() - t0;
}

}  // namespace

TEST_CASE("Hub75PioStub: the blocking model returns after the frame's whole wire time") {
    Started s(kFrameBytes, 4);
    CHECK(submitsTake(s, 3) >= 3 * kFrameWire);
}

TEST_CASE("Hub75PioStub: the ring queues up to its depth without waiting, and the next submit waits for the first chunk") {
    Started s(kFrameBytes, 4, kSlowerDivider);   // one chunk a frame, four in flight
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    const auto t0 = Clock::now();
    for (int i = 0; i < 4; i++) CHECK(s.submit() == 0);
    CHECK(Clock::now() - t0 < kSlowerWire);    // four queued, none waited for
    CHECK(s.submit() == 0);                    // the fifth finds the ring full
    CHECK(Clock::now() - t0 >= kSlowerWire);   // and waits for the first to complete
}

TEST_CASE("Hub75PioStub: a frame splits into chunks of the buffer size, each taking a place in the ring") {
    {
        Started s(kFrameBytes, 2, kSlowerDivider);   // a frame is one chunk, two in flight
        mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
        const auto t0 = Clock::now();
        CHECK(s.submit() == 0);
        CHECK(s.submit() == 0);
        CHECK(Clock::now() - t0 < kSlowerWire / 2);
    }
    {
        Started s(kFrameBytes / 2, 2, kSlowerDivider);   // a frame is two chunks: one frame fills the ring
        mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
        const auto t0 = Clock::now();
        CHECK(s.submit() == 0);
        CHECK(Clock::now() - t0 < kSlowerWire / 2);
        CHECK(s.submit() == 0);   // the second frame waits for the first frame's first chunk, half a frame
        CHECK(Clock::now() - t0 >= kSlowerWire / 2);
    }
}

TEST_CASE("Hub75PioStub: the queue model can be switched back to blocking") {
    Started s(kFrameBytes, 4, kSlowerDivider);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    CHECK(submitsTake(s, 2) < kSlowerWire);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Blocking);
    CHECK(submitsTake(s, 2) >= 2 * kSlowerWire);
}

TEST_CASE("Hub75PioStub: on the ring a stop with drain waits for the queue to empty, and the stub then refuses submits") {
    Started s(kFrameBytes, 4);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    const auto t0 = Clock::now();
    for (int i = 0; i < 3; i++) CHECK(s.submit() == 0);
    std::vector<uint32_t> blank(kFrameWords, 0xFFFF);
    CHECK(s.backend->submitBlank(blank.data(), kFrameBytes) == 0);
    CHECK(s.backend->stop(true, s.error));
    CHECK(Clock::now() - t0 >= 4 * kFrameWire);   // three frames and the blank, every chunk out
    CHECK(mm::platform::stubBackendPinsDark());
    CHECK(s.submit() == -ENODEV);                 // stopped
}

TEST_CASE("Hub75PioStub: a stop with drain clear runs while a submit hangs, darkens the pins and refuses the submits after it") {
    Started s(kFrameBytes, 4);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    StubFaults faults;
    faults.hangSubmits = true;
    mm::platform::stubBackendSetFaults(faults);
    std::atomic<int> rc{-1};
    std::thread stuck([&] { rc = s.submit(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the submit is inside the hang
    CHECK_FALSE(mm::platform::stubBackendPinsDark());
    const auto t0 = Clock::now();
    CHECK(s.backend->stop(false, s.error));
    CHECK(Clock::now() - t0 < kFrameWire);   // no wait for the queue
    CHECK(mm::platform::stubBackendPinsDark());
    mm::platform::stubBackendSetFaults({});   // the kernel lets the submit go
    stuck.join();
    CHECK(rc == 0);                           // the submit that was inside returns as it would
    CHECK(s.submit() == -ENODEV);             // and a later one is refused
    CHECK(mm::platform::stubBackendSubmitsAfterStop() == 1);
    CHECK(s.backend->stop(true, s.error));    // a second stop is a no-op
    CHECK(mm::platform::stubBackendRealChunksInFlightAtStop() == 0);
}

TEST_CASE("Hub75PioStub: a stop records whether it drained, a second stop is not counted, and a start clears the dark word") {
    Started s(kFrameBytes, 4);
    CHECK(mm::platform::stubBackendStops() == 0);
    CHECK_FALSE(mm::platform::stubBackendLastStopDrained());
    CHECK(s.backend->stop(true, s.error));
    CHECK(mm::platform::stubBackendStops() == 1);
    CHECK(mm::platform::stubBackendLastStopDrained());
    CHECK(mm::platform::stubBackendPinsDark());
    CHECK(s.backend->stop(false, s.error));            // finds the stub stopped: a no-op
    CHECK(mm::platform::stubBackendStops() == 1);
    CHECK(mm::platform::stubBackendLastStopDrained());   // the no-op did not overwrite what the real stop recorded

    mm::platform::PioStartConfig config;
    config.clkdivInt = kSlowDivider;
    config.xferBufSize = kFrameBytes;
    config.xferBufCount = 4;
    REQUIRE(s.backend->start(config, s.error));
    CHECK_FALSE(mm::platform::stubBackendPinsDark());   // the started SM drives its pins again
    CHECK(s.backend->stop(false, s.error));
    CHECK(mm::platform::stubBackendStops() == 2);
    CHECK_FALSE(mm::platform::stubBackendLastStopDrained());
    CHECK(mm::platform::stubBackendPinsDark());
}

TEST_CASE("Hub75PioStub: a stop that fails reports it, leaves the pins as they were and still counts") {
    Started s(kFrameBytes, 4);
    StubFaults faults;
    faults.failStop = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(s.backend->stop(false, s.error));
    CHECK(s.error.find("stop failed") != std::string::npos);
    CHECK_FALSE(mm::platform::stubBackendPinsDark());
    CHECK(mm::platform::stubBackendStops() == 1);
    CHECK(s.submit() == -ENODEV);   // the stub is stopped all the same
}

TEST_CASE("Hub75PioStub: a stop that failed runs again on the next stop, darkens the pins when it works and is counted once") {
    Started s(kFrameBytes, 4);
    StubFaults faults;
    faults.failStop = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(s.backend->stop(false, s.error));
    CHECK_FALSE(s.backend->stop(true, s.error));   // the fault is still set: the retry fails too, and says so
    CHECK(s.error.find("stop failed") != std::string::npos);
    CHECK_FALSE(mm::platform::stubBackendPinsDark());
    mm::platform::stubBackendSetFaults({});
    s.error.clear();
    CHECK(s.backend->stop(true, s.error));   // the retry works
    CHECK(s.error.empty());
    s.error = "an earlier stop's text";
    CHECK(s.backend->stop(true, s.error));   // dark now: a no-op that still returns the string cleared
    CHECK(s.error.empty());
    CHECK(mm::platform::stubBackendPinsDark());
    CHECK(mm::platform::stubBackendStops() == 1);                   // one stop of a started stub, however many tries
    CHECK_FALSE(mm::platform::stubBackendLastStopDrained());        // the record is the first stop's
}

TEST_CASE("Hub75PioStub: a FIFO that times out on a stop with drain is a note on a stop that darkens the pins, and a stop with drain clear has none") {
    StubFaults faults;
    faults.drainTimesOut = true;
    {
        Started s(kFrameBytes, 4);
        mm::platform::stubBackendSetFaults(faults);
        CHECK(s.backend->stop(true, s.error));   // the pins are dark: the stop worked
        CHECK(s.error.find("did not drain") != std::string::npos);
        CHECK(mm::platform::stubBackendPinsDark());
    }
    {
        Started s(kFrameBytes, 4);
        mm::platform::stubBackendSetFaults(faults);
        CHECK(s.backend->stop(false, s.error));   // no wait, so no timeout
        CHECK(s.error.empty());
    }
    {
        Started s(kFrameBytes, 4);
        faults.failStop = true;
        mm::platform::stubBackendSetFaults(faults);
        CHECK_FALSE(s.backend->stop(true, s.error));   // a failed stop names both the note and the failure
        CHECK(s.error.find("did not drain") != std::string::npos);
        CHECK(s.error.find("stop failed") != std::string::npos);
        CHECK_FALSE(mm::platform::stubBackendPinsDark());
    }
}

TEST_CASE("Hub75PioStub: the faults refuse the open, the start and the next submits, and a reset clears them") {
    mm::platform::stubBackendReset();
    std::unique_ptr<mm::platform::PioBackend> backend(mm::platform::createStubBackend());
    std::string error;
    StubFaults faults;
    faults.failOpen = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(backend->open(error));
    CHECK(error.find("open") != std::string::npos);

    faults = {};
    faults.failStart = true;
    mm::platform::stubBackendSetFaults(faults);
    REQUIRE(backend->open(error));
    mm::platform::PioStartConfig config;
    config.clkdivInt = 1;
    config.xferBufSize = 64;
    config.xferBufCount = 4;
    CHECK_FALSE(backend->start(config, error));
    CHECK(mm::platform::stubBackendLastStart().starts == 0);   // a refused start is no start

    mm::platform::stubBackendSetFaults({});
    REQUIRE(backend->start(config, error));
    std::vector<uint32_t> frame(16, 0);
    faults = {};
    faults.failSubmits = 2;
    mm::platform::stubBackendSetFaults(faults);
    CHECK(backend->submitFrame(frame.data(), 64) == -EIO);
    CHECK(backend->submitFrame(frame.data(), 64) == -EIO);
    CHECK(backend->submitFrame(frame.data(), 64) == 0);   // the third is the first past the count
    CHECK(mm::platform::stubBackendSubmittedFrames() == 1);   // a failed submit is no frame

    faults.failSubmits = -1;
    mm::platform::stubBackendSetFaults(faults);
    for (int i = 0; i < 5; i++) CHECK(backend->submitFrame(frame.data(), 64) == -EIO);
    faults.submitError = -EINTR;   // a test sets the code a signal would leave
    mm::platform::stubBackendSetFaults(faults);
    CHECK(backend->submitFrame(frame.data(), 64) == -EINTR);
    faults.submitError = -ETIMEDOUT;   // what a 6.12 kernel's 1 s wait hands back through the patched piolib
    mm::platform::stubBackendSetFaults(faults);
    CHECK(backend->submitFrame(frame.data(), 64) == -ETIMEDOUT);
    mm::platform::stubBackendReset();
    CHECK(backend->submitFrame(frame.data(), 64) == 0);
    backend->close();
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioStub: the start record keeps the configuration and a copy of the program") {
    Started s(8192, 4);
    const mm::platform::StubStartRecord start = mm::platform::stubBackendLastStart();
    CHECK(start.starts == 1);
    CHECK(start.config.clkdivInt == kSlowDivider);
    CHECK(start.config.xferBufSize == 8192);
    CHECK(start.config.xferBufCount == 4);
    CHECK(start.config.programLength == 3);
    CHECK(start.program == std::vector<uint16_t>{1, 2, 3});
    s.program.assign(3, 0);   // the record holds a copy, not the pointer's words
    CHECK(mm::platform::stubBackendLastStart().program == std::vector<uint16_t>{1, 2, 3});
}

TEST_CASE("Hub75PioStub: a blank is recorded apart from the frames, and is the last frame captured") {
    Started s(kFrameBytes, 4);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    CHECK(s.submit() == 0);
    CHECK(mm::platform::stubBackendBlanks() == 0);
    std::vector<uint32_t> blank(1000, 0xABCD);
    CHECK(s.backend->submitBlank(blank.data(), 4000) == 0);
    CHECK(mm::platform::stubBackendBlanks() == 1);
    CHECK(mm::platform::stubBackendLastBlankBytes() == 4000);
    CHECK(mm::platform::stubBackendSubmittedFrames() == 1);   // the blank is no frame
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 1000);
    CHECK(last[0] == 0xABCD);
}

TEST_CASE("Hub75PioStub: a blank that fails to submit returns the fault's code, the frames still go, and the stop still stops the stub") {
    Started s(kFrameBytes, 4);
    StubFaults faults;
    faults.failBlank = true;
    mm::platform::stubBackendSetFaults(faults);
    std::vector<uint32_t> blank(16, 0);
    CHECK(s.submit() == 0);
    CHECK(s.backend->submitBlank(blank.data(), 64) == -EIO);
    CHECK(mm::platform::stubBackendBlankAttempts() == 1);
    CHECK(mm::platform::stubBackendBlanks() == 0);   // a failed blank is no blank
    CHECK(s.backend->stop(true, s.error));
    CHECK(s.submit() == -ENODEV);   // stopped all the same
}

TEST_CASE("Hub75PioStub: each submitted frame leaves its identity, the last 64 kept, oldest first") {
    Started s(kFrameBytes, 4);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    std::vector<uint32_t> words16(16);
    const uint64_t total = 70;
    for (uint64_t i = 0; i < total; i++) {
        for (size_t w = 0; w < words16.size(); w++) words16[w] = static_cast<uint32_t>(i * 100 + w);
        CHECK(s.backend->submitFrame(words16.data(), 64) == 0);
    }
    mm::platform::StubFrameId ids[mm::platform::kStubFrameIdRing + 8];
    CHECK(mm::platform::stubBackendFrameIds(nullptr, 4) == 0);
    const size_t n = mm::platform::stubBackendFrameIds(ids, mm::platform::kStubFrameIdRing + 8);
    REQUIRE(n == mm::platform::kStubFrameIdRing);
    CHECK(ids[0].index == total - n + 1);
    CHECK(ids[n - 1].index == total);
    CHECK(ids[n - 1].first == static_cast<uint32_t>((total - 1) * 100));
    CHECK(ids[n - 1].last == static_cast<uint32_t>((total - 1) * 100 + 15));
    CHECK(ids[n - 1].words == 16);
    CHECK(ids[n - 1].checksum != ids[n - 2].checksum);   // different words, a different checksum
    mm::platform::StubFrameId newest[2];
    CHECK(mm::platform::stubBackendFrameIds(newest, 2) == 2);   // a short buffer takes the newest, oldest first
    CHECK(newest[1].index == total);
    CHECK(newest[0].index == total - 1);
}

TEST_CASE("Hub75PioStub: a submit before the start fails, and the reset puts every record back") {
    mm::platform::stubBackendReset();
    std::unique_ptr<mm::platform::PioBackend> backend(mm::platform::createStubBackend());
    std::vector<uint32_t> frame(16, 1);
    CHECK(backend->submitFrame(frame.data(), 64) == -ENODEV);   // the piolib backend answers the same before its start
    Started s(kFrameBytes, 4);
    CHECK(s.submit() == 0);
    mm::platform::stubBackendReset();
    CHECK(mm::platform::stubBackendSubmittedFrames() == 0);
    CHECK(mm::platform::stubBackendLastStart().starts == 0);
    CHECK(mm::platform::stubBackendBlanks() == 0);
    mm::platform::StubFrameId id;
    CHECK(mm::platform::stubBackendFrameIds(&id, 1) == 0);
    std::vector<uint32_t> out(8);
    CHECK(mm::platform::stubBackendCopyLastFrame(out.data(), out.size()) == 0);
}

TEST_CASE("Hub75PioStub: a start the kernel would refuse is refused with -EINVAL") {
    mm::platform::stubBackendReset();
    std::unique_ptr<mm::platform::PioBackend> backend(mm::platform::createStubBackend());
    std::string error;
    REQUIRE(backend->open(error));
    mm::platform::PioStartConfig config;
    config.clkdivInt = 1;
    config.xferBufSize = 4096;
    config.xferBufCount = 4;
    CHECK(backend->start(config, error));   // four buffers of a multiple of 4 is the largest the kernel takes

    for (uint32_t count : {5u, 9u}) {
        config.xferBufCount = count;
        CHECK_FALSE(backend->start(config, error));
        CHECK(error.find("EINVAL") != std::string::npos);
    }
    config.xferBufCount = 4;
    for (uint32_t size : {0u, 2u, 4094u}) {
        config.xferBufSize = size;
        CHECK_FALSE(backend->start(config, error));
        CHECK(error.find("EINVAL") != std::string::npos);
    }
    CHECK(mm::platform::stubBackendLastStart().starts == 1);   // only the good start counted
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioStub: an early FIFO-empty stop leaves real chunks in flight unless the blank filled the ring") {
    // Four real chunks of 4 KiB in flight, then a blank in the ring model.
    const auto inFlight = [](uint32_t blankBytes) {
        Started s(4096, 4, kSlowerDivider);
        mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
        StubFaults faults;
        faults.fifoEmptyEarly = true;
        mm::platform::stubBackendSetFaults(faults);
        std::vector<uint32_t> frame(1024, 0x10);
        for (int i = 0; i < 4; i++) CHECK(s.backend->submitFrame(frame.data(), 4096) == 0);
        std::vector<uint32_t> blank(blankBytes / 4, 0);
        CHECK(s.backend->submitBlank(blank.data(), blankBytes) == 0);
        CHECK(s.backend->stop(true, s.error));
        return mm::platform::stubBackendRealChunksInFlightAtStop();
    };
    CHECK(inFlight(4096) == 3);       // a one-chunk blank is queued behind the first real chunk's wait: three are still out
    CHECK(inFlight(4 * 4096) == 0);   // a blank as deep as the ring waits out every real chunk
}

TEST_CASE("Hub75PioStub: a safe stop leaves no real chunk in flight") {
    Started s(4096, 4, kSlowDivider);
    mm::platform::stubBackendSetQueueModel(StubQueueModel::Ring);
    std::vector<uint32_t> frame(1024, 0x10);
    for (int i = 0; i < 4; i++) CHECK(s.backend->submitFrame(frame.data(), 4096) == 0);
    std::vector<uint32_t> blank(1024, 0);
    CHECK(s.backend->submitBlank(blank.data(), 4096) == 0);
    CHECK(s.backend->stop(true, s.error));   // fault-free, the stop drains first
    CHECK(mm::platform::stubBackendRealChunksInFlightAtStop() == 0);
}

TEST_CASE("Hub75PioStub: a start before the open is refused, as the piolib backend refuses it") {
    mm::platform::stubBackendReset();
    std::unique_ptr<mm::platform::PioBackend> backend(mm::platform::createStubBackend());
    std::string error;
    mm::platform::PioStartConfig config;
    config.clkdivInt = 1;
    config.xferBufSize = 4096;
    config.xferBufCount = 4;
    CHECK_FALSE(backend->start(config, error));
    CHECK(error.find("not opened") != std::string::npos);
    CHECK(mm::platform::stubBackendLastStart().starts == 0);
}
