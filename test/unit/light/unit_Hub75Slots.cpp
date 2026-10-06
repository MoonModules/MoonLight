/// @module Hub75Driver
/// @also Hub75Slots

#include "doctest.h"
#include "light/drivers/Hub75Slots.h"

#include <cstring>
#include <vector>

// The encoder's contract on plain buffers, with no ESP32 and no panel: each case pins one thing a wall shows wrong without saying why.

namespace {

// A frame of `w * h` lights, every channel zero, so a test sets only what it means.
std::vector<uint8_t> blackFrame(uint16_t w, uint16_t h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, 0);
}

void setPixel(std::vector<uint8_t>& f, uint16_t w, uint16_t x, uint16_t y,
              uint8_t r, uint8_t g, uint8_t b) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    f[i + 0] = r; f[i + 1] = g; f[i + 2] = b;
}

/// Slot `n` as the 16-bit little-endian bus word, so no check tests one byte and misses the lines above bit 7.
uint16_t slot(const std::vector<uint8_t>& out, size_t n) {
    return static_cast<uint16_t>(out[n * 2] | (out[n * 2 + 1] << 8));
}

}  // namespace

TEST_CASE("HUB75 encode: the frame is bit-plane major, one word per column") {
    mm::Hub75Geometry geo;
    geo.width = 8; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;

    // 2 planes x 2 scan rows x 8 columns x 1 pair = 32 slots, and 2 bytes each because the bus is 16 bits wide.
    CHECK(geo.frameSlots() == 32);
    CHECK(geo.frameBytes() == 32 * 2);

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0xAA);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
}

TEST_CASE("HUB75 encode: the address on a data word is the row in the LATCH, one behind") {
    // Seen on a wall: the picture displaced one scan row, the last row drawn over the first. The latch holds the row last strobed, so a data word addressed to its own row lights the previous row's data on it.
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;
    mm::Hub75Layout lay;   // the DEFAULT layout: a=8, b=9, above the low byte

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Row 0's lit words name the LAST row and its latch word names row 0; row 1's name row 0, then row 1.
    const size_t row1 = geo.width;           // row 1 starts right after row 0's columns
    const uint16_t latch = geo.width - 1;    // the last column's word is the latch
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out, x) & (1u << lay.a)) != 0);
    CHECK((slot(out, latch) & (1u << lay.a)) == 0);
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out, row1 + x) & (1u << lay.a)) == 0);
    CHECK((slot(out, row1 + latch) & (1u << lay.a)) != 0);

    // And on EVERY lit word, not only the first: an address changing mid-row ghosts the row.
    for (uint16_t x = 1; x < latch; x++) {
        CHECK((slot(out, x) & (1u << lay.a)) == (slot(out, 0) & (1u << lay.a)));
    }

    // A board is free to re-map the lines into the low byte, and that must keep working: Hub75Layout exists so a different wiring costs a struct rather than an encoder.
    mm::Hub75Layout low;
    low.r1 = 0; low.g1 = 1; low.b1 = 2; low.r2 = 3; low.g2 = 4; low.b2 = 5;
    low.a = 6; low.b = 7; low.c = 7; low.d = 7; low.e = 7;
    low.lat = 6; low.oe = 7;
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out2.data(), geo, low) == geo.frameBytes());
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out2, x) & (1u << low.a)) != 0);
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out2, row1 + x) & (1u << low.a)) == 0);
}

TEST_CASE("HUB75 encode: a color bit lands on its own line, for both half-panels") {
    // A walking-one per color line: the test that catches a swapped pair (red where green should be) and an upper/lower mix-up. A panel shows both as "wrong colors", which is the least diagnostic symptom there is.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 4;
    mm::Hub75Layout lay;

    struct Case { const char* name; uint8_t r, g, b; uint8_t bit; bool lower; };
    const Case cases[] = {
        {"upper red",   0xFF, 0, 0,    lay.r1, false},
        {"upper green", 0, 0xFF, 0,    lay.g1, false},
        {"upper blue",  0, 0, 0xFF,    lay.b1, false},
        {"lower red",   0xFF, 0, 0,    lay.r2, true},
        {"lower green", 0, 0xFF, 0,    lay.g2, true},
        {"lower blue",  0, 0, 0xFF,    lay.b2, true},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto rgb = blackFrame(geo.width, geo.height);
        // Scan row 0 drives panel row 0 (upper) and row 0 + height/2 = 2 (lower).
        setPixel(rgb, geo.width, 0, c.lower ? 2 : 0, c.r, c.g, c.b);

        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

        // At full value every plane carries the bit; column 0 of scan row 0 of plane 0.
        CHECK((slot(out, 0) & (1u << c.bit)) != 0);
        // and no OTHER color line is set by it.
        const uint8_t colorBits = static_cast<uint8_t>(
            (1u << lay.r1) | (1u << lay.g1) | (1u << lay.b1) |
            (1u << lay.r2) | (1u << lay.g2) | (1u << lay.b2));
        CHECK((slot(out, 0) & colorBits) == (1u << c.bit));
    }
}

TEST_CASE("HUB75 encode: depth below 8 keeps the HIGH bits") {
    // Dropping the low bits costs precision; dropping the high ones would cost RANGE, so a bright pixel would come out dim. The difference is invisible in a buffer dump and obvious on a wall, which is the wrong way round.
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xC0, 0, 0);   // 1100 0000: both top bits set

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Both planes of a 2-bit depth read bits 6 and 7, so both carry red.
    const size_t plane0 = 0;
    // Plane 1 starts after plane 0's single pass (2^0 = 1), in SLOTS.
    const size_t plane1 = static_cast<size_t>(geo.scanRows()) * geo.width;
    CHECK((slot(out, plane0) & (1u << lay.r1)) != 0);
    CHECK((slot(out, plane1) & (1u << lay.r1)) != 0);

    // 0x30 (0011 0000) is in the LOW half: at 2-bit depth it reads as black, which is the precision loss the depth control trades for refresh.
    auto dim = blackFrame(geo.width, geo.height);
    setPixel(dim, geo.width, 0, 0, 0x30, 0, 0);
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(dim.data(), out2.data(), geo, lay) == geo.frameBytes());
    CHECK((slot(out2, plane0) & (1u << lay.r1)) == 0);
    CHECK((slot(out2, plane1) & (1u << lay.r1)) == 0);
}

TEST_CASE("HUB75 encode: a panel driving four rows per address step encodes every pair") {
    // A 64-row 1/16 panel steps 16 addresses and drives FOUR rows each: two pairs, the second offset by 2 x scanRate. An encoder assuming two rows would leave three quarters of the panel dark, and the geometry is common enough to matter.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 8; geo.scanRate = 2; geo.bitDepth = 4;
    mm::Hub75Layout lay;

    CHECK(geo.rowsPerScan() == 4);
    // 4 planes x 2 scan rows x 2 columns x 2 pairs = 32 slots, 64 bytes.
    CHECK(geo.frameSlots() == 32);
    CHECK(geo.frameBytes() == 32 * 2);

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 2, 0xFF, 0, 0);   // row 2 = address step 0, pair 1, upper

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Pair 0 (rows 0 and 4) is black; pair 1 (rows 2 and 6) carries the red.
    CHECK((slot(out, 0) & (1u << lay.r1)) == 0);              // pair 0, column 0
    CHECK((slot(out, geo.width) & (1u << lay.r1)) != 0);      // pair 1, column 0
    // One latch per address step, on the last pair's last word, not at the end of each pair.
    CHECK((slot(out, geo.width - 1) & (1u << lay.lat)) == 0);
    CHECK((slot(out, 2 * geo.width - 1) & (1u << lay.lat)) != 0);
}

TEST_CASE("HUB75 encode: the last column's word latches, dark") {
    // Dark while the shift register hands its row to the output drivers, or the addressed row briefly shows the previous row's data.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    lay.lat = 6; lay.oe = 7;   // both in the low byte, so the encoded byte shows them

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    const size_t latch = geo.width - 1;   // the last column's word
    CHECK((slot(out, latch) & (1u << lay.oe)) == 0);    // dark
    CHECK((slot(out, latch) & (1u << lay.lat)) != 0);   // and latching
    CHECK((slot(out, 0) & (1u << lay.lat)) == 0);       // and only there
}

TEST_CASE("HUB75 encode: the DMA wrap lights the last row once, from row 0's data words") {
    // The peripheral LOOPS this buffer, so the last row is strobed dark by its blank and what lights it is row 0's data words at the top, addressed to it. A dark tail word here fixed a window that never existed.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    lay.lat = 6; lay.oe = 7;
    lay.a = 8; lay.b = 9; lay.c = 10; lay.d = 11; lay.e = 12;

    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 255);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // The frame's last word is the last row's latch word: dark, latching, addressed to that row.
    const size_t last = geo.frameSlots() - 1;
    const uint16_t lastRow = geo.scanRows() - 1;
    CHECK((slot(out, last) & (1u << lay.oe)) == 0);
    CHECK((slot(out, last) & (1u << lay.lat)) != 0);
    CHECK((slot(out, last) & (1u << lay.a)) == (lastRow & 1 ? (1u << lay.a) : 0u));

    // And the first word lights, addressed to that same last row, whose data the latch now holds.
    CHECK((slot(out, 0) & (1u << lay.oe)) != 0);
    CHECK((slot(out, 0) & (1u << lay.a)) == (lastRow & 1 ? (1u << lay.a) : 0u));
}

// Seen on a 64x64 panel: row 63 ghosting onto row 32, rows 0 and 32 brighter, flashing. The peripheral idles its lines LOW between scans, and with OE encoded as the panel reads it (active low) that idle bus lit address 0 with the last latched row.
TEST_CASE("HUB75 encode: the OE bit is set where the panel is lit, so an all-zero idle bus is dark") {
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0);   // black: color says nothing about OE
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    // The top plane fills its row, so every column word lights but the latch word.
    const size_t top = static_cast<size_t>(geo.bitDepth - 1) * geo.scanRows() * geo.width;
    for (uint16_t x = 0; x + 1 < geo.width; x++) CHECK((slot(out, top + x) & (1u << lay.oe)) != 0);
    CHECK((slot(out, top + geo.width - 1) & (1u << lay.oe)) == 0);
}

// Seen on the 64x64 panel: the last column dark whatever the effect, and the picture one column left. Every word is a clock, so a separate latch word shifted a 65th value into a 64-pixel row.
TEST_CASE("HUB75 encode: a row clocks exactly its width, so the last column reaches the panel's last pixel") {
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 3, 0, 0xFF, 0, 0);   // the LAST column, red
    setPixel(rgb, geo.width, 0, 0, 0, 0xFF, 0);   // the FIRST column, green
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(geo.frameSlots() == static_cast<size_t>(geo.bitDepth) * geo.width);   // one scan row: width words a plane, nothing more
    CHECK((slot(out, 0) & (1u << lay.g1)) != 0);   // first word: column 0, shifted in first, lands on the far pixel
    CHECK((slot(out, 3) & (1u << lay.r1)) != 0);   // the latch word still carries column 3's color
    CHECK((slot(out, 3) & (1u << lay.lat)) != 0);
}

namespace {
// How many words of plane p's first row carry the OE (lit) bit.
size_t litWords(const std::vector<uint8_t>& out, const mm::Hub75Geometry& geo, const mm::Hub75Layout& lay, uint8_t p) {
    const size_t base = static_cast<size_t>(p) * geo.scanRows() * geo.width;
    size_t n = 0;
    for (uint16_t x = 0; x < geo.width; x++) n += (slot(out, base + x) & (1u << lay.oe)) != 0;
    return n;
}
}  // namespace

// Seen on the 64x64 panel: brightness coarse and out of order. Equal-time planes made a pixel's brightness count its set bits, so 7 (0111) outshone 8 (1000); binary time makes it follow the value.
TEST_CASE("HUB75 encode: each plane is lit twice as long as the one below, the top plane for the whole row") {
    mm::Hub75Geometry geo;
    geo.width = 9; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 4;   // 8 words that can light, plus the latch
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(litWords(out, geo, lay, 0) == 1);
    CHECK(litWords(out, geo, lay, 1) == 2);
    CHECK(litWords(out, geo, lay, 2) == 4);
    CHECK(litWords(out, geo, lay, 3) == 8);
}

// The slider used to scale values, so at about 75 every channel fell below 4 bits and the panel went black. As time, every plane keeps its share and only the shortest windows round away.
TEST_CASE("HUB75 encode: brightness shortens every plane's window, keeping the values") {
    mm::Hub75Geometry geo;
    geo.width = 9; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 4;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xFF, 0, 0);
    geo.brightness = 128;
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(litWords(out, geo, lay, 3) == 4);   // half of 8
    CHECK(litWords(out, geo, lay, 2) == 2);
    CHECK(litWords(out, geo, lay, 1) == 1);
    CHECK((slot(out, 0) & (1u << lay.r1)) != 0);   // the pixel's value is untouched: full red still shifts in
    geo.brightness = 0;
    std::vector<uint8_t> dark(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), dark.data(), geo, lay) == geo.frameBytes());
    for (uint8_t p = 0; p < geo.bitDepth; p++) CHECK(litWords(dark, geo, lay, p) == 0);
}

// Plane 0's window is 1/32 of the top plane's at 6-bit, so near a quarter brightness it rounds to nothing: the limit the encoder's docs state.
TEST_CASE("HUB75 encode: below a quarter brightness the lowest 6-bit plane of a 64-column row is dark") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 6; geo.brightness = 64;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(litWords(out, geo, lay, 0) == 0);
    CHECK(litWords(out, geo, lay, 5) == 16);   // the top plane keeps its quarter of 63
}

// A one-column panel: its only word is the latch, so the row encodes, dark, rather than underflowing the window.
TEST_CASE("HUB75 encode: a one-column row is its latch word") {
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xFF, 0, 0);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    for (size_t s = 0; s < geo.frameSlots(); s++) {
        CHECK((slot(out, s) & (1u << lay.lat)) != 0);
        CHECK((slot(out, s) & (1u << lay.oe)) == 0);
        CHECK((slot(out, s) & (1u << lay.r1)) != 0);   // the color still shifts in
    }
}

TEST_CASE("HUB75 encode: an unusable geometry writes nothing") {
    // Zero rather than a partial write. A half-encoded frame on a panel is a worse failure than a dark one, and the driver turns this into a status the user reads.
    auto rgb = blackFrame(8, 8);
    std::vector<uint8_t> out(4096, 0xAA);

    mm::Hub75Geometry odd;      // 3 rows per address step: no pair for the third
    odd.width = 8; odd.height = 6; odd.scanRate = 2; odd.bitDepth = 4;
    CHECK(odd.valid() == false);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), odd) == 0);

    mm::Hub75Geometry indivisible;
    indivisible.width = 8; indivisible.height = 7; indivisible.scanRate = 2;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), indivisible) == 0);

    mm::Hub75Geometry deep;   // above the 6-bit cap, which is what valid() enforces
    deep.width = 8; deep.height = 8; deep.scanRate = 4; deep.bitDepth = 7;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), deep) == 0);

    mm::Hub75Geometry ok;
    ok.width = 8; ok.height = 8; ok.scanRate = 4; ok.bitDepth = 4;
    CHECK(mm::hub75Encode(nullptr, out.data(), ok) == 0);
    CHECK(mm::hub75Encode(rgb.data(), nullptr, ok) == 0);

    // Nothing was written by any of the refusals.
    CHECK(out[0] == 0xAA);
}

TEST_CASE("HUB75 frame size is what decides the peripheral") {
    // The sizes that decide Parlio (65,535 bytes a transfer) against LCD_CAM, two bytes a slot, pinned at 4-bit so a wire-format change cannot move the cliff unseen.
    mm::Hub75Geometry one;    // one 64x64 panel, 1/32 scan
    one.width = 64; one.height = 64; one.scanRate = 32; one.bitDepth = 4;
    CHECK(one.frameBytes() == (4 * 32 * 64 * 1) * 2);            // 16,384
    CHECK(one.frameBytes() < 65535u);                          // Parlio carries one panel

    mm::Hub75Geometry four;   // 128x128, 1/32 scan
    four.width = 128; four.height = 128; four.scanRate = 32; four.bitDepth = 4;
    CHECK(four.frameBytes() == (4 * 32 * 128 * 2) * 2);          // 65,536
    CHECK(four.frameBytes() > 65535u);                         // four panels miss the cap by one byte

    mm::Hub75Geometry sixteen;   // 256x256, 1/32 scan
    sixteen.width = 256; sixteen.height = 256; sixteen.scanRate = 32; sixteen.bitDepth = 4;
    CHECK(sixteen.frameBytes() == (4 * 32 * 256 * 4) * 2);        // 262,144
    CHECK(sixteen.frameBytes() > 65535u);                       // Parlio cannot carry it
}
