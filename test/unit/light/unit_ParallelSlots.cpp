/// @module I80Peripheral
/// @also Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/ParallelSlots.h"

#include <cstring>
#include <vector>

// The success spec for the LCD_CAM 3-slot encode, written RED before the encoder exists (the increment-1 methodology): a known wire row + lane mask → the exact slot-byte stream. Pins the transpose (one bus byte carries one bit of every lane), MSB-first bit order, and the lanes-active-mask rule that keeps short strands idle-LOW.

namespace {

// out holds channels*8 triplets of (slot0, slot1, slot2).
struct Slots {
    uint8_t bytes[4 * 8 * 3];   // up to 4 channels
    const uint8_t* triplet(int bit) const { return bytes + bit * 3; }
};

} // namespace

// One lane, one byte 0xA5: slot0 always the mask, slot1 follows the bits MSB-first, slot2 always zero.
TEST_CASE("LCD encoder: one lane, MSB-first, 3 slots per bit") {
    uint8_t wire[8 * 4] = {};
    wire[0] = 0xA5;   // lane 0, channel 0: 1010 0101
    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 1, s.bytes);

    const uint8_t expectBits[8] = {1, 0, 1, 0, 0, 1, 0, 1};
    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x01);                          // pulse start: active lanes HIGH
        CHECK(t[1] == (expectBits[bit] ? 0x01 : 0x00)); // data slot
        CHECK(t[2] == 0x00);                          // pulse tail: all LOW
    }
}

// Two lanes 0xFF/0x00 in one row: the data slot carries lane 0's bit only, the transpose itself.
TEST_CASE("LCD encoder: transpose across two lanes") {
    uint8_t wire[8 * 1] = {};   // stride = channels = 1
    wire[0 * 1 + 0] = 0xFF;   // lane 0: all ones
    wire[1 * 1 + 0] = 0x00;   // lane 1: all zeros
    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x03), 1, s.bytes);

    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x03);   // both lanes pulse-start HIGH
        CHECK(t[1] == 0x01);   // only lane 0 carries a 1
        CHECK(t[2] == 0x00);
    }
}

// A lane excluded from the mask contributes to NEITHER slot 0 nor slot 1, even with garbage wire bytes, short strands idle LOW (no white flashes).
TEST_CASE("LCD encoder: inactive lanes stay LOW regardless of wire content") {
    uint8_t wire[8 * 4];
    std::memset(wire, 0xFF, sizeof(wire));   // garbage everywhere
    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 1, s.bytes);   // only lane 0 active

    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x01);   // lane 1..7 absent from the pulse start
        CHECK(t[1] == 0x01);   // and from the data slot
        CHECK(t[2] == 0x00);
    }
}

// Mask 0 (a row past every lane's strand) is a fully idle row.
TEST_CASE("LCD encoder: empty mask emits all-zero slots") {
    uint8_t wire[8 * 4];
    std::memset(wire, 0xFF, sizeof(wire));
    Slots s{};
    std::memset(s.bytes, 0xEE, sizeof(s.bytes));
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x00), 1, s.bytes);
    for (int i = 0; i < 8 * 3; i++) CHECK(s.bytes[i] == 0x00);
}

// Channel order comes from Correction (logical red → GRB wire {0,255,0}); the encoder is order-agnostic.
TEST_CASE("LCD encoder: GRB ordering via Correction") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    const uint8_t rgb[3] = {255, 0, 0};   // logical red
    uint8_t wire[8 * 4] = {};
    corr.apply(rgb, wire, 3);                 // lane 0 wire = {0, 255, 0}

    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 3, s.bytes);
    for (int bit = 0; bit < 8; bit++) {
        CHECK(s.triplet(bit)[1] == 0x00);        // G byte: all zero data
        CHECK(s.triplet(8 + bit)[1] == 0x01);    // R byte: all ones data
        CHECK(s.triplet(16 + bit)[1] == 0x00);   // B byte: all zero data
    }
}

// RGBW rows emit 4 channels × 8 bits × 3 slots = 96 bytes.
TEST_CASE("LCD encoder: RGBW row is 96 slot bytes") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRBW);
    const uint8_t rgb[3] = {10, 10, 10};
    uint8_t wire[8 * 4] = {};
    corr.apply(rgb, wire, 3);

    uint8_t out[4 * 8 * 3];
    std::memset(out, 0xEE, sizeof(out));
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 4, out);
    // The last triplet was written (its tail slot is 0, not the 0xEE poison).
    CHECK(out[4 * 8 * 3 - 1] == 0x00);
}

// The branch-free SWAR transpose must equal the naive per-bit-per-lane gather it replaced, for EVERY lane pattern and mask, the whole point is a behavior-identical speedup. Pin it directly (not just via a few golden rows) so a future edit to the delta-swap constants can't silently corrupt a plane.
TEST_CASE("LCD encoder: SWAR transpose equals the naive gather for all patterns") {
    auto naiveData = [](const uint8_t* lane8, uint8_t mask, int bit) -> uint8_t {
        uint8_t data = 0;
        for (uint8_t lane = 0; lane < 8; lane++)
            if (mask & (1u << lane))
                data |= static_cast<uint8_t>(((lane8[lane] >> bit) & 1u) << lane);
        return data;
    };
    uint8_t lanes[8];
    for (int trial = 0; trial < 4096; trial++) {
        uint32_t s = static_cast<uint32_t>(trial) * 2654435761u + 1u;
        for (int i = 0; i < 8; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; lanes[i] = s & 0xFF; }
        const uint8_t mask = static_cast<uint8_t>((trial * 40503u) & 0xFF);
        uint8_t masked[8];
        for (int i = 0; i < 8; i++) masked[i] = (mask & (1u << i)) ? lanes[i] : 0;
        uint8_t plane[8];
        mm::transposeLanes8x8(masked, plane);
        for (int bit = 0; bit < 8; bit++) CHECK(plane[bit] == naiveData(lanes, mask, bit));
    }
}

// The 16-lane transpose (transposeLanes16x8, for the 16-bit bus) must equal the naive 16-lane gather for EVERY pattern and mask, the low byte of each uint16 plane is lanes 0..7, the high byte lanes 8..15. Cycles mask shapes including high-lane-only (lanes 8..15 set, 0..7 clear), the case the byte-split hinges on.
TEST_CASE("LCD encoder: SWAR 16-lane transpose equals the naive gather") {
    auto naive16 = [](const uint8_t* lane16, uint16_t mask, int bit) -> uint16_t {
        uint16_t d = 0;
        for (uint8_t lane = 0; lane < 16; lane++)
            if (mask & (1u << lane))
                d |= static_cast<uint16_t>(((lane16[lane] >> bit) & 1u) << lane);
        return d;
    };
    uint8_t lanes[16];
    for (int trial = 0; trial < 4096; trial++) {
        uint32_t s = static_cast<uint32_t>(trial) * 2246822519u + 7u;
        for (int i = 0; i < 16; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; lanes[i] = s & 0xFF; }
        uint16_t mask;
        if (trial % 3 == 0)      mask = static_cast<uint16_t>(trial * 40503u);
        else if (trial % 3 == 1) mask = static_cast<uint16_t>((trial * 40503u) & 0x00FF);
        else                     mask = static_cast<uint16_t>((trial * 40503u) & 0xFF00);
        uint8_t masked[16];
        for (int i = 0; i < 16; i++) masked[i] = (mask & (1u << i)) ? lanes[i] : 0;
        uint16_t plane[8];
        mm::transposeLanes16x8(masked, plane);
        for (int bit = 0; bit < 8; bit++) CHECK(plane[bit] == naive16(lanes, mask, bit));
    }
}

// 16-lane golden encoder cases: the uint16 slot carries 16 data lines. Prove a high lane (15) lands in the plane's high byte, and a boundary pair (7 + 8). The wire lane stride is `channels` (here 1), so lane L's byte 0 is wire[L * channels].
TEST_CASE("LCD encoder 16-lane: high lane and byte-boundary transpose") {
    uint16_t out[3 * 8];   // 1 channel × 8 bits × 3 slots, uint16 slots
    {   // lane 15 all-ones → data slot has bit 15 set on every bit (stride = channels = 1)
        uint8_t wire[16 * 1] = {};
        wire[15 * 1] = 0xFF;
        mm::encodeWs2812ParallelSlots<uint16_t>(wire, static_cast<uint16_t>(1u << 15), 1, out);
        for (int bit = 0; bit < 8; bit++) {
            CHECK(out[bit * 3 + 0] == (1u << 15));   // slot0: active mask (lane 15)
            CHECK(out[bit * 3 + 1] == (1u << 15));   // slot1: lane 15's bit
            CHECK(out[bit * 3 + 2] == 0);            // slot2: tail
        }
    }
    {   // lane 7 = 0xFF, lane 8 = 0xFF → data slot has bits 7 AND 8 (across the byte split)
        uint8_t wire[16 * 1] = {};
        wire[7 * 1] = 0xFF; wire[8 * 1] = 0xFF;
        const uint16_t mask = static_cast<uint16_t>((1u << 7) | (1u << 8));
        mm::encodeWs2812ParallelSlots<uint16_t>(wire, mask, 1, out);
        for (int bit = 0; bit < 8; bit++)
            CHECK(out[bit * 3 + 1] == mask);         // both lanes carry a 1 on every bit
    }
}

// RGBW 16-lane row: 4 channels × 8 bits × 3 slots = 96 uint16 slots, all written.
TEST_CASE("LCD encoder 16-lane: RGBW row is 96 uint16 slots, all written") {
    uint8_t wire[16 * 4] = {};
    wire[0] = 10; wire[1] = 10; wire[2] = 10; wire[3] = 10;   // lane 0 RGBW
    uint16_t out[4 * 8 * 3];
    for (auto& v : out) v = 0xEEEE;   // poison
    mm::encodeWs2812ParallelSlots<uint16_t>(wire, static_cast<uint16_t>(0x0001), 4, out);
    CHECK(out[4 * 8 * 3 - 1] == 0);   // last tail slot written (not the poison)
}

// ---------------------------------------------------------------------------
// Shift-register (74HCT595) encode. The strands are not on the GPIOs here: each data pin feeds a '595 whose 8 outputs are the strands. A '595 is serial-in, so every WS2812 slot above becomes kPinExpanderOutputs shift cycles, and a LATCH bit (a real bus lane) presents the byte on the last one. These tests pin the parts that no host can otherwise prove until the physical board exists: the shift ordering, the latch timing, and which strand lands on which output.
// ---------------------------------------------------------------------------

namespace {

constexpr uint8_t kSh = mm::kPinExpanderOutputs;   // 8

// Slots per WS2812 bit in shift mode: 3 (start/data/tail) x kPinExpanderOutputs cycles.
constexpr int kSlotsPerBit = 3 * kSh;

// The encoder writes channels*8 bits, each kSlotsPerBit slots.
inline int bitBase(int bit) { return bit * kSlotsPerBit; }

// ---------------------------------------------------------------------------
// A 74HCT595 SIMULATOR, the thing these tests should have had from the start.
//
// Asserting on raw bus-word indices pins the encoder's INTERNAL LAYOUT, not its behavior: the original latch test asserted "latch on the LAST word of a slot", passed for two days, and the panel was broken the whole time. What a strand actually receives is the only contract that matters, and it depends on how the '595 works:
//
//   - The bus's pixel clock (WR) is the '595's SHIFT clock: EVERY bus word clocks one bit into the
//     shift register (bit P of the word -> the register on physical pin P).
//   - The '595's OUTPUTS do not change while shifting. They update only on the LATCH (RCLK) rising
//     edge, which copies the shift register into the storage register.
//   - So during any bus word, the strand sees the storage register, i.e. the byte latched at the
//     most recent latch edge. That is a ONE-SLOT PIPELINE: what you clock in during slot N appears
//     on the wire during slot N+1.
//
// This simulator replays the emitted words through that model and returns, per bus word, the level each strand actually sees. Tests then assert on the WAVEFORM, which cannot silently drift.
struct ShiftSim {
    // level[word][strand], what strand `s` sees on the wire during bus word `word`.
    std::vector<std::vector<bool>> level;

    // Replay `nWords` bus words through physPins '595s of `outPerPin` outputs each.
    ShiftSim(const uint8_t* words, int nWords, uint8_t physPins, uint8_t latchBit,
             uint8_t outPerPin) {
        const int nStrands = physPins * outPerPin;
        std::vector<uint16_t> shiftReg(physPins, 0);     // the serial shift registers
        std::vector<uint16_t> storage(physPins, 0);      // the latched outputs
        bool prevLatch = false;
        level.reserve(nWords);
        for (int w = 0; w < nWords; w++) {
            const uint8_t word = words[w];
            const bool latchNow = (word >> latchBit) & 1u;
            // RCLK rising edge: copy shift -> storage. Do this BEFORE this word's shift, because the latch and the shift clock ride the same bus word and RCLK captures what is already in.
            if (latchNow && !prevLatch) storage = shiftReg;
            prevLatch = latchNow;
            // SRCLK: every bus word shifts one bit in, per physical pin.
            for (uint8_t p = 0; p < physPins; p++) {
                const bool bit = (word >> p) & 1u;
                shiftReg[p] = static_cast<uint16_t>((shiftReg[p] << 1) | (bit ? 1u : 0u));
            }
            // What each strand sees right now = its bit of the STORAGE register.
            std::vector<bool> row(nStrands, false);
            for (uint8_t p = 0; p < physPins; p++)
                for (uint8_t o = 0; o < outPerPin; o++)
                    row[p * outPerPin + o] = (storage[p] >> o) & 1u;
            level.push_back(row);
        }
    }

    // The level strand `s` sees during bus word `w`.
    bool at(int w, int s) const { return level[w][s]; }
};

} // namespace

// The latch is pulsed on the LAST shift cycle of every slot and nowhere else, that is what presents the shifted byte on the '595 outputs. A latch that fired early would present a half-shifted byte; one that never fired would leave the strands dark.
// The latch fires on the FIRST bus word of each slot, never the last, and this is THE bug that broke the first bench run, so it is pinned hard.
//
// The i80 WR (pixel clock) is the '595's shift clock, so every bus word clocks one bit in, INCLUDING the word carrying the latch. RCLK is rising-edge triggered, so it must fire when the slot's 8 bits are already all in, which is one word AFTER the last shift word, i.e. word 0 of the NEXT slot.
// Latching on the LAST word (the intuitive choice, and what this encoder shipped first) asserts RCLK while the 8th bit is still being clocked, so the '595 presents a byte shifted one short: on real hardware only the first LED or two of every strand lit.
//
// The old version of this test asserted `latch iff LAST cycle`, it passed while the panel was broken, which is exactly why the assertion is now written the other way round.
TEST_CASE("shift encoder: latch pulses on the FIRST cycle of each slot (not the last)") {
    uint8_t wire[128 * 3] = {};
    wire[0] = 0xFF;                       // strand 0 (pin 0, shift pos 7), channel 0
    uint8_t out[8 * kSlotsPerBit] = {};
    const uint8_t latchBit = 4;           // 4 data pins → latch on bus bit 4
    mm::encodeWs2812ShiftSlots<uint8_t>(wire, /*activeMask=*/1u, /*physPins=*/4, latchBit, kSh, 1, out);

    const uint8_t latch = static_cast<uint8_t>(1u << latchBit);
    for (int bit = 0; bit < 8; bit++) {
        for (int slot = 0; slot < 3; slot++) {         // start, data, tail
            for (int c = 0; c < kSh; c++) {
                const uint8_t w = out[bitBase(bit) + slot * kSh + c];
                const bool isFirst = (c == 0);
                CHECK(((w & latch) != 0) == isFirst);  // latch iff FIRST cycle of the slot
            }
        }
    }
}

// A '595 shifts MSB-of-the-register-first: the bit clocked in FIRST ends up on the LAST output (QH). So strand V (output V) must be carried on shift cycle (kPinExpanderOutputs-1-V). Get this backwards and every strand lights its neighbor's data, the failure mode that is nearly impossible to debug on a wired panel, so it is pinned here.
TEST_CASE("shift encoder: strand N rides the correct shift cycle ('595 MSB-first)") {
    for (uint8_t strand = 0; strand < kSh; strand++) {
        uint8_t wire[128 * 3] = {};
        wire[strand * 1] = 0xFF;   // channels=1: strand `strand`, channel 0, all bits set
        uint8_t out[8 * kSlotsPerBit] = {};
        const uint8_t latchBit = 1;   // 1 data pin → latch on bus bit 1
        mm::encodeWs2812ShiftSlots<uint8_t>(wire, uint64_t(1) << strand, /*physPins=*/1,
                                            latchBit, kSh, 1, out);
        // Data byte is 0xFF, so every DATA slot cycle that carries this strand must set pin 0's bit; every other cycle must not.
        const uint8_t expectCycle = static_cast<uint8_t>(kSh - 1 - strand);
        for (int bit = 0; bit < 8; bit++) {
            for (int c = 0; c < kSh; c++) {
                const uint8_t w = out[bitBase(bit) + 1 * kSh + c];   // the DATA slot
                const bool pin0 = (w & 0x01) != 0;
                CHECK(pin0 == (c == expectCycle));
            }
        }
    }
}

// The whole point of the fan-out: strands on DIFFERENT physical pins ride the same shift cycle in parallel (different bus bits), while strands on the SAME pin are serialized across cycles. This is why extra strands are free but the x8 is not.
TEST_CASE("shift encoder: strands on different pins share a cycle, same pin serialize") {
    uint8_t wire[128 * 3] = {};
    // channels=1. Strand 0 = pin 0 / pos 0; strand 8 = pin 1 / pos 0 → same shift cycle.
    wire[0] = 0xFF;   // strand 0  → pin 0
    wire[8] = 0xFF;   // strand 8  → pin 1
    uint8_t out[8 * kSlotsPerBit] = {};
    const uint8_t latchBit = 2;   // 2 data pins → latch on bus bit 2
    const uint64_t mask = (uint64_t(1) << 0) | (uint64_t(1) << 8);
    mm::encodeWs2812ShiftSlots<uint8_t>(wire, mask, /*physPins=*/2, latchBit, kSh, 1, out);

    const uint8_t cycle = static_cast<uint8_t>(kSh - 1 - 0);   // both are shift pos 0
    for (int bit = 0; bit < 8; bit++) {
        const uint8_t w = out[bitBase(bit) + 1 * kSh + cycle];   // DATA slot, their cycle
        CHECK((w & 0x01) != 0);   // pin 0 carries strand 0
        CHECK((w & 0x02) != 0);   // pin 1 carries strand 8 — SAME cycle, parallel
    }
}

// A strand whose strip is shorter than the longest must idle LOW for the rest of the frame (the activeMask rule), it must not flash white. Same contract as the direct encoder, but it has to survive the fan-out: an inactive strand contributes no set bit on ANY cycle.
TEST_CASE("shift encoder: inactive strands idle LOW on every cycle") {
    uint8_t wire[128 * 3] = {};
    for (auto& b : wire) b = 0xFF;   // every strand's wire is hot...
    uint8_t out[8 * kSlotsPerBit] = {};
    const uint8_t latchBit = 2;
    // ...but only strand 0 is ACTIVE. Strand 1 (pin 0, pos 1) must stay dark.
    mm::encodeWs2812ShiftSlots<uint8_t>(wire, /*activeMask=*/1u, /*physPins=*/2, latchBit, kSh, 1, out);

    const uint8_t inactiveCycle = static_cast<uint8_t>(kSh - 1 - 1);   // strand 1's cycle
    for (int bit = 0; bit < 8; bit++) {
        // Pin 1 has no active strand at all → never set, on any cycle or slot.
        for (int slot = 0; slot < 3; slot++)
            for (int c = 0; c < kSh; c++)
                CHECK((out[bitBase(bit) + slot * kSh + c] & 0x02) == 0);
        // Pin 0 on strand 1's cycle: the wire byte is 0xFF but the strand is inactive, so the data bit must still be 0.
        CHECK((out[bitBase(bit) + 1 * kSh + inactiveCycle] & 0x01) == 0);
        // ...and the same on the PULSE-START slot. This is the case a per-pin "any live lane" mask misses: strands 0 and 1 SHARE physical pin 0, so a mask that says "pin 0 has an active lane" drives the start-pulse HIGH on *every* cycle of that pin, including the cycle that clocks strand 1's shift position. The inactive strand then presents a full WS2812 pulse-start and lights white. The activity test is per STRAND (per cycle), not per pin.
        CHECK((out[bitBase(bit) + 0 * kSh + inactiveCycle] & 0x01) == 0);
    }
}

// The real-world shape of the rule above, and the one that bites on hardware: two strands on the SAME '595, one longer than the other. Once the short strand is exhausted its activeMask bit clears while its neighbor keeps rendering, so the pin stays busy, and only a PER-CYCLE activity test can keep the exhausted strand dark. (A per-pin "has any live lane" mask drives the pulse-start HIGH on every cycle of that pin, and the short strand flashes white at full brightness for the rest of the frame.)
TEST_CASE("shift encoder: an exhausted strand stays dark while its pin-mate keeps rendering") {
    uint8_t wire[128 * 3] = {};
    for (auto& b : wire) b = 0xFF;   // both strands' wire bytes are hot
    uint8_t out[8 * kSlotsPerBit] = {};
    const uint8_t latchBit = 3;
    // Strand 0 (pin 0, pos 0) active; strand 1 (pin 0, pos 1) EXHAUSTED, same physical pin.
    mm::encodeWs2812ShiftSlots<uint8_t>(wire, /*activeMask=*/1u, /*physPins=*/1, latchBit, kSh, 1, out);

    const uint8_t liveCycle = static_cast<uint8_t>(kSh - 1 - 0);   // strand 0's cycle
    const uint8_t deadCycle = static_cast<uint8_t>(kSh - 1 - 1);   // strand 1's cycle
    for (int bit = 0; bit < 8; bit++) {
        // The live strand still gets its full pulse: HIGH start, data, LOW tail.
        CHECK((out[bitBase(bit) + 0 * kSh + liveCycle] & 0x01) != 0);
        // The exhausted strand clocks in 0 on EVERY slot of its own cycle, start included.
        CHECK((out[bitBase(bit) + 0 * kSh + deadCycle] & 0x01) == 0);
        CHECK((out[bitBase(bit) + 1 * kSh + deadCycle] & 0x01) == 0);
        CHECK((out[bitBase(bit) + 2 * kSh + deadCycle] & 0x01) == 0);
    }
}

// The pulse-start slot clocks in a 1 for every ACTIVE STRAND (so the '595 presents the start of the WS2812 pulse on that output), and the tail slot clocks in zeros. Note this is a per-CYCLE property, not a per-pin one: cycle c carries shift position `kSh-1-c`, so the bit belongs to exactly one strand of that pin. With all 8 strands of a pin active, the pin is HIGH on all 8 start-slot cycles, clocking in 0xFF, which the '595 presents as all-outputs-HIGH.
TEST_CASE("shift encoder: start slot is HIGH and tail slot LOW across every cycle") {
    uint8_t wire[128 * 3] = {};
    wire[0] = 0x00;   // data all zero — proves start/tail levels don't depend on the data
    uint8_t out[8 * kSlotsPerBit] = {};
    const uint8_t latchBit = 1;
    // All kSh strands of pin 0 active, so every start-slot cycle has a live strand.
    const uint64_t allOnPin0 = (uint64_t(1) << kSh) - 1u;
    mm::encodeWs2812ShiftSlots<uint8_t>(wire, allOnPin0, /*physPins=*/1, latchBit, kSh, 1, out);

    const uint8_t latch = static_cast<uint8_t>(1u << latchBit);
    for (int bit = 0; bit < 8; bit++) {
        for (int c = 0; c < kSh; c++) {
            // start slot: pin 0 HIGH on every cycle (latch bit masked off)
            CHECK((out[bitBase(bit) + 0 * kSh + c] & ~latch) == 0x01);
            // tail slot: everything LOW (latch bit masked off)
            CHECK((out[bitBase(bit) + 2 * kSh + c] & ~latch) == 0x00);
        }
    }
}

// =========================================================================== THE TEST THAT MATTERS: replay the emitted words through a '595 simulator and check the strand receives a real WS2812 waveform. Every earlier shift test asserted bus-word indices, the encoder's internal layout, and they all passed while the panel showed garbage. This one asserts what the LED actually sees, so it fails when the hardware would.
//
// The WS2812 wire contract (ParallelSlots.h): each data bit is 3 slots, all-HIGH pulse start, the data bit, all-LOW tail. So a "1" is HIGH for 2 slots and a "0" for 1 slot.
TEST_CASE("shift encoder: the STRAND receives a correct WS2812 waveform (595 pipeline modelled)") {
    constexpr uint8_t kPins = 2;      // 2 '595s, like the 15-strand bench board
    constexpr uint8_t kLatchBit = 2;  // bus bit 2 = latch (data on bits 0,1)
    constexpr uint8_t kCh = 1;        // one channel keeps the expected stream short

    // Strand 0 gets 0xA5 = 1010 0101. Strand 0 lives on pin 0, shift position 0.
    uint8_t wire[128 * kCh] = {};
    wire[0] = 0xA5;

    const int nWords = kCh * 8 * 3 * kSh;          // channels x bits x slots x words-per-slot
    std::vector<uint8_t> out(static_cast<size_t>(nWords), 0);
    mm::encodeWs2812ShiftSlots<uint8_t>(wire, /*activeMask=*/1u, kPins,
                                        kLatchBit, kSh, kCh, out.data());

    const ShiftSim sim(out.data(), nWords, kPins, kLatchBit, kSh);

    // Walk the bits the strand actually sees. Because of the '595's one-slot pipeline, the waveform the strand receives is offset by one slot from the words we clocked, so read the levels from slot 1 onward, and group them 3 slots to a WS2812 bit.
    const uint8_t expect[8] = {1, 0, 1, 0, 0, 1, 0, 1};   // 0xA5, MSB first
    for (int bit = 0; bit < 8; bit++) {
        // Slot index of this bit's three slots, on the WIRE (one slot after we clocked them).
        const int s0 = 3 * bit + 1;   // pulse start
        const int s1 = s0 + 1;        // data
        const int s2 = s0 + 2;        // tail
        if ((s2 + 1) * kSh > nWords) break;   // the last bit's tail runs off the end of the frame

        // Sample the middle of each slot (any word inside it, the level is held across the slot).
        const int mid = kSh / 2;
        const bool start = sim.at(s0 * kSh + mid, 0);
        const bool data  = sim.at(s1 * kSh + mid, 0);
        const bool tail  = sim.at(s2 * kSh + mid, 0);

        CHECK(start == true);                       // every bit opens with the HIGH pulse
        CHECK(data  == (expect[bit] != 0));         // then the data bit itself
        CHECK(tail  == false);                      // then the LOW tail
    }
}

// THE RESET. After the last data bit the frame ends with a zeroed latch pad, >=300 us of idle that tells every WS2812 "frame over, latch what you have". The strand MUST be LOW for that whole pad.
//
// With a '595 that is not automatic, and this is the bug the first waveform test walked straight past (it `break`s before the final bit, with a comment noting the tail "runs off the end of the frame").
// The pipeline is one slot deep: the byte clocked during slot N is presented during slot N+1. The LAST clocked slot therefore needs a latch edge AFTER it, and a pad of pure zeros contains no latch bit at all. So the '595 keeps presenting the final DATA byte for the entire pad:
//
// final wire byte even (last bit 0) -> strand idles LOW  -> resets correctly final wire byte ODD  (last bit 1) -> strand idles HIGH -> NEVER resets
//
// A strand that never sees the reset appends the next frame's bits to an unlatched stream and garbles, content-dependently, which is the worst kind of bug to chase. Hence: the pad must open with one latch-only word.
TEST_CASE("shift encoder: the strand idles LOW through the latch pad (the frame reset)") {
    constexpr uint8_t kPins = 2;
    constexpr uint8_t kLatchBit = 2;
    constexpr uint8_t kCh = 1;
    constexpr int kPadWords = 64;          // stand-in for the real (much longer) zeroed pad

    // Two cases, and the ODD one is the bug: the final wire byte's LAST bit decides the idle level.
    for (uint8_t pattern : {uint8_t{0xA4}, uint8_t{0xA5}}) {   // even (…0), odd (…1)
        uint8_t wire[128 * kCh] = {};
        wire[0] = pattern;

        const int nWords = kCh * 8 * 3 * kSh;
        std::vector<uint8_t> out(static_cast<size_t>(nWords) + kPadWords, 0);   // frame + ZEROED pad
        mm::encodeWs2812ShiftSlots<uint8_t>(wire, /*activeMask=*/1u, kPins, kLatchBit, kSh, kCh,
                                            out.data());
        // The driver closes every shift frame with one latch-only word at the head of the pad; the test must model the same stream, because THAT is what reaches the wire.
        mm::encodeWs2812ShiftLatchPad<uint8_t>(kLatchBit, out.data() + nWords);

        const ShiftSim sim(out.data(), nWords + kPadWords, kPins, kLatchBit, kSh);

        // Every word of the pad must read LOW on the strand, that IS the WS2812 reset.
        for (int w = nWords + kSh; w < nWords + kPadWords; w++) {
            INFO("pattern=", (int)pattern, " pad word=", w);
            CHECK(sim.at(w, 0) == false);
        }
    }
}

// **The prefill/data split must be byte-identical to the whole-slot encoder.**
//
// Two thirds of the shift encoder's stores write frame-CONSTANTS: the pulse-start word (which strands are active) and the pulse-tail word (all-LOW) are the same for every light in the frame.
// Only the middle word carries pixel data. So the constants are pre-filled once (cold path) and the per-light encoder writes only the data word, which is what takes the encode from ~9.7 µs/light to ~3 µs on an S3, and it is the difference between 8 fps and 25 fps on a 48-strand panel.
//
// That is only sound if prefill + data == the whole-slot encode, byte for byte. A single wrong word here is a corrupted waveform on every strand, so it is pinned exactly.
TEST_CASE("shift encoder: prefill + data-only == the whole-slot encode, byte for byte") {
    constexpr uint8_t kPins = 2, kCh = 3;
    constexpr uint32_t kRows = 5;
    const uint8_t latchBit = 3;
    const uint64_t mask = 0xFFFFu;   // all 16 strands of 2 pins active

    // A dense, varied wire pattern per row, a zeroed buffer would hide a bug in either path.
    uint8_t wire[64 * kCh];   // the encoder reads all 64 strands' bytes, as the driver's scratch holds them
    for (size_t i = 0; i < sizeof(wire); i++) wire[i] = static_cast<uint8_t>(i * 37 + 11);

    const size_t slotsPerLight = static_cast<size_t>(kCh) * 8 * 3 * kSh;
    std::vector<uint8_t> whole(kRows * slotsPerLight, 0);
    std::vector<uint8_t> split(kRows * slotsPerLight, 0);

    // Reference: the whole-slot encoder, every word written per light.
    uint8_t* w = whole.data();
    for (uint32_t r = 0; r < kRows; r++) {
        mm::encodeWs2812ShiftSlots<uint8_t>(wire, mask, kPins, latchBit, kSh, kCh, w);
        w += slotsPerLight;
    }

    // The fast path: constants once, then data-only per light.
    mm::prefillWs2812ShiftConstants<uint8_t>(mask, kPins, latchBit, kSh, kCh, kRows, split.data());
    uint8_t* s = split.data();
    for (uint32_t r = 0; r < kRows; r++) {
        mm::encodeWs2812ShiftData<uint8_t>(wire, mask, kPins, latchBit, kSh, kCh, s);
        s += slotsPerLight;
    }

    CHECK(std::memcmp(whole.data(), split.data(), whole.size()) == 0);
}

// The same, with a SHORT strand, the case the per-cycle active mask exists for. An exhausted strand sharing a '595 with a longer one must stay dark, and the prefill is what encodes that (its pulse-start word omits the dead strand's pin bit on that cycle). If the prefill and the encoder disagreed about which strands are live, the short one would flash white at full brightness.
TEST_CASE("shift encoder: prefill + data-only agree on an exhausted strand") {
    constexpr uint8_t kPins = 1, kCh = 3;
    constexpr uint32_t kRows = 3;
    const uint8_t latchBit = 4;
    const uint64_t mask = 0x1u;   // ONLY strand 0 of pin 0 — strands 1..7 are exhausted

    uint8_t wire[64 * kCh];   // the encoder reads all 64 strands' bytes, as the driver's scratch holds them
    for (size_t i = 0; i < sizeof(wire); i++) wire[i] = 0xFF;   // every strand's wire is hot...

    const size_t slotsPerLight = static_cast<size_t>(kCh) * 8 * 3 * kSh;
    std::vector<uint8_t> whole(kRows * slotsPerLight, 0);
    std::vector<uint8_t> split(kRows * slotsPerLight, 0);

    uint8_t* w = whole.data();
    for (uint32_t r = 0; r < kRows; r++) {
        mm::encodeWs2812ShiftSlots<uint8_t>(wire, mask, kPins, latchBit, kSh, kCh, w);
        w += slotsPerLight;
    }
    mm::prefillWs2812ShiftConstants<uint8_t>(mask, kPins, latchBit, kSh, kCh, kRows, split.data());
    uint8_t* s = split.data();
    for (uint32_t r = 0; r < kRows; r++) {
        mm::encodeWs2812ShiftData<uint8_t>(wire, mask, kPins, latchBit, kSh, kCh, s);
        s += slotsPerLight;
    }

    CHECK(std::memcmp(whole.data(), split.data(), whole.size()) == 0);
}

// **The hot-path sweep, the safety net for the packed-transpose rewrite.**
//
// `encodeWs2812ShiftData` keeps the whole transpose in registers: it packs the lane bytes straight into the SWAR word and shifts each bit-plane byte back out, with no staging arrays. The packing depends on the PIN COUNT, and the 16-bit bus adds a second packed word (pins 8-15), so there are several distinct paths, and a wrong shift or mask in any of them silently corrupts a strand.
//
// So sweep every pin count against the whole-slot encoder, on BOTH bus widths, with a dense pattern and exhausted strands mixed in. `encodeWs2812ShiftSlots` is the reference: the simple, unoptimized form that the '595 simulator already validates end-to-end. (This sweep earned its keep immediately: it caught a real bug in a batched-transpose variant that the rest of the suite passed clean.)
TEST_CASE("shift encoder: the packed transpose matches the reference at every pin count") {
    constexpr uint8_t kCh = 3;

    auto sweep = [&](auto slotTag, uint8_t physPins, uint64_t activeMask) {
        using Slot = decltype(slotTag);
        const uint8_t latchBit = static_cast<uint8_t>(sizeof(Slot) == 1 ? 7 : 15);
        const size_t slots = static_cast<size_t>(kCh) * 8 * 3 * kSh;

        // A dense, varied wire, every strand a different byte, so a mis-shifted bit shows up.
        uint8_t wire[64 * kCh];
        for (size_t i = 0; i < sizeof(wire); i++) wire[i] = static_cast<uint8_t>(i * 53 + 7);

        std::vector<Slot> ref(slots, 0), got(slots, 0);
        mm::encodeWs2812ShiftSlots<Slot>(wire, activeMask, physPins, latchBit, kSh, kCh, ref.data());
        mm::prefillWs2812ShiftConstants<Slot>(activeMask, physPins, latchBit, kSh, kCh, 1, got.data());
        mm::encodeWs2812ShiftData<Slot>(wire, activeMask, physPins, latchBit, kSh, kCh, got.data());

        INFO("bus=", sizeof(Slot), " physPins=", physPins, " mask=", activeMask);
        // Binary struct compare, not a string: `== 0` is the correct test for identical bytes. NOLINTNEXTLINE(bugprone-suspicious-string-compare)
        CHECK(std::memcmp(ref.data(), got.data(), slots * sizeof(Slot)) == 0);
    };

    for (uint8_t pins = 1; pins <= 8; pins++) {
        const uint64_t all = (pins * kSh >= 64) ? ~0ull : ((1ull << (pins * kSh)) - 1u);
        sweep(uint8_t{0}, pins, all);          // 8-bit bus, every strand live
        sweep(uint8_t{0}, pins, all & 0x5555555555555555ull);   // every other strand exhausted
        sweep(uint8_t{0}, pins, 1u);           // only strand 0 — the sparsest case
    }
    // The 16-bit bus: the second packed word, and pin counts that span the 8-lane split.
    //
    // Capped at 8 pins, because that is what the hardware allows: the expander fans each pin out to kPinExpanderOutputs strands, and the driver rejects anything past kMaxStrands (64), so 8 pins × 8 is the ceiling, and a 9th pin is a configuration that can never reach this encoder. (Sweeping past it would also shift a uint64 activeMask by ≥64, which is undefined behavior in the TEST.)
    for (uint8_t pins = 1; pins <= 8; pins++) {
        const uint64_t all = (pins * kSh >= 64) ? ~0ull : ((1ull << (pins * kSh)) - 1u);
        sweep(uint16_t{0}, pins, all);
        sweep(uint16_t{0}, pins, all & 0x3333333333333333ull);
        sweep(uint16_t{0}, pins, 1u);
    }
}
