/// @module platform

/// Pins the GPIO engine against the stub backend: the options it refuses, the geometry it reads back, the first word a red pixel lights, the PWM timing and column-time model it derives, the multiplex and repack tables, the dark-first setup order, and the blank frame its shutdown leaves. No hardware is touched; the stub captures every submit and logs every setup step.
///
/// @technical
///
/// ## Sign bench calibration
///
/// The slowdown index, the three readings, the calibrated column times and the 1% tolerance are in hub75_gpio_backend.h (Column time, Calibration, Tolerance). The readings and frame times are `kReadings` in the code.
///
/// ## Column time and 8-bit rates
///
/// The column-time model is in hub75_gpio_backend.h (Column time, Dark interval and slowdown 0).
///
/// The 8-bit rates are the model's: 511 / 487 / 464 Hz at true slowdowns 1 / 2 / 3 against 681 / 649 / 619 at 6-bit. The sign is shift-bound at every plane and every slowdown, so the 8-bit frame is exactly 8/6 the 6-bit one.
///
/// ## Frame time model
///
/// On the sign at 8-bit and 160 ns LSB, per the longer-of rule in hub75_gpio_backend.h (Predicted frame time), the top plane's OE (160 ns x 128 = 20.48 us) stays under one group's shift (128 cols x 235 ns + 0.5 us = 30.58 us even at slowdown 1), so every plane is shift-bound at every slowdown.
///
/// Where a plane's OE outlasts the shift, the dump's pulse wait paces the wire frame by the same rule (hub75_gpio_dump.h, The sequence).

#include "doctest.h"
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_engine.h"
#include "platform/rpi/hub75_gpio_multiplex.h"
#include "platform/rpi/hub75_gpio_pins.h"
#include "platform/rpi/hub75_gpio_pwm.h"
#include "platform/rpi/hub75_gpio_repack.h"
#include "hub75_engine_bench.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using mm::test::gpioSignOptions;
using mm::test::gpioSmall;

namespace {

// Wait up to `ms` for the stub to have submitted a frame whose word `index` has `bit` set, or all of `expect` when `bit` is -1.
bool waitForWord(size_t index, int bit, uint32_t expect, int ms = 2000) {
    return mm::test::waitForWord(mm::platform::gpioStubCopyLastFrame, index, bit, expect, ms);
}

// One GPIO word repacked from the three lanes' slots, `lanes` of them driven.
uint32_t repacked(const mm::platform::GpioRepackTables& tables, const uint16_t (&slot)[3], int lanes) {
    const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
    uint32_t gpio = 0;
    mm::platform::repackSlotsToGpio(ptrs, lanes, 1, tables, &gpio);
    return gpio;
}

}  // namespace

TEST_CASE("Hub75GpioEngine: the options are refused with reasons before anything opens") {
    std::string error;
    mm::platform::Hub75GpioOptions o = gpioSmall();
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));

    o = gpioSmall(); o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.parallel = 4;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.panelsPerLane = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.panelsPerLane = 33;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.rows = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.bitDepth = 1;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.bitDepth = 9;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    // 7 and 8 open: the GPIO output goes to 8 real planes, the encoder's limit, like the PIO side.
    o = gpioSmall(); o.bitDepth = 7;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.bitDepth = 8;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.pwmLsbNs = 49;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.pwmLsbNs = 3001;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.slowdown = -2;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.slowdown = 5;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.multiplexing = mm::platform::kHub75MultiplexCount;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    // Stripe is the one mapper the engine runs, at the driven scan rows/4: a 32-high Stripe panel is 1/8, and the direct 1/16 scan with Stripe is refused.
    o = gpioSmall(); o.multiplexing = mm::platform::kHub75MultiplexStripe; o.scanRate = 8;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = gpioSmall(); o.multiplexing = mm::platform::kHub75MultiplexStripe;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("Stripe") != std::string::npos);
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    o = gpioSmall(); o.scanRate = 32;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("32-high at 1/16") != std::string::npos);

    // The engine refuses the same geometry on open and stays stopped.
    mm::platform::Hub75GpioEngine engine;
    CHECK_FALSE(engine.open(o, error, true));
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75GpioEngine: a frame publishes and submits, and the geometry reads back") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSmall(), error, true));
    mm::test::checkSmallEngine(engine);
    CHECK(engine.columnTimeNs() == 235);   // auto slowdown resolves to 1 on the stub: 223 + 12 ns a column
    CHECK(engine.lsbActualNs() == 128);   // 130 ns asks divider 32: 128 ns actual
    // Predicted refresh, computed: 2 planes x 16 rows x (64 cols x 235 ns + 0.5 us dark) = 497,280 ns.
    CHECK(std::abs(engine.refreshHz() - 1e9f / 497280.0f) < 1.0f);


    mm::test::publishBlackFrame(engine);
    CHECK(engine.pulseTimeouts() == 0);
    CHECK(engine.lastSubmitUs() > 0);   // the stub slept the wire time
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75GpioEngine: open waits for the refresh thread's realtime answer, so a read right after it never finds the answer unknown") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    for (int i = 0; i < 40; i++) {   // the answer was still unknown in about half of the opens before open waited for it
        REQUIRE(engine.open(gpioSmall(), error, true));
        CHECK(engine.realtimeAnswered());
        engine.shutdown();
    }
}

TEST_CASE("Hub75GpioEngine: the PWM clocking derives the LSB from the PLLD rate, 500 MHz on a Pi 3 and 750 MHz on a Pi 4") {
    constexpr uint32_t kPi3Plld = 500'000'000;
    constexpr uint32_t kPi4Plld = 750'000'000;
    CHECK(mm::platform::hub75GpioPlldHz(mm::platform::PiSoc::Bcm2837) == kPi3Plld);
    CHECK(mm::platform::hub75GpioPlldHz(mm::platform::PiSoc::Bcm2711) == kPi4Plld);
    CHECK(mm::platform::hub75GpioPlldHz(mm::platform::PiSoc::Other) == kPi3Plld);

    // A Pi 3 steps the LSB by 4 ns: divider = lsbNs / 4.
    const mm::platform::GpioPwmTiming fast = mm::platform::hub75GpioPwmTiming(50, kPi3Plld);
    CHECK(fast.divider == 12);
    CHECK(fast.lsbActualNs == 48);
    const mm::platform::GpioPwmTiming def = mm::platform::hub75GpioPwmTiming(130, kPi3Plld);
    CHECK(def.divider == 32);
    CHECK(def.lsbActualNs == 128);
    const mm::platform::GpioPwmTiming slow = mm::platform::hub75GpioPwmTiming(3000, kPi3Plld);
    CHECK(slow.divider == 750);
    CHECK(slow.lsbActualNs == 3000);

    // A Pi 4 steps it by 2.67 ns: divider = lsbNs x 3 / 8, so the stated nanoseconds hold on both.
    const mm::platform::GpioPwmTiming fast4 = mm::platform::hub75GpioPwmTiming(50, kPi4Plld);
    CHECK(fast4.divider == 18);
    CHECK(fast4.lsbActualNs == 48);
    const mm::platform::GpioPwmTiming def4 = mm::platform::hub75GpioPwmTiming(130, kPi4Plld);
    CHECK(def4.divider == 48);
    CHECK(def4.lsbActualNs == 128);
    const mm::platform::GpioPwmTiming slow4 = mm::platform::hub75GpioPwmTiming(3000, kPi4Plld);
    CHECK(slow4.divider == 1125);
    CHECK(slow4.lsbActualNs == 3000);
    CHECK(mm::platform::hub75GpioPwmTiming(160, kPi4Plld).lsbActualNs == 160);   // the sign's LSB lights 160 ns on a Pi 4, not 107

    // Truncation, not rounding: 51 ns takes divider 12 on a Pi 3, like 50 ns.
    CHECK(mm::platform::hub75GpioPwmTiming(51, kPi3Plld).divider == 12);
    // The divider clamps to the 12-bit field, and to 1 below it.
    CHECK(mm::platform::hub75GpioPwmTiming(65535, kPi4Plld).divider == 4095);
    CHECK(mm::platform::hub75GpioPwmTiming(1, kPi3Plld).divider == 1);
    CHECK(mm::platform::hub75GpioPulseTicks(0) == 2);
    CHECK(mm::platform::hub75GpioPulseTicks(5) == 64);
    CHECK(mm::platform::hub75GpioPulseTicks(7) == 256);   // the top plane at 8-bit
}

TEST_CASE("Hub75GpioEngine: the pulse words split at 16 ticks and carry two zero sentinels") {
    // Plane 0: 2 ticks, one word holding the full range.
    const mm::platform::GpioPulseWords p0 = mm::platform::hub75GpioPulseWords(0);
    CHECK(p0.range == 2);
    CHECK(p0.count == 3);
    CHECK(p0.words[0] == 2);
    CHECK(p0.words[1] == 0);
    CHECK(p0.words[2] == 0);

    // Plane 2: 8 ticks, still one word.
    const mm::platform::GpioPulseWords p2 = mm::platform::hub75GpioPulseWords(2);
    CHECK(p2.range == 8);
    CHECK(p2.count == 3);

    // Plane 3: 16 ticks, the split boundary -- eight words of 2.
    const mm::platform::GpioPulseWords p3 = mm::platform::hub75GpioPulseWords(3);
    CHECK(p3.range == 2);
    CHECK(p3.count == 10);
    for (int i = 0; i < 8; i++) CHECK(p3.words[i] == 2);
    CHECK(p3.words[8] == 0);
    CHECK(p3.words[9] == 0);

    // Plane 5: 64 ticks -- eight words of 8.
    const mm::platform::GpioPulseWords p5 = mm::platform::hub75GpioPulseWords(5);
    CHECK(p5.range == 8);
    CHECK(p5.count == 10);
    uint16_t total = 0;
    for (int i = 0; i < 8; i++) total = static_cast<uint16_t>(total + p5.words[i]);
    CHECK(total == 64);

    // Plane 7: 256 ticks, the top plane at 8-bit -- eight words of 32, then the two sentinels.
    const mm::platform::GpioPulseWords p7 = mm::platform::hub75GpioPulseWords(7);
    CHECK(p7.range == 32);
    CHECK(p7.count == 10);
    for (int i = 0; i < 8; i++) CHECK(p7.words[i] == 32);
    total = 0;
    for (int i = 0; i < 8; i++) total = static_cast<uint16_t>(total + p7.words[i]);
    CHECK(total == 256);
    CHECK(p7.words[8] == 0);
    CHECK(p7.words[9] == 0);
}

TEST_CASE("Hub75GpioEngine: the column-time model is calibrated at 223 ns plus 12 ns a slowdown unit") {
    // The sign's column time: ~15 ns of writes and loop overhead and two delay loops a column; slowdown 0 runs no delay loop, so only the writes remain (hub75_gpio_backend.h, Column time).
    CHECK(mm::platform::hub75GpioColumnTimeNs(0) == 15);
    CHECK(mm::platform::hub75GpioColumnTimeNs(1) == 235);
    CHECK(mm::platform::hub75GpioColumnTimeNs(2) == 247);
    CHECK(mm::platform::hub75GpioColumnTimeNs(3) == 259);
    CHECK(mm::platform::hub75GpioColumnTimeNs(4) == 271);
    CHECK(mm::platform::hub75GpioColumnTimeNs(-1) == 15);   // clamped
    CHECK(mm::platform::hub75GpioColumnTimeNs(9) == 271);   // clamped
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::PiSoc::Bcm2711) == 2);
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::PiSoc::Bcm2837) == 1);
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::PiSoc::Other) == 1);
    // The one home for auto slowdown: the resolver the backends and the output's rate prediction share.
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::PiSoc::Bcm2711) == 2);
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::PiSoc::Bcm2837) == 1);
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::PiSoc::Other) == 1);
    CHECK(mm::platform::hub75GpioResolveSlowdown(3, mm::platform::PiSoc::Bcm2711) == 3);
    CHECK(mm::platform::hub75GpioResolveSlowdown(0, mm::platform::PiSoc::Bcm2711) == 0);
}

TEST_CASE("Hub75GpioEngine: the peripherals of a Pi 3 sit at 0x3F000000 and of a Pi 4 at 0xFE000000, and any other host has none") {
    CHECK(mm::platform::hub75GpioPeripheralBase(mm::platform::PiSoc::Bcm2837) == 0x3F000000u);
    CHECK(mm::platform::hub75GpioPeripheralBase(mm::platform::PiSoc::Bcm2711) == 0xFE000000u);
    CHECK(mm::platform::hub75GpioPeripheralBase(mm::platform::PiSoc::Other) == 0u);
}

TEST_CASE("Hub75GpioEngine: the predicted frame time is shift-bound at 130 ns and OE-bound at 3000 ns") {
    // The wall, 6-bit, 130 ns LSB, 235 ns columns: every plane shift-bound, 6 x 32 x (256 cols x 235 ns + 0.5 us) = 11,646,720 ns, ~86 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 256, 6, 130, 235) == 11646720ull);
    // The wall at 3000 ns LSB: planes 0-4 shift-bound (60,660 ns), plane 5 OE-bound (96 us): 32 x (5 x 60,660 + 96,000) = 12,777,600 ns, ~78 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 256, 6, 3000, 235) == 12777600ull);
    // One 64x64 panel, 6-bit, 130 ns: 6 x 32 x (64 x 235 + 500) = 2,983,680 ns, ~335 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 64, 6, 130, 235) == 2983680ull);
    // A group of 65,536 words (256 cols x 32 panels, 128 rows at 1/8 scan) does not fit 16 bits: 6 x 8 x (65,536 x 235 ns + 0.5 us) = 739,270,080 ns.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 65536, 6, 130, 235) == 739270080ull);
    // Stripe on the small panel: 2 planes x 8 rows x (128 cols x 235 ns + 0.5 us dark) = 489,280 ns, the same pixel clocks as direct and half the dark intervals.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 2, 130, 235) == 489280ull);
    // The sign, 6-bit, 160 ns LSB, slowdown 3 (259 ns columns): every plane shift-bound, 6 x 8 x (128 x 259 + 500) = 1,615,296 ns, ~619 Hz, checked against the measured 620 fps by the reconciliation test below.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 6, 160, 259) == 1615296ull);
    // The sign at 3000 ns LSB: planes 0-3 shift-bound (33,652 ns), planes 4-5 OE-bound (48 us, 96 us): 8 x (4 x 33,652 + 48,000 + 96,000) = 2,228,864 ns.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 6, 3000, 259) == 2228864ull);
    // The sign, 8-bit, 160 ns LSB: every plane shift-bound at every slowdown, so 8 x 8 x (128 x 235 + 500) = 1,957,120 ns at slowdown 1, 2,055,424 at 2, 2,153,728 at 3, ~511 / 487 / 464 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 160, 235) == 1957120ull);
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 160, 247) == 2055424ull);
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 160, 259) == 2153728ull);
    // The sign at 8-bit and 3000 ns LSB: planes 0-3 shift-bound, planes 4-7 OE-bound (48, 96, 192, 384 us): 8 x (4 x 33,652 + 48,000 + 96,000 + 192,000 + 384,000) = 6,836,864 ns, ~146 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 3000, 259) == 6836864ull);
}

TEST_CASE("Hub75GpioEngine: the sign's predicted rate matches this engine's shown rate at true slowdowns 1, 2 and 3 within 1%") {
    // The calibration readings: 682, 649 and 620 fps on the sign at 6-bit and 160 ns, shift-bound at every slowdown, so the frame is the model's alone (hub75_gpio_backend.h, Column time).
    struct Reading {
        int slowdown;
        uint64_t frameNs;
        double measuredHz;
    };
    static const Reading kReadings[] = {
        {1, 1467840ull, 682.0},
        {2, 1541568ull, 649.0},
        {3, 1615296ull, 620.0},
    };
    for (const Reading& r : kReadings) {
        const uint64_t frameNs = mm::platform::hub75GpioPredictedFrameNs(
            8, 128, 6, 160, mm::platform::hub75GpioColumnTimeNs(r.slowdown));
        CHECK(frameNs == r.frameNs);
        const double predictedHz = 1e9 / static_cast<double>(frameNs);
        CHECK(std::abs(predictedHz - r.measuredHz) / r.measuredHz < 0.01);
    }

    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSignOptions(), error, true));
    CHECK(engine.columnTimeNs() == 247);
    CHECK(std::abs(engine.refreshHz() - 649.0f) / 649.0f < 0.01f);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the sign's predicted rate at 8-bit is 8/6 the 6-bit frame, by true slowdown") {
    // The sign is shift-bound at every plane at a 160 ns LSB, so 8-bit is 8/6 the 6-bit frame, pinned within 1% of the model.
    struct Reading {
        int slowdown;
        uint64_t frameNs;
        double predictedHz;
    };
    static const Reading kReadings[] = {
        {1, 1957120ull, 510.9},
        {2, 2055424ull, 486.5},
        {3, 2153728ull, 464.3},
    };
    for (const Reading& r : kReadings) {
        const uint64_t frameNs = mm::platform::hub75GpioPredictedFrameNs(
            8, 128, 8, 160, mm::platform::hub75GpioColumnTimeNs(r.slowdown));
        CHECK(frameNs == r.frameNs);
        const double predictedHz = 1e9 / static_cast<double>(frameNs);
        CHECK(std::abs(predictedHz - r.predictedHz) / r.predictedHz < 0.01);
    }

    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    mm::platform::Hub75GpioOptions o = gpioSignOptions();
    o.bitDepth = 8;
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.columnTimeNs() == 247);
    CHECK(std::abs(engine.refreshHz() - 486.5f) / 486.5f < 0.01f);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: a red pixel at (0,0) lights the top plane's first word, and no control bit leaks into the stream") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSmall(), error, true));

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 128;   // pixel (0,0), red at half: plane 0 dark, plane 1 lit (bit 7 set, bit 6 clear)
    engine.publishRgb(frame.data());
    // Plane 1, row 0, column 0: slot 1024. R1 of lane 0 is GPIO 11.
    REQUIRE(waitForWord(1024, 11, 0));
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024] == 0x800u);   // only GPIO 11: the color bit, no address, no latch, no OE
    CHECK(last[0] == 0u);          // plane 0: the half-red pixel's low bit is clear
    CHECK(last[63] == 0u);         // row 0's last word: no color, and the latch rides the CPU, not the stream
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the multiplex table holds normal first and Stripe second, and an unknown type is direct") {
    using mm::platform::hub75GpioMultiplexEntry;
    CHECK(mm::platform::kHub75MultiplexCount == 2);
    // Normal first, Stripe second: the control's order and the table's, Stripe being one mapping among hzeller's 24, not the default.
    CHECK(std::string(hub75GpioMultiplexEntry(0).name) == "normal");
    for (uint8_t i = 0; i < mm::platform::kHub75MultiplexCount; i++) CHECK(std::string(hub75GpioMultiplexEntry(i).name) == mm::platform::kHub75GpioMultiplexNames[i]);   // the table and the select share one list
    CHECK(std::string(hub75GpioMultiplexEntry(1).name) == "Stripe");
    CHECK(hub75GpioMultiplexEntry(0).stretch == 1);
    CHECK(hub75GpioMultiplexEntry(1).stretch == 2);
    CHECK(hub75GpioMultiplexEntry(0).scanDivisor == 2);
    CHECK(hub75GpioMultiplexEntry(1).scanDivisor == 4);
    // An unknown type falls back to the first entry, normal.
    CHECK(std::string(hub75GpioMultiplexEntry(2).name) == "normal");
    CHECK(hub75GpioMultiplexEntry(2).stretch == 1);
    CHECK(hub75GpioMultiplexEntry(2).scanDivisor == 2);
    // Normal is the identity, through the table.
    const auto p = mm::platform::hub75GpioMultiplexMap(0, 5, 7, 32, 64);
    CHECK(p.x == 5);
    CHECK(p.y == 7);
    const auto q = mm::platform::hub75GpioMultiplexMap(0, 63, 31, 32, 64);
    CHECK(q.x == 63);
    CHECK(q.y == 31);
}

TEST_CASE("Hub75GpioEngine: the Stripe map folds a 32x64 panel into 16 driven rows x 128 columns") {
    using mm::platform::hub75GpioMultiplexMap;
    // The Stripe entry, through the table: every row of a 32x64 panel, driven row (y/16)*8 + y%8, columns +64 where (y%16) < 8, both halves of each 16-row group, at both ends of the row.
    for (uint8_t y = 0; y < 32; y++) {
        const auto p0 = hub75GpioMultiplexMap(1, 0, y, 32, 64);
        const auto p63 = hub75GpioMultiplexMap(1, 63, y, 32, 64);
        const uint8_t expectRow = static_cast<uint8_t>((y / 16) * 8 + y % 8);
        const bool fold = (y % 16) < 8;
        CHECK(p0.y == expectRow);
        CHECK(p63.y == expectRow);
        CHECK(p0.x == (fold ? 64 : 0));
        CHECK(p63.x == (fold ? 127 : 63));
    }
    // The fold boundary: (y%16) == 7 folds, == 8 does not, in both groups.
    CHECK(hub75GpioMultiplexMap(1, 0, 7, 32, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 8, 32, 64).x == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 23, 32, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 24, 32, 64).x == 0);
    // The address at the group boundaries: y=15 and y=16, and y=31.
    CHECK(hub75GpioMultiplexMap(1, 0, 15, 32, 64).y == 7);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 32, 64).y == 8);
    CHECK(hub75GpioMultiplexMap(1, 0, 31, 32, 64).y == 15);
    // Two logical rows share each driven row, on opposite halves.
    CHECK(hub75GpioMultiplexMap(1, 0, 0, 32, 64).y == hub75GpioMultiplexMap(1, 0, 8, 32, 64).y);
    CHECK(hub75GpioMultiplexMap(1, 0, 0, 32, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 8, 32, 64).x == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 32, 64).y == hub75GpioMultiplexMap(1, 0, 24, 32, 64).y);
    // A 64-row panel folds the same way, in 32-row groups.
    CHECK(hub75GpioMultiplexMap(1, 0, 0, 64, 64).y == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 15, 64, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 15, 64, 64).y == 15);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 64, 64).x == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 64, 64).y == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 31, 64, 64).y == 15);
    CHECK(hub75GpioMultiplexMap(1, 0, 32, 64, 64).y == 16);
    CHECK(hub75GpioMultiplexMap(1, 0, 63, 64, 64).y == 31);
    CHECK(hub75GpioMultiplexMap(1, 0, 63, 64, 64).x == 0);
    // A panel too short to fold maps to itself.
    CHECK(hub75GpioMultiplexMap(1, 5, 1, 2, 64).x == 5);
    CHECK(hub75GpioMultiplexMap(1, 5, 1, 2, 64).y == 1);
    // The entry's own function agrees with the table: the table selects the mapping and leaves it as the entry defines it.
    for (uint8_t y = 0; y < 32; y++) {
        const auto viaTable = hub75GpioMultiplexMap(1, 0, y, 32, 64);
        const auto direct = mm::platform::hub75GpioStripeMap(0, y, 32, 64);
        CHECK(viaTable.x == direct.x);
        CHECK(viaTable.y == direct.y);
    }
}

TEST_CASE("Hub75GpioEngine: Stripe halves the scan and the rows and doubles the columns; normal is unchanged") {
    using mm::platform::hub75GpioMultiplexScanRate;
    using mm::platform::hub75GpioMultiplexDrivenRows;
    using mm::platform::hub75GpioMultiplexDrivenCols;
    CHECK(hub75GpioMultiplexScanRate(0, 32) == 16);
    CHECK(hub75GpioMultiplexScanRate(0, 64) == 32);
    CHECK(hub75GpioMultiplexScanRate(1, 32) == 8);   // the sign: 1/8 scan of the driven 16 rows
    CHECK(hub75GpioMultiplexScanRate(1, 64) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(0, 32) == 32);
    CHECK(hub75GpioMultiplexDrivenRows(1, 32) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(1, 64) == 32);
    CHECK(hub75GpioMultiplexDrivenCols(0, 64) == 64);
    CHECK(hub75GpioMultiplexDrivenCols(1, 64) == 128);
    CHECK(hub75GpioMultiplexDrivenCols(1, 128) == 256);
    // An unknown type is direct.
    CHECK(hub75GpioMultiplexScanRate(2, 32) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(2, 32) == 32);
    CHECK(hub75GpioMultiplexDrivenCols(2, 64) == 64);
}

TEST_CASE("Hub75GpioEngine: Stripe folds four logical rows onto one address step, and normal leaves each on its own") {
    // One address step, four logical rows: rows 1, 9, 17 and 25 all ride step 1, rows 1 and 17 folded to column 64 (R1 and R2), rows 9 and 25 at column 0 (R1 and R2).
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[(1 * 64 + 0) * 3] = 128;    // (0,1): driven (64,1), R1, step 1
    frame[(9 * 64 + 0) * 3] = 128;    // (0,9): driven (0,1), R1, step 1
    frame[(17 * 64 + 0) * 3] = 128;   // (0,17): driven (64,9), R2, step 1
    frame[(25 * 64 + 0) * 3] = 128;   // (0,25): driven (0,9), R2, step 1
    std::vector<uint32_t> last(2048);

    mm::platform::Hub75GpioOptions o = gpioSmall();
    o.multiplexing = mm::platform::kHub75MultiplexStripe;
    o.scanRate = 8;   // Stripe drives the 16 driven rows at 1/8
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(o, error, true));
    // The canvas stays the logical 64x32; the driven frame is 128 wide and 16 high, the same 2,048 slots a frame.
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.drivenWidth() == 128);
    CHECK(engine.drivenHeight() == 16);
    CHECK(engine.frameSlots() == 2048);
    // Predicted refresh, computed: 2 planes x 8 rows x (128 cols x 235 ns + 0.5 us dark) = 489,280 ns.
    CHECK(std::abs(engine.refreshHz() - 1e9f / 489280.0f) < 1.0f);
    engine.publishRgb(frame.data());
    // Plane 1, step 1: slots 1024 + 128 + x. R1 of lane 0 is GPIO 11, R2 GPIO 8.
    REQUIRE(waitForWord(1024 + 128 + 64, 11, 0));
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024 + 128 + 64] == 0x900u);   // rows 1 and 17 share the word: R1 + R2, column 64
    CHECK(last[1024 + 128 + 0] == 0x900u);    // rows 9 and 25 share the word: R1 + R2, column 0
    CHECK(last[1024 + 4 * 128] == 0u);        // step 4 is dark: a stripes-of-4 shape would put row 1 there
    CHECK(last[128 + 64] == 0u);              // plane 0: the half-red pixels' low bit is clear
    CHECK(last[128 + 0] == 0u);
    engine.shutdown();

    // Normal: each row stays on its own step, rows 1 and 17 sharing step 1 (R1 and R2), rows 9 and 25 sharing step 9, all at column 0.
    o.multiplexing = mm::platform::kHub75MultiplexNormal;
    o.scanRate = 16;
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine plain;
    REQUIRE(plain.open(o, error, true));
    CHECK(plain.drivenWidth() == 64);
    CHECK(plain.drivenHeight() == 32);
    plain.publishRgb(frame.data());
    REQUIRE(waitForWord(1024 + 1 * 64, 11, 0));
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024 + 1 * 64] == 0x900u);
    CHECK(last[1024 + 9 * 64] == 0x900u);
    CHECK(last[1024 + 128 + 64] == 0u);   // the folded word is dark: step 3, column 0
    plain.shutdown();
}

TEST_CASE("Hub75GpioEngine: Stripe with two panels a lane folds each panel into its own stretch of the driven row") {
    // Two 64x32 panels in a lane, 128 logical columns, driven as 256 columns of 16 rows: panel 1's columns sit at driven 128 and up, its folded rows 64 columns further.
    mm::platform::Hub75GpioOptions o = gpioSmall();
    o.panelsPerLane = 2;
    o.multiplexing = mm::platform::kHub75MultiplexStripe;
    o.scanRate = 8;
    mm::test::GpioRun run(o);
    auto& engine = run.engine;
    CHECK(engine.drivenWidth() == 256);
    CHECK(engine.drivenHeight() == 16);
    REQUIRE(engine.frameSlots() == 4096);   // 2 planes x 8 rows x 256 columns

    std::vector<uint8_t> frame(128 * 32 * 3, 0);
    auto lightRed = [&](int x, int y) { frame[(static_cast<size_t>(y) * 128 + x) * 3] = 128; };   // plane 1, R1 or R2 by the driven row
    lightRed(0, 1);    // panel 0, row 1 folds: driven (64, 1)
    lightRed(64, 1);   // panel 1, row 1 folds: driven (128 + 64, 1)
    lightRed(64, 9);   // panel 1, row 9 does not fold: driven (128 + 0, 1)
    engine.publishRgb(frame.data());
    constexpr size_t kPlane1Step1 = 8 * 256 + 256;   // plane 1's groups follow plane 0's 8, and step 1 is the second
    REQUIRE(waitForWord(kPlane1Step1 + 192, 11, 0));
    std::vector<uint32_t> last(4096);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 4096);
    CHECK(last[kPlane1Step1 + 64] == 0x800u);    // panel 0's folded pixel, lane 0's R1
    CHECK(last[kPlane1Step1 + 192] == 0x800u);   // panel 1's folded pixel: its stretch, 128, plus the fold, 64
    CHECK(last[kPlane1Step1 + 128] == 0x800u);   // panel 1's unfolded pixel, at its stretch alone
    size_t lit = 0;
    for (const uint32_t w : last) lit += w != 0;
    CHECK(lit == 3);                              // no other word is lit: the copies do not overlap
}

TEST_CASE("Hub75GpioEngine: the sign's Stripe geometry -- 3 parallel lanes, 64x96 logical, 128 driven columns") {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 3;
    o.scanRate = 8;
    o.multiplexing = mm::platform::kHub75MultiplexStripe;
    o.bitDepth = 6;
    o.pwmLsbNs = 160;
    o.slowdown = 3;
    std::string error;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 96);
    CHECK(engine.lanes() == 3);
    CHECK(engine.drivenWidth() == 128);
    CHECK(engine.drivenHeight() == 16);
    CHECK(engine.frameSlots() == 6144);   // 6 planes x 8 rows x 128 columns
    CHECK(engine.columnTimeNs() == 259);   // slowdown 3: 223 + 3*12 ns a column
    // Predicted refresh, computed: 6 planes x 8 rows x (128 cols x 259 ns + 0.5 us dark) = 1,615,296 ns, ~619 Hz.
    CHECK(std::abs(engine.refreshHz() - 1e9f / 1615296.0f) < 1.0f);
    std::vector<uint8_t> frame(64 * 96 * 3, 0);
    engine.publishRgb(frame.data());
    // Wait for the encode: the refresh thread re-submits the initial frame while the published one waits, so only the encode count shows the published frame was encoded.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (engine.framesEncoded() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesEncoded() > 0);
    while (engine.framesSubmitted() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesSubmitted() > 0);
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the repack tables match the reference, and control bits never reach the word") {
    mm::platform::GpioRepackTables tables;
    mm::platform::buildGpioRepackTables(tables);
    // Every color combination on each lane, the other lanes dark.
    mm::test::forEveryLaneColor([&](const uint16_t (&slot)[3]) {
        CHECK(repacked(tables, slot, 3) == mm::platform::packGpioWordReference(slot[0], slot[1], slot[2], 3));
    });
    // Every control combination on lane 0's word: the address, latch and OE bits leave the word unchanged, since the dump loop and the pulser own those lines and the repack strips them.
    for (uint16_t v = 0; v < 128; v++) {
        uint16_t slot[3] = {static_cast<uint16_t>((v << 8) | 0x3F), 0, 0};
        CHECK(repacked(tables, slot, 3) == mm::platform::packGpioWordReference(0x003F, 0, 0, 3));
    }
    // One lane only: the other lanes' color stays out of the word.
    uint16_t slot[3] = {0x003F, 0x003F, 0x003F};
    const uint32_t gpio = repacked(tables, slot, 1);
    CHECK(gpio == mm::platform::packGpioWordReference(slot[0], slot[1], slot[2], 1));
    CHECK(gpio == mm::platform::packGpioWordReference(0x003F, 0, 0, 3));
}

TEST_CASE("Hub75GpioEngine: the stub backend logs the setup steps, dark values first") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSmall(), error, true));
    // The stub writes this log itself, so the case pins the stub's order and the engine's calls into it; the hardware backend's own order (hub75_gpio_backend_hw.cpp, Dark-first setup) is shown by the bench alone.
    CHECK(mm::platform::gpioStubSetupLog() == "dark-values,directions,pwm-config,oe-mux");
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: an open engine holds the bytes the options total, and a shut-down or failed one holds 0") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSmall(), error, true));
    CHECK(engine.heapBytes() == mm::platform::hub75GpioBufferBytes(gpioSmall()));   // the strip and the remainders counted with the frame buffers
    engine.shutdown();
    CHECK(engine.heapBytes() == 0);
    engine.failAllocations(true);
    CHECK_FALSE(engine.open(gpioSmall(), error, true));
    CHECK(engine.heapBytes() == 0);
    engine.failAllocations(false);
    engine.failEncodes(-1);   // the first frame's encode fails, after the buffers were taken
    CHECK_FALSE(engine.open(gpioSmall(), error, true));
    CHECK(engine.heapBytes() == 0);
    engine.failEncodes(0);
}

TEST_CASE("Hub75GpioEngine: a frame an OE pulse timed out in counts as a failed submit, and the refresh goes on") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSmall(), error, true));
    mm::test::publishBlackFrame(engine);
    CHECK(engine.submitErrors() == 0);
    mm::platform::gpioStubTimeOutPulseWaits(1);
    REQUIRE(mm::test::waitUntil([&] { return engine.submitErrors() == 1; }));
    CHECK(engine.pulseTimeouts() == 1);
    const uint64_t shown = engine.framesSubmitted();
    CHECK(mm::test::waitUntil([&] { return engine.framesSubmitted() > shown; }));   // the next frame shows straight away
    CHECK(engine.submitErrors() == 1);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: a second open is refused, and shutdown is idempotent and leaves a blank frame last") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(gpioSmall(), error, true));
    CHECK_FALSE(engine.open(gpioSmall(), error, true));
    CHECK(error == "the engine is already running");

    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white, so the blank after it is unmistakable
    engine.publishRgb(frame.data());
    REQUIRE(waitForWord(1024, 11, 0));
    engine.shutdown();
    engine.shutdown();   // a second shutdown is harmless
    CHECK_FALSE(engine.isRunning());
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == 0u);   // every color bit clear

    // The engine opens again after a shutdown, and the second open runs the full dark-first setup again (release and re-acquire inside one process).
    REQUIRE(engine.open(gpioSmall(), error, true));
    CHECK(mm::platform::gpioStubSetupLog() ==
          "dark-values,directions,pwm-config,oe-mux,dark-values,directions,pwm-config,oe-mux");
    engine.shutdown();
}
