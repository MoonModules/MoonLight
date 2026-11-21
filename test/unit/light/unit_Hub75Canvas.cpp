/// @module Hub75Canvas

#include "doctest.h"
#include "light/drivers/Hub75Canvas.h"

#include <vector>

using namespace mm;

// The expected numbers come from docs/reference/hardware/hub75-refresh.md's formula and tables, worked by hand, and from the placement rule in Hub75Canvas.h; none is read back from the code under test.

namespace {

// A 128 by 64 panel's worth of settings: the rows, columns, row addresses, bit planes and, past the default, the LSB time.
PanelSpec panel(uint16_t rows, uint16_t cols, uint16_t scan, uint8_t bits, uint16_t lsb = 130) {
    PanelSpec p;
    p.rows = rows; p.cols = cols; p.scanRows = scan; p.bits = bits; p.lsbNs = lsb;
    return p;
}

// A PIO-style panel's clocking: no lit time outlasts a row's shift, one clock a pixel at 10 MHz, the latch riding the last clock.
PanelSpec pioClock(uint16_t scan, uint8_t bits, bool encoderHold) {
    PanelSpec p;
    p.scanRows = scan; p.bits = bits; p.lsbNs = 0; p.shiftTenthsPerPx = 1000; p.latchTenthsNs = 0; p.pairs = 1; p.encoderHold = encoderHold;
    return p;
}

void checkPos(uint32_t i, uint16_t rows, uint16_t cols, uint16_t panelsPerLane, uint16_t x, uint16_t y) {
    const CanvasPos p = laneCanvasPos(i, rows, cols, panelsPerLane);
    INFO("light " << i << " rows " << rows << " cols " << cols << " panelsPerLane " << panelsPerLane);
    CHECK(p.x == x);
    CHECK(p.y == y);
}

} // namespace

TEST_CASE("Hub75Canvas: one panel's lights run along its rows") {
    checkPos(0, 64, 128, 1, 0, 0);
    checkPos(1, 64, 128, 1, 1, 0);
    checkPos(127, 64, 128, 1, 127, 0);
    checkPos(128, 64, 128, 1, 0, 1);
    checkPos(8191, 64, 128, 1, 127, 63);
}

TEST_CASE("Hub75Canvas: the second panel of a lane starts one panel to the right") {
    checkPos(8191, 64, 128, 2, 127, 63);
    checkPos(8192, 64, 128, 2, 128, 0);
    checkPos(8321, 64, 128, 2, 129, 1);
    checkPos(16383, 64, 128, 2, 255, 63);
}

TEST_CASE("Hub75Canvas: six panels as two to a lane, three lanes, fill a 256 by 192 canvas") {
    checkPos(0, 64, 128, 2, 0, 0);
    checkPos(8192, 64, 128, 2, 128, 0);
    checkPos(16384, 64, 128, 2, 0, 64);
    checkPos(40960, 64, 128, 2, 128, 128);
    checkPos(49151, 64, 128, 2, 255, 191);
}

TEST_CASE("Hub75Canvas: the same six panels as three to a lane, two lanes, make a 384 by 128 canvas") {
    checkPos(0, 64, 128, 3, 0, 0);
    checkPos(16384, 64, 128, 3, 256, 0);
    checkPos(24576, 64, 128, 3, 0, 64);
    checkPos(49151, 64, 128, 3, 383, 127);
}

TEST_CASE("Hub75Canvas: every light lands on its own cell and the cells fill the canvas exactly") {
    struct Config { uint16_t rows, cols; uint32_t panels; uint16_t panelsPerLane; };
    const Config configs[] = {{64, 128, 6, 2}, {64, 128, 6, 3}, {32, 64, 4, 2}, {16, 32, 3, 3}, {8, 16, 5, 1}, {64, 64, 6, 6}};
    for (const Config& c : configs) {
        const uint32_t width = static_cast<uint32_t>(c.cols) * c.panelsPerLane;
        const uint32_t height = static_cast<uint32_t>(c.rows) * (c.panels / c.panelsPerLane);
        std::vector<bool> seen(static_cast<size_t>(width) * height, false);
        bool inside = true, unique = true;
        const uint32_t lights = c.panels * c.rows * c.cols;
        for (uint32_t i = 0; i < lights; i++) {
            const CanvasPos p = laneCanvasPos(i, c.rows, c.cols, c.panelsPerLane);
            if (p.x >= width || p.y >= height) { inside = false; continue; }
            const size_t cell = static_cast<size_t>(p.y) * width + p.x;
            if (seen[cell]) unique = false;
            seen[cell] = true;
        }
        INFO("rows " << c.rows << " cols " << c.cols << " panels " << c.panels << " panelsPerLane " << c.panelsPerLane);
        CHECK(inside);
        CHECK(unique);
        CHECK(lights == width * height);   // as many lights as cells, so unique and inside mean every cell is filled
    }
}

TEST_CASE("Hub75Canvas: refresh falls with lane length, and 8 bits beats 11") {
    // 128 by 64 panels at 1/32 scan, the 130 ns default LSB: the page's table.
    const uint16_t eight[] = {420, 243, 165, 124, 100, 83};
    const uint16_t eleven[] = {102, 86, 74, 65, 57, 50};
    for (uint32_t n = 1; n <= 6; n++) {
        INFO("panels in the lane " << n);
        CHECK(hub75PredictedHz(128 * n, panel(0, 0, 32, 8)) == eight[n - 1]);
        CHECK(hub75PredictedHz(128 * n, panel(0, 0, 32, 11)) == eleven[n - 1]);
    }
    CHECK(hub75PredictedHz(256, panel(0, 0, 32, 11, 50)) == 142);   // the page's 50 ns figure for two panels
}

TEST_CASE("Hub75Canvas: an old small panel keeps a long lane fast") {
    CHECK(hub75PredictedHz(32 * 15, panel(0, 0, 8, 8)) == 530);     // a 32 by 16 panel at 1/8 scan, fifteen on a lane
    CHECK(hub75PredictedHz(32 * 30, panel(0, 0, 8, 8)) == 267);
}

namespace {

void checkArr(const Arrangement& a, uint16_t panelsPerLane, uint16_t parallel, uint32_t w, uint32_t h, uint16_t hz) {
    INFO("arrangement " << a.panelsPerLane << " x " << a.parallel);
    CHECK(a.panelsPerLane == panelsPerLane);
    CHECK(a.parallel == parallel);
    CHECK(a.width == w);
    CHECK(a.height == h);
    CHECK(a.hz == hz);
}

} // namespace

TEST_CASE("Hub75Canvas: six panels are offered wide before deep") {
    Arrangement a[4];
    REQUIRE(panelArrangements(6, panel(64, 128, 32, 8), 3, a, 4) == 3);
    checkArr(a[0], 2, 3, 256, 192, 243);
    checkArr(a[1], 3, 2, 384, 128, 165);
    checkArr(a[2], 6, 1, 768, 64, 83);
}

TEST_CASE("Hub75Canvas: two, four and five panels have the arrangements that use them all") {
    Arrangement a[4];
    REQUIRE(panelArrangements(2, panel(64, 128, 32, 8), 3, a, 4) == 2);
    checkArr(a[0], 1, 2, 128, 128, 420);
    checkArr(a[1], 2, 1, 256, 64, 243);
    REQUIRE(panelArrangements(4, panel(64, 128, 32, 8), 3, a, 4) == 2);
    checkArr(a[0], 2, 2, 256, 128, 243);
    checkArr(a[1], 4, 1, 512, 64, 124);
    REQUIRE(panelArrangements(5, panel(64, 128, 32, 8), 3, a, 4) == 1);
    checkArr(a[0], 5, 1, 640, 64, 100);
}

TEST_CASE("Hub75Canvas: the board's lane limit bounds how wide the offer spreads") {
    Arrangement a[4];
    REQUIRE(panelArrangements(12, panel(64, 128, 32, 8), 3, a, 4) == 3);
    checkArr(a[0], 4, 3, 512, 192, 124);
    checkArr(a[1], 6, 2, 768, 128, 83);
    checkArr(a[2], 12, 1, 1536, 64, 42);
    REQUIRE(panelArrangements(6, panel(64, 128, 32, 8), 1, a, 4) == 1);   // one lane only, as a board with one output
    checkArr(a[0], 6, 1, 768, 64, 83);
}

TEST_CASE("Hub75Canvas: no arrangement puts more panels on a lane than the card's panelsPerLane control takes") {
    Arrangement a[4];
    REQUIRE(panelArrangements(40, panel(32, 64, 16, 8), 3, a, 4) == 1);   // 1 lane of 40 is past kPanelsPerLaneMax
    CHECK(a[0].panelsPerLane == 20);
    CHECK(a[0].parallel == 2);
    CHECK(panelArrangements(41, panel(32, 64, 16, 8), 3, a, 4) == 0);    // 41 divides only into a lane of 41
}

TEST_CASE("Hub75Canvas: a full buffer stops the list without writing past it") {
    Arrangement a[2];
    CHECK(panelArrangements(6, panel(64, 128, 32, 8), 3, a, 2) == 2);
}

TEST_CASE("Hub75Canvas: keeping the lanes already built means fewer panels or more panels, never a recable") {
    KeepLanes k = keepLanes(7, panel(64, 128, 32, 8), 3, 3);
    checkArr(k.use, 3, 2, 384, 128, 165);
    CHECK(k.spare == 1);
    checkArr(k.add, 3, 3, 384, 192, 165);
    CHECK(k.added == 2);

    k = keepLanes(5, panel(64, 128, 32, 8), 2, 3);
    checkArr(k.use, 2, 2, 256, 128, 243);
    CHECK(k.spare == 1);
    checkArr(k.add, 2, 3, 256, 192, 243);
    CHECK(k.added == 1);

    k = keepLanes(6, panel(64, 128, 32, 8), 4, 3);
    checkArr(k.use, 4, 1, 512, 64, 124);
    CHECK(k.spare == 2);
    checkArr(k.add, 4, 2, 512, 128, 124);
    CHECK(k.added == 2);
}

TEST_CASE("Hub75Canvas: adding panels is not offered past the board's lane limit") {
    const KeepLanes k = keepLanes(10, panel(64, 128, 32, 8), 3, 3);
    checkArr(k.use, 3, 3, 384, 192, 165);
    CHECK(k.spare == 1);
    CHECK(k.add.panelsPerLane == 0);   // a fourth lane exceeds the board
    const KeepLanes fits = keepLanes(6, panel(64, 128, 32, 8), 3, 3);
    checkArr(fits.use, 3, 2, 384, 128, 165);
    CHECK(fits.spare == 0);
    CHECK(fits.add.panelsPerLane == 0);   // already a whole number of lanes
}

TEST_CASE("Hub75Canvas: keeping the lanes caps the panels at what the board's lanes can carry") {
    // Six panels in lanes of 2 want three lanes; a board with one carries 2 of them, the other 4 spare.
    const KeepLanes k = keepLanes(6, panel(64, 128, 32, 8), 2, 1);
    checkArr(k.use, 2, 1, 256, 64, 243);
    CHECK(k.spare == 4);
    CHECK(k.add.panelsPerLane == 0);
}

TEST_CASE("Hub75Canvas: the card's LSB time moves every rate the offers quote") {
    // Two 128 by 64 panels at 11 bits, the page's own figures: 86 Hz in one lane at the 130 ns default, 142 Hz at 50 ns.
    Arrangement a[4];
    REQUIRE(panelArrangements(2, panel(64, 128, 32, 11), 3, a, 4) == 2);
    checkArr(a[0], 1, 2, 128, 128, 102);
    checkArr(a[1], 2, 1, 256, 64, 86);
    REQUIRE(panelArrangements(2, panel(64, 128, 32, 11, 50), 3, a, 4) == 2);
    checkArr(a[0], 1, 2, 128, 128, 201);
    checkArr(a[1], 2, 1, 256, 64, 142);
    const KeepLanes k = keepLanes(3, panel(64, 128, 32, 11, 50), 2, 3);
    checkArr(k.use, 2, 1, 256, 64, 142);
}

// A PIO-style quote (pioClock); the flag says the frame is the in-tree encoder's, so a narrow row's hold words count as shift.
TEST_CASE("Hub75Canvas: where the frame is the encoder's, a narrow row's hold words count as shift and halve its quoted rate") {
    // 64 columns at 8 bits: the pass is 129 words, so the frame doubles and the rate halves, 1221 to 606 Hz.
    CHECK(hub75PredictedHz(64, pioClock(16, 8, true)) == 606);
    CHECK(hub75PredictedHz(64, pioClock(16, 8, false)) == 1221);
    // A 16-column row stretches to the same 129-word pass, so it quotes the same rate as the 64-column one.
    CHECK(hub75PredictedHz(16, pioClock(16, 8, true)) == 606);
    // Two 64-column panels chained are 128 words a row: no hold, so the rate is the shift's own.
    CHECK(hub75PredictedHz(128, pioClock(16, 8, true)) == 610);
    // The wall's 256-word rows take zero hold words: the flag is a no-op there, 153 Hz either way.
    CHECK(hub75PredictedHz(256, pioClock(32, 8, true)) == 153);
    CHECK(hub75PredictedHz(256, pioClock(32, 8, false)) == 153);
    // The 6-bit 16-word corner in rate terms: the hold stretches the pass 16 to 33 words, so the quote halves.
    CHECK(hub75PredictedHz(16, pioClock(16, 6, true)) == 3157);
    CHECK(hub75PredictedHz(16, pioClock(16, 6, false)) == 6510);
}

TEST_CASE("Hub75Canvas: a panel with no shift, latch or lit time quotes the top rate, 65535 Hz") {
    PanelSpec spec;
    spec.rows = 32; spec.cols = 64; spec.scanRows = 16; spec.bits = 8; spec.lsbNs = 0;
    spec.shiftTenthsPerPx = 0; spec.latchTenthsNs = 0;
    CHECK(hub75PredictedHz(64, spec) == 65535);
}

TEST_CASE("Hub75Canvas: an encoder-hold arrangement quotes the stretched rate") {
    // One 64 by 32 panel at 8 bits on the PIO output: the arrangement's rate counts the hold words, 606 Hz, not the 1221 the shift alone would quote.
    PanelSpec spec;
    spec.rows = 32; spec.cols = 64; spec.scanRows = 16; spec.bits = 8; spec.lsbNs = 0;
    spec.shiftTenthsPerPx = 1000; spec.latchTenthsNs = 0; spec.pairs = 1; spec.encoderHold = true;
    Arrangement a[4];
    REQUIRE(panelArrangements(1, spec, 1, a, 4) == 1);
    checkArr(a[0], 1, 1, 64, 32, 606);
    // Two of the same panels chained: 128 words a row, no hold, so the rate is the shift's own.
    REQUIRE(panelArrangements(2, spec, 1, a, 4) == 1);
    checkArr(a[0], 2, 1, 128, 32, 610);
    // The same panel with the flag cleared quotes the shift alone.
    spec.encoderHold = false;
    REQUIRE(panelArrangements(1, spec, 1, a, 4) == 1);
    checkArr(a[0], 1, 1, 64, 32, 1221);
}

namespace {

// Feed a layout's lights to the walk in index order, as `place(i)` puts them, and say which panel breaks plain rows.
template <typename Place>
uint32_t offPanel(uint16_t rows, uint16_t cols, uint32_t lights, Place place) {
    PanelWalk walk(rows, cols);
    for (uint32_t i = 0; i < lights; i++) {
        const CanvasPos p = place(i);
        walk.light(i, p.x, p.y);
    }
    return walk.firstOffPanel();
}

}  // namespace

TEST_CASE("Hub75Canvas: panels laid out in cabling order, each in plain rows, walk true") {
    // Six 8 by 16 panels, 2 across and 3 down, chained along each row of panels: the Pi 5 wall in miniature.
    CHECK(offPanel(8, 16, 6 * 128, [](uint32_t i) {
              const uint32_t k = i / 128, j = i % 128;
              return CanvasPos{static_cast<uint16_t>((k % 2) * 16 + j % 16), static_cast<uint16_t>((k / 2) * 8 + j / 16)};
          }) == PanelWalk::kNone);
    // The same panels chained down each column of panels (YX): another cabling, still plain rows inside each panel.
    CHECK(offPanel(8, 16, 6 * 128, [](uint32_t i) {
              const uint32_t k = i / 128, j = i % 128;
              return CanvasPos{static_cast<uint16_t>((k / 3) * 16 + j % 16), static_cast<uint16_t>((k % 3) * 8 + j / 16)};
          }) == PanelWalk::kNone);
}

TEST_CASE("Hub75Canvas: a panel mounted mirrored or turned a quarter still walks plain rows in its own orientation") {
    CHECK(offPanel(8, 16, 128, [](uint32_t j) {   // rows run right to left
              return CanvasPos{static_cast<uint16_t>(15 - j % 16), static_cast<uint16_t>(j / 16)};
          }) == PanelWalk::kNone);
    CHECK(offPanel(8, 16, 128, [](uint32_t j) {   // portrait: a panel row runs down the picture
              return CanvasPos{static_cast<uint16_t>(7 - j / 16), static_cast<uint16_t>(j % 16)};
          }) == PanelWalk::kNone);
}

TEST_CASE("Hub75Canvas: one raster across two panels in series breaks the first panel's rows") {
    // A 32 by 8 raster: light 16 sits at x 16 of row 0, where the panel's own second row should start.
    CHECK(offPanel(8, 16, 256, [](uint32_t i) { return CanvasPos{static_cast<uint16_t>(i % 32), static_cast<uint16_t>(i / 32)}; }) == 0);
}

TEST_CASE("Hub75Canvas: a snaked panel breaks plain rows, and the walk names the first such panel") {
    const auto snakeSecond = [](uint32_t i) {
        const uint32_t k = i / 128, j = i % 128, row = j / 16, col = j % 16;
        const uint32_t x = (k == 1 && row % 2 == 1) ? 15 - col : col;   // only panel 1 snakes
        return CanvasPos{static_cast<uint16_t>(k * 16 + x), static_cast<uint16_t>(row)};
    };
    CHECK(offPanel(8, 16, 2 * 128, snakeSecond) == 1);
}
