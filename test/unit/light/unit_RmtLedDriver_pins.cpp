/// @module RmtLedDriver
/// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/RmtLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"

#include <cstring>

// These tests pin the MULTI-PIN surface: the `pins` / `ledsPerPin` text-control parsing (shared free functions in PinList.h, used by RmtLedDriver and I80Peripheral precedent) and the slice arithmetic down to per-pin symbol offsets. All pure host logic, the RMT peripheral is never touched; on desktop the channel init is inert but parsing and slicing must behave identically, which is exactly what lets CI guard them.

namespace {

void wire(mm::RmtLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    REQUIRE(src.allocate(lights, 3));   // a masked alloc failure would fail cases downstream
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);   // 3 out-channels
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// --- parsePinList -----------------------------------------------------------

// "18,17,16" parses to three pins in list order, the order defines the buffer slices.
TEST_CASE("parsePinList accepts a comma-separated list, in order") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("18,17,16", pins, 8, n) == nullptr);
    REQUIRE(n == 3);
    CHECK(pins[0] == 18);
    CHECK(pins[1] == 17);
    CHECK(pins[2] == 16);
}

// A single pin (the default "18") and spaces around tokens are both fine.
TEST_CASE("parsePinList accepts a single pin and spaces around tokens") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("18", pins, 8, n) == nullptr);
    REQUIRE(n == 1);
    CHECK(pins[0] == 18);

    CHECK(mm::parsePinList(" 18, 17 ", pins, 8, n) == nullptr);
    REQUIRE(n == 2);
    CHECK(pins[1] == 17);
}

// A "lo-hi" token expands to the inclusive range, the same idiom as the IP-destination list, so a human types consecutive pins once. Single pins and ranges mix freely in one CSV.
TEST_CASE("parsePinList expands inclusive ranges and mixes them with single pins") {
    uint16_t pins[16] = {};
    uint8_t n = 0;
    // "20-23" → 20,21,22,23.
    CHECK(mm::parsePinList("20-23", pins, 16, n) == nullptr);
    REQUIRE(n == 4);
    CHECK(pins[0] == 20);
    CHECK(pins[3] == 23);
    // Combo: two ranges plus a single pin, in order → 20,21,22,35,38,39,40 (3 + 1 + 3 = 7 pins).
    CHECK(mm::parsePinList("20-22,35,38-40", pins, 16, n) == nullptr);
    REQUIRE(n == 7);
    CHECK(pins[0] == 20); CHECK(pins[2] == 22);
    CHECK(pins[3] == 35);
    CHECK(pins[4] == 38); CHECK(pins[6] == 40);
    // A single-value "range" (lo==hi) is one pin, not an error.
    CHECK(mm::parsePinList("7-7", pins, 16, n) == nullptr);
    REQUIRE(n == 1);
    CHECK(pins[0] == 7);
}

// A range that runs backwards, or overlaps an existing pin, or overruns the cap, is rejected, the same guards a flat list gets, applied to each expanded pin.
TEST_CASE("parsePinList rejects a backwards range, a range duplicate, and a range over the cap") {
    uint16_t pins[16] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("23-20", pins, 16, n) != nullptr);      // hi < lo
    CHECK(mm::parsePinList("20-22,21", pins, 16, n) != nullptr);   // 21 already in the range
    CHECK(mm::parsePinList("18-", pins, 16, n) != nullptr);        // no hi after the dash
    CHECK(mm::parsePinList("0-5", pins, 4, n) != nullptr);         // 6 pins over a cap of 4
}

TEST_CASE("parsePinList rejects bad input with a static error message") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    // Bad token, trailing comma (empty token), and the empty string are all invalid, the driver idles with the message in its status field.
    CHECK(mm::parsePinList("18,x", pins, 8, n) != nullptr);
    CHECK(mm::parsePinList("18,", pins, 8, n) != nullptr);
    CHECK(mm::parsePinList("", pins, 8, n) != nullptr);
}

// maxPins is the chip's RMT TX-channel cap: 5 pins fail an S3-sized 4, fit a classic 8.
TEST_CASE("parsePinList enforces maxPins (the chip's TX-channel cap)") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    // 5 pins through an S3-sized cap of 4 → rejected.
    CHECK(mm::parsePinList("1,2,3,4,5", pins, 4, n) != nullptr);
    // The same list fits the classic-ESP32 cap of 8.
    CHECK(mm::parsePinList("1,2,3,4,5", pins, 8, n) == nullptr);
    CHECK(n == 5);
}

// The same GPIO twice would double-drive one strand, rejected at parse time.
TEST_CASE("parsePinList rejects duplicate pins") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("18,17,18", pins, 8, n) != nullptr);
}

// The crash guard (WROVER bench 2026-07-13): a value like 999 parses as a valid integer but is not a GPIO, handing it to IDF's gpio_func_sel() faults ("GPIO number error" → reset). parsePinList rejects the WHOLE list when any entry exceeds the chip's MM_MAX_GPIO ceiling, so a garbage pin never reaches hardware; the driver idles with "pin out of range for this chip" in its status. On the host, MM_MAX_GPIO defaults to 63, so 999 (and 64) are out of range, 63 is the last accepted pin.
TEST_CASE("parsePinList rejects an out-of-range pin (the gpio_func_sel crash guard)") {
    uint16_t pins[8] = {};
    uint8_t n = 0;
    CHECK(mm::parsePinList("2,4,999,14", pins, 8, n) != nullptr);   // 999 → reject the whole list
    CHECK(mm::parsePinList("18,64", pins, 8, n) != nullptr);        // one past the host ceiling (63)
    CHECK(mm::parsePinList("63", pins, 8, n) == nullptr);           // the ceiling itself is valid
    CHECK(n == 1);
}

// --- assignCounts -----------------------------------------------------------

// Explicit "100,100,50" maps one count to each pin by position.
TEST_CASE("assignCounts takes explicit per-pin counts") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100,100,50", 3, 250, counts) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(counts[1] == 100);
    CHECK(counts[2] == 50);
}

// A SINGLE number broadcasts: that many on EVERY pin (the NumPy/CSS scalar idiom, "one number = that many each"), NOT "on the first pin only, split the rest".
TEST_CASE("assignCounts broadcasts a single value to every pin") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100", 3, 1000, counts) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(counts[1] == 100);
    CHECK(counts[2] == 100);   // every pin, not just the first
}

// Broadcast is clamped so the running sum never reads past the buffer: with 250 lights and 100/pin, pins 0+1 take 100 each, pin 2 gets the last 50.
TEST_CASE("assignCounts broadcast clamps to the remaining buffer") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100", 3, 250, counts) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(counts[1] == 100);
    CHECK(counts[2] == 50);
}

// A LIST shorter than the pin count still maps what it names, then even-splits the rest over the unlisted pins (distinguishes list-of-one-plus-comma from broadcast).
TEST_CASE("assignCounts maps a short list, even-splits the unlisted remainder") {
    // "100," is a list (has a comma), pin0=100 explicit, pins 1+2 split the 150 left.
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100,", 3, 250, counts) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(counts[1] == 75);
    CHECK(counts[2] == 75);
}

TEST_CASE("assignCounts with an empty list splits evenly, last pin takes the rounding remainder") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("", 3, 100, counts) == nullptr);
    CHECK(counts[0] == 33);
    CHECK(counts[1] == 33);
    CHECK(counts[2] == 34);
}

TEST_CASE("assignCounts clamps so the sum never exceeds the buffer") {
    mm::nrOfLightsType counts[8] = {};
    // Explicit counts overrun the 250-light buffer: second pin clamps to what's left.
    CHECK(mm::assignCounts("200,200", 2, 250, counts) == nullptr);
    CHECK(counts[0] == 200);
    CHECK(counts[1] == 50);
    // A single count larger than the whole buffer clamps to the buffer.
    CHECK(mm::assignCounts("300", 1, 250, counts) == nullptr);
    CHECK(counts[0] == 250);
}

TEST_CASE("assignCounts clamps a pin to the WS2812 ceiling and warns (drives 2048, not zero)") {
    // A 128×128 grid (16384 lights) all on one pin is ~490 ms/frame (FPS 1) on a WS2812 line. The driver drives the first kMaxWs2812LedsPerPin (output stays lit), count clamped, not zeroed, and `warn` is set (not nullptr) so the driver shows a Warning while still running. The return stays nullptr (a clamp is not an idling error).
    mm::nrOfLightsType counts[8] = {};
    const char* warn = nullptr;
    CHECK(mm::assignCounts("", 1, 16384, counts, mm::kMaxWs2812LedsPerPin, &warn) == nullptr);
    CHECK(counts[0] == mm::kMaxWs2812LedsPerPin);
    CHECK(warn != nullptr);
    CHECK(std::strcmp(warn, mm::kClampedWarning) == 0);   // warned, but not an error. Compare CONTENTS:
                                                         // the literal is inline constexpr, so its ADDRESS need not be unique across translation units, clang merges them, GCC does not.

    // Without a cap (a clocked-SPI type), the same 16384 passes unclamped, no warning.
    warn = mm::kClampedWarning;  // ensure it's reset to null on the no-clamp path
    CHECK(mm::assignCounts("", 1, 16384, counts, /*maxPerPin=*/0, &warn) == nullptr);
    CHECK(counts[0] == 16384);
    CHECK(warn == nullptr);

    // The ceiling is per-pin, not total: 4096 over two pins (2048 each) fits, no warning.
    CHECK(mm::assignCounts("", 2, 4096, counts, mm::kMaxWs2812LedsPerPin, &warn) == nullptr);
    CHECK(counts[0] == mm::kMaxWs2812LedsPerPin);
    CHECK(counts[1] == mm::kMaxWs2812LedsPerPin);
    CHECK(warn == nullptr);

    // An explicit SMALL count is the user's choice, not a clamp, must NOT warn.
    CHECK(mm::assignCounts("100", 1, 1000, counts, mm::kMaxWs2812LedsPerPin, &warn) == nullptr);
    CHECK(counts[0] == 100);
    CHECK(warn == nullptr);

    // An oversized SINGLE value broadcasts then clamps: "5000" wants 5000 on every pin over a 6000-light buffer. Broadcast walks the buffer, pin0 takes 5000, pin1 the remaining 1000, pin2 gets 0, then each is clamped to the WS2812 ceiling. So pin0 → 2048 (clamped from 5000, warn), pin1 → 1000 (under the ceiling), pin2 → 0.
    CHECK(mm::assignCounts("5000", 3, 6000, counts, mm::kMaxWs2812LedsPerPin, &warn) == nullptr);
    CHECK(counts[0] == mm::kMaxWs2812LedsPerPin);   // 5000 → clamped to 2048
    CHECK(counts[1] == 1000);                       // remaining buffer after pin0's 5000
    CHECK(counts[2] == 0);                           // buffer already consumed
    CHECK(warn != nullptr);
    CHECK(std::strcmp(warn, mm::kClampedWarning) == 0);
}

TEST_CASE("assignCounts handles a zero-light buffer (0×0×0 grid) as all-zero") {
    mm::nrOfLightsType counts[8] = {0xFF, 0xFF, 0xFF};
    CHECK(mm::assignCounts("", 3, 0, counts) == nullptr);
    CHECK(counts[0] == 0);
    CHECK(counts[1] == 0);
    CHECK(counts[2] == 0);
}

TEST_CASE("assignCounts rejects a bad token") {
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("100,x", 2, 250, counts) != nullptr);
}

TEST_CASE("assignCounts ignores extra counts beyond the pin list") {
    // Robust to any input: a stale longer ledsPerPin after pins shrank is not an error, the extra entries just don't apply.
    mm::nrOfLightsType counts[8] = {};
    CHECK(mm::assignCounts("10,20,30", 2, 100, counts) == nullptr);
    CHECK(counts[0] == 10);
    CHECK(counts[1] == 20);
}

// --- driver-level slicing ----------------------------------------------------

TEST_CASE("RmtLedDriver slices the buffer across pins (even split)") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17,16");
    wire(d, src, corr, 90);

    REQUIRE(d.pinCount() == 3);
    CHECK(d.pinLightCount(0) == 30);
    CHECK(d.pinLightCount(1) == 30);
    CHECK(d.pinLightCount(2) == 30);
    // Slice i starts at sumBefore(i) × outCh × 8 words.
    CHECK(d.pinFrameOffsetBytes(0) == 0);
    CHECK(d.pinFrameOffsetBytes(1) == static_cast<size_t>(30) * 3);
    CHECK(d.pinFrameOffsetBytes(2) == static_cast<size_t>(60) * 3);
}

TEST_CASE("RmtLedDriver slices the buffer per ledsPerPin") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17,16");
    std::strcpy(d.ledsPerPin, "50,20,20");
    wire(d, src, corr, 90);

    REQUIRE(d.pinCount() == 3);
    CHECK(d.pinLightCount(0) == 50);
    CHECK(d.pinLightCount(1) == 20);
    CHECK(d.pinLightCount(2) == 20);
    CHECK(d.pinFrameOffsetBytes(1) == static_cast<size_t>(50) * 3);
    CHECK(d.pinFrameOffsetBytes(2) == static_cast<size_t>(70) * 3);
}

TEST_CASE("RmtLedDriver idles with a status error on a bad pin list") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,nope");
    wire(d, src, corr, 64);

    CHECK(d.pinCount() == 0);            // no pins → tick() emits nothing
    CHECK(d.status() != nullptr);        // the parse error is surfaced, not silent
    // Recovery: fixing the control and rebuilding clears the error.
    std::strcpy(d.pins, "18");
    d.applyState();
    CHECK(d.pinCount() == 1);
    // Config valid → the parse error is cleared and the neutral consumption info shows.
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
    CHECK(std::strstr(d.status() ? d.status() : "", "driving") != nullptr);
}

TEST_CASE("RmtLedDriver with the empty default pins idles cleanly (no pin assumed)") {
    // Pins now default UNSET (the "default only when it cannot do harm" rule, the strand is user-soldered). A fresh, unconfigured driver must idle, not grab a GPIO: zero pins, a status note, and a crash-safe no-op tick().
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    REQUIRE(d.pins[0] == '\0');           // the empty default, not a bench guess
    wire(d, src, corr, 64);               // wire() leaves pins as-is (empty)

    CHECK(d.pinCount() == 0);             // nothing claimed
    CHECK(d.status() != nullptr);         // "set pins" surfaced, not silent
    d.tick();                             // must be a no-op, not a crash
    // Setting pins later brings it live (the user-configures-then-runs flow).
    std::strcpy(d.pins, "18");
    d.applyState();
    CHECK(d.pinCount() == 1);
    // Config valid → the parse error is cleared and the neutral consumption info shows.
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
    CHECK(std::strstr(d.status() ? d.status() : "", "driving") != nullptr);
}

TEST_CASE("RmtLedDriver re-slices when the source buffer changes") {
    // setSourceBuffer must recompute counts (the Drivers container re-passes the buffer on every prepareTree), a grid resize updates the even split.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17");
    wire(d, src, corr, 100);
    CHECK(d.pinLightCount(0) == 50);

    src.allocate(200, 3);
    d.setSourceBuffer(&src);
    d.applyState();
    CHECK(d.pinLightCount(0) == 100);
    CHECK(d.pinLightCount(1) == 100);
}

// --- source-buffer window (start / count) ------------------------------------
//
// Every driver reads the SAME shared buffer and outputs a contiguous slice [start, start+count) of it (count 0 = to the end). Two drivers on disjoint windows is how the onboard status LED (window [0,1)) coexists with the main strip (window [1, N)) without either stealing the other's lights. The slice arithmetic lives on DriverBase (windowSlice/setWindow), pinned here through RmtLedDriver because it is the host-runnable concrete driver.

TEST_CASE("RmtLedDriver window: ledsPerPin distributes over the window, not the whole buffer") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,17");
    d.setWindow(/*start=*/10, /*count=*/40);   // this driver owns lights [10,50)
    wire(d, src, corr, 100);                    // 100-light shared buffer

    REQUIRE(d.pinCount() == 2);
    // The even split is over the 40-light WINDOW, not the 100-light buffer.
    CHECK(d.pinLightCount(0) == 20);
    CHECK(d.pinLightCount(1) == 20);
    CHECK(d.windowStart() == 10);
}

TEST_CASE("RmtLedDriver window: count 0 means the rest of the buffer from start") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18");
    d.setWindow(/*start=*/1, /*count=*/0);   // from light 1 to the end
    wire(d, src, corr, 65);                   // onboard LED took light 0; 64 remain

    REQUIRE(d.pinCount() == 1);
    CHECK(d.pinLightCount(0) == 64);          // 65 - 1 = 64
}

// The DEFAULT window (count_ = kWindowAll = 65535) must mean "all lights", even on a buffer LARGER than 65535. nrOfLightsType is uint32 on a PSRAM board (the desktop test target), so a big grid can exceed 65535, treating the default as a literal 65535 count would silently cap output there. Tested on the window slice directly (a driver's per-pin WS2812 cap would otherwise mask it).
TEST_CASE("window: default kWindowAll resolves to ALL lights on a >65535 buffer") {
    mm::RmtLedDriver d;   // any concrete DriverBase — the window math lives on the base
    // Fresh driver: start_ = 0, count_ = kWindowAll (no setWindow). A 70000-light buffer exceeds 65535.
    CHECK(d.resolveWindowLenForTest(70000) == 70000);   // ALL of it, not capped at 65535
    // An explicit real count still clamps to the buffer, unchanged.
    d.setWindow(/*start=*/0, /*count=*/1000);
    CHECK(d.resolveWindowLenForTest(70000) == 1000);
    // Explicit 0 still means "to the end" (the belt-and-braces sentinel), even past 65535.
    d.setWindow(/*start=*/0, /*count=*/0);
    CHECK(d.resolveWindowLenForTest(70000) == 70000);
}

TEST_CASE("RmtLedDriver window: a size-1 window at 0 is the onboard-LED case") {
    // The pairing that drove this feature: one driver renders ONLY light 0 (an onboard status LED), a second renders the strip from light 1 on. Here we pin the first half, exactly one light, regardless of buffer size.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "48");
    d.setWindow(/*start=*/0, /*count=*/1);
    wire(d, src, corr, 64);

    REQUIRE(d.pinCount() == 1);
    CHECK(d.pinLightCount(0) == 1);           // just the onboard LED
}

TEST_CASE("RmtLedDriver window: a start past the buffer end yields an empty slice") {
    // Robust to any input: a window beyond the current buffer (e.g. the grid shrank) must clamp to zero lights, not read out of bounds.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18");
    d.setWindow(/*start=*/200, /*count=*/10);
    wire(d, src, corr, 64);

    CHECK(d.pinLightCount(0) == 0);           // nothing to drive; tick() is a no-op
    d.tick();                                 // must not crash / overrun
}

// --- tick() robustness -------------------------------------------------------
//
// tick()'s transmit-all/wait-all concurrency body is gated out on the desktop (platform::rmtTxChannels == 0 → it returns at the top), exactly as I80Peripheral::tick() is. So the host can pin only the reachable contract: tick() must never crash or overrun for any pin configuration, grid size, or uninitialized state. The concurrency path itself (parallel transmit, longest-strand cost) is proven on hardware by the real-frame loopback self-test, the platform boundary keeps it out of CI, which is by design.

// tick() is a safe no-op across single-pin, multi-pin and zero-grid configs.
TEST_CASE("RmtLedDriver tick is crash-safe for every pin configuration") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);

    SUBCASE("single pin, populated grid") {
        mm::RmtLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "18");
        wire(d, src, corr, 64);
        d.tick();                       // host: inert; must not crash/overrun
    }
    SUBCASE("multi-pin even split") {
        mm::RmtLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "18,17,16");
        wire(d, src, corr, 90);
        REQUIRE(d.pinCount() == 3);
        d.tick();
    }
    SUBCASE("zero-light grid — counts and offsets stay zero") {
        // A 0-light buffer allocates nothing (allocate() returns false by design), so wire it by hand rather than through the success-asserting helper, the point is that tick() tolerates the empty buffer.
        mm::RmtLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "18,17");
        CHECK_FALSE(src.allocate(0, 3));
        d.defineControls();
        d.setSourceBuffer(&src);
        d.correctionForTest() = corr;
        d.applyState();
        d.tick();                       // 0×0×0 must be a clean no-op
    }
    SUBCASE("tick before any buffer is wired") {
        mm::RmtLedDriver d;
        d.defineControls();
        d.tick();                       // uninitialized: the guards must hold
    }
    CHECK(true);                        // reached here ⇒ no crash in any subcase
}

// --- wire timing ------------------------------------------------------------
//
// A strip's bit rate is not universal: the shipped default satisfies WS2812, WS2812B and SK6812 together, but a 12V WS2811 in its low-speed mode wants twice the bit cell and reads the default as noise past the first few lights (issue #94). The `timing` control is what lets a user say so.

TEST_CASE("the default timing is the one that drives WS2812B and SK6812 alike") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 8);
    const auto& c = d.wireTimingForTest();
    CHECK(c.t0h_ns == 350);
    CHECK(c.t1h_ns == 700);
    CHECK(c.period_ns == 1250);
}

TEST_CASE("selecting 400 kHz doubles the bit cell, which is what a 12V WS2811 strip decodes") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.timing = mm::RmtLedDriver::kTiming400k;
    wire(d, src, corr, 8);
    const auto& c = d.wireTimingForTest();
    CHECK(c.t0h_ns == 500);
    CHECK(c.t1h_ns == 1200);
    CHECK(c.period_ns == 2500);
}

TEST_CASE("the WS2811 fast mode keeps the 1.25 us cell with narrower pulses") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.timing = mm::RmtLedDriver::kTiming800kFast;
    wire(d, src, corr, 8);
    const auto& c = d.wireTimingForTest();
    CHECK(c.t0h_ns == 250);
    CHECK(c.t1h_ns == 600);
    CHECK(c.period_ns == 1250);
}

TEST_CASE("custom timing is taken as the user typed it") {
    // The escape hatch: a strip whose datasheet matches no preset is a control change rather than a firmware release, and the numbers a user finds by trying are ones they can report back.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.timing = mm::RmtLedDriver::kTimingCustom;
    d.t0hNs = 400; d.t1hNs = 850; d.periodNs = 1400;
    wire(d, src, corr, 8);
    const auto& c = d.wireTimingForTest();
    CHECK(c.t0h_ns == 400);
    CHECK(c.t1h_ns == 850);
    CHECK(c.period_ns == 1400);
}

TEST_CASE("custom timing that no chip could decode is ordered rather than emitted") {
    // A "1" pulse shorter than a "0" pulse, or a cell too short to contain the pulse, is not a slow strip: it is a typo. Emitting it would drive the line with something undecodable, so the values are ordered into a shape a receiver can at least read.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.timing = mm::RmtLedDriver::kTimingCustom;
    d.t0hNs = 900; d.t1hNs = 300; d.periodNs = 400;
    wire(d, src, corr, 8);
    const auto& c = d.wireTimingForTest();
    CHECK(c.t1h_ns > c.t0h_ns);
    CHECK(c.period_ns > c.t1h_ns);
}

TEST_CASE("switching timing takes effect without reconfiguring the pins") {
    // Live reconfiguration: a user trying presets against a strip must not have to re-enter their wiring between attempts, and the frame after the change carries the new timing.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strncpy(d.pins, "2,4", sizeof(d.pins));
    wire(d, src, corr, 18);
    CHECK(d.wireTimingForTest().period_ns == 1250);

    d.timing = mm::RmtLedDriver::kTiming400k;
    d.applyState();                                  // what a control change triggers
    CHECK(d.wireTimingForTest().period_ns == 2500);
    CHECK(std::strcmp(d.pins, "2,4") == 0);          // and the wiring is untouched
}
