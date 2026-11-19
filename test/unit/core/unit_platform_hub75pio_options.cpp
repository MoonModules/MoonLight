/// @module platform

/// Pins what the PIO seam's recorder refuses, on every host, whether the engine is built in or left out: the option rules are plain C++ in hub75_pio_options.cpp, built everywhere, so a build that has no engine (MSVC) refuses the same walls with the same reasons as one that has.

#include "doctest.h"
#include "platform/Hub75Pio.h"
#include "platform/rpi/hub75_pio_options.h"
#include "unit/light/hub75_port.h"

#include <string>

namespace {

mm::platform::Hub75PioOptions wall() {
    mm::platform::Hub75PioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.panelsPerLane = 2;
    o.parallel = 3;
    return o;
}

}  // namespace

TEST_CASE("Hub75PioOptions: the rules accept the bench wall and refuse each option outside them, with the rule's reason") {
    std::string error;
    CHECK(mm::platform::hub75PioCheckOptions(wall(), error));
    struct Case { void (*bend)(mm::platform::Hub75PioOptions&); const char* reason; };
    const Case cases[] = {
        {[](mm::platform::Hub75PioOptions& o) { o.bitDepth = 9; }, "bitDepth must be 2..8"},
        {[](mm::platform::Hub75PioOptions& o) { o.bitDepth = 1; }, "bitDepth must be 2..8"},
        {[](mm::platform::Hub75PioOptions& o) { o.parallel = 4; }, "parallel must be 1..3"},
        {[](mm::platform::Hub75PioOptions& o) { o.panelsPerLane = 0; }, "panelsPerLane must be 1..32"},
        {[](mm::platform::Hub75PioOptions& o) { o.scanRate = 64; }, "scanRate must be 8, 16 or 32"},
        {[](mm::platform::Hub75PioOptions& o) { o.pixelClockHz = 0; }, "pixelClockHz must be non-zero"},
        {[](mm::platform::Hub75PioOptions& o) { o.latchBlankWords = 9; }, "latchBlankWords must be 0..8"},
        {[](mm::platform::Hub75PioOptions& o) { o.rows = 32; }, "32-high at 1/16"},
    };
    for (const Case& c : cases) {
        mm::platform::Hub75PioOptions o = wall();
        c.bend(o);
        CAPTURE(c.reason);
        CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
        CHECK(error.find(c.reason) != std::string::npos);
    }
}

TEST_CASE("Hub75PioOptions: the recorder refuses the same options, so a build without the engine does not accept what a Pi would not") {
    mm::PioPortGuard guard;   // the recorder
    mm::platform::Hub75PioOptions o = wall();
    o.bitDepth = 9;   // outside the encoder's 2..8
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("bitDepth must be 2..8") != std::string::npos);
    CHECK(mm::platform::hub75PioWidth() == 0);
    o.bitDepth = 8;
    CHECK(mm::platform::hub75PioOpen(o));
}
