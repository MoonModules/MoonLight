/// @module platform

/// Pins what the GPIO seam's recorder refuses, on every host, whether the engine is built in or left out: the option rules are plain C++ in hub75_gpio_options.cpp, built everywhere, so a build that has no engine (MSVC) refuses the same walls with the same reasons as one that has.

#include "doctest.h"
#include "platform/Hub75Gpio.h"
#include "platform/rpi/hub75_gpio_options.h"
#include "unit/light/hub75_port.h"

#include <string>

namespace {

mm::platform::Hub75GpioOptions wall() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.panelsPerLane = 2;
    o.parallel = 3;
    return o;
}

// One 32-high Stripe panel: the sign's, driven as 16 rows at 1/8.
mm::platform::Hub75GpioOptions stripePanel() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 8;
    o.multiplexing = mm::platform::kHub75MultiplexStripe;
    return o;
}

}  // namespace

TEST_CASE("Hub75GpioOptions: the rules accept the bench wall and refuse each option outside them, with the rule's reason") {
    std::string error;
    CHECK(mm::platform::hub75GpioCheckOptions(wall(), error));
    struct Case { void (*bend)(mm::platform::Hub75GpioOptions&); const char* reason; };
    const Case cases[] = {
        {[](mm::platform::Hub75GpioOptions& o) { o.bitDepth = 9; }, "bitDepth must be 2..8"},
        {[](mm::platform::Hub75GpioOptions& o) { o.bitDepth = 1; }, "bitDepth must be 2..8"},
        {[](mm::platform::Hub75GpioOptions& o) { o.parallel = 4; }, "parallel must be 1..3"},
        {[](mm::platform::Hub75GpioOptions& o) { o.panelsPerLane = 0; }, "panelsPerLane must be 1..32"},
        {[](mm::platform::Hub75GpioOptions& o) { o.scanRate = 64; }, "scanRate must be 8, 16 or 32"},
        {[](mm::platform::Hub75GpioOptions& o) { o.pwmLsbNs = 49; }, "pwmLsbNs must be 50..3000 ns"},
        {[](mm::platform::Hub75GpioOptions& o) { o.pwmLsbNs = 3001; }, "pwmLsbNs must be 50..3000 ns"},
        {[](mm::platform::Hub75GpioOptions& o) { o.slowdown = -2; }, "slowdown must be -1 (auto) or 0..4"},
        {[](mm::platform::Hub75GpioOptions& o) { o.slowdown = 5; }, "slowdown must be -1 (auto) or 0..4"},
        {[](mm::platform::Hub75GpioOptions& o) { o.multiplexing = 2; }, "multiplexing must be 0 (normal) or 1 (Stripe)"},
        {[](mm::platform::Hub75GpioOptions& o) { o.rows = 32; }, "32-high at 1/16"},
    };
    for (const Case& c : cases) {
        mm::platform::Hub75GpioOptions o = wall();
        c.bend(o);
        CAPTURE(c.reason);
        CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
        CHECK(error.find(c.reason) != std::string::npos);
    }
}

TEST_CASE("Hub75GpioOptions: the LSB time and the slowdown take their bounds, 50 and 3000 ns, -1 and 4") {
    std::string error;
    mm::platform::Hub75GpioOptions o = wall();
    for (const uint16_t lsb : {mm::platform::kHub75GpioPwmLsbMinNs, mm::platform::kHub75GpioPwmLsbMaxNs}) {
        o.pwmLsbNs = lsb;
        CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    }
    o = wall();
    for (const int8_t slowdown : {int8_t{-1}, int8_t{0}, mm::platform::kHub75GpioSlowdownMax}) {
        o.slowdown = slowdown;
        CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    }
}

TEST_CASE("Hub75GpioOptions: Stripe drives rows/4 address steps, so a 32-high panel is 1/8 and a direct 1/16 is refused") {
    std::string error;
    mm::platform::Hub75GpioOptions o = stripePanel();
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o.scanRate = 16;   // the direct scan of the same rows
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("Stripe drives rows/4 address steps") != std::string::npos);
    o = stripePanel();
    o.rows = 64;
    o.scanRate = 16;   // a 64-high Stripe panel is 1/16
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
}

TEST_CASE("Hub75GpioOptions: the driven lane is the logical one under normal and half the rows, double the columns under Stripe") {
    mm::platform::Hub75GpioOptions o = stripePanel();
    o.panelsPerLane = 3;
    CHECK(mm::platform::hub75GpioDrivenWidth(o) == 3 * 128);
    CHECK(mm::platform::hub75GpioDrivenHeight(o) == 16);
    o.multiplexing = mm::platform::kHub75MultiplexNormal;
    CHECK(mm::platform::hub75GpioDrivenWidth(o) == 3 * 64);
    CHECK(mm::platform::hub75GpioDrivenHeight(o) == 32);
}

TEST_CASE("Hub75GpioOptions: a driven lane wider than 65,535 is refused, though the logical lane fits") {
    std::string error;
    mm::platform::Hub75GpioOptions o = stripePanel();
    o.cols = 1100;
    o.panelsPerLane = 32;   // 35,200 logical, 70,400 driven
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("lane wider than 65,535") != std::string::npos);
}

TEST_CASE("Hub75GpioOptions: a Stripe panel whose driven width wraps 16 bits is refused") {
    std::string error;
    mm::platform::Hub75GpioOptions o = stripePanel();
    o.cols = 40000;   // driven, 80,000 wide: a 16-bit width reads 14,464
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("lane wider than 65,535") != std::string::npos);
    CHECK(mm::platform::hub75GpioDrivenWidth(o) == 80000);
    o.multiplexing = mm::platform::kHub75MultiplexNormal;   // direct, 40,000 fits
    o.scanRate = 16;
    o.cols = 40000;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
}

TEST_CASE("Hub75GpioOptions: the recorder refuses the same options, so a build without the engine does not accept what a Pi would not") {
    mm::GpioPortGuard guard;   // the recorder
    mm::platform::Hub75GpioOptions o = wall();
    o.bitDepth = 9;   // outside the encoder's 2..8
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75GpioLastError()).find("bitDepth must be 2..8") != std::string::npos);
    CHECK(mm::platform::hub75GpioWidth() == 0);
    o.bitDepth = 8;
    CHECK(mm::platform::hub75GpioOpen(o));
}

TEST_CASE("Hub75GpioOptions: a wall whose buffers pass the 256 MiB ceiling is refused with its figure and the limit, and one at the ceiling is taken") {
    std::string error;
    mm::platform::Hub75GpioOptions o = wall();
    REQUIRE(mm::refusesPastCeiling(o, mm::platform::hub75GpioBufferBytes, mm::platform::hub75GpioCheckOptions, mm::platform::kGpioMaxBufferBytes, error));
    CHECK(error.find("the wall's buffers exceed the engine's memory limit") != std::string::npos);
    CHECK(error.find("the limit is 256 MB") != std::string::npos);
}

TEST_CASE("Hub75GpioOptions: the buffer figure counts the RGB triple buffer, the strips, the slots, the encoded triple buffer and the blank") {
    // One 64x32 panel at 2 bits, 1/16 scan: 2,048 slots a frame; RGB 3 x 6,144 B, a strip and its remainders 2 x 6,144 B, slots 4,096 B, encoded 3 x 8,192 B, blank 8,192 B.
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    CHECK(mm::platform::hub75GpioBufferBytes(o) == 3 * 6144 + 2 * 6144 + 4096 + 3 * 8192 + 8192);
}
