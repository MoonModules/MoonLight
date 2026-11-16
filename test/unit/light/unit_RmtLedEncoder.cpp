/// @module RmtLedDriver
/// @also Correction

#include "doctest.h"
#include "light/drivers/RmtSymbol.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"

#include <cstdint>

// The wire bytes and their bit shapes are the CI-tier proof of correctness for the LED driver.
// The bit EXPANSION itself no longer runs on the host: the driver hands the peripheral wire bytes and the expansion happens in the IDF bytes encoder or, on the classic ESP32, in the level-5 refill. Neither exists off-target, so these tests pin the two halves that ARE host-visible:
//   - makeRmtSymbol's word layout, which is what pushBitTiming programs the hardware with;
//   - Correction's channel ordering, which decides which byte goes out first.
// `expand` below mirrors what the hardware does (MSB-first, one symbol per data bit) so the contract stays legible and a change in bit order still fails a test rather than only a wall.

namespace {

// Reference expander: what the bytes encoder and rmtHiFill both do to each wire byte. MSB-first, one symbol per bit, `sym1` for a set bit. Mirrors the hardware so the ordering contract is checkable on the host; it is not the shipping path.
void expand(const uint8_t* wire, uint8_t channels, uint16_t t0h, uint16_t t1h,
            uint16_t period, uint32_t* out) {
    const uint32_t sym0 = mm::makeRmtSymbol(t0h, 1, static_cast<uint16_t>(period - t0h), 0);
    const uint32_t sym1 = mm::makeRmtSymbol(t1h, 1, static_cast<uint16_t>(period - t1h), 0);
    size_t s = 0;
    for (uint8_t ch = 0; ch < channels; ch++)
        for (int bit = 7; bit >= 0; bit--)
            out[s++] = (wire[ch] & (1u << bit)) ? sym1 : sym0;
}

// Default WS2812B timing at a 40 MHz / 25 ns-per-tick RMT resolution: t0h 350 ns -> 14 ticks,  t1h 700 ns -> 28 ticks,  period 1250 ns -> 50 ticks.
constexpr uint16_t T0H = 14;
constexpr uint16_t T1H = 28;
constexpr uint16_t PERIOD = 50;

// Decode a symbol word back to its two (level, duration) halves for assertions.
struct Half { uint8_t level; uint16_t duration; };
Half low16(uint32_t s)  { return { static_cast<uint8_t>((s >> 15) & 1), static_cast<uint16_t>(s & 0x7FFF) }; }
Half high16(uint32_t s) { return { static_cast<uint8_t>((s >> 31) & 1), static_cast<uint16_t>((s >> 16) & 0x7FFF) }; }

// Assert one symbol is a correct WS2812 bit: HIGH for `highTicks`, then LOW for (PERIOD - highTicks).
void checkBit(uint32_t sym, uint16_t highTicks) {
    Half h0 = low16(sym);
    Half h1 = high16(sym);
    CHECK(h0.level == 1);
    CHECK(h0.duration == highTicks);
    CHECK(h1.level == 0);
    CHECK(h1.duration == static_cast<uint16_t>(PERIOD - highTicks));
}

} // namespace

TEST_CASE("wire bytes expand MSB-first, 0 and 1 bits get the right pulse widths") {
    // 0xA5 = 1010 0101, MSB first.
    const uint8_t wire[1] = {0xA5};
    uint32_t out[8] = {};
    expand(wire, 1, T0H, T1H, PERIOD, out);

    const uint8_t bits[8] = {1, 0, 1, 0, 0, 1, 0, 1};  // MSB..LSB of 0xA5
    for (int i = 0; i < 8; i++) {
        checkBit(out[i], bits[i] ? T1H : T0H);
    }
}

TEST_CASE("a light's channels go out in wire-byte order, 8 bits each") {
    // One light of `channels` wire bytes: byte 0, then byte 1, then byte 2, eight bits each.
    const uint8_t wire[3] = {0xFF, 0x00, 0x80};  // byte0 all-ones, byte1 zero, byte2 MSB set
    uint32_t out[24] = {};
    expand(wire, 3, T0H, T1H, PERIOD, out);

    checkBit(out[0], T1H);   // byte0 MSB = 1
    checkBit(out[7], T1H);   // byte0 LSB = 1 (0xFF)
    checkBit(out[8], T0H);   // byte1 MSB = 0 (0x00)
    checkBit(out[15], T0H);  // byte1 LSB = 0
    checkBit(out[16], T1H);  // byte2 MSB = 1 (0x80)
    checkBit(out[17], T0H);  // byte2 bit 1 = 0
}

TEST_CASE("GRB ordering comes from Correction: the driver ships whatever bytes it produced") {
    // Correction with GRB preset turns logical RGB into wire GRB; the encoder then just emits the bytes it's handed. Logical red (255,0,0) → wire GRB (0,255,0): green byte first. So the FIRST 8 symbols (wire byte 0 = G = 0x00) are all 0s, and the SECOND 8 (wire byte 1 = R = 0xFF) are all 1s.
    mm::Correction c;
    mm::rebuildFromPreset(c, 255, mm::PresetOrder::GRB);   // full brightness, GRB
    const uint8_t logicalRed[3] = {255, 0, 0};
    uint8_t wire[4] = {};
    c.apply(logicalRed, wire, 3);              // -> GRB: {0, 255, 0}

    uint32_t out[24] = {};
    expand(wire, c.outChannels, T0H, T1H, PERIOD, out);

    for (int i = 0; i < 8; i++)  checkBit(out[i], T0H);       // G byte = 0x00
    for (int i = 8; i < 16; i++) checkBit(out[i], T1H);       // R byte = 0xFF
    for (int i = 16; i < 24; i++) checkBit(out[i], T0H);      // B byte = 0x00
}

TEST_CASE("an RGBW preset puts four bytes per light on the wire") {
    mm::Correction c;
    mm::rebuildFromPreset(c, 255, mm::PresetOrder::GRBW);  // 4 output channels
    CHECK(c.outChannels == 4);
    const uint8_t logical[3] = {10, 20, 30};
    uint8_t wire[4] = {};
    c.apply(logical, wire, 3);

    uint32_t out[32] = {};
    expand(wire, c.outChannels, T0H, T1H, PERIOD, out);
    // 4 channels * 8 bits = 32 symbols; spot-check the last symbol is a valid bit.
    Half h0 = low16(out[31]);
    CHECK(h0.level == 1);
    CHECK((h0.duration == T0H || h0.duration == T1H));
}
