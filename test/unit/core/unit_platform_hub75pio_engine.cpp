/// @module platform

/// Pins the PIO engine against the stub backend: the options it refuses, the clock divider it rounds, the wide words its refresh thread submits for a known frame, and the blank frame its shutdown leaves, and what the engine hands the backend (the start configuration, the blank's size). No hardware is touched; the stub captures every submit, keeps its records and raises the faults a case asks for.
///
/// The double buffer's cases land beside these with the double buffer: no torn frame, the newest frame wins, the submit cadence holds while an encode runs, the refresh thread blanks as it leaves its loop and the closing thread stops the backend after it, and a failed encode keeps the last frame, each under both stub queue models.
///
/// The real PIO stays out of reach: the closed gate refuses an engine opened directly, on every build.

#include "doctest.h"
#include "platform/Hub75Pio.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_pio_engine.h"
#include "platform/rpi/hub75_pio_pins.h"
#include "platform/rpi/hub75_pio_repack.h"
#include "hub75_engine_bench.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using mm::test::pioSmall;

// Wait up to `ms` for the stub to have submitted a frame whose word `index` has `bit` set, or all of `expect` when `bit` is -1.
bool waitForWord(size_t index, int bit, uint32_t expect, int ms = 2000) {
    return mm::test::waitForWord(mm::platform::stubBackendCopyLastFrame, index, bit, expect, ms);
}

// One wide word repacked from the three lanes' slots, `lanes` of them driven.
uint32_t repacked(const mm::platform::RepackTables& tables, const uint16_t (&slot)[3], int lanes) {
    const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
    uint32_t wide = 0;
    mm::platform::repackSlots(ptrs, lanes, 1, tables, &wide);
    return wide;
}

}  // namespace

TEST_CASE("Hub75PioEngine: the options are refused with reasons before anything opens") {
    std::string error;
    mm::platform::Hub75PioOptions o = pioSmall();
    CHECK(mm::platform::hub75PioCheckOptions(o, error));

    o = pioSmall(); o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.parallel = 4;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.panelsPerLane = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.panelsPerLane = 33;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.rows = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.bitDepth = 1;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.bitDepth = 7;
    CHECK(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.bitDepth = 8;
    CHECK(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.bitDepth = 9;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.latchBlankWords = 9;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = pioSmall(); o.pixelClockHz = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    o = pioSmall(); o.scanRate = 32;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    CHECK(error.find("32-high at 1/16") != std::string::npos);

    // The engine refuses the same geometry on open, and the engine stays stopped.
    mm::platform::Hub75PioEngine engine;
    CHECK_FALSE(engine.open(o, error, true));
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75PioEngine: a frame publishes and submits, and the geometry reads back") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(pioSmall(), error, true));
    CHECK(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "stub (no hardware)");
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.lanes() == 1);
    CHECK(engine.frameSlots() == 2048);
    CHECK(engine.pixelClockHz() == 10000000);   // a 4.0 divider, exact
    CHECK(std::abs(engine.refreshHz() - 10000000.0f / 2048.0f) < 1.0f);
    CHECK(engine.dmaBytesPerSecond() == 40000000);

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    engine.publishRgb(frame.data());
    CHECK(mm::test::waitFirstSubmit(engine));
    CHECK(mm::test::waitUntil([&] { return engine.framesEncoded() > 0; }));   // the refresh thread takes the published frame after its first submit
    CHECK(engine.framesPublished() == 1);
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75PioEngine: the pixel clock rounds to the divider, and tops out at 40 MHz") {
    // The RP1 divider register holds an integer part and an 8-bit fraction: the pixel clocks the card and the tests use round to these.
    struct Expected { uint32_t pixelClockHz; uint16_t intPart; uint8_t fracPart; uint32_t actualHz; };
    const Expected expected[] = {
        {1000000, 40, 0, 1000000},   {5000000, 8, 0, 5000000},   {10000000, 4, 0, 10000000},
        {12000000, 3, 85, 12004689}, {20000000, 2, 0, 20000000}, {25000000, 1, 154, 24975610},
        {40000000, 1, 0, 40000000},  {50000000, 1, 0, 40000000},
    };
    for (const Expected& e : expected) {
        const mm::platform::PioClockDivider divider = mm::platform::pioClockDivider(e.pixelClockHz);
        CHECK(divider.intPart == e.intPart);
        CHECK(divider.fracPart == e.fracPart);
        CHECK(divider.actualHz == e.actualHz);
    }
    std::string error;
    mm::platform::Hub75PioEngine engine;
    REQUIRE(engine.open(pioSmall(), error, true));
    CHECK(engine.pixelClockHz() == 10000000);
    engine.shutdown();

    mm::platform::Hub75PioOptions o = pioSmall();
    o.pixelClockHz = 12000000;   // a 3.333 divider: 3 + 85/256
    REQUIRE(engine.open(o, error, true));
    CHECK(std::abs(static_cast<int64_t>(engine.pixelClockHz()) - 12000000) < 12000);   // within 0.1%
    engine.shutdown();

    o.pixelClockHz = 50000000;   // past the divider's floor: 40 MHz
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.pixelClockHz() == 40000000);
    engine.shutdown();
}

TEST_CASE("Hub75PioEngine: a red pixel at (0,0) lights the top plane's first word, and the row ends on a dark latch") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(pioSmall(), error, true));

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;   // pixel (0,0), red
    engine.publishRgb(frame.data());
    // Plane 1, row 0, column 0: slot 1024. R1 of lane 0 is GPIO 11, wide bit 9; the address names the last row, 15, on A-D; OE is clear, the panel lit.
    REQUIRE(waitForWord(1024, 9, 0));
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024] == 0x00F00200u);
    // Row 0's last word latches in the dark: STB set, OE set (blanked), no color.
    CHECK(last[63] == ((1u << mm::platform::kWideBitStb) | (1u << mm::platform::kWideBitOe)));
    engine.shutdown();
}

TEST_CASE("Hub75PioEngine: the repack tables match the reference, color by color and control by control") {
    mm::platform::RepackTables tables;
    mm::platform::buildRepackTables(tables);
    // Every color combination on each lane, the other lanes dark.
    mm::test::forEveryLaneColor([&](const uint16_t (&slot)[3]) {
        CHECK(repacked(tables, slot, 3) == mm::platform::packWideWordReference(slot[0], slot[1], slot[2], 3));
    });
    // Every control combination: the seven control bits ride lane 0's word.
    for (uint16_t v = 0; v < 128; v++) {
        uint16_t slot[3] = {static_cast<uint16_t>(v << 8), 0, 0};
        CHECK(repacked(tables, slot, 3) == mm::platform::packWideWordReference(slot[0], slot[1], slot[2], 3));
    }
    // One lane only: the other lanes' color stays out of the word.
    uint16_t slot[3] = {0x003F, 0x003F, 0x003F};
    const uint32_t wide = repacked(tables, slot, 1);
    CHECK(wide == mm::platform::packWideWordReference(slot[0], slot[1], slot[2], 1));
    CHECK(wide == mm::platform::packWideWordReference(0x003F, 0, 0, 3));
}

TEST_CASE("Hub75PioEngine: latch blanking darkens the words before each latch, and only those") {
    std::vector<uint32_t> wide(2048, 0);   // every word lit: OE clear throughout
    mm::platform::applyLatchBlanking(wide.data(), wide.size(), 64, 2);
    for (size_t i = 0; i < wide.size(); i++) {
        const bool blanked = (wide[i] & (1u << mm::platform::kWideBitOe)) != 0;
        const size_t inRow = i % 64;
        CHECK(blanked == (inRow == 61 || inRow == 62));   // the two words before each row's latch word, the latch itself untouched
    }
}

TEST_CASE("Hub75PioEngine: a second open is refused, and shutdown is idempotent and leaves a blank frame last") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(pioSmall(), error, true));
    CHECK_FALSE(engine.open(pioSmall(), error, true));
    CHECK(error == "PIO engine already running");
    mm::platform::Hub75PioOptions invalid = pioSmall();
    invalid.parallel = 9;
    CHECK_FALSE(engine.open(invalid, error, true));
    CHECK(error == "PIO engine already running");   // the engine's state is refused before the options are read

    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white, so the blank after it is unmistakable
    engine.publishRgb(frame.data());
    REQUIRE(waitForWord(1024, 9, 0));
    engine.shutdown();
    engine.shutdown();   // a second shutdown is harmless
    CHECK_FALSE(engine.isRunning());
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == mm::platform::kBlankWideWord);

    // The engine opens again after a shutdown.
    REQUIRE(engine.open(pioSmall(), error, true));
    engine.shutdown();
}

TEST_CASE("Hub75PioEngine: 8-bit opens, encodes and submits, the narrow row stretched") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    mm::platform::Hub75PioOptions o = pioSmall();
    o.bitDepth = 8;
    REQUIRE(engine.open(o, error, true));
    // 8 planes x 16 rows x a 129-word pass (64 shift words + 65 hold words), against 2,048 at 2-bit: the narrow row stretches, so the frame is more than 8/6 the 6-bit one.
    CHECK(engine.frameSlots() == 16512);
    CHECK(std::abs(engine.refreshHz() - 10000000.0f / 16512.0f) < 1.0f);

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;   // pixel (0,0), red: the full byte encodes, LSB included
    engine.publishRgb(frame.data());
    CHECK(mm::test::waitFirstSubmit(engine));
    CHECK(mm::test::waitUntil([&] { return engine.framesEncoded() > 0; }));   // the refresh thread takes the published frame after its first submit
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75PioEngine: latch blanking strides by the pass, hold words included") {
    // At 8 bits the small geometry's groups are 129 words, not 64: blanking darkens the words before each group's latch (126, 127) and leaves positions 12, 13 lit; plane 7's row 1 shows the top plane's full window, lit except where blanked.
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    mm::platform::Hub75PioOptions o = pioSmall();
    o.bitDepth = 8;
    o.latchBlankWords = 2;
    REQUIRE(engine.open(o, error, true));

    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white: every word lit inside its window
    engine.publishRgb(frame.data());
    REQUIRE(mm::test::waitFirstSubmit(engine));
    std::vector<uint32_t> last(16512);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 16512);

    const size_t group = (7 * 16 + 1) * 129;   // plane 7, row 1
    const uint32_t oe = 1u << mm::platform::kWideBitOe;   // set = blanked, clear = lit
    CHECK((last[group + 12] & oe) == 0);    // lit: a 64-word stride would have blanked this one
    CHECK((last[group + 13] & oe) == 0);    // lit: and this one
    CHECK((last[group + 125] & oe) == 0);   // lit: the word before the blanking
    CHECK((last[group + 126] & oe) != 0);   // blanked: two words before the latch
    CHECK((last[group + 127] & oe) != 0);   // blanked: one word before the latch
    CHECK((last[group + 128] & (1u << mm::platform::kWideBitStb)) != 0);   // the latch word latches
    CHECK((last[group + 128] & oe) != 0);   // and is dark
    engine.shutdown();
}

TEST_CASE("Hub75PioEngine: the backend starts with the clock's divider, the frame's buffers and the edge's program") {
    for (uint32_t clock : {5000000u, 10000000u, 20000000u, 12000000u}) {
        CAPTURE(clock);
        mm::platform::Hub75PioOptions o = pioSmall();
        o.pixelClockHz = clock;
        mm::test::PioRun run(o);
        const mm::platform::StubStartRecord start = mm::platform::stubBackendLastStart();
        const mm::platform::PioClockDivider divider = mm::platform::pioClockDivider(clock);
        CHECK(start.starts == 1);
        CHECK(start.config.clkdivInt == divider.intPart);
        CHECK(start.config.clkdivFrac == divider.fracPart);
        CHECK(start.config.xferBufCount == 4);
        CHECK(start.config.xferBufSize == run.engine.frameSlots() * 4);   // a frame under 64 KiB: one buffer holds it
    }
    // A frame past 64 KiB takes the full buffer: 8-bit is 16,512 words, 66,048 bytes.
    mm::platform::Hub75PioOptions deep = pioSmall();
    deep.bitDepth = 8;
    mm::test::PioRun big(deep);
    CHECK(mm::platform::stubBackendLastStart().config.xferBufSize == 65536);
}

TEST_CASE("Hub75PioEngine: the clock edge picks the program the backend loads") {
    mm::platform::Hub75PioOptions o = pioSmall();
    std::vector<uint16_t> rising, falling;
    {
        mm::test::PioRun run(o);
        rising = mm::platform::stubBackendLastStart().program;
    }
    o.clockFallingEdge = true;
    {
        mm::test::PioRun run(o);
        falling = mm::platform::stubBackendLastStart().program;
        CHECK(mm::platform::stubBackendLastStart().config.programLength == static_cast<int>(falling.size()));
    }
    CHECK_FALSE(rising.empty());
    CHECK_FALSE(falling.empty());
    CHECK(rising != falling);
    mm::test::PioRun again(pioSmall());
    CHECK(mm::platform::stubBackendLastStart().program == rising);
}

TEST_CASE("Hub75PioEngine: the shutdown hands the backend one blank, as deep as the whole DMA ring") {
    mm::test::PioRun run(pioSmall());
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    CHECK(mm::platform::stubBackendBlanks() == 0);
    run.engine.shutdown();
    const mm::platform::StubStartRecord start = mm::platform::stubBackendLastStart();
    CHECK(mm::platform::stubBackendBlanks() == 1);
    // A 2,048-word frame is 8 KiB, so the ring is four 8 KiB buffers: 32 KiB, four frames deep.
    CHECK(mm::platform::stubBackendLastBlankBytes() == start.config.xferBufCount * start.config.xferBufSize);
    CHECK(mm::platform::stubBackendLastBlankBytes() == 4 * 2048 * 4);
    CHECK(mm::platform::stubBackendStops() == 1);
    CHECK(mm::platform::stubBackendLastStopDrained());   // the thread came back, so the stop waits out the blank (a stop that skipped the wait would leave the queue to the kernel)
    run.engine.shutdown();
    CHECK(mm::platform::stubBackendBlanks() == 1);   // still 1: a second shutdown adds no blank
    CHECK(mm::platform::stubBackendStops() == 1);    // and a second shutdown adds no stop
}

TEST_CASE("Hub75PioEngine: a backend that will not open or start refuses the open, and the engine stays stopped") {
    mm::platform::stubBackendReset();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    mm::platform::StubFaults faults;
    faults.failOpen = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(engine.open(pioSmall(), error, true));
    CHECK(error.find("open") != std::string::npos);
    CHECK_FALSE(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "(none)");

    faults = {};
    faults.failStart = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(engine.open(pioSmall(), error, true));
    CHECK(error.find("start") != std::string::npos);
    CHECK_FALSE(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "(none)");
    CHECK(mm::platform::stubBackendLastStart().starts == 0);

    mm::platform::stubBackendReset();
    CHECK(engine.open(pioSmall(), error, true));   // the faults are the stub's, so the next open is clean
    engine.shutdown();
}

// Every submit from now on fails with `code`.
static void failEverySubmit(int code) {
    mm::platform::StubFaults faults;
    faults.failSubmits = -1;
    faults.submitError = code;
    mm::platform::stubBackendSetFaults(faults);
}

// The failures are counted and no frame is shown, whatever code the ioctl returns, and the engine recovers when they stop.
static void checkFailingSubmitsRecover(int code) {
    mm::test::PioRun run(pioSmall());
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    failEverySubmit(code);
    const uint64_t errors = run.engine.submitErrors();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // a submit that was past the fault finishes
    const uint64_t shown = run.engine.framesSubmitted();
    const uint64_t taken = mm::platform::stubBackendSubmittedFrames();
    CHECK(mm::test::waitUntil([&] { return run.engine.submitErrors() >= errors + 5; }));   // five more failures
    CHECK(run.engine.framesSubmitted() == shown);                     // none counted as shown
    CHECK(mm::platform::stubBackendSubmittedFrames() == taken);       // and the stub took none
    CHECK(mm::test::waitUntil([&] { return run.engine.fps() == 0; }, 4000));   // the rate shown falls with the misses
    mm::platform::stubBackendSetFaults({});
    CHECK(mm::test::waitUntil([&] { return run.engine.framesSubmitted() > shown; }));
}

TEST_CASE("Hub75PioEngine: submits that fail are counted and show no frame, and the engine recovers when they stop") {
    checkFailingSubmitsRecover(-EIO);
}

TEST_CASE("Hub75PioEngine: a submit that times out, as the 6.12 kernel's does, is counted and shows no frame, and the engine recovers") {
    checkFailingSubmitsRecover(-ETIMEDOUT);   // what a 6.12 kernel's 1 s wait returns
}

TEST_CASE("Hub75PioEngine: a blank that fails to submit is counted once, and the shutdown still finishes with the pins dark") {
    mm::test::PioRun run(pioSmall());
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    mm::platform::StubFaults faults;
    faults.failBlank = true;   // the frames go out, so the refresh thread's last submit succeeds and it tries the blank
    mm::platform::stubBackendSetFaults(faults);
    CHECK(run.engine.submitErrors() == 0);
    CHECK(run.engine.shutdown());
    CHECK(run.engine.submitErrors() == 1);
    CHECK(mm::platform::stubBackendBlankAttempts() == 1);
    CHECK(mm::platform::stubBackendPinsDark());
    CHECK_FALSE(mm::platform::stubBackendLastStopDrained());   // the blank was skipped, so the stop skips its wait
    CHECK_FALSE(run.engine.isRunning());
}

TEST_CASE("Hub75PioEngine: a FIFO that did not drain after the blank is a note on a dark panel, and the shutdown says the panel is dark") {
    mm::test::PioRun run(pioSmall());
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    mm::platform::StubFaults faults;
    faults.drainTimesOut = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK(run.engine.shutdown());
    CHECK(mm::platform::stubBackendLastStopDrained());   // the blank went out, so the stop waited and polled
    CHECK(mm::platform::stubBackendPinsDark());
    const std::string said = run.engine.shutdownError();
    CHECK(said.find("the panel is dark") != std::string::npos);
    CHECK(said.find("did not drain") != std::string::npos);        // the note rides along
    CHECK(said.find("may hold its last row") == std::string::npos);
    CHECK(said.find("the stop failed") == std::string::npos);
    mm::platform::stubBackendSetFaults({});
    std::string error;
    REQUIRE(run.engine.open(pioSmall(), error, true));
    CHECK(run.engine.shutdown());
    CHECK(run.engine.shutdownError().empty());   // a clean stop leaves the report empty
}

TEST_CASE("Hub75PioEngine: a close after a submit that timed out sends no blank, waits for none, and leaves the panel dark with nothing to report") {
    mm::test::PioRun run(pioSmall());
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    REQUIRE(run.engine.shutdown());   // a first run ends with its blank out and a stop that waited
    REQUIRE(mm::platform::stubBackendLastStopDrained());
    const uint32_t blanksBefore = mm::platform::stubBackendBlankAttempts();
    std::string error;
    REQUIRE(run.engine.open(pioSmall(), error, true));   // the second run's close depends on its own run alone
    REQUIRE(mm::test::waitFirstSubmit(run.engine));
    mm::platform::StubFaults faults;
    faults.failSubmits = -1;
    faults.submitError = -ETIMEDOUT;   // a kernel that waits for delivery, with the SM not draining
    faults.drainTimesOut = true;       // so the FIFO would stay full for a stop that waited
    mm::platform::stubBackendSetFaults(faults);
    REQUIRE(mm::test::waitUntil([&] { return run.engine.submitErrors() > 0; }));
    CHECK(run.engine.shutdown());
    CHECK(mm::platform::stubBackendBlankAttempts() == blanksBefore);   // the second run tried no blank
    CHECK_FALSE(mm::platform::stubBackendLastStopDrained());
    CHECK(mm::platform::stubBackendPinsDark());
    CHECK(run.engine.shutdownError().empty());   // no drain wait, so no timeout to note, and no claim that the panel may hold its row
}

TEST_CASE("Hub75PioEngine: the wire time of the blank is rounded up from the divider, a fraction included") {
    CHECK(mm::platform::pioWireMicros(2048, 400, 0) == 20480);      // 100 kHz: 10 us a word
    CHECK(mm::platform::pioWireMicros(65536, 4, 0) == 6554);        // the ring at 10 MHz: 6,553.6 us
    CHECK(mm::platform::pioWireMicros(65536, 4, 128) == 7373);      // divider 4.5: 7,372.8 us
    CHECK(mm::platform::pioWireMicros(0, 4, 0) == 0);
    CHECK(mm::platform::pioWireMicros(1, 1, 0) == 1);               // a word at the fastest divider is under 1 us, and rounds up
}

TEST_CASE("Hub75PioEngine: the wait for the blank doubles the ring's wire time for the feed lag, at every divider") {
    CHECK(mm::platform::pioBlankWaitMicros(65536, 4, 0) == 2 * 6554);   // 13 ms at 10 MHz
    CHECK(mm::platform::pioBlankWaitMicros(65536, 400, 0) == 2 * 655360);   // 1.3 s at 100 kHz
    CHECK(mm::platform::pioBlankWaitMicros(65536, 4000, 0) == 2 * 6553600);   // 13 s at 10 kHz: the whole wait runs
    CHECK(mm::platform::pioBlankWaitMicros(0, 4, 0) == 0);
}

TEST_CASE("Hub75PioEngine: the shutdown's wire time is the frame and the ring-deep blank, doubled for the feed lag and given half again") {
    // 10 kHz, a 2,048-word frame and an 8,192-word ring: 204.8 ms and 819.2 ms on the wire.
    CHECK(mm::platform::pioShutdownWireMs(2048, 8192, 4000, 0) == 3072);
    CHECK(mm::platform::pioShutdownWireMs(2048, 8192, 4000, 0) > static_cast<uint64_t>(mm::platform::kHub75JoinDeadlineMs));
    CHECK(mm::platform::pioShutdownWireMs(2048, 8192, 4, 0) < static_cast<uint64_t>(mm::platform::kHub75JoinDeadlineMs));   // 10 MHz: 3 ms, the floor decides
    CHECK(mm::platform::pioShutdownWireMs(0, 0, 4, 0) == 0);
    // A big frame counts as much as the ring: 16,512 words at the same clock.
    CHECK(mm::platform::pioShutdownWireMs(16512, 8192, 4000, 0) == 7412);   // (1651.2 + 819.2 ms) x 3, rounded up
    // At least the blank's own doubled wait, which is what the engine's deadline rests on.
    for (const int d : {1, 4, 400, 4000, 65535}) {
        const uint16_t divider = static_cast<uint16_t>(d);
        CHECK(mm::platform::pioShutdownWireMs(2048, 8192, divider, 0) * 1000 >= mm::platform::pioWireMicros(8192, divider, 0) * mm::platform::kPioFeedLagFactor);
    }
}

TEST_CASE("Hub75PioEngine: a failed encode keeps the last good frame submitting, and the next good one replaces it") {
    mm::test::PioRun run(pioSmall());
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;   // pixel (0,0), red
    run.engine.publishRgb(frame.data());
    REQUIRE(waitForWord(1024, 9, 0));
    const uint64_t encoded = run.engine.framesEncoded();

    run.engine.failEncodes(1);
    std::fill(frame.begin(), frame.end(), 0);
    run.engine.publishRgb(frame.data());   // an all-black frame, whose encode fails
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the refresh thread takes it within a frame time of 0.2 ms
    CHECK(run.engine.framesEncoded() == encoded);   // the failed encode leaves the count as it was
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024] == 0x00F00200u);   // the red pixel is still on show

    run.engine.publishRgb(frame.data());   // the same black frame, now encoded
    CHECK(mm::test::waitUntil([&] { return run.engine.framesEncoded() > encoded; }));
    CHECK(mm::test::waitUntil([&] {
        return mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048 && last[1024] != 0x00F00200u;
    }));
}

TEST_CASE("Hub75PioEngine: the frame records tell a repeated frame from a new one") {
    mm::test::PioRun run(pioSmall());
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    REQUIRE(mm::test::waitUntil([&] { return mm::platform::stubBackendSubmittedFrames() >= 3; }));
    mm::platform::StubFrameId ids[mm::platform::kStubFrameIdRing];
    size_t n = mm::platform::stubBackendFrameIds(ids, mm::platform::kStubFrameIdRing);
    REQUIRE(n >= 3);
    for (size_t i = 1; i < n; i++) {
        CHECK(ids[i].index == ids[i - 1].index + 1);       // oldest first, one after another
        CHECK(ids[i].checksum == ids[i - 1].checksum);     // with the publish skipped: the same frame again
        CHECK(ids[i].words == 2048);
    }
    const uint64_t black = ids[0].checksum;

    std::fill(frame.begin(), frame.end(), 200);
    run.engine.publishRgb(frame.data());
    REQUIRE(mm::test::waitUntil([&] {
        mm::platform::StubFrameId id{};
        const size_t k = mm::platform::stubBackendFrameIds(&id, 1);   // the newest one
        return k == 1 && id.checksum != black;
    }));
    mm::platform::StubFrameId newest{};
    REQUIRE(mm::platform::stubBackendFrameIds(&newest, 1) == 1);
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(newest.first == last.front());
    CHECK(newest.last == last.back());
}

TEST_CASE("Hub75PioEngine: on the ring model the engine submits, and the shutdown still leaves a blank") {
    mm::platform::stubBackendReset();
    mm::platform::stubBackendSetQueueModel(mm::platform::StubQueueModel::Ring);
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(pioSmall(), error, true));
    std::vector<uint8_t> frame(64 * 32 * 3, 255);
    engine.publishRgb(frame.data());
    CHECK(mm::test::waitUntil([&] { return engine.framesSubmitted() >= 5; }));
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
    CHECK(mm::platform::stubBackendBlanks() == 1);
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == mm::platform::kBlankWideWord);
    mm::platform::stubBackendReset();
}

// The gate as the program leaves it before the test binary's main closes it: a program's gate is open.
const bool gateOpenAtStartup = mm::platform::pioBackendGateOpen();

TEST_CASE("Hub75PioEngine: the real-backend gate starts open, as a program's does, and the test binary closes it") {
    CHECK(gateOpenAtStartup);
}

TEST_CASE("Hub75PioEngine: an engine opened directly, with no test mode, is refused by the closed gate on every build") {
    // The seam refuses early with its own reason; a path around it, such as this one, meets the closed gate in createPioBackend, so no case reaches /dev/pio0.
    const uint32_t opens = mm::platform::hub75PioTestRealOpens();
    const uint32_t refusals = mm::platform::pioBackendGateRefusals();
    REQUIRE_FALSE(mm::platform::pioBackendGateOpen());   // the test binary closed it before this case
    mm::platform::Hub75PioEngine engine;
    std::string error;
    CHECK_FALSE(engine.open(pioSmall(), error));   // the real backend, as production asks for it
    CHECK(error == mm::platform::kPioTestRefusal);
    CHECK_FALSE(engine.isRunning());
    CHECK(mm::platform::hub75PioTestRealOpens() == opens);
    CHECK(mm::platform::pioBackendGateRefusals() == refusals + 1);   // the closed gate refused it, on every build
}

TEST_CASE("Hub75PioEngine: the real-backend gate is open or closed, a closed gate hands out no real backend, and the stub backend is never behind it") {
    const bool wasOpen = mm::platform::pioBackendGateOpen();
    const uint32_t opens = mm::platform::pioBackendRealOpens();
    mm::platform::setPioBackendGateOpen(false);
    CHECK(mm::platform::createPioBackend(false) == nullptr);
    CHECK(std::string(mm::platform::pioBackendRefusal()) == mm::platform::kPioTestRefusal);
    std::unique_ptr<mm::platform::PioBackend> stub(mm::platform::createPioBackend(true));
    CHECK(stub != nullptr);   // the stub opens whatever the gate says
    CHECK(mm::platform::pioBackendRealOpens() == opens);
    mm::platform::setPioBackendGateOpen(true);
    CHECK(mm::platform::pioBackendGateOpen());
#ifndef MM_HUB75_PIO_HAVE_RP1
    CHECK(mm::platform::createPioBackend(false) == nullptr);   // an open gate in a build with no rp1 backend: the build says why
    CHECK(std::string(mm::platform::pioBackendRefusal()) == mm::platform::kPioNoBackendRefusal);
    CHECK(mm::platform::pioBackendRealOpens() == opens);
#endif
    mm::platform::setPioBackendGateOpen(wasOpen);
}
