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
        {[](mm::platform::Hub75PioOptions& o) { o.pixelClockHz = 0; }, "pixelClockHz is under the engine's floor"},
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

TEST_CASE("Hub75PioOptions: the pixel clock floor is 50 kHz, refusing one hertz under it and taking the floor") {
    std::string error;
    mm::platform::Hub75PioOptions o = wall();
    o.pixelClockHz = mm::platform::kPioMinPixelClockHz - 1;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    CHECK(error == "pixelClockHz is under the engine's floor (it must be at least 50000 Hz, which keeps a 64 KiB chunk inside the 6.12 kernel's 1 s wait)");
    o.pixelClockHz = mm::platform::kPioMinPixelClockHz;
    CHECK(mm::platform::hub75PioCheckOptions(o, error));
    // The floor keeps a 64 KiB chunk, 16,384 words at one word a pixel clock, inside the 6.12 kernel's 1 s wait, with the margin stated.
    constexpr uint32_t kChunkWords = 65536 / 4;
    CHECK(mm::platform::kPioMinPixelClockHz >= 3 * kChunkWords);
}

TEST_CASE("Hub75PioOptions: a wall whose buffers pass the 256 MiB ceiling is refused with its figure and the limit, and one at the ceiling is taken") {
    std::string error;
    mm::platform::Hub75PioOptions o = wall();
    o.parallel = 3;
    o.rows = 240;
    o.scanRate = 8;
    o.cols = 2047;
    o.bitDepth = 8;
    // Panels per lane grow the wall one 2,047-pixel step at a time: the last that fits, and the first that does not.
    int fits = 0;
    for (int panels = 1; panels <= mm::platform::kHub75PanelsPerLaneMax; panels++) {
        o.panelsPerLane = static_cast<uint8_t>(panels);
        if (mm::platform::hub75PioBufferBytes(o) > mm::platform::kPioMaxBufferBytes) break;
        fits = panels;
    }
    REQUIRE(fits > 0);
    REQUIRE(fits < mm::platform::kHub75PanelsPerLaneMax);
    o.panelsPerLane = static_cast<uint8_t>(fits);
    CHECK(mm::platform::hub75PioCheckOptions(o, error));
    o.panelsPerLane = static_cast<uint8_t>(fits + 1);
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    const uint64_t need = (mm::platform::hub75PioBufferBytes(o) + 1024 * 1024 - 1) / (1024 * 1024);
    CHECK(error == "the wall's buffers exceed the engine's memory limit (this wall's buffers need " + std::to_string(need) + " MB, the limit is 256 MB)");
}

TEST_CASE("Hub75PioOptions: a buffer total exactly at the ceiling passes, and one byte over is refused") {
    CHECK_FALSE(mm::platform::pioBufferBytesExceedCeiling(mm::platform::kPioMaxBufferBytes));
    CHECK(mm::platform::pioBufferBytesExceedCeiling(mm::platform::kPioMaxBufferBytes + 1));
}
