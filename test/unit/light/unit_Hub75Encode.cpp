/// @module Hub75Driver
/// @also Hub75Encode

/// Pins the encoder's contract on plain buffers, with no ESP32 and no panel: each case pins one thing a panel shows wrong and leaves unexplained.
///
/// @technical
///
/// ## Low brightness
///
/// The brightness below which the lowest plane drops out is in Hub75Encode.h, Brightness is time: 64 on a 256-word row at 8 bits, half on a stretched row.
///
/// ## Hold words at 6 bits
///
/// The rule is in Hub75Bus.h, Hold words. At 6 bits it stretches every row of 1 to 16 words to 33, and a row of 17 words or more keeps its rounding.

#include "doctest.h"
#include "platform/Hub75Encode.h"

#include <span>
#include <vector>

namespace {

/// The layout the encoder writes: each test reads a line's bit from it, so a test pins where a line sits in the word.
constexpr mm::Hub75Layout kLayout{};

// A frame of `w * h` lights, every channel zero, so a test sets only what it means.
std::vector<uint8_t> blackFrame(uint16_t w, uint16_t h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, 0);
}

// Every channel of every light at `value`, encoded, its encode checked to fill the geometry's bytes: black for a test that reads only the control bits, white for the lit-window vectors.
std::vector<uint8_t> encodeFilled(const mm::Hub75Geometry& geo, uint8_t value) {
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, value);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
    return out;
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

/// The first slot of the row that lights plane p's first row: the one after it latches, since a row is lit while the next one clocks in, and the buffer wraps. The stride is the pass, hold words included.
size_t litRowBase(const mm::Hub75Geometry& geo, uint8_t p) {
    const size_t rowsInFrame = static_cast<size_t>(geo.bitDepth) * geo.scanRows();
    return (static_cast<size_t>(p) * geo.scanRows() + 1) % rowsInFrame * geo.passWords();
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

    auto out = encodeFilled(geo, 0);

    // Row 0's lit words name the LAST row and its latch word names row 0; row 1's name row 0, then row 1.
    const size_t row1 = geo.passWords();           // row 1 starts right after row 0's pass
    const uint16_t latch = geo.passWords() - 1;    // the last word of the pass is the latch
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out, x) & (1u << kLayout.a)) != 0);
    CHECK((slot(out, latch) & (1u << kLayout.a)) == 0);
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out, row1 + x) & (1u << kLayout.a)) == 0);
    CHECK((slot(out, row1 + latch) & (1u << kLayout.a)) != 0);

    // And on EVERY lit word, not only the first: an address changing mid-row ghosts the row.
    for (uint16_t x = 1; x < latch; x++) {
        CHECK((slot(out, x) & (1u << kLayout.a)) == (slot(out, 0) & (1u << kLayout.a)));
    }
}

TEST_CASE("HUB75 encode: the layout is the wire: color on bits 0-5, address on 8-12, latch on 13, lit on 14") {
    // The ESP32 port wires its pins in this order (platform_esp32_hub75.cpp, buildPinOrder), so a bit that moves here alone shows as wrong colors on a panel.
    CHECK(kLayout.r1 == 0); CHECK(kLayout.g1 == 1); CHECK(kLayout.b1 == 2);
    CHECK(kLayout.r2 == 3); CHECK(kLayout.g2 == 4); CHECK(kLayout.b2 == 5);
    CHECK(kLayout.a == 8); CHECK(kLayout.b == 9); CHECK(kLayout.c == 10); CHECK(kLayout.d == 11); CHECK(kLayout.e == 12);
    CHECK(kLayout.lat == 13);
    CHECK(kLayout.oe == 14);
}

// @step driver-setup/add-hub75
TEST_CASE("HUB75 encode: a color bit lands on its own line, for both half-panels") {
    // A walking-one per color line: the test that catches a swapped pair (red where green should be) and an upper/lower mix-up. A panel shows both as "wrong colors", which is the least diagnostic symptom there is.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 4;

    struct Case { const char* name; uint8_t r, g, b; uint8_t bit; bool lower; };
    const Case cases[] = {
        {"upper red",   0xFF, 0, 0,    kLayout.r1, false},
        {"upper green", 0, 0xFF, 0,    kLayout.g1, false},
        {"upper blue",  0, 0, 0xFF,    kLayout.b1, false},
        {"lower red",   0xFF, 0, 0,    kLayout.r2, true},
        {"lower green", 0, 0xFF, 0,    kLayout.g2, true},
        {"lower blue",  0, 0, 0xFF,    kLayout.b2, true},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto rgb = blackFrame(geo.width, geo.height);
        // Scan row 0 drives panel row 0 (upper) and row 0 + height/2 = 2 (lower).
        setPixel(rgb, geo.width, 0, c.lower ? 2 : 0, c.r, c.g, c.b);

        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

        // At full value every plane carries the bit; column 0 of scan row 0 of plane 0. A 2-column row at 4 bits holds 7 words before its shift words, so the color sits at the hold, not at slot 0.
        const size_t col0 = geo.holdWords();
        CHECK((slot(out, col0) & (1u << c.bit)) != 0);
        // and no OTHER color line is set by it.
        const uint8_t colorBits = static_cast<uint8_t>(
            (1u << kLayout.r1) | (1u << kLayout.g1) | (1u << kLayout.b1) |
            (1u << kLayout.r2) | (1u << kLayout.g2) | (1u << kLayout.b2));
        CHECK((slot(out, col0) & colorBits) == (1u << c.bit));
    }
}

TEST_CASE("HUB75 encode: depth below 8 keeps the HIGH bits") {
    // Dropping the low bits costs precision; dropping the high ones would cost RANGE, so a bright pixel would come out dim. The difference is invisible in a buffer dump and obvious on a wall, which is the wrong way round.
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xC0, 0, 0);   // 1100 0000: both top bits set

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

    // Both planes of a 2-bit depth read bits 6 and 7, so both carry red. The 1-column row holds 2 words, so the color sits at the hold.
    const size_t plane0 = geo.holdWords();
    // Plane 1 starts after plane 0's single pass (2^0 = 1), in SLOTS: a 3-word pass, and the color at its hold.
    const size_t plane1 = static_cast<size_t>(geo.scanRows()) * geo.passWords() + geo.holdWords();
    CHECK((slot(out, plane0) & (1u << kLayout.r1)) != 0);
    CHECK((slot(out, plane1) & (1u << kLayout.r1)) != 0);

    // 0x30 (0011 0000) is in the LOW half: at 2-bit depth it reads as black, which is the precision loss the depth control trades for refresh.
    auto dim = blackFrame(geo.width, geo.height);
    setPixel(dim, geo.width, 0, 0, 0x30, 0, 0);
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(dim.data(), out2.data(), geo) == geo.frameBytes());
    CHECK((slot(out2, plane0) & (1u << kLayout.r1)) == 0);
    CHECK((slot(out2, plane1) & (1u << kLayout.r1)) == 0);
}

TEST_CASE("HUB75 encode: a panel driving four rows per address step encodes every pair") {
    // A 64-row 1/16 panel steps 16 addresses and drives FOUR rows each: two pairs, the second offset by 2 x scanRate. An encoder assuming two rows would leave three quarters of the panel dark, and the geometry is common enough to matter.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 8; geo.scanRate = 2; geo.bitDepth = 4;

    CHECK(geo.rowsPerScan() == 4);
    // 4 planes x 2 scan rows x a 9-word pass (4 shift words + 5 hold words) = 72 slots, 144 bytes.
    CHECK(geo.holdWords() == 5);   // 2^(4-1)+1 - 4: the 4-word row stretches to 9
    CHECK(geo.frameSlots() == 72);
    CHECK(geo.frameBytes() == 72 * 2);

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 2, 0xFF, 0, 0);   // row 2 = address step 0, pair 1, upper

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

    // Pair 0 (rows 0 and 4) is black; pair 1 (rows 2 and 6) carries the red. Both sit past the hold words.
    CHECK((slot(out, geo.holdWords()) & (1u << kLayout.r1)) == 0);              // pair 0, column 0
    CHECK((slot(out, geo.holdWords() + geo.width) & (1u << kLayout.r1)) != 0);  // pair 1, column 0
    // One latch per address step, on the last pair's last word, not at the end of each pair.
    CHECK((slot(out, geo.holdWords() + geo.width - 1) & (1u << kLayout.lat)) == 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << kLayout.lat)) != 0);
}

TEST_CASE("HUB75 encode: the last column's word latches, dark") {
    // Dark while the shift register hands its row to the output drivers, or the addressed row briefly shows the previous row's data.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;

    auto out = encodeFilled(geo, 0);

    const size_t latch = geo.width - 1;   // the last column's word
    CHECK((slot(out, latch) & (1u << kLayout.oe)) == 0);    // dark
    CHECK((slot(out, latch) & (1u << kLayout.lat)) != 0);   // and latching
    CHECK((slot(out, 0) & (1u << kLayout.lat)) == 0);       // and only there
}

TEST_CASE("HUB75 encode: the DMA wrap lights the last row once, from row 0's data words") {
    // The peripheral LOOPS this buffer, so the last row is strobed dark by its blank and what lights it is row 0's data words at the top, addressed to it.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;

    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 255);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

    // The frame's last word is the last row's latch word: dark, latching, addressed to that row.
    const size_t last = geo.frameSlots() - 1;
    const uint16_t lastRow = geo.scanRows() - 1;
    CHECK((slot(out, last) & (1u << kLayout.oe)) == 0);
    CHECK((slot(out, last) & (1u << kLayout.lat)) != 0);
    CHECK((slot(out, last) & (1u << kLayout.a)) == (lastRow & 1 ? (1u << kLayout.a) : 0u));

    // And the first word lights, addressed to that same last row, whose data the latch now holds.
    CHECK((slot(out, 0) & (1u << kLayout.oe)) != 0);
    CHECK((slot(out, 0) & (1u << kLayout.a)) == (lastRow & 1 ? (1u << kLayout.a) : 0u));
}

// Seen on two chained 64x64 panels: a line at rows 31 and 63, the last row lit with the next plane's window while the latch held the plane before. White sums the same, so the wrap test above passes either way.
TEST_CASE("HUB75 encode: every row of a value is lit as long as every other, the last row of each half included") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 64; geo.scanRate = 32; geo.bitDepth = 6; geo.brightness = 255;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0x7F);   // below half: every plane but the top one
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

    // As the panel sees it: a row lights what the row before it latched, so a row's brightness sums the next row's lit words over the planes where its bit is set.
    const uint16_t rows = geo.scanRows();
    const size_t rowsInFrame = static_cast<size_t>(geo.bitDepth) * rows;
    std::vector<uint32_t> upper(rows, 0), lower(rows, 0);
    for (size_t b = 0; b < rowsInFrame; b++) {
        const size_t latchedFrom = (b + rowsInFrame - 1) % rowsInFrame;
        const uint16_t data = slot(out, latchedFrom * geo.passWords() + geo.holdWords());
        uint32_t lit = 0;
        for (size_t x = 0; x < geo.passWords(); x++) lit += (slot(out, b * geo.passWords() + x) & (1u << kLayout.oe)) != 0;
        const uint16_t row = static_cast<uint16_t>(latchedFrom % rows);
        if (data & (1u << kLayout.r1)) upper[row] += lit;
        if (data & (1u << kLayout.r2)) lower[row] += lit;
    }
    REQUIRE(upper[0] > 0);
    for (uint16_t r = 0; r < rows; r++) {
        CAPTURE(r);
        CHECK(upper[r] == upper[0]);   // row r of the upper half
        CHECK(lower[r] == upper[0]);   // row r + 32: the lower half, row 63 the bottom edge
    }
}

// Seen on a 64x64 panel: row 63 ghosting onto row 32, rows 0 and 32 brighter, flashing. The peripheral idles its lines LOW between scans, and with OE encoded as the panel reads it (active low) that idle bus lit address 0 with the last latched row.
TEST_CASE("HUB75 encode: the OE bit is set where the panel is lit, so an all-zero idle bus is dark") {
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0);   // black: color is irrelevant to OE
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
    // The top plane fills its row, so every word of the pass lights but the latch word.
    const size_t top = litRowBase(geo, geo.bitDepth - 1);
    for (size_t x = 0; x + 1 < geo.passWords(); x++) CHECK((slot(out, top + x) & (1u << kLayout.oe)) != 0);
    CHECK((slot(out, top + geo.passWords() - 1) & (1u << kLayout.oe)) == 0);
}

// Seen on the 64x64 panel: the last column dark whatever the effect, and the picture one column left. Every word is a clock, so a separate latch word shifted a 65th value into a 64-pixel row.
TEST_CASE("HUB75 encode: a row clocks exactly its width, so the last column reaches the panel's last pixel") {
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 3, 0, 0xFF, 0, 0);   // the LAST column, red
    setPixel(rgb, geo.width, 0, 0, 0, 0xFF, 0);   // the FIRST column, green
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
    CHECK(geo.frameSlots() == static_cast<size_t>(geo.bitDepth) * geo.width);   // one scan row: width words a plane
    CHECK((slot(out, 0) & (1u << kLayout.g1)) != 0);   // first word: column 0, shifted in first, lands on the far pixel
    CHECK((slot(out, 3) & (1u << kLayout.r1)) != 0);   // the latch word still carries column 3's color
    CHECK((slot(out, 3) & (1u << kLayout.lat)) != 0);
}

namespace {
// How many words light plane p's first row: the words of the row after it that carry the OE (lit) bit. The count runs over the whole pass, hold words included: a hold word inside the window lights the panel.
size_t litWords(const std::vector<uint8_t>& out, const mm::Hub75Geometry& geo, uint8_t p) {
    const size_t base = litRowBase(geo, p);
    size_t n = 0;
    for (size_t x = 0; x < geo.passWords(); x++) n += (slot(out, base + x) & (1u << kLayout.oe)) != 0;
    return n;
}

}  // namespace

// Brightness follows the value: with binary time, 8 (1000) outshines 7 (0111), where equal-time planes would count set bits and light 7 brighter.
TEST_CASE("HUB75 encode: each plane is lit twice as long as the one below, the top plane for the whole row") {
    mm::Hub75Geometry geo;
    geo.width = 9; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 4;   // 8 words that can light, plus the latch
    auto out = encodeFilled(geo, 0);
    CHECK(litWords(out, geo, 0) == 1);
    CHECK(litWords(out, geo, 1) == 2);
    CHECK(litWords(out, geo, 2) == 4);
    CHECK(litWords(out, geo, 3) == 8);
}

// Brightness is time, so every plane keeps its share and only the shortest windows round away, where scaling the values would drop every channel below 4 bits at about 75 and black the panel.
TEST_CASE("HUB75 encode: brightness shortens every plane's window, keeping the values") {
    mm::Hub75Geometry geo;
    geo.width = 9; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 4;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xFF, 0, 0);
    geo.brightness = 128;
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
    CHECK(litWords(out, geo, 3) == 4);   // half of 8
    CHECK(litWords(out, geo, 2) == 2);
    CHECK(litWords(out, geo, 1) == 1);
    CHECK((slot(out, 0) & (1u << kLayout.r1)) != 0);   // the pixel's value is untouched: full red still shifts in
    geo.brightness = 0;
    std::vector<uint8_t> dark(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), dark.data(), geo) == geo.frameBytes());
    for (uint8_t p = 0; p < geo.bitDepth; p++) CHECK(litWords(dark, geo, p) == 0);
}

// Plane 0's window is 1/32 of the top plane's at 6-bit, so near a quarter brightness it rounds to zero: the limit the encoder's docs state.
TEST_CASE("HUB75 encode: below a quarter brightness the lowest 6-bit plane of a 64-column row is dark") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 6; geo.brightness = 64;
    auto out = encodeFilled(geo, 0);
    CHECK(litWords(out, geo, 0) == 0);
    CHECK(litWords(out, geo, 5) == 16);   // the top plane keeps its quarter of 63
}

// A one-column panel: its only shift word is the latch, so the pass holds two words before it and the row encodes lit, not dark. The hold words carry no color and no latch; the latch word still carries the column's color, dark.
TEST_CASE("HUB75 encode: a one-column row holds two words, then latches") {
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xFF, 0, 0);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
    CHECK(geo.holdWords() == 2);
    CHECK(geo.passWords() == 3);
    CHECK(geo.frameSlots() == 6);   // 2 planes x 1 scan row x a 3-word pass
    // Plane 0's group: the top plane's window is 2 words, so both hold words light. Plane 1's: the lowest plane's window is 1 word, so only the first hold word lights.
    CHECK((slot(out, 0) & (1u << kLayout.oe)) != 0);
    CHECK((slot(out, 1) & (1u << kLayout.oe)) != 0);
    CHECK((slot(out, 3) & (1u << kLayout.oe)) != 0);
    CHECK((slot(out, 4) & (1u << kLayout.oe)) == 0);
    for (size_t s : {0u, 1u, 3u, 4u}) {
        CHECK((slot(out, s) & (1u << kLayout.lat)) == 0);   // no latch on a hold word
        CHECK((slot(out, s) & (1u << kLayout.r1)) == 0);    // and no color
    }
    for (size_t s : {2u, 5u}) {
        CHECK((slot(out, s) & (1u << kLayout.lat)) != 0);   // the latch word latches
        CHECK((slot(out, s) & (1u << kLayout.oe)) == 0);    // dark
        CHECK((slot(out, s) & (1u << kLayout.r1)) != 0);    // and the color still shifts in
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

    mm::Hub75Geometry deep;   // above the encoder's ceiling, which is what valid() enforces; 7 and 8 encode, the hold words stretching their narrow rows
    deep.width = 8; deep.height = 8; deep.scanRate = 4; deep.bitDepth = mm::kHub75EncodeMaxBitDepth + 1;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), deep) == 0);

    mm::Hub75Geometry steps;   // 64 address steps on the encoder's five address lines: rows 32 to 63 would alias 0 to 31
    steps.width = 8; steps.height = 128; steps.scanRate = 64; steps.bitDepth = 4;
    CHECK(steps.valid() == false);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), steps) == 0);

    mm::Hub75Geometry huge;   // 4,295,098,368 encoded bytes at 8 bits, past the 4,294,967,295 that 32 bits hold; at 7 bits the same panel fits
    huge.width = 32769; huge.height = 16384; huge.scanRate = 16; huge.bitDepth = 8;
    CHECK(huge.valid() == false);
    huge.bitDepth = 7;
    CHECK(huge.valid());

    mm::Hub75Geometry ok;
    ok.width = 8; ok.height = 8; ok.scanRate = 4; ok.bitDepth = 4;
    CHECK(mm::hub75Encode(nullptr, out.data(), ok) == 0);
    CHECK(mm::hub75Encode(rgb.data(), nullptr, ok) == 0);

    // Every refusal leaves the buffer untouched.
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

namespace {
// The lit-word vector of a geometry, plane by plane, so a test pins the whole vector in one loop.
void checkVector(const std::vector<uint8_t>& out, const mm::Hub75Geometry& geo, std::span<const size_t> want) {
    for (uint8_t p = 0; p < want.size(); p++) CHECK(litWords(out, geo, p) == want[p]);
}
}  // namespace

TEST_CASE("HUB75 encode: at 8 bits every input bit has its own plane, LSB first") {
    // The golden vector: three red pixels whose values pick out the bottom bit, the top bit and every bit, so a plane that read the wrong bit shows at once. An 8-column row at 8 bits holds 121 words, so the color sits past the hold.
    mm::Hub75Geometry geo;
    geo.width = 8; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 8;
    CHECK(geo.valid());
    CHECK(geo.holdWords() == 121);   // 2^(8-1)+1 - 8: the 8-word row stretches to 129
    CHECK(geo.frameSlots() == 8 * 2 * 129);   // 8 planes x 2 scan rows x a 129-word pass

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0x01, 0, 0);   // bit 0 only
    setPixel(rgb, geo.width, 1, 0, 0x80, 0, 0);   // bit 7 only
    setPixel(rgb, geo.width, 2, 0, 0xFF, 0, 0);   // every bit
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

    // Plane p's row 0 starts at slot p * scanRows * passWords, past the hold words; the pixel's red bit rides its own plane's word.
    for (uint8_t p = 0; p < 8; p++) {
        const size_t base = static_cast<size_t>(p) * geo.scanRows() * geo.passWords() + geo.holdWords();
        CHECK(((slot(out, base + 0) >> kLayout.r1) & 1) == (p == 0 ? 1 : 0));   // 0x01: plane 0 alone
        CHECK(((slot(out, base + 1) >> kLayout.r1) & 1) == (p == 7 ? 1 : 0));   // 0x80: plane 7 alone
        CHECK(((slot(out, base + 2) >> kLayout.r1) & 1) == 1);                  // 0xFF: every plane
    }
    // The frame ends on the last row's latch word, dark, as at every depth.
    CHECK((slot(out, geo.frameSlots() - 1) & (1u << kLayout.lat)) != 0);
    CHECK((slot(out, geo.frameSlots() - 1) & (1u << kLayout.oe)) == 0);
}

TEST_CASE("HUB75 encode: at 8 bits the lit windows halve plane by plane on a wide row") {
    // A 256-word row, full brightness: the LSB lights 2 words, the top plane fills the row less its latch. No hold: 256 words is wide enough.
    mm::Hub75Geometry geo;
    geo.width = 256; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
    CHECK(geo.holdWords() == 0);
    const auto out = encodeFilled(geo, 0xFF);
    const size_t want[8] = {2, 4, 8, 16, 32, 64, 128, 255};
    checkVector(out, geo, want);
}

TEST_CASE("HUB75 encode: at 8 bits every plane lights on every row width, narrowest included") {
    // The hold words: a narrow row stretches until its lowest plane is one word, so the eighth bit shows on a 16-column panel at exact powers-of-two ratios, from 16 columns past 256 to 1024 wide, at 7 and 8 bits, full brightness.
    for (uint16_t width : {16, 17, 32, 64, 65, 128, 192, 193, 256, 512, 1024}) {
        for (uint8_t bits : {7, 8}) {
            CAPTURE(width);
            CAPTURE(bits);
            mm::Hub75Geometry geo;
            geo.width = width; geo.height = 32; geo.scanRate = 16; geo.bitDepth = bits;
            const auto out = encodeFilled(geo, 0xFF);
            for (uint8_t p = 0; p < bits; p++) CHECK(litWords(out, geo, p) >= 1);
        }
    }
    // The exact vectors the hold produces at full brightness: a stretched row doubles plane by plane, and where the lowest plane already lights by rounding the hold does not engage.
    struct Row { uint16_t width; uint8_t bits; size_t hold; std::vector<size_t> want; };
    const Row rows[] = {
        {64, 8, 65, {1, 2, 4, 8, 16, 32, 64, 128}},        // 64 to 129 words a pass: the frame doubles
        {16, 8, 113, {1, 2, 4, 8, 16, 32, 64, 128}},       // 16 to 129 words a pass: the frame grows eightfold
        {16, 7, 49, {1, 2, 4, 8, 16, 32, 64}},             // 16 to 65 words a pass
        {64, 7, 0, {1, 2, 4, 8, 16, 32, 63}},   // no hold: the lowest plane is already a word, and the top plane is one short of the row
        {65, 8, 0, {1, 1, 2, 4, 8, 16, 32, 64}},   // 65 words at 8 bits, one past the stretch: the rounding stands, as a 17-word row at 6 bits does
        {33, 7, 0, {1, 1, 2, 4, 8, 16, 32}},   // 33 words at 7 bits, likewise
        {1024, 8, 0, {8, 16, 32, 64, 128, 256, 512, 1023}},   // a 1024-wide row doubles exactly, the top plane one short of the row
    };
    for (const Row& row : rows) {
        CAPTURE(row.width);
        CAPTURE(row.bits);
        mm::Hub75Geometry geo;
        geo.width = row.width; geo.height = 32; geo.scanRate = 16; geo.bitDepth = row.bits;
        CHECK(geo.holdWords() == row.hold);
        checkVector(encodeFilled(geo, 0xFF), geo, row.want);
    }
}

// The low-brightness prediction: the LSB drops out below brightness 64 on a 256-word row and below half brightness on a stretched row (@xref{low-brightness|Technical → Low brightness}).
TEST_CASE("HUB75 encode: at low brightness the lowest 8-bit plane drops out, on a 256-word row and on a stretched row") {
    struct Row { uint16_t width; uint8_t brightness; std::vector<size_t> want; };
    const Row rows[] = {
        {256, 64, {1, 1, 2, 4, 8, 16, 32, 64}},
        {256, 63, {0, 1, 2, 4, 8, 16, 32, 63}},
        {64, 128, {1, 1, 2, 4, 8, 16, 32, 64}},
        {64, 127, {0, 1, 2, 4, 8, 16, 32, 64}},
    };
    for (const Row& row : rows) {
        CAPTURE(row.width);
        CAPTURE(row.brightness);
        mm::Hub75Geometry geo;
        geo.width = row.width; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = row.brightness;
        checkVector(encodeFilled(geo, 0xFF), geo, row.want);
    }
}

TEST_CASE("HUB75 encode: at 6 bits every row of 1 to 16 words stretches to a 33-word pass and a row of 17 words and up keeps its rounding") {
    // The hold words and the vectors they give (@xref{hold-words-at-6-bits|Technical → Hold words at 6 bits}).
    for (uint16_t width = 1; width <= 16; width++) {
        CAPTURE(width);
        mm::Hub75Geometry geo;
        geo.width = width; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 6;
        CHECK(geo.passWords() == 33);
        const size_t want[6] = {1, 2, 4, 8, 16, 32};
        checkVector(encodeFilled(geo, 0xFF), geo, want);
    }
    struct Row { uint16_t width; std::vector<size_t> want; };
    const Row rows[] = {
        {17, {1, 1, 2, 4, 8, 16}},
        {64, {2, 4, 8, 16, 32, 63}},
        {256, {8, 16, 32, 64, 128, 255}},
    };
    for (const Row& row : rows) {
        CAPTURE(row.width);
        mm::Hub75Geometry geo;
        geo.width = row.width; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 6;
        CHECK(geo.holdWords() == 0);
        checkVector(encodeFilled(geo, 0xFF), geo, row.want);
    }
}

TEST_CASE("HUB75 encode: at every depth from 2 to 8 a row of 2^(depth-2) words stretches to exact binary windows and one word more keeps its rounding") {
    // The threshold at each depth, including the ESP32-selectable 3, 4 and 5: a stretched pass is 2^(depth-1) + 1 words and plane p lights 2^p of them; the next width holds none, and its windows are the rounded 2^p x 2^(depth-2) / 2^(depth-1).
    for (uint8_t bits = 2; bits <= 8; bits++) {
        CAPTURE(bits);
        const uint16_t narrow = static_cast<uint16_t>(1u << (bits - 2));
        mm::Hub75Geometry stretched;
        stretched.width = narrow; stretched.height = 32; stretched.scanRate = 16; stretched.bitDepth = bits;
        CHECK(stretched.holdWords() == (1u << (bits - 1)) + 1 - narrow);
        CHECK(stretched.passWords() == (1u << (bits - 1)) + 1);
        std::vector<size_t> exact, rounded;
        for (uint8_t p = 0; p < bits; p++) {
            exact.push_back(size_t{1} << p);
            rounded.push_back(p == 0 ? 1 : size_t{1} << (p - 1));
        }
        checkVector(encodeFilled(stretched, 0xFF), stretched, exact);

        mm::Hub75Geometry wider = stretched;
        wider.width = static_cast<uint16_t>(narrow + 1);
        CHECK(wider.holdWords() == 0);
        checkVector(encodeFilled(wider, 0xFF), wider, rounded);
    }
}

TEST_CASE("HUB75 encode: a row of 196,605 words keeps its top plane's window whole, past what 32 bits hold") {
    // The window numerator is (pass - 1) x brightness x 2^plane, which exceeds 2^32 from about 131,600 row words at 8 bits: 65,535 columns and 3 color passes a step is 196,605. A 32-bit numerator would wrap the top plane's window.
    mm::Hub75Geometry geo;
    geo.width = 65535; geo.height = 12; geo.scanRate = 2; geo.bitDepth = 8;
    REQUIRE(geo.valid());
    CHECK(geo.rowWords() == 196605);
    CHECK(geo.holdWords() == 0);
    const auto out = encodeFilled(geo, 0xFF);
    CHECK(litWords(out, geo, 7) == 196604);   // the top plane fills the pass less its latch
    CHECK(litWords(out, geo, 0) == 1536);     // 196,604 / 128, rounded
}

TEST_CASE("HUB75 encode: an empty row takes no hold words") {
    mm::Hub75Geometry empty;
    empty.width = 0;
    CHECK(empty.holdWords() == 0);
    CHECK(empty.frameBytes() == 0);
}

TEST_CASE("HUB75 encode: brightness 0 stays dark at 8 bits") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = 0;
    const auto out = encodeFilled(geo, 0xFF);
    for (uint8_t p = 0; p < 8; p++) CHECK(litWords(out, geo, p) == 0);
}

TEST_CASE("HUB75 encode: the hold words carry no color and no latch, are addressed to the latched row, and light inside the window") {
    // A 16-column row at 8 bits: 113 hold words lead each group. They push out of the shift register before the latch, so they carry no color; they name the latched row, so the light they give falls on the row the latch holds.
    mm::Hub75Geometry geo;
    geo.width = 16; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
    CHECK(geo.holdWords() == 113);
    CHECK(geo.passWords() == 129);
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);   // white: every color bit set where a word carries data
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());

    const uint8_t colorBits = static_cast<uint8_t>(
        (1u << kLayout.r1) | (1u << kLayout.g1) | (1u << kLayout.b1) |
        (1u << kLayout.r2) | (1u << kLayout.g2) | (1u << kLayout.b2));
    // Row 0's words name the last row, 15: A-D set, E clear.
    const uint16_t lastRowAddr = static_cast<uint16_t>((1u << kLayout.a) | (1u << kLayout.b) | (1u << kLayout.c) | (1u << kLayout.d));
    const uint16_t addrBits = static_cast<uint16_t>((1u << kLayout.a) | (1u << kLayout.b) | (1u << kLayout.c) | (1u << kLayout.d) | (1u << kLayout.e));
    for (size_t i = 0; i < geo.holdWords(); i++) {
        CHECK((slot(out, i) & colorBits) == 0);              // no color
        CHECK((slot(out, i) & (1u << kLayout.lat)) == 0);        // no latch
        CHECK((slot(out, i) & addrBits) == lastRowAddr);     // addressed to the latched row
    }
    // Plane 0's row 0 shows the top plane's window (128 words), so the hold words light; plane 1's shows the lowest plane's (1 word), so only its first hold word lights.
    CHECK((slot(out, geo.holdWords() - 1) & (1u << kLayout.oe)) != 0);
    const size_t plane1 = static_cast<size_t>(geo.scanRows()) * geo.passWords();
    CHECK((slot(out, plane1) & (1u << kLayout.oe)) != 0);
    CHECK((slot(out, plane1 + 1) & (1u << kLayout.oe)) == 0);
    // The shift words follow the hold words: the first carries column 0's color, the last latches, dark, carrying the last column's.
    CHECK((slot(out, geo.holdWords()) & colorBits) != 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << kLayout.lat)) != 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << kLayout.oe)) == 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << kLayout.r1)) != 0);
}
