/// @module Hub75Fit

#include "doctest.h"
#include "light/drivers/Hub75Fit.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace mm;

// The expected texts are written by hand; the rates in them come from the refresh page's formula worked by hand rather than taken from the code under test. 128 by 64 panels at 1/32 scan, 8 bits.

namespace {

// A card's geometry as the messages read it: one panel's rows, columns, row addresses and bit planes, then the lane and the outputs the board drives.
CardGeometry wall(uint16_t rows, uint16_t cols, uint16_t scan, uint8_t bits, uint16_t panelsPerLane, uint8_t maxLanes, const char* board) {
    CardGeometry g;
    g.panel.rows = rows; g.panel.cols = cols; g.panel.scanRows = scan; g.panel.bits = bits; g.panel.lsbNs = 130;   // the refresh page's lit time
    g.panelsPerLane = panelsPerLane; g.maxLanes = maxLanes; g.board = board;
    return g;
}

// What one geometry's message needs; out of line, so the sweep's frame stays under the stack limit.
DOCTEST_NOINLINE size_t lengthOf(uint16_t rows, uint16_t cols, uint32_t panels, uint16_t panelsPerLane, uint8_t maxLanes) {
    const GeometryFit why = checkGeometry(panels, panelsPerLane, maxLanes);
    if (why == GeometryFit::Ok) return 0;
    static char buf[4096];
    const size_t n = geometryMessage(why, panels, wall(rows, cols, rows / 2, 8, panelsPerLane, maxLanes, "Pi PIO"), buf, sizeof(buf));
    return n == std::strlen(buf) ? n : SIZE_MAX;   // a cut inside a buffer this size fails the caller's check
}

// The longest text one panel size and count write, over every lane length up to 64 and board of up to six lanes.
size_t longestFor(uint16_t rows, uint16_t cols, uint32_t panels) {
    size_t longest = 0;
    for (uint16_t i = 0; i < 64 * 6; i++) longest = std::max(longest, lengthOf(rows, cols, panels, static_cast<uint16_t>(1 + i / 6), static_cast<uint8_t>(1 + i % 6)));
    return longest;
}

// Every geometry a card could meet, at the sizes panels come in: the longest text any of them writes.
size_t longestMessage() {
    size_t longest = 0;
    const uint16_t rowsOf[] = {16, 32, 64};
    const uint16_t colsOf[] = {32, 64, 128, 256};
    for (uint16_t rows : rowsOf)
        for (uint16_t cols : colsOf)
            for (uint32_t panels = 1; panels <= 64; panels++) longest = std::max(longest, longestFor(rows, cols, panels));
    return longest;
}

std::string message(GeometryFit why, uint32_t panels, uint16_t panelsPerLane, uint8_t maxLanes, const char* board = "Pi PIO") {
    static char buf[kGeometryMessageMax];   // static: the test frames stay under the stack limit
    const size_t n = geometryMessage(why, panels, wall(64, 128, 32, 8, panelsPerLane, maxLanes, board), buf, sizeof(buf));
    INFO("returned " << n);
    return std::string(buf);
}

} // namespace

TEST_CASE("Hub75Fit: panels that cable into lanes of the card's length are fine") {
    CHECK(checkGeometry(6, 2, 3) == GeometryFit::Ok);    // 3 lanes of 2
    CHECK(checkGeometry(6, 3, 3) == GeometryFit::Ok);    // 2 lanes of 3
    CHECK(checkGeometry(6, 6, 3) == GeometryFit::Ok);    // 1 lane of 6
    CHECK(checkGeometry(1, 1, 3) == GeometryFit::Ok);
}

TEST_CASE("Hub75Fit: no panel, a zero panelsPerLane, uneven lanes and too many lanes are named apart") {
    CHECK(checkGeometry(0, 2, 3) == GeometryFit::Empty);
    CHECK(checkGeometry(6, 0, 3) == GeometryFit::Empty);
    CHECK(checkGeometry(7, 2, 3) == GeometryFit::Indivisible);
    CHECK(checkGeometry(6, 4, 3) == GeometryFit::Indivisible);
    CHECK(checkGeometry(2, 3, 3) == GeometryFit::Indivisible);   // fewer panels than one lane
    CHECK(checkGeometry(6, 2, 1) == GeometryFit::TooManyLanes);   // 3 lanes on a one-lane board
    CHECK(checkGeometry(12, 2, 3) == GeometryFit::TooManyLanes);  // 6 lanes
}

TEST_CASE("Hub75Fit: an uneven set names the fix in full, and the way to keep the lanes built") {
    CHECK(message(GeometryFit::Indivisible, 7, 2, 3) ==
          "7 panels cannot be cabled in lanes of 2. Cable all 7 (rates computed from the pixel clock): 1 lane of 7 (896x64, 72 Hz). "
          "Keep lanes of 2 (rates computed from the pixel clock): use 6 panels, 1 spare, as 3 lanes (256x192, 243 Hz).");
    CHECK(message(GeometryFit::Indivisible, 6, 4, 3) ==
          "6 panels cannot be cabled in lanes of 4. Cable all 6 (rates computed from the pixel clock): 3 lanes of 2 (256x192, 243 Hz); 2 lanes of 3 (384x128, 165 Hz); "
          "1 lane of 6 (768x64, 83 Hz). Keep lanes of 4 (rates computed from the pixel clock): use 4 panels, 2 spare, as 1 lane (512x64, 124 Hz), "
          "or add 2 panels for 2 lanes (512x128, 124 Hz).");
}

TEST_CASE("Hub75Fit: a set no panelsPerLane of the card's can use whole skips to keeping the lanes") {
    CHECK(message(GeometryFit::Indivisible, 41, 2, 3) ==
          "41 panels cannot be cabled in lanes of 2. Keep lanes of 2 (rates computed from the pixel clock): use 6 panels, 35 spare, as 3 lanes (256x192, 243 Hz).");
}

TEST_CASE("Hub75Fit: one panel is named in the singular") {
    const std::string s = message(GeometryFit::Indivisible, 1, 2, 3);
    CHECK(s.rfind("1 panel cannot be cabled in lanes of 2. Cable all 1 (rates computed from the pixel clock): 1 lane of 1 (128x64, ", 0) == 0);
    CHECK(s.find("add 1 panel for 1 lane (256x64, ") != std::string::npos);
}

TEST_CASE("Hub75Fit: a board with too few lanes says how many it drives") {
    CHECK(message(GeometryFit::TooManyLanes, 6, 2, 1, "Pi PIO") ==
          "6 panels in lanes of 2 need 3 lanes, but the Pi PIO board drives at most 1. Cable all 6 (rates computed from the pixel clock): 1 lane of 6 (768x64, 83 Hz). "
          "Keep lanes of 2 (rates computed from the pixel clock): use 2 panels, 4 spare, as 1 lane (256x64, 243 Hz).");
}

TEST_CASE("Hub75Fit: no whole panel in the layout is said plainly") {
    CHECK(message(GeometryFit::Empty, 0, 2, 3) == "The layout holds fewer lights than one 128x64 panel.");
}

TEST_CASE("Hub75Fit: a message that fits is never cut, and one that does not says how long it needed") {
    const size_t longest = longestMessage();
    CHECK(longest + 1 <= kGeometryMessageMax);   // with the terminator

    static char small[16];
    const size_t needed = geometryMessage(GeometryFit::Indivisible, 7, wall(64, 128, 32, 8, 2, 3, "Pi PIO"), small, sizeof(small));
    CHECK(needed > sizeof(small) - 1);           // the caller can see it was cut
    CHECK(std::strlen(small) == sizeof(small) - 1);
}
