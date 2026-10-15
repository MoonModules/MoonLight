#pragma once

#include "core/util/color.h"   // scale8 (nscale8 builds on it), RGB not needed here

#include <cstdint>

/// @defgroup math8 The 8-bit fixed-point tier
/// @{
/// Integer trig, beat timing, saturating arithmetic and a fast generator.
///
/// @attribution FastLED, Mark Kriegsman
///
/// @moreinfo
///
/// This is the recognizable surface an embedded or LED developer already knows, under the same names, written fresh against our own architecture.
/// It carries the ideas, the names and the textbook algorithms of FastLED's lib8tion, and the code is ours.
///
/// It is all integer and table-backed where that pays, with no float and no heap, so it is safe in the render loop.
/// The time-dependent helpers take the current time as a parameter rather than reading a clock, which keeps the time source at the domain edge instead of buried in core.
/// A timebase shifts the phase origin, so an effect wanting its oscillation to start at a chosen moment passes that moment.
///
/// ## Why the range map divides rather than shifts
///
/// The input's top must reach the range's top exactly.
/// An earlier form scaled by a power of two, which made the top unreachable and collapsed a one-step span: a bar height of one could never be reached.
/// Dividing by the full byte range fixes it, and the span is held wide so the widest case cannot wrap.

namespace mm {

/// The sine table over a 256-step circle, from which the cosine is a quarter turn.
inline constexpr uint8_t sin8_lut[256] = {
    128,131,134,137,140,144,147,150,153,156,159,162,165,168,171,174,
    177,179,182,185,188,191,193,196,199,201,204,206,209,211,213,216,
    218,220,222,224,226,228,230,232,234,235,237,239,240,241,243,244,
    245,246,248,249,250,250,251,252,253,253,254,254,254,255,255,255,
    255,255,255,255,254,254,254,253,253,252,251,250,250,249,248,246,
    245,244,243,241,240,239,237,235,234,232,230,228,226,224,222,220,
    218,216,213,211,209,206,204,201,199,196,193,191,188,185,182,179,
    177,174,171,168,165,162,159,156,153,150,147,144,140,137,134,131,
    128,125,122,119,116,112,109,106,103,100,97,94,91,88,85,82,
    79,77,74,71,68,65,63,60,57,55,52,50,47,45,43,40,
    38,36,34,32,30,28,26,24,22,21,19,17,16,15,13,12,
    11,10,8,7,6,6,5,4,3,3,2,2,2,1,1,1,
    1,1,1,1,2,2,2,3,3,4,5,6,6,7,8,10,
    11,12,13,15,16,17,19,21,22,24,26,28,30,32,34,36,
    38,40,43,45,47,50,52,55,57,60,63,65,68,71,74,77,
    79,82,85,88,91,94,97,100,103,106,109,112,116,119,122,125
};

constexpr uint8_t sin8(uint8_t i) { return sin8_lut[i]; }
constexpr uint8_t cos8(uint8_t i) { return sin8_lut[static_cast<uint8_t>(i + 64)]; }

/// A triangle wave, the textbook fold of a ramp and a sharper alternative to the sine.
constexpr uint8_t triwave8(uint8_t i) {
    return i < 128 ? static_cast<uint8_t>(i * 2)
                   : static_cast<uint8_t>((255 - i) * 2);
}

// Fast octant atan2: angle of (x, y) as 0..255 around the full circle (x, y in int16 range).
constexpr uint8_t atan2_8(int16_t y, int16_t x) {
    uint8_t r = 0;
    if (y < 0) { y = static_cast<int16_t>(-y); r = 0x80; }
    if (x < 0) { x = static_cast<int16_t>(-x); r = static_cast<uint8_t>(r | 0x40); }
    uint8_t offset = (x > y) ? 0 : 32;
    if (x < y) { int16_t t = y; y = x; x = t; }
    uint8_t b = (x == 0) ? 0 : static_cast<uint8_t>((static_cast<uint16_t>(y) * 64) / static_cast<uint16_t>(x));
    return static_cast<uint8_t>(r + offset + b);
}

// Octagonal distance approximation: |Δ| without a sqrt (max + half-min — the cheap 8-bit norm).
constexpr uint8_t dist8(int16_t dx, int16_t dy) {
    int16_t ax = dx < 0 ? static_cast<int16_t>(-dx) : dx;
    int16_t ay = dy < 0 ? static_cast<int16_t>(-dy) : dy;
    return static_cast<uint8_t>(ax > ay ? ax + (ay >> 1) : ay + (ax >> 1));
}

/// Add or subtract clamping at the ends, so a bright pixel plus more stays bright.
constexpr uint8_t qadd8(uint8_t a, uint8_t b) {
    uint16_t t = static_cast<uint16_t>(a) + b;
    return t > 255 ? 255 : static_cast<uint8_t>(t);
}
constexpr uint8_t qsub8(uint8_t a, uint8_t b) {
    return a > b ? static_cast<uint8_t>(a - b) : 0;
}

/// Scale a byte by a fraction, under the name an LED developer already reaches for.
constexpr uint8_t nscale8(uint8_t val, uint8_t scale) { return scale8(val, scale); }

/// Rescale a byte onto a range, the input's top reaching the range's top exactly.
constexpr uint8_t map8(uint8_t in, uint8_t rangeStart, uint8_t rangeEnd) {
    const uint16_t span = static_cast<uint16_t>(rangeEnd - rangeStart);
    return static_cast<uint8_t>(rangeStart + (static_cast<uint16_t>(in) * span) / 255u);
}

// beat8: a 0..255 sawtooth completing `bpm` cycles per minute, measured from `timebase`.
constexpr uint8_t beat8(uint8_t bpm, uint32_t ms, uint32_t timebase = 0) {
    if (bpm == 0) return 0;
    const uint32_t period = 60000u / bpm;
    if (period == 0) return 0;
    const uint32_t pos = (ms - timebase) % period;
    return static_cast<uint8_t>((pos * 256u) / period);
}

/// A sine oscillating between two bounds at a given rate, with a timebase and a fixed offset.
constexpr uint8_t beatsin8(uint8_t bpm, uint32_t ms, uint8_t low = 0, uint8_t high = 255,
                           uint32_t timebase = 0, uint8_t phase = 0) {
    const uint8_t beat = static_cast<uint8_t>(beat8(bpm, ms, timebase) + phase);
    const uint8_t s = sin8(beat);                      // 0..255 sine
    const uint8_t range = static_cast<uint8_t>(high - low);
    return static_cast<uint8_t>(low + scale8(s, range));
}

// beatsin16: 16-bit range version, for positions across a wide grid.
constexpr uint16_t beatsin16(uint8_t bpm, uint32_t ms, uint16_t low = 0, uint16_t high = 65535,
                             uint32_t timebase = 0, uint8_t phase = 0) {
    const uint8_t beat = static_cast<uint8_t>(beat8(bpm, ms, timebase) + phase);
    const uint8_t s = sin8(beat);                      // 0..255 sine
    const uint16_t range = static_cast<uint16_t>(high - low);
    return static_cast<uint16_t>(low + ((static_cast<uint32_t>(s) * range) >> 8));
}

/// A small seedable generator, cheap on the hot path and reproducible per effect.
class Random8 {
public:
    constexpr explicit Random8(uint32_t seed = 0x1234ABCDu) : state_(seed ? seed : 1u) {}
    /// Seed the sequence, so a run is reproducible.
    constexpr void seed(uint32_t s) { state_ = s ? s : 1u; }
    /// The next byte in the sequence.
    constexpr uint8_t next8() { return static_cast<uint8_t>(advance() >> 24); }
    /// The next two bytes.
    constexpr uint16_t next16() { return static_cast<uint16_t>(advance() >> 16); }
    // 0..bound-1 (bound>0): scale the 16-bit draw to avoid modulo bias on small bounds.
    /// The next value below a bound, or between two.
    constexpr uint8_t below(uint8_t bound) {
        return bound ? static_cast<uint8_t>((static_cast<uint32_t>(next16()) * bound) >> 16) : 0;
    }
    // min..max-1 (FastLED's random8(min,max)): an offset draw over the half-open range.
    /// The next value below a bound, or between two.
    constexpr uint8_t below(uint8_t min, uint8_t max) {
        return max > min ? static_cast<uint8_t>(min + below(static_cast<uint8_t>(max - min))) : min;
    }
private:
    constexpr uint32_t advance() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    uint32_t state_;
};

/// @}
}  // namespace mm
