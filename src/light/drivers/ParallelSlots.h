#pragma once

#include <cstddef>   // size_t (the shift-register encoder's wire indexing)
#include <cstdint>
#include "platform_config.h"   // MM_RAMFUNC: the ring ISR runs these encoders on a drain deadline

namespace mm {

/// WS2812 encode for parallel buses: the contract between a parallel driver and its peripheral.
/// It is named for the wire unit it builds, one pixel-clock slot being one bus word.
/// The peripheral sets that word's width, and both the i80 and Parlio buses use it.
/// A pure data transform with no platform include, so the host test pins it without an ESP32.
///
/// @attribution hpwit, Adafruit, FastLED
///
/// @technical
///
/// ## The three slots
///
/// Every WS2812 data bit becomes three bus slots. A 1 bit is high for two of them, a 0 bit for one:
///
///     slot 0: activeMask       every active lane high, the pulse start
///     slot 1: data bits & mask lane L's current bit at bus bit L
///     slot 2: 0x00             every lane low, the pulse tail
///
/// A short lane leaves both slot 0 and slot 1 once its lights run out.
/// It then idles low rather than flashing white, its mask bit being clear.
///
/// ## What the encoders take
///
/// `Slot` is deduced from the call: a `uint8_t` for an 8-lane bus and a `uint16_t` for a 16-lane one.
/// One bus word is one slot, bit L being data line L, so the word width is the lane count and the 8-bit call sites need no change.
///
/// `wire` holds the corrected wire bytes lane-major, indexed as `wire[lane * channels + channel]`.
/// Only lanes set in the active mask are read, so an inactive lane may hold garbage.
/// The lane stride is `channels` rather than a fixed four, so a light of any channel count works.
///
/// ## The shift-register path
///
/// The same wire contract is fanned out through \'595 expanders, so each data pin drives several strands.
/// The fan-out multiplies the slot count rather than the pin count.
///
/// The part is a 74HCT and not a plain 74HC.
/// At 5 V the plain part's input threshold sits above 3.3 V, so a high is not guaranteed to read as a 1.
/// The symptom is flaky strands rather than dead ones.
///
/// The fan-out grows on pins rather than cascade depth, because depth doubles the required pixel clock and there is no exact divide in the band that would need.
/// The board details are on the drivers page.
///
/// ## Two shift-mode optimizations worth knowing
///
/// The prefill writes the frame's constant words once, because only one of a slot's three words carries pixel data.
/// Rewriting the other two per light would burn two thirds of the encoder's stores.
///
/// The active-pin helper is deliberately not called by the whole-slot encoder.
/// There the mask test is fused with the lane gather, so routing it through the helper would walk the pins twice.
///
/// ## Why the 64-bit mask is split into halves
///
/// Splitting once makes every strand test a 32-bit variable shift, which is a single Xtensa instruction.
/// The direct form, masking against a `1ULL` shifted by a runtime value, compiles on the 32-bit Xtensa to an `__ashldi3` library call of roughly 50 cycles.
/// At 144 tests a row that call cost about 7,000 cycles a row, the encoder's dominant cost, attributed on the bench.
/// It is the sibling of the 32-bit SWAR lesson: 64-bit operations synthesize on this target.
///
/// The SWAR pack follows the same shape.
/// Lane `p` is byte `p` of the 8x8 matrix: byte `p` of the first register when `p` is under four, byte `p - 4` of the second otherwise.
/// A 16-lane bus needs a second pair for pins 8 to 15, which the 8-bit path never touches and the compiler drops.
///
/// ## What a prefill run covers
///
/// The `rows` argument is how many rows share one active set.
/// The caller re-prefills per run of rows with the same mask, because an exhausted strand changes the mask at the row where it runs out.
/// Uniform-length strands, the common case, are a single run.
///
/// ## Why the transpose is the emit
///
/// Each shift cycle's eight bit-planes are stored the moment they are computed, while they are still in registers.
/// Staging them in an array first cannot work on this target.
/// Eight cycles of two words each exceeds the register file, so every plane spills to the stack and is reloaded once per bit.
/// Measured on an S3, that staging cost 97 word loads and stores a light against the 17 byte-stores of actual output.
///
/// This is the lesson the lane array taught one level down, where removing it took a light from 8.85 to 6.19 microseconds, and staging the planes was the identical pattern.
/// hpwit's driver stages nothing either, his transpose storing straight into the DMA buffer at its pulse offsets; that was studied rather than copied.
///
/// The price is a strided store, one cycle's eight planes landing a bit-stride apart rather than contiguously.
/// That is one address add per store, far cheaper than a spill and a reload.
///
/// ## Why the transpose is SWAR
///
/// The data slot is an 8 by 8 bit-matrix transpose, and the measured render-loop hot spot.
/// It is around 85% of the driver frame at scale.
/// So it uses the branch-free delta-swap from Hacker's Delight rather than a per-bit gather loop.
/// Same result, no table, far fewer operations.
///

/// The 8 by 8 bit-transpose on the packed representation, which keeps it in registers.
inline uint64_t MM_RAMFUNC transposeBits8x8(uint64_t x) {
    uint64_t t;
    t = (x ^ (x >> 7))  & 0x00AA00AA00AA00AAULL; x = x ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL; x = x ^ t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL; x = x ^ t ^ (t << 28);
    return x;
}

/// The same butterfly on a register pair, which is what the 32-bit device wants.
inline void MM_RAMFUNC transposeBits8x8Pair(uint32_t& lo, uint32_t& hi) {
    uint32_t t;
    t = (lo ^ (lo >> 7))  & 0x00AA00AAu; lo = lo ^ t ^ (t << 7);
    t = (hi ^ (hi >> 7))  & 0x00AA00AAu; hi = hi ^ t ^ (t << 7);
    t = (lo ^ (lo >> 14)) & 0x0000CCCCu; lo = lo ^ t ^ (t << 14);
    t = (hi ^ (hi >> 14)) & 0x0000CCCCu; hi = hi ^ t ^ (t << 14);
    // The only round that crosses the halves, as one masked exchange rather than a wide shift.
    t = (lo ^ (hi << 4)) & 0xF0F0F0F0u; lo ^= t; hi ^= (t >> 4);
}

/// Transpose 8 lane bytes into 8 bit-plane bytes; inactive lanes must be passed as 0.
inline void MM_RAMFUNC transposeLanes8x8(const uint8_t* in, uint8_t* out) {
    uint64_t x = 0;
    for (int r = 0; r < 8; r++) x |= static_cast<uint64_t>(in[r]) << (8 * r);
    x = transposeBits8x8(x);
    for (int c = 0; c < 8; c++) out[c] = static_cast<uint8_t>(x >> (8 * c));
}

/// Transpose 16 lane bytes into 8 bit-plane halfwords, for a 16-bit bus, as two independent 8-lane passes that reuse the same butterfly and add no new constants.
inline void MM_RAMFUNC transposeLanes16x8(const uint8_t* in, uint16_t* out) {
    uint8_t lo[8], hi[8];
    transposeLanes8x8(in,     lo);   // lanes 0..7  → low byte of each plane
    transposeLanes8x8(in + 8, hi);   // lanes 8..15 → high byte of each plane
    for (int b = 0; b < 8; b++)
        out[b] = static_cast<uint16_t>(lo[b]) | static_cast<uint16_t>(hi[b] << 8);
}

/// Encode one row, one light across every lane, in direct mode with one lane per pin.
template <class Slot>
inline void MM_RAMFUNC encodeWs2812ParallelSlots(const uint8_t* wire, Slot activeMask,
                                 uint8_t channels, Slot* out) {
    constexpr uint8_t kLanes = sizeof(Slot) * 8;   // 8 or 16
    for (uint8_t ch = 0; ch < channels; ch++) {
        // Inactive lanes are zeroed, so they contribute no set bit to any plane.
        uint8_t lanes[kLanes];
        for (uint8_t lane = 0; lane < kLanes; lane++)
            lanes[lane] = (activeMask & (Slot(1) << lane)) ? wire[lane * channels + ch] : 0;
        Slot plane[8];
        if constexpr (sizeof(Slot) == 1) transposeLanes8x8(lanes, plane);
        else                             transposeLanes16x8(lanes, plane);
        for (int bit = 7; bit >= 0; bit--) {   // MSB-first per byte
            *out++ = activeMask;    // slot 0: pulse start (active lanes HIGH)
            *out++ = plane[bit];    // slot 1: bit `bit` of every active lane
            *out++ = 0;             // slot 2: pulse tail (all LOW)
        }
    }
}

/// Outputs per physical data pin when a 74HCT595 expander is fitted; one register is eight.
inline constexpr uint8_t kPinExpanderOutputs = 8;   // one 74HCT595 per data pin

/// Close a shift-register frame with one latch-only word, without which the register keeps presenting the last data byte.
template <class Slot>
inline void MM_RAMFUNC encodeWs2812ShiftLatchPad(uint8_t latchBit, Slot* out) {
    *out = static_cast<Slot>(Slot(1) << latchBit);
}

/// Encode one row through the shift-register expander, indexed by expanded lane.
template <class Slot>
inline void encodeWs2812ShiftSlots(const uint8_t* wire, uint64_t activeMask,
                                   uint8_t physPins, uint8_t latchBit, uint8_t outPerPin,
                                   uint8_t channels, Slot* out) {
    constexpr uint8_t kLanes = sizeof(Slot) * 8;   // bus width: 8 or 16
    if (outPerPin == 0 || outPerPin > kPinExpanderOutputs) return;
    const Slot latch = static_cast<Slot>(Slot(1) << latchBit);
    for (uint8_t ch = 0; ch < channels; ch++) {
        // One bit-plane per shift cycle, built by reusing the same transpose once per cycle over
        Slot plane[kPinExpanderOutputs][8];
        // Per SHIFT CYCLE, not per pin: one aggregated mask would flash the shorter strand white.
        Slot activePins[kPinExpanderOutputs] = {};
        // 32-bit halves: a runtime-v 64-bit shift is a library call on Xtensa (see encodeWs2812ShiftData).
        const uint32_t maskLo = static_cast<uint32_t>(activeMask);
        const uint32_t maskHi = static_cast<uint32_t>(activeMask >> 32);
        for (uint8_t c = 0; c < outPerPin; c++) {
            // '595 shifts MSB-first: the first bit clocked in lands on the last output.
            const uint8_t pos = static_cast<uint8_t>(outPerPin - 1 - c);
            uint8_t lanes[kLanes] = {};
            for (uint8_t p = 0; p < physPins && p < kLanes; p++) {
                const uint8_t v = static_cast<uint8_t>(p * outPerPin + pos);
                const uint32_t live = (v < 32) ? (maskLo >> v) : (maskHi >> (v - 32));
                if (!(live & 1u)) continue;   // inactive: idle LOW
                lanes[p] = wire[static_cast<size_t>(v) * channels + ch];
                activePins[c] |= static_cast<Slot>(Slot(1) << p);
            }
            if constexpr (sizeof(Slot) == 1) transposeLanes8x8(lanes, plane[c]);
            else                             transposeLanes16x8(lanes, plane[c]);
        }
        for (int bit = 7; bit >= 0; bit--) {   // MSB-first per byte, as the wire contract
            // The one-slot pipeline: a '595 updates on the latch, so each slot clocks in the byte the strand sees one slot later.
            for (uint8_t c = 0; c < outPerPin; c++) {
                const Slot first = (c == 0) ? latch : Slot(0);   // RCLK on word 0 of each slot
                // clocked in slot N  ->  seen by the strand in slot N+1
                out[c]                 = static_cast<Slot>(activePins[c] | first);   // -> seen: pulse start (HIGH)
                out[outPerPin + c]     = static_cast<Slot>(plane[c][bit] | first);   // -> seen: the data bit
                out[2 * outPerPin + c] = first;                                      // -> seen: pulse tail (LOW)
            }
            out += 3 * outPerPin;
        }
    }
}

/// Which pins carry an active strand on each shift cycle, one bus word per cycle.
template <class Slot>
inline void MM_RAMFUNC shiftActivePins(uint64_t activeMask, uint8_t physPins, uint8_t outPerPin,
                            Slot (&out)[kPinExpanderOutputs]) {
    constexpr uint8_t kLanes = sizeof(Slot) * 8;
    // 32-bit halves: a runtime-v 64-bit shift is a library call on Xtensa (see encodeWs2812ShiftData).
    const uint32_t maskLo = static_cast<uint32_t>(activeMask);
    const uint32_t maskHi = static_cast<uint32_t>(activeMask >> 32);
    for (uint8_t c = 0; c < outPerPin; c++) {
        const uint8_t pos = static_cast<uint8_t>(outPerPin - 1 - c);   // '595 shifts MSB-first
        Slot bits = 0;
        for (uint8_t p = 0; p < physPins && p < kLanes; p++) {
            const uint8_t v = static_cast<uint8_t>(p * outPerPin + pos);
            const uint32_t live = (v < 32) ? (maskLo >> v) : (maskHi >> (v - 32));
            if (live & 1u) bits |= static_cast<Slot>(Slot(1) << p);
        }
        out[c] = bits;
    }
}

/// Write the frame's constant shift-mode words once, so the per-light encoder skips them.
template <class Slot>
inline void MM_RAMFUNC prefillWs2812ShiftConstants(uint64_t activeMask, uint8_t physPins, uint8_t latchBit,
                                        uint8_t outPerPin, uint8_t channels, uint32_t rows,
                                        Slot* out) {
    if (outPerPin == 0 || outPerPin > kPinExpanderOutputs) return;
    const Slot latch = static_cast<Slot>(Slot(1) << latchBit);

    Slot activePins[kPinExpanderOutputs] = {};
    shiftActivePins<Slot>(activeMask, physPins, outPerPin, activePins);

    // Every channel and bit of this row shares a start and tail; the data word belongs to the encoder.
    const uint32_t bitsPerLight = static_cast<uint32_t>(channels) * 8u;
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t b = 0; b < bitsPerLight; b++) {
            for (uint8_t c = 0; c < outPerPin; c++) {
                const Slot first = (c == 0) ? latch : Slot(0);   // RCLK rides word 0 of each slot
                out[c]                 = static_cast<Slot>(activePins[c] | first);
                out[2 * outPerPin + c] = first;
            }
            out += 3 * outPerPin;
        }
    }
}

/// The per-light data encode, writing ONLY the data word of each slot; the output is identical to the whole-slot encoder's, provided the prefill ran with the same mask.
template <class Slot>
inline void MM_RAMFUNC encodeWs2812ShiftData(const uint8_t* wire, uint64_t activeMask, uint8_t physPins,
                                  uint8_t latchBit, uint8_t outPerPin, uint8_t channels, Slot* out) {
    constexpr uint8_t kLanes = sizeof(Slot) * 8;
    if (outPerPin == 0 || outPerPin > kPinExpanderOutputs) return;
    const Slot latch = static_cast<Slot>(Slot(1) << latchBit);
    // Words per WS2812 bit: each bit is one 3-word slot per shift cycle.
    const size_t bitStride = static_cast<size_t>(3) * outPerPin;
    // The mask is split into 32-bit halves once, so every strand test below is one Xtensa shift.
    const uint32_t maskLo = static_cast<uint32_t>(activeMask);
    const uint32_t maskHi = static_cast<uint32_t>(activeMask >> 32);

    // The transpose IS the emit: each plane is stored while still in registers, never staged.
    for (uint8_t ch = 0; ch < channels; ch++) {
        Slot* chBase = out + static_cast<size_t>(ch) * 8 * bitStride;
        for (uint8_t c = 0; c < outPerPin; c++) {
            const uint8_t pos = static_cast<uint8_t>(outPerPin - 1 - c);
            // Lane p is byte p of the 8x8 matrix, so it packs straight into the SWAR register pair.
            uint32_t loA = 0, loB = 0, hiA = 0, hiB = 0;
            for (uint8_t p = 0; p < physPins && p < kLanes; p++) {
                const uint8_t v = static_cast<uint8_t>(p * outPerPin + pos);
                // 32-bit half test: see the maskLo/maskHi split above.
                const uint32_t live = (v < 32) ? (maskLo >> v) : (maskHi >> (v - 32));
                if (!(live & 1u)) continue;   // exhausted strand: idle LOW
                const uint32_t b = wire[static_cast<size_t>(v) * channels + ch];
                if (p < 8) { if (p < 4) loA |= b << (8 * p); else loB |= b << (8 * (p - 4)); }
                else       { const uint8_t q = static_cast<uint8_t>(p - 8);
                             if (q < 4) hiA |= b << (8 * q); else hiB |= b << (8 * (q - 4)); }
            }
            transposeBits8x8Pair(loA, loB);
            if constexpr (sizeof(Slot) != 1) transposeBits8x8Pair(hiA, hiB);

            const Slot first = (c == 0) ? latch : Slot(0);   // RCLK rides word 0 of each slot
            Slot* dst = chBase + outPerPin + c;              // the DATA word of bit 7's slot, cycle c
            for (int bit = 7; bit >= 0; bit--) {             // MSB-first per byte, as the wire contract
                const uint8_t sh = static_cast<uint8_t>(8 * (bit & 3));
                Slot data;
                if constexpr (sizeof(Slot) == 1) {
                    data = static_cast<Slot>(((bit < 4) ? loA : loB) >> sh);
                } else {
                    data = static_cast<Slot>((((bit < 4) ? loA : loB) >> sh) & 0xFF)
                         | static_cast<Slot>(((((bit < 4) ? hiA : hiB) >> sh) & 0xFF) << 8);
                }
                // ONLY the data word: the slot's other two are the prefilled constants.
                *dst = static_cast<Slot>(data | first);
                dst += bitStride;
            }
        }
    }
}

} // namespace mm
