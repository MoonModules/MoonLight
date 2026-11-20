/// @module platform

/// Pins the PIO engine's two threads and its shutdown, each under both stub queue models: the blocking submit of a 6.12 kernel and the ring of a kernel that queues. The encode thread hands frames to the refresh thread through a triple buffer, so these cases read what the stub was handed.
///
/// They pin that no frame is torn, the newest frame wins, the submits keep their pace while an encode runs, a failed encode keeps the frame on show, and the shutdown has the refresh thread blank ring-deep as it leaves its loop, stops the backend from the closing thread, and frees the buffers, or stops with the pins dark and keeps the buffers when a submit hangs.
///
/// Frames are told apart by the stub's checksums, and the values used are 0, 64, 128 and 192, which the 2-bit depth keeps apart. A frame of one value encodes to one set of words, so the checksum a uniform frame settles at is its reference, and a frame torn between two encodes matches no reference.
///
/// The cases run the wire at 1 MHz, a 2 ms frame, so the polls that read the stub's 64 identities miss none.

#include "doctest.h"
#include "hub75_engine_bench.h"
#include "platform/Hub75Pio.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_pio_engine.h"
#include "platform/rpi/hub75_pio_pins.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <csignal>
    #include <pthread.h>
#endif

namespace {

using mm::platform::StubQueueModel;
using Clock = std::chrono::steady_clock;

// One 64x32 panel at 1/16 scan, 2-bit, on a 1 MHz clock: a 2,048-word frame takes 2 ms on the wire.
mm::platform::Hub75PioOptions slowSmall() {
    mm::platform::Hub75PioOptions o = mm::test::pioSmall();
    o.pixelClockHz = 1000000;
    return o;
}

// The bench wall: 128x64 panels, two in series on each of three lanes, whose buffers run to megabytes.
mm::platform::Hub75PioOptions benchWall() {
    mm::platform::Hub75PioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.panelsPerLane = 2;
    o.parallel = 3;
    return o;
}

constexpr uint64_t kMegabyte = 1024 * 1024;

std::vector<uint8_t> uniform(size_t bytes, uint8_t value) { return std::vector<uint8_t>(bytes, value); }

constexpr size_t kSmallRgb = 64 * 32 * 3;

// Calls `body` with a run under each queue model whose first frame went out and whose join deadline is 150 ms, as the wedge cases begin.
template <class Body>
void withShortDeadline(Body body) {
    mm::test::forBothQueueModels([&](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        REQUIRE(mm::test::waitFirstSubmit(run.engine));
        run.engine.setJoinDeadlineMs(150);
        body(run);
    });
}

// Hangs the submits and refuses the stop, then shuts the run down: the engine wedged with a stop that failed. Returns the faults, for a case to change.
mm::platform::StubFaults wedgeWithFailedStop(mm::test::PioRun& run) {
    mm::platform::StubFaults faults;
    faults.hangSubmits = true;
    faults.failStop = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(run.engine.shutdown());
    return faults;
}

}  // namespace

TEST_CASE("Hub75PioBuffer: frames published as fast as the render thread can are submitted whole, never torn") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        mm::platform::StubFaults slowCopy;
        slowCopy.copyMicros = 800;   // the stub reads the frame over 0.8 ms, so a buffer written meanwhile arrives torn
        mm::platform::stubBackendSetFaults(slowCopy);
        const std::vector<uint8_t> a = uniform(kSmallRgb, 64), b = uniform(kSmallRgb, 192);
        const uint64_t black = mm::test::settledChecksum(run.engine, uniform(kSmallRgb, 0));
        const uint64_t refA = mm::test::settledChecksum(run.engine, a);
        const uint64_t refB = mm::test::settledChecksum(run.engine, b);
        REQUIRE(std::set<uint64_t>{black, refA, refB}.size() == 3);

        mm::test::IdWatcher watcher;
        std::atomic<bool> done{false};
        std::thread render([&] {
            // Paced over about 120 ms, so hundreds of encodes run while the stub is still reading a frame out over 0.8 ms.
            for (int i = 0; i < 400; i++) {
                run.engine.publishRgb((i % 2 ? a : b).data());
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            done = true;
        });
        while (!done) {
            watcher.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        render.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        watcher.poll();
        for (const uint64_t sum : watcher.checksums) CHECK((sum == black || sum == refA || sum == refB));
        CHECK(watcher.checksums.size() >= 2);   // the stream did change: some of A and B arrived
    });
}

TEST_CASE("Hub75PioBuffer: of three frames published before the encode thread looks, only the newest is submitted") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        const uint64_t refOne = mm::test::settledChecksum(run.engine, uniform(kSmallRgb, 64));
        const uint64_t refTwo = mm::test::settledChecksum(run.engine, uniform(kSmallRgb, 128));
        const uint64_t refThree = mm::test::settledChecksum(run.engine, uniform(kSmallRgb, 192));
        REQUIRE(std::set<uint64_t>{refOne, refTwo, refThree}.size() == 3);
        mm::test::settledChecksum(run.engine, uniform(kSmallRgb, 0));   // back to black, then:

        run.engine.pauseEncodes(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));   // the encode thread is idle
        for (const uint8_t v : {64, 128, 192}) run.engine.publishRgb(uniform(kSmallRgb, v).data());
        const uint64_t encoded = run.engine.framesEncoded();
        mm::platform::stubBackendResetCapture();   // the identities from here on are the ones the three publishes cause
        mm::test::IdWatcher watcher;
        run.engine.pauseEncodes(false);
        CHECK(mm::test::waitUntil([&] { return run.engine.framesEncoded() > encoded; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        watcher.poll();
        CHECK(watcher.checksums.count(refThree) == 1);
        CHECK(watcher.checksums.count(refOne) == 0);
        CHECK(watcher.checksums.count(refTwo) == 0);
        CHECK(run.engine.framesEncoded() == encoded + 1);   // one encode took the three publishes
        CHECK(run.engine.framesPublished() >= 3);
    });
}

TEST_CASE("Hub75PioBuffer: with nothing new to show, the frame on show keeps submitting at the wire's pace") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        const uint64_t ref = mm::test::settledChecksum(run.engine, uniform(kSmallRgb, 77));
        const uint64_t submitted = mm::platform::stubBackendSubmittedFrames();
        CHECK(mm::test::waitUntil([&] { return mm::platform::stubBackendSubmittedFrames() >= submitted + 10; }));
        mm::platform::StubFrameId ids[mm::platform::kStubFrameIdRing];
        const size_t n = mm::platform::stubBackendFrameIds(ids, 10);
        REQUIRE(n == 10);
        for (size_t i = 0; i < n; i++) CHECK(ids[i].checksum == ref);
    });
}

TEST_CASE("Hub75PioBuffer: the submits keep their cadence while an encode runs") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        REQUIRE(mm::test::waitFirstSubmit(run.engine));
        run.engine.delayEncodes(150000);   // each encode takes 150 ms longer
        run.engine.publishRgb(uniform(kSmallRgb, 50).data());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the encode thread is inside the encode
        const uint64_t encoded = run.engine.framesEncoded();
        const uint64_t before = run.engine.framesSubmitted();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const uint64_t during = run.engine.framesSubmitted() - before;
        CHECK(run.engine.framesEncoded() == encoded);   // still encoding
        // A 2 ms frame gives about 40 submits in 80 ms; an engine that encoded in the submit loop would give none.
        CHECK(during >= 20);
        CHECK(mm::test::waitUntil([&] { return run.engine.framesEncoded() > encoded; }, 3000));
    });
}

TEST_CASE("Hub75PioBuffer: a failed encode keeps the last good frame on show, once per frame whatever the lanes") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::platform::Hub75PioOptions o = slowSmall();
        o.parallel = 3;   // three lanes: the hook fails a frame, not a lane
        mm::test::PioRun run(o, model);
        const size_t rgb = 64 * 96 * 3;
        const uint64_t ref = mm::test::settledChecksum(run.engine, uniform(rgb, 64));
        const uint64_t encoded = run.engine.framesEncoded();

        run.engine.failEncodes(1);
        run.engine.publishRgb(uniform(rgb, 192).data());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));   // the encode thread takes it and fails
        CHECK(run.engine.framesEncoded() == encoded);
        mm::test::IdWatcher watcher;
        watcher.poll();
        CHECK(mm::test::newestChecksum() == ref);   // the frame on show is still the good one

        run.engine.publishRgb(uniform(rgb, 192).data());   // the next frame encodes
        CHECK(mm::test::waitUntil([&] { return run.engine.framesEncoded() == encoded + 1; }));   // one frame, not three lanes
        CHECK(mm::test::waitUntil([&] { return mm::test::newestChecksum() != ref; }));
    });
}

TEST_CASE("Hub75PioBuffer: three lanes painted different colors each reach their own pins") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::platform::Hub75PioOptions o = slowSmall();
        o.parallel = 3;
        mm::test::PioRun run(o, model);
        // Lane 0 red, lane 1 green, lane 2 blue, whole strips: 64 x 32 x 3 bytes each.
        std::vector<uint8_t> rgb(64 * 96 * 3, 0);
        const size_t strip = 64 * 32 * 3;
        for (size_t px = 0; px < 64 * 32; px++) {
            rgb[0 * strip + px * 3 + 0] = 255;
            rgb[1 * strip + px * 3 + 1] = 255;
            rgb[2 * strip + px * 3 + 2] = 255;
        }
        mm::test::settledChecksum(run.engine, rgb);
        std::vector<uint32_t> frame(2048);
        REQUIRE(mm::platform::stubBackendCopyLastFrame(frame.data(), frame.size()) == 2048);

        // The color pins of each lane's red, green and blue, upper and lower half.
        uint32_t want = 0, all = 0;
        for (int lane = 0; lane < 3; lane++) {
            const mm::platform::LanePins& p = mm::platform::kLanePins[lane];
            const uint8_t red[2] = {p.r1, p.r2}, green[2] = {p.g1, p.g2}, blue[2] = {p.b1, p.b2};
            for (int half = 0; half < 2; half++) {
                all |= 1u << mm::platform::wideBitForGpio(red[half]);
                all |= 1u << mm::platform::wideBitForGpio(green[half]);
                all |= 1u << mm::platform::wideBitForGpio(blue[half]);
            }
            const uint8_t* lit = lane == 0 ? red : lane == 1 ? green : blue;
            for (int half = 0; half < 2; half++) want |= 1u << mm::platform::wideBitForGpio(lit[half]);
        }
        // The top plane (slot 1024 on) lights each lane's own color alone; the other fifteen color pins stay dark.
        CHECK((frame[1024] & all) == want);
        CHECK((frame[1024 + 31] & all) == want);   // and the same across the row
    });
}

TEST_CASE("Hub75PioBuffer: the brightness sets the lit words, which half of it halves, and none of it lights nothing") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        const std::vector<uint8_t> white = uniform(kSmallRgb, 255);
        std::vector<uint32_t> frame(2048);
        const auto lit = [&](uint8_t brightness) {
            run.engine.setBrightness(brightness);
            mm::test::settledChecksum(run.engine, white);
            REQUIRE(mm::platform::stubBackendCopyLastFrame(frame.data(), frame.size()) == 2048);
            return mm::test::litWords(frame);
        };
        const size_t full = lit(255);
        const size_t half = lit(128);
        const size_t none = lit(0);
        CHECK(full > 0);
        CHECK(half * 2 >= full * 85 / 100);   // within 15% of half
        CHECK(half * 2 <= full * 115 / 100);
        CHECK(none == 0);
    });
}

TEST_CASE("Hub75PioBuffer: the shutdown joins the encode thread and then the refresh thread, blanks ring-deep and frees the buffers") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        REQUIRE(mm::test::waitFirstSubmit(run.engine));
        CHECK(run.engine.heapBytes() > 0);
        run.engine.delayEncodes(80000);
        run.engine.publishRgb(uniform(kSmallRgb, 5).data());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));   // an encode is in flight
        const auto t0 = Clock::now();
        CHECK(run.engine.shutdown());
        CHECK(Clock::now() - t0 >= std::chrono::milliseconds(40));   // the join waited for the encode
        CHECK(run.engine.threadsJoined());
        CHECK_FALSE(run.engine.wedged());
        CHECK(mm::platform::stubBackendBlanks() == 1);
        CHECK(mm::platform::stubBackendSubmitsAfterStop() == 0);   // no thread submitted after the stop
        const mm::platform::StubStartRecord start = mm::platform::stubBackendLastStart();
        CHECK(mm::platform::stubBackendLastBlankBytes() >= start.config.xferBufCount * start.config.xferBufSize);
        CHECK(run.engine.heapBytes() == 0);
        CHECK_FALSE(run.engine.isRunning());
        const uint64_t encoded = run.engine.framesEncoded();
        run.engine.publishRgb(uniform(kSmallRgb, 5).data());   // a publish after the shutdown is ignored
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(run.engine.framesEncoded() == encoded);
    });
}

TEST_CASE("Hub75PioBuffer: the blank is as deep as the DMA ring for every frame size, and all of it is dark") {
    struct Wall { const char* name; mm::platform::Hub75PioOptions options; };
    mm::platform::Hub75PioOptions deep = mm::test::pioSmall();
    deep.bitDepth = 8;   // a 66,048-byte frame: one full 64 KiB buffer and a spill
    mm::platform::Hub75PioOptions lanes = mm::test::pioSmall();
    lanes.parallel = 3;
    for (const Wall& wall : {Wall{"small", mm::test::pioSmall()}, Wall{"8-bit", deep}, Wall{"3 lanes", lanes}}) {
        CAPTURE(wall.name);
        mm::test::forBothQueueModels([&](StubQueueModel model) {
            mm::test::PioRun run(wall.options, model);
            REQUIRE(mm::test::waitFirstSubmit(run.engine));
            run.engine.shutdown();
            const mm::platform::StubStartRecord start = mm::platform::stubBackendLastStart();
            const uint32_t ring = start.config.xferBufCount * start.config.xferBufSize;
            CHECK(ring > 0);
            CHECK(mm::platform::stubBackendLastBlankBytes() >= ring);
            std::vector<uint32_t> blank(ring / 4);
            REQUIRE(mm::platform::stubBackendCopyLastFrame(blank.data(), blank.size()) == blank.size());
            CHECK(std::all_of(blank.begin(), blank.end(), [](uint32_t w) { return w == mm::platform::kBlankWideWord; }));
        });
    }
}

TEST_CASE("Hub75PioBuffer: the refresh thread submits the blank as it leaves its loop, and the closing thread makes no submit") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        REQUIRE(mm::test::waitFirstSubmit(run.engine));
        const std::thread::id closing = std::this_thread::get_id();
        REQUIRE(run.engine.shutdown());
        CHECK(mm::platform::stubBackendBlanks() == 1);
        const std::thread::id blanker = mm::platform::stubBackendBlankThread();
        CHECK(blanker != std::thread::id{});
        CHECK(blanker != closing);                                                   // the blank came from the refresh thread
        CHECK(mm::platform::stubBackendSubmitsFromThread(blanker) > 1);              // which also made the frame submits
        CHECK(mm::platform::stubBackendSubmitsFromThread(closing) == 0);             // the closing thread made no submit
        CHECK(mm::platform::stubBackendSubmitsAfterStop() == 0);                     // and no submit came after the stop
        CHECK(mm::platform::stubBackendPinsDark());
    });
}

TEST_CASE("Hub75PioBuffer: the blank is skipped after a failed last submit, and the stop still darkens the pins") {
    mm::test::PioRun run(slowSmall());
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    mm::platform::StubFaults faults;
    faults.failSubmits = -1;
    mm::platform::stubBackendSetFaults(faults);
    REQUIRE(mm::test::waitUntil([&] { return run.engine.submitErrors() > 0; }));   // the refresh thread's last submit failed
    CHECK(run.engine.shutdown());
    CHECK(mm::platform::stubBackendBlankAttempts() == 0);   // no blank was tried
    CHECK(mm::platform::stubBackendPinsDark());             // the stop darkened the panel all the same
}

TEST_CASE("Hub75PioBuffer: the rate shown falls to 0 while every submit fails, and rises again when they stop") {
    mm::test::PioRun run(mm::test::pioSmall());
    REQUIRE(mm::test::waitUntil([&] { return run.engine.fps() > 0; }, 3000));   // the first one-second window closes
    mm::platform::StubFaults faults;
    faults.failSubmits = -1;
    mm::platform::stubBackendSetFaults(faults);
    CHECK(mm::test::waitUntil([&] { return run.engine.fps() == 0; }, 4000));    // the next window counts no frame
    CHECK(run.engine.submitErrors() > 0);
    mm::platform::stubBackendSetFaults({});
    CHECK(mm::test::waitUntil([&] { return run.engine.fps() > 0; }, 3000));
}

TEST_CASE("Hub75PioBuffer: after the shutdown the rates read 0 and nothing is held") {
    mm::test::PioRun run(mm::test::pioSmall());
    REQUIRE(mm::test::waitUntil([&] { return run.engine.fps() > 0; }, 3000));
    CHECK(run.engine.refreshHz() > 0.0f);
    run.engine.shutdown();
    CHECK(run.engine.fps() == 0);
    CHECK(run.engine.refreshHz() == 0.0f);
    CHECK(run.engine.heapBytes() == 0);
}

TEST_CASE("Hub75PioBuffer: a failed open frees what it allocated") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    mm::platform::StubFaults faults;
    faults.failStart = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(engine.open(mm::test::pioSmall(), error, true));
    CHECK(engine.heapBytes() == 0);
    engine.failEncodes(-1);
    mm::platform::stubBackendSetFaults({});
    CHECK_FALSE(engine.open(mm::test::pioSmall(), error, true));
    CHECK(engine.heapBytes() == 0);
}

TEST_CASE("Hub75PioBuffer: a wall whose buffers exceed the memory available is refused, naming both figures in MB, and no buffer allocated") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(benchWall(), error, true));
    const uint64_t need = engine.heapBytes();   // what this wall's buffers take
    REQUIRE(engine.shutdown());
    REQUIRE(need > kMegabyte);
    engine.setAvailableMemory(kMegabyte);       // 1 MB available
    mm::platform::stubBackendReset();
    CHECK_FALSE(engine.open(benchWall(), error, true));
    CHECK(error == "the wall's buffers need " + std::to_string((need + kMegabyte - 1) / kMegabyte) + " MB and 1 MB is available");
    CHECK(engine.heapBytes() == 0);
    CHECK(engine.refreshHz() == 0);        // the sized wall's counters are reset
    CHECK_FALSE(engine.isRunning());
    CHECK(engine.framesEncoded() == 0);   // no thread ran
    CHECK(mm::platform::stubBackendLastStart().starts == 0);   // refused before the backend started
}

TEST_CASE("Hub75PioBuffer: a wall opens when its buffers fit the memory available to the byte, and is refused one byte short") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(benchWall(), error, true));
    const uint64_t need = engine.heapBytes();
    REQUIRE(engine.shutdown());
    engine.setAvailableMemory(need - 1);
    CHECK_FALSE(engine.open(benchWall(), error, true));
    CHECK(engine.heapBytes() == 0);
    engine.setAvailableMemory(need);   // the same wall now fits: a refusal leaves the engine ready for the next open
    CHECK(engine.open(benchWall(), error, true));
    CHECK(engine.heapBytes() == need);   // the estimate and the allocation agree
    CHECK(engine.shutdown());
    CHECK(engine.heapBytes() == 0);
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: an allocation that runs out of memory frees the partial buffers and refuses with a reason") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    engine.failAllocations(true);   // the memory check passes, then the allocation throws once the buffers are sized
    CHECK_FALSE(engine.open(mm::test::pioSmall(), error, true));
    CHECK(error.find("ran out of memory allocating the wall's buffers") != std::string::npos);
    CHECK(engine.heapBytes() == 0);
    CHECK_FALSE(engine.isRunning());
    engine.failAllocations(false);
    CHECK(engine.open(mm::test::pioSmall(), error, true));   // the engine opens once memory is back
    CHECK(engine.shutdown());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: a refresh thread stuck in a submit is reported at the deadline with the pins dark, the buffers kept, and a later shutdown finishes") {
    withShortDeadline([](mm::test::PioRun& run) {
        mm::test::hangSubmits();   // the refresh thread is inside the hung submit

        const auto t0 = Clock::now();
        CHECK_FALSE(run.engine.shutdown());
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(1500));   // bounded, not forever
        CHECK(run.engine.wedged());
        CHECK(run.engine.shutdownError().find("did not return") != std::string::npos);
        CHECK(run.engine.shutdownError().find("the panel is dark") != std::string::npos);   // the stop succeeded, so the report says so
        CHECK(run.engine.shutdownError().find("stays held until a close after the refresh thread returns") != std::string::npos);
        CHECK(run.engine.shutdownError().find("may hold its last row") == std::string::npos);
        CHECK(mm::platform::stubBackendPinsDark());                    // the stop ran with the thread still inside
        CHECK(mm::platform::stubBackendStops() == 1);
        CHECK_FALSE(mm::platform::stubBackendLastStopDrained());       // and it skipped the wait: the thread stayed out
        CHECK(mm::platform::stubBackendBlanks() == 0);                 // and submitted no blank
        CHECK(mm::platform::stubBackendSubmitsFromThread(std::this_thread::get_id()) == 0);
        CHECK(run.engine.heapBytes() > 0);                             // the stuck thread still reads them
        CHECK(std::string(run.engine.backendName()) != "(none)");
        std::string error;
        CHECK_FALSE(run.engine.open(mm::test::pioSmall(), error, true));   // refused while it is stuck
        CHECK(error.find("did not return") != std::string::npos);
        mm::platform::Hub75PioOptions invalid = mm::test::pioSmall();
        invalid.parallel = 9;
        CHECK_FALSE(run.engine.open(invalid, error, true));
        CHECK(error.find("did not return") != std::string::npos);   // the wedge is refused before the options are read

        // Only the first stop pays the deadline: a stop that finds the thread still stuck answers at once.
        const auto again = Clock::now();
        CHECK_FALSE(run.engine.shutdown());
        CHECK_FALSE(run.engine.shutdown());
        CHECK(Clock::now() - again < std::chrono::milliseconds(100));

        mm::platform::stubBackendSetFaults({});   // the kernel lets the submit go
        CHECK(mm::test::waitUntil([&] { return run.engine.shutdown(); }));   // once the thread has returned, the stop finishes
        CHECK_FALSE(run.engine.wedged());
        CHECK(run.engine.threadsJoined());
        CHECK(mm::platform::stubBackendBlanks() == 0);              // the released thread found the backend stopped and sent no blank
        CHECK(mm::platform::stubBackendSubmitsAfterStop() == 1);    // its one attempt, refused
        CHECK(run.engine.submitErrors() == 0);                      // a refused blank is no failure
        CHECK(mm::platform::stubBackendPinsDark());
        CHECK(mm::platform::stubBackendStops() == 1);               // the later shutdown found the SM stopped already
        CHECK_FALSE(mm::platform::stubBackendLastStopDrained());    // so the one stop on record is the wedged one, made with the wait skipped
        CHECK(run.engine.heapBytes() == 0);
        CHECK(run.engine.open(mm::test::pioSmall(), error, true));   // and the engine opens again
        CHECK_FALSE(mm::platform::stubBackendPinsDark());            // the new start cleared the dark word, so the next check is the next stop's own
        CHECK(run.engine.shutdown());
        CHECK(mm::platform::stubBackendPinsDark());
        CHECK(mm::platform::stubBackendLastStopDrained());           // a clean shutdown waits
    });
}

TEST_CASE("Hub75PioBuffer: a refresh thread stuck in a submit, with a stop that fails, is reported without claiming the panel dark") {
    withShortDeadline([](mm::test::PioRun& run) {
        mm::platform::StubFaults faults;
        faults.hangSubmits = true;
        faults.failStop = true;   // the disable or the pin write is refused
        mm::platform::stubBackendSetFaults(faults);

        CHECK_FALSE(run.engine.shutdown());
        CHECK(run.engine.wedged());
        const std::string why = run.engine.shutdownError();
        CHECK(why.find("did not return") != std::string::npos);
        CHECK(why.find("the stop failed") != std::string::npos);
        CHECK(why.find("stub stop failed") != std::string::npos);        // the backend's own reason rides along
        CHECK(why.find("may hold its last row") != std::string::npos);
        CHECK(why.find("the panel is dark") == std::string::npos);        // it must not claim what the stop did not do
        CHECK_FALSE(mm::platform::stubBackendPinsDark());
        CHECK(run.engine.submitErrors() == 0);                            // a failed stop is no failed submit
        CHECK_FALSE(run.engine.shutdown());                               // a later stop that finds the thread stuck
        CHECK(run.engine.shutdownError() == why);                         // leaves the report as it was

        mm::platform::stubBackendSetFaults({});
        CHECK(mm::test::waitUntil([&] { return run.engine.shutdown(); }));
        CHECK(run.engine.shutdownError().empty());
    });
}

TEST_CASE("Hub75PioBuffer: a clean shutdown whose stop fails returns true, frees the buffers and reports the failure, and one whose stop works reports none") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(slowSmall(), model);
        REQUIRE(mm::test::waitFirstSubmit(run.engine));
        mm::platform::StubFaults faults;
        faults.failStop = true;
        mm::platform::stubBackendSetFaults(faults);
        CHECK(run.engine.shutdown());   // the thread joined and the buffers are free: the answer stays true
        CHECK_FALSE(run.engine.wedged());
        CHECK(run.engine.heapBytes() == 0);
        const std::string why = run.engine.shutdownError();
        CHECK(why.find("the stop failed") != std::string::npos);
        CHECK(why.find("stub stop failed") != std::string::npos);   // the backend's own reason rides along
        CHECK(why.find("may hold its last row") != std::string::npos);
        CHECK(why.find("did not return") == std::string::npos);     // the thread came back
        CHECK(why.find("the panel is dark") == std::string::npos);
        CHECK_FALSE(mm::platform::stubBackendPinsDark());
        CHECK(run.engine.submitErrors() == 0);   // a failed stop is no failed submit

        mm::platform::stubBackendSetFaults({});
        std::string error;
        REQUIRE(run.engine.open(slowSmall(), error, true));
        CHECK(run.engine.shutdown());
        CHECK(run.engine.shutdownError().empty());   // a stop that worked leaves no report
        CHECK(mm::platform::stubBackendPinsDark());
    });
}

TEST_CASE("Hub75PioBuffer: a stop that failed on the wedged path is retried by the next shutdown, which says the panel is dark once it works") {
    withShortDeadline([](mm::test::PioRun& run) {
        mm::platform::StubFaults faults = wedgeWithFailedStop(run);
        CHECK(run.engine.shutdownError().find("may hold its last row") != std::string::npos);
        CHECK_FALSE(mm::platform::stubBackendPinsDark());

        faults.failStop = false;   // the kernel takes the stop now, the thread is still stuck
        mm::platform::stubBackendSetFaults(faults);
        CHECK_FALSE(run.engine.shutdown());   // the thread is still inside the kernel: the retry runs on the wedged path
        CHECK(mm::platform::stubBackendPinsDark());
        const std::string why = run.engine.shutdownError();
        CHECK(why.find("did not return") != std::string::npos);
        CHECK(why.find("the panel is dark") != std::string::npos);        // the retry replaced the failure
        CHECK(why.find("may hold its last row") == std::string::npos);
        CHECK(mm::platform::stubBackendStops() == 1);                      // one stop of a started stub: the retry is no second stop

        mm::platform::stubBackendSetFaults({});
        CHECK(mm::test::waitUntil([&] { return run.engine.shutdown(); }));
        CHECK(run.engine.shutdownError().empty());
        CHECK(mm::platform::stubBackendPinsDark());
    });
}

TEST_CASE("Hub75PioBuffer: a later shutdown whose retried stop fails again finishes, and names the failure") {
    withShortDeadline([](mm::test::PioRun& run) {
        mm::platform::StubFaults faults = wedgeWithFailedStop(run);
        CHECK(run.engine.shutdownError().find("did not return") != std::string::npos);

        faults.hangSubmits = false;   // the thread returns, and the stop stays refused
        mm::platform::stubBackendSetFaults(faults);
        CHECK(mm::test::waitUntil([&] { return run.engine.shutdown(); }));   // joined, the buffers freed
        CHECK(run.engine.heapBytes() == 0);
        const std::string why = run.engine.shutdownError();
        CHECK(why.find("the stop failed") != std::string::npos);   // not cleared: the retry failed
        CHECK(why.find("stub stop failed") != std::string::npos);
        CHECK(why.find("did not return") == std::string::npos);    // the thread has come back, so the report is the stop's alone
        CHECK_FALSE(mm::platform::stubBackendPinsDark());
    });
}

TEST_CASE("Hub75PioBuffer: an encode thread that fails to start does not hang the open on a refresh thread stuck in a submit") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    mm::test::HangRelease release(engine);   // a failed check still lets the stuck thread go before the engine dies
    engine.setJoinDeadlineMs(150);
    engine.failEncodeThreadStart(true);
    mm::platform::StubFaults faults;
    faults.hangSubmits = true;   // the refresh thread blocks in its first submit, or in its blank
    mm::platform::stubBackendSetFaults(faults);
    std::string error;
    const auto t0 = Clock::now();
    CHECK_FALSE(engine.open(slowSmall(), error, true));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(1500));   // bounded by the deadline, not by the stuck thread
    CHECK(error.find("failed to start the encode thread") != std::string::npos);
    CHECK(engine.wedged());   // the shutdown found the thread stuck and kept what it reads
    CHECK(engine.shutdownError().find("did not return") != std::string::npos);
    CHECK(engine.shutdownError().find("the panel is dark") != std::string::npos);
    CHECK(mm::platform::stubBackendPinsDark());
    CHECK(engine.heapBytes() > 0);

    mm::platform::stubBackendSetFaults({});
    CHECK(mm::test::waitUntil([&] { return engine.shutdown(); }));
    CHECK(engine.heapBytes() == 0);
    engine.failEncodeThreadStart(false);
    CHECK(engine.open(slowSmall(), error, true));   // and the engine opens again
    CHECK(engine.shutdown());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: an encode thread that fails to start, with the refresh thread free, leaves the engine stopped with the panel dark") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    engine.failEncodeThreadStart(true);
    std::string error;
    CHECK_FALSE(engine.open(slowSmall(), error, true));
    CHECK(error.find("failed to start the encode thread") != std::string::npos);
    CHECK_FALSE(engine.wedged());
    CHECK_FALSE(engine.isRunning());
    CHECK(engine.threadsJoined());
    CHECK(engine.heapBytes() == 0);
    CHECK(std::string(engine.backendName()) == "(none)");
    CHECK(mm::platform::stubBackendPinsDark());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: a refresh thread that fails to start refuses the open and leaves the engine stopped, the backend closed and the panel dark") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    engine.failRefreshThreadStart(true);
    std::string error;
    CHECK_FALSE(engine.open(slowSmall(), error, true));
    CHECK(error == "failed to start the refresh thread");
    CHECK_FALSE(engine.isRunning());
    CHECK_FALSE(engine.wedged());
    CHECK(engine.threadsJoined());
    CHECK(engine.heapBytes() == 0);
    CHECK(std::string(engine.backendName()) == "(none)");   // the backend was closed and dropped
    CHECK(mm::platform::stubBackendStops() == 1);           // after one stop
    CHECK(mm::platform::stubBackendPinsDark());

    engine.failRefreshThreadStart(false);
    CHECK(engine.open(slowSmall(), error, true));   // and the engine opens again
    CHECK(engine.shutdown());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: at a slow pixel clock the join deadline follows the wire time, so a healthy shutdown is not reported stuck") {
    // 20 kHz: a word takes 50 us, the 2,048-word frame 102 ms and the 8,192-word ring 410 ms on the wire, so the blocking blank's submit alone outlasts the 100 ms floor set here; a deadline that stayed at its floor would report the healthy thread stuck.
    mm::platform::Hub75PioOptions o = mm::test::pioSmall();
    o.pixelClockHz = 20000;
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    mm::test::HangRelease release(engine);   // a deadline that fails here leaves the engine wedged: the thread is let go and joined before the engine dies
    engine.setJoinDeadlineMs(100);   // the floor, before the open sizes the wall
    std::string error;
    REQUIRE(engine.open(o, error, true));
    const uint64_t wireMs = mm::platform::pioShutdownWireMs(engine.frameSlots(), 4 * 2048, 2000, 0);
    CHECK(wireMs == 1536);   // (102.4 + 409.6 ms) doubled for the feed lag and half again
    CHECK(engine.joinDeadlineMs() == static_cast<int>(wireMs));
    REQUIRE(mm::test::waitFirstSubmit(engine));
    CHECK(engine.shutdown());   // the blank's submit takes 410 ms against the 100 ms floor
    CHECK_FALSE(engine.wedged());
    CHECK(mm::platform::stubBackendBlanks() == 1);
    CHECK(mm::platform::stubBackendLastStopDrained());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: a clock slow enough raises the join deadline above the 3 s floor") {
    // 10 kHz: (205 + 819 ms) doubled for the feed lag and half again is 3,072 ms, past the floor.
    mm::platform::Hub75PioOptions o = mm::test::pioSmall();
    o.pixelClockHz = 10000;
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    mm::test::HangRelease release(engine);   // as above
    std::string error;
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.joinDeadlineMs() == 3072);
    CHECK(engine.shutdown());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: a fast clock leaves the join deadline at its floor, and the floor is what a caller sets") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(mm::test::pioSmall(), error, true));
    CHECK(engine.joinDeadlineMs() == mm::platform::kHub75JoinDeadlineMs);
    CHECK(engine.shutdown());
    engine.setJoinDeadlineMs(7000);
    REQUIRE(engine.open(mm::test::pioSmall(), error, true));
    CHECK(engine.joinDeadlineMs() == 7000);
    CHECK(engine.shutdown());
    mm::platform::stubBackendReset();
}

TEST_CASE("Hub75PioBuffer: a publish racing the shutdown never writes into a freed buffer") {
    mm::test::forBothQueueModels([](StubQueueModel model) {
        mm::test::PioRun run(mm::test::pioSmall(), model);
        const std::vector<uint8_t> frame = uniform(kSmallRgb, 9);
        std::atomic<bool> stop{false};
        std::thread render([&] {
            while (!stop) run.engine.publishRgb(frame.data());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(run.engine.shutdown());   // under the address sanitizer, a write into the freed slots is the failure
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stop = true;
        render.join();
        CHECK(run.engine.heapBytes() == 0);
    });
}

TEST_CASE("Hub75PioBuffer: an early FIFO-empty poll after the blank still finds the pins dark, since the blank covers the DMA ring") {
    // B2-01: the stub stops as soon as the blank is queued, as the piolib backend's poll can, and records how many real chunks were still in flight. A blank shorter than the ring leaves some, whatever the frame's size.
    mm::platform::Hub75PioOptions deep = slowSmall();
    deep.bitDepth = 8;   // two chunks a frame, the first ending inside the frame
    mm::platform::Hub75PioOptions lanes = slowSmall();
    lanes.parallel = 3;
    for (const mm::platform::Hub75PioOptions& o : {slowSmall(), deep, lanes}) {
        mm::test::PioRun run(o, StubQueueModel::Ring);
        mm::platform::StubFaults faults;
        faults.fifoEmptyEarly = true;
        mm::platform::stubBackendSetFaults(faults);
        const size_t rgb = static_cast<size_t>(o.cols) * o.rows * o.parallel * 3;
        run.engine.publishRgb(uniform(rgb, 255).data());   // white: a chunk ends on a lit word
        REQUIRE(mm::test::waitUntil([&] { return mm::platform::stubBackendSubmittedFrames() >= 8; }));
        run.engine.shutdown();
        CHECK(mm::platform::stubBackendBlanks() == 1);
        CHECK(mm::platform::stubBackendRealChunksInFlightAtStop() == 0);
    }
}

TEST_CASE("Hub75PioBuffer: every frame the engine submits comes from a thread that blocks the stop signals") {
    mm::test::PioRun run(slowSmall());
    REQUIRE(mm::test::waitUntil([] { return mm::platform::stubBackendSubmittedFrames() >= 5; }));
    CHECK(mm::platform::stubBackendSubmitsWithStopOpen() == 0);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("Hub75PioBuffer: the stop signals are blocked on the thread that calls blockStopSignals, and on no other") {
    // The refresh thread calls it as it starts, so a service stop lands on another thread, outside every transfer.
    const auto blocked = [](int signal) {
        sigset_t now;
        pthread_sigmask(SIG_BLOCK, nullptr, &now);
        return sigismember(&now, signal) == 1;
    };
    bool onThread[3] = {};
    bool otherThreadSigusr = true;
    std::thread t([&] {
        mm::platform::blockStopSignals();
        onThread[0] = blocked(SIGINT);
        onThread[1] = blocked(SIGTERM);
        onThread[2] = blocked(SIGHUP);
        otherThreadSigusr = blocked(SIGUSR1);   // only the stop signals are blocked
    });
    t.join();
    CHECK(onThread[0]);
    CHECK(onThread[1]);
    CHECK(onThread[2]);
    CHECK_FALSE(otherThreadSigusr);
    CHECK_FALSE(blocked(SIGTERM));   // this thread skipped the call, so it still takes a stop
}
#endif

TEST_CASE("Hub75PioBuffer: the triple buffer's writer never holds the slot the reader reads, whatever the order of publishes and takes") {
    mm::platform::TripleBuffer<uint32_t> t;
    t.reset(4);
    CHECK(t.size() == 4);
    CHECK_FALSE(t.take());   // before any publish: take returns false, the reader keeps its zeros
    CHECK(t.current()[0] == 0);
    CHECK(t.writeSlot() != t.current());

    // A publish hands the writer's slot over and gives it another; the take brings the published one to the reader.
    t.writeSlot()[0] = 11;
    const uint32_t* handed = t.writeSlot();
    t.publishWritten();
    CHECK(t.writeSlot() != handed);   // the writer moves on from the slot it handed over
    CHECK(t.writeSlot() != t.current());
    CHECK(t.take());
    CHECK(t.current() == handed);
    CHECK(t.current()[0] == 11);
    CHECK(t.writeSlot() != t.current());
    CHECK_FALSE(t.take());            // the same frame as the last take: the reader keeps its frame
    CHECK(t.current()[0] == 11);

    // Three publishes before a take: the third is the one delivered, and the reader's slot always differs from the writer's.
    for (uint32_t v : {21u, 22u, 23u}) {
        t.writeSlot()[0] = v;
        const uint32_t* before = t.writeSlot();
        t.publishWritten();
        CHECK(t.writeSlot() != before);
        CHECK(t.writeSlot() != t.current());
    }
    CHECK(t.take());
    CHECK(t.current()[0] == 23);
    CHECK(t.writeSlot() != t.current());
    CHECK(t.published() == 4);

    // Interleaved, for many rounds: the slot the reader holds always differs from the one the writer fills.
    for (uint32_t round = 0; round < 50; round++) {
        t.writeSlot()[0] = 100 + round;
        t.publishWritten();
        if (round % 3 != 2) t.take();
        CHECK(t.writeSlot() != t.current());
    }
    t.release();
    CHECK(t.size() == 0);
}

TEST_CASE("Hub75PioBuffer: a submit that never returns reads as a stall, the rate falls to 0 meanwhile, and the stall ends when it returns") {
    mm::test::PioRun run(mm::test::pioSmall());
    run.engine.setStallMs(200);
    CHECK_FALSE(run.engine.stalled());
    REQUIRE(mm::test::waitUntil([&] { return run.engine.fps() > 0; }, 3000));   // the first window closes: a real rate
    mm::platform::StubFaults faults;
    faults.hangSubmits = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK(mm::test::waitUntil([&] { return run.engine.stalled(); }, 2000));
    CHECK(run.engine.fps() == 0);   // not the last rate, held by a thread that counts no more frames
    mm::platform::stubBackendSetFaults({});
    CHECK(mm::test::waitUntil([&] { return !run.engine.stalled(); }, 2000));
    CHECK(run.engine.fps() > 0);    // the window's own figure speaks again
}
