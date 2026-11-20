#pragma once

/// The numeric vocabulary every effect and script writes against.
///
/// @technical
///
/// ## Why a 16-bit tier alongside the 8-bit one
///
/// An 8-bit result has 256 levels, which a wall of twelve thousand lights shows as a staircase in a slow gradient, so the contract is 16-bit and effects look smooth at every size.
///
/// The 8-bit tier stays for genuinely 8-bit domains, a palette index and a hue being modulo 256 by design, and as the internal fast path.
///
/// ## The cost was measured, not assumed
///
/// Interpolating the 8-bit table, which adds no bytes, was tried first and rejected: its endpoints rounded to 8 bits leave about one percent of amplitude error, worse than the classic 16-bit forms.
///
/// A small quarter-wave 16-bit table with the same interpolation measures two orders of magnitude better for a few hundred bytes, where an exact form spends eight times the table.
///
/// ## Why the sine is signed
///
/// The unsigned form, its midpoint at the zero crossing, broke every ported snippet doing arithmetic on the LED world's most-ported symbol, and eleven of our own calls subtracted the midpoint back.
///
/// The signed contract matches the main branches of the two codebases users port from: one declares it in its header, and the other's effects add the midpoint back for an unsigned value.
///
/// A standard name carries the standard contract; the 8-bit sine keeps its own, since palette and hue domains are genuinely unsigned.
///
/// ## How the wave is reconstructed
///
/// A quarter-wave lookup with linear interpolation: the angle's top bits pick the quadrant, the next six the entry and the low byte the step between, mirrored in odd quadrants and negated in the upper half.
///
/// ## What the helpers exist to stop being hand-rolled
///
/// The range mapper puts the fencepost in one place, which six effects hand-rolled: callers map to the extent and the pixel writer clamps, the form that keeps the last column.
///
/// The narrow integer root is a binary restoring method, with no divide and no float, since effects call it per pixel; a squared comparison against a threshold skips it at an eighth of the cost.
///
/// The hash addresses randomness by position, which a stream cannot: a stream advances per call, so a device with one extra frame diverges for good, where a hashed starfield matches on every device.
///
/// ## Why a follower needs two rates
///
/// A single-rate follower makes the attack as sluggish as the decay, so like a broadcast peak programme meter this one rises almost at once and falls slowly; equal rates make it the single-rate form.
///
/// The falling-peak meter rises instantly and decays slowly, since a peak easing upward misses transients and one dropping at once is gone before the eye reads it.
///
/// The simple follower is frame-rate dependent, as every LED codebase's is; a caller needing time independence scales its rate by its own step.
///
/// ## Why decay is stated as a half-life
///
/// The caller states what they mean, half a value gone after so many milliseconds, and the exponent carries the elapsed time, so two short steps and one long one reach the same place.
///
/// A fixed fraction kept per frame has a half-life that moves with the frame rate, a long tail at sixty frames a second and an instant clear at twenty times that.
///
/// ## Why the polar pair is 16-bit too
///
/// The 8-bit forms take an octant arctangent and an octagonal distance, up to a tenth off a true radius and saturating at the byte's top, which shows as corners on a large fixture's circles.
///
/// The 16-bit forms address a grid in polar coordinates, behind rings, spirals, kaleidoscopes and tunnels: the angle feeds the sine and the palette, and the distance is a true Euclidean radius.
///
/// ## Why easings are here
///
/// These are Robert Penner's curves under their industry-standard names: an easing bends a progress value so motion accelerates and settles, where a linear ramp looks mechanical.
///
/// ## Three places the obvious arithmetic overflows
///
/// The range mapper widens every operand first, since a full-width span overflows a same-width subtraction; two spans' product fits the wider type for every range a caller maps.
///
/// The integer root starts from a power-of-two bound, since a Newton step starting at the type's maximum wrapped and returned zero; half the bit width starts above the root and in range.
///
/// The distance squares and roots in the wider unsigned type, since the narrow form saturates far inside the normal range: two coordinates of seventy thousand already sum past its maximum.
///
/// ## Two casts the arctangent needs
///
/// It folds into the first octant on unsigned magnitudes widened first, since negating the signed minimum has no representation and the obvious negation is undefined at one input per axis.
///
/// It subtracts from zero rather than applying unary minus, the same modular operation, which one major compiler reports as a warning its build treats as an error.

#include "core/util/math8.h"
#include "platform/platform.h"   // MM_NONBLOCKING: the hot path calls halfLifeKeep every frame

#include <cstdint>

namespace mm {

/// An angle where a full turn is the type's range, so overflow is the wrap and no modulo is needed.
using angle16 = uint16_t;
/// A fraction of one across the type's range, for interpolation weights and easing.
using frac16  = uint16_t;

/// Quarter-wave sine table: sin(pi/2 * i/64) * 32767, 65 entries (0..64 inclusive, so the last segment has both endpoints). 130 bytes in flash; the other three quadrants come from symmetry.
inline constexpr int16_t sin16_quarter[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

/// Sine over a 16-bit angle, returning a signed result: the widely ported contract.
constexpr int16_t sin16(angle16 theta) {
    const uint8_t quadrant = static_cast<uint8_t>(theta >> 14);       // 0..3
    const uint16_t pos     = static_cast<uint16_t>(theta & 0x3FFF);   // position within the quadrant
    // Odd quadrants run the quarter wave backwards (sin descends from the peak).
    const uint16_t walk = (quadrant & 1) ? static_cast<uint16_t>(0x4000 - pos) : pos;
    // Wide on purpose: the table has 65 entries, so the top index is valid and must stay so.
    const uint16_t idx  = static_cast<uint16_t>(walk >> 8);           // 0..64
    const uint8_t frac  = static_cast<uint8_t>(walk & 0xFF);
    const int32_t a = sin16_quarter[idx];
    // At the top index the fraction is zero, so clamping costs nothing and needs no branch.
    const int32_t b = sin16_quarter[idx < 64 ? idx + 1 : 64];
    int32_t v = a + (((b - a) * frac) >> 8);
    if (quadrant >= 2) v = -v;                                        // lower half of the circle
    return static_cast<int16_t>(v);
}

/// Cosine: a quarter turn ahead of sine. Signed, like `sin16`.
constexpr int16_t cos16(angle16 theta) { return sin16(static_cast<angle16>(theta + 16384)); }

// ---- Range mapping ---------------------------------------------------------

/// Map `v` from [inLo,inHi] to [outLo,outHi] in 64-bit intermediate, clamped to the input range.
constexpr int32_t map32(int32_t v, int32_t inLo, int32_t inHi, int32_t outLo, int32_t outHi) {
    if (inHi == inLo) return outLo;                       // zero span: no meaningful ratio
    if (inHi > inLo) { if (v <= inLo) return outLo; if (v >= inHi) return outHi; }
    else             { if (v >= inLo) return outLo; if (v <= inHi) return outHi; }
    // Every operand widens before the arithmetic: see the appendix on what that bounds.
    const int64_t inSpan  = static_cast<int64_t>(inHi)  - static_cast<int64_t>(inLo);
    const int64_t outSpan = static_cast<int64_t>(outHi) - static_cast<int64_t>(outLo);
    const int64_t num     = (static_cast<int64_t>(v) - static_cast<int64_t>(inLo)) * outSpan;
    return static_cast<int32_t>(static_cast<int64_t>(outLo) + num / inSpan);
}

// ---- Beat phase ------------------------------------------------------------

/// A resolution- and framerate-independent BPM phase: fleet time times the rate, so every device of the network reads one phase at one moment.
///
/// A function of the time and the rate alone, as WLED's `strip.now * speed` is: a device starting late or restarting is in step from its first frame, and a rate change jumps the phase.
///
/// The product stays in 64 bits and divides only at the read, since `ms * bpm * scale / 60000` rounded per step would freeze a slow rate.
///
/// Usage: one member per animated quantity; call `advanceTo(nowMs, rate)` once per frame with the layer's fleet time, then read as often as needed. `rate` is BPM-like: the caller's speed control, whatever its units.
class BeatPhase {
public:
    /// Take this frame's phase from the fleet time and the rate; a rate of 0 holds the phase at zero.
    void advanceTo(uint32_t nowMs, uint32_t rate) { num_ = static_cast<uint64_t>(nowMs) * rate; }

    /// The phase scaled by `scale` and divided late: `phase(256)` is the uint8 angle form
    uint32_t phase(uint32_t scale) const {
        return static_cast<uint32_t>((num_ * scale) / 60000u);
    }

    /// The undivided numerator, for a caller that scales differently (NoiseEffect multiplies the
    uint64_t numerator() const { return num_; }

    /// The same for a caller whose rate already carries a factor.
    void advanceScaled(uint32_t elapsedMs, uint64_t scaledRate) { num_ = static_cast<uint64_t>(elapsedMs) * scaledRate; }

    /// Zero the phase until the next advance.
    void reset() { num_ = 0; }

private:
    uint64_t num_ = 0;   ///< the raw time·rate numerator, divided by 60000 only at the read
};

/// Integer square root, rounded down, by the binary restoring method: no divide and no float.
constexpr uint64_t isqrt64(uint64_t x) {
    if (x < 2) return x;                      // 0 and 1 are their own roots
    // From a power-of-two bound, not from the input: starting there overflows the first step.
    uint64_t r = 1;
    for (uint64_t v = x; v > 0; v >>= 2) r <<= 1;
    uint64_t last;
    do {
        last = r;
        r = (r + x / r) >> 1;
    } while (r < last);
    return last;
}

/// log2 of `v` in 8.8 fixed point, 0 for v under 2: whole octaves from the top bit, the fraction from the bits below it read linearly, within 0.09 of an octave.
constexpr uint32_t log2Q8(uint32_t v) {
    if (v < 2) return 0;
    uint32_t msb = 0;
    while ((v >> msb) > 1) msb++;
    const uint32_t frac = msb >= 8 ? (v >> (msb - 8)) & 0xFF : (v << (8 - msb)) & 0xFF;
    return (msb << 8) | frac;
}

constexpr uint32_t isqrt(uint32_t v) {
    uint32_t rem = v, root = 0, bit = 1u << 30;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= root + bit) { rem -= root + bit; root = (root >> 1) + bit; }
        else                   { root >>= 1; }
        bit >>= 2;
    }
    return root;
}

/// The arctangent over one octant, the other seven coming from symmetry.
inline constexpr int16_t atan16_octant[33] = {
        0,   326,   651,   975,  1297,  1617,  1933,  2246,
     2555,  2860,  3159,  3453,  3742,  4025,  4302,  4572,
     4836,  5094,  5344,  5589,  5826,  6058,  6282,  6500,
     6712,  6917,  7117,  7310,  7498,  7679,  7856,  8026,
     8192
};

/// Angle of (x, y) as an angle16, measured counter-clockwise from +x. Full 16-bit resolution, so a gradient swept around the circle has no visible steps on a large fixture.
inline angle16 atan16(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;                       // the center has no direction
    // Fold into the first octant on unsigned magnitudes: see the appendix on both casts.
    uint32_t ax = x < 0 ? 0u - static_cast<uint32_t>(x) : static_cast<uint32_t>(x);
    uint32_t ay = y < 0 ? 0u - static_cast<uint32_t>(y) : static_cast<uint32_t>(y);
    const bool swap = ay > ax;
    if (swap) { const uint32_t t = ax; ax = ay; ay = t; }
    // Within one octant the arctangent is near-linear, so a corrected linear read suffices.
    const uint32_t ratio = static_cast<uint32_t>((static_cast<uint64_t>(ay) << 16) / (ax ? ax : 1u));
    // A table with linear interpolation, the same shape and the same reason as the sine above.
    const uint16_t idx  = static_cast<uint16_t>(ratio >> 11);           // 0..32
    const uint16_t frac = static_cast<uint16_t>((ratio >> 3) & 0xFF);
    const int32_t lo = atan16_octant[idx];
    const int32_t hi = atan16_octant[idx < 32 ? idx + 1 : 32];
    uint32_t oct = static_cast<uint32_t>(lo + (((hi - lo) * frac) >> 8));
    uint16_t a = swap ? static_cast<uint16_t>(16384 - oct) : static_cast<uint16_t>(oct);
    if (x < 0) a = static_cast<uint16_t>(32768 - a);                    // reflect into quadrant 2/3
    if (y < 0) a = static_cast<uint16_t>(65536 - a);                    // and below the axis
    return static_cast<angle16>(a);
}

/// True Euclidean distance from the origin to (dx, dy): a real radius, not the octagon `dist8` approximates, and it does not saturate at 255.
inline uint32_t dist16(int32_t dx, int32_t dy) {
    // Squared wide and rooted wide: the narrow form is wrong far inside the normal range.
    const uint64_t ax = static_cast<uint64_t>(dx < 0 ? -static_cast<int64_t>(dx) : dx);
    const uint64_t ay = static_cast<uint64_t>(dy < 0 ? -static_cast<int64_t>(dy) : dy);
    const uint64_t d2 = ax * ax + ay * ay;
    const uint64_t d = isqrt64(d2);
    return static_cast<uint32_t>(d > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : d);
}

/// Quadratic ease in-out: accelerate from rest, decelerate to rest. The default choice.
constexpr frac16 easeInOutQuad(frac16 t) {
    const uint32_t v = t;
    if (v < 32768) return static_cast<frac16>((2u * v * v) >> 16);
    const uint32_t u = 65535u - v;
    return static_cast<frac16>(65535u - ((2u * u * u) >> 16));
}

/// Cubic ease in-out: the same shape with a longer, softer settle.
constexpr frac16 easeInOutCubic(frac16 t) {
    const uint64_t v = t;
    if (v < 32768) return static_cast<frac16>((4ull * v * v * v) >> 32);
    const uint64_t u = 65535ull - v;
    return static_cast<frac16>(65535ull - ((4ull * u * u * u) >> 32));
}

/// Quadratic ease out: fast start, gentle settle. The "arrives and rests" curve.
constexpr frac16 easeOutQuad(frac16 t) {
    const uint32_t u = 65535u - t;
    return static_cast<frac16>(65535u - ((u * u) / 65535u));
}

// --- Followers and meters ---------------------------------------------------------------------

/// A one-pole follower: the current value moves a fraction of the way toward the target each frame.
constexpr uint8_t smoothFollow(uint8_t current, uint8_t target, uint8_t rate) {
    if (rate == 0 || current == target) return current;
    if (rate == 255) return target;                   // full rate arrives, rather than stopping one short
    const int32_t delta = static_cast<int32_t>(target) - current;
    // Rounded away from zero, so every nonzero rate makes progress in both directions.
    const int32_t step = (delta * rate) / 256;
    if (step != 0) return static_cast<uint8_t>(current + step);
    return static_cast<uint8_t>(current + (delta > 0 ? 1 : -1));
}

/// A meter's ballistic: rise at one rate, fall at another. `smoothFollow` with two time constants.
constexpr uint8_t ballistic(uint8_t current, uint8_t target, uint8_t rise, uint8_t fall) {
    return smoothFollow(current, target, target > current ? rise : fall);
}

/// The falling-peak meter: rise INSTANTLY to a new high, then decay slowly, which is what catches a transient and leaves it readable. Every VU meter with a floating peak dot is this function.
constexpr uint8_t peakHold(uint8_t peak, uint8_t value, uint8_t decay) {
    if (value > peak) return value;                    // instant attack
    return peak > decay ? static_cast<uint8_t>(peak - decay) : 0;
}

// --- Position-addressable randomness -----------------------------------------------------------

/// A hash of up to four integers: random-looking, but a pure function of its inputs.
constexpr uint16_t hashInt(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t seed = 0) {
    uint32_t h = a * 0x9E3779B1u;                      // the golden-ratio constant, standard mixer
    h ^= (b + 0x85EBCA6Bu + (h << 6) + (h >> 2));
    h ^= (c + 0xC2B2AE35u + (h << 6) + (h >> 2));
    h ^= (seed + 0x27D4EB2Fu + (h << 6) + (h >> 2));
    h ^= h >> 15; h *= 0x2545F491u; h ^= h >> 13;      // avalanche
    return static_cast<uint16_t>(h >> 16);
}

/// Fold an angle into mirrored wedges, which gives any field sampled through it that symmetry.
inline angle16 kaleido(angle16 a, uint8_t segments) {
    if (segments < 2) return a;                        // one segment is the identity
    const uint32_t wedge = 65536u / segments;
    uint32_t within = a % wedge;                       // position inside this wedge
    const uint32_t index = a / wedge;
    // Returns the folded coordinate, one wedge wide, mirroring alternates so the seams join.
    if (index & 1) within = wedge - 1 - within;
    return static_cast<angle16>(within);
}

/// A triangle wave: the fold of a ramp, cheaper and sharper than a sine for a linear sweep.
constexpr uint16_t triwave16(uint16_t i) {
    return i < 32768 ? static_cast<uint16_t>(i * 2)
                     : static_cast<uint16_t>((65535 - i) * 2);
}

/// A sawtooth completing a given number of cycles per minute, measured from a timebase.
constexpr uint16_t beat16(uint8_t bpm, uint32_t ms, uint32_t timebase = 0) {
    if (bpm == 0) return 0;
    const uint32_t period = 60000u / bpm;
    if (period == 0) return 0;
    const uint32_t pos = (ms - timebase) % period;
    return static_cast<uint16_t>((pos * 65536u) / period);
}

/// Half-life decay: the fraction of a value that SURVIVES after `dtMs`, as a 0..65536 weight, annotated because `draw::decay` calls it every frame.
inline uint32_t halfLifeKeep(uint32_t dtMs, uint32_t halfLifeMs) MM_NONBLOCKING {
    if (dtMs == 0 || halfLifeMs == 0) return 65536;
    // A table on the fraction and a shift for the whole halvings, held as the drop below full.
    static constexpr uint16_t kPow2Drop[33] = {
            0,  1404,  2779,  4123,  5439,  6727,  7987,  9220,
        10427, 11608, 12763, 13894, 15001, 16084, 17143, 18180,
        19195, 20188, 21160, 22111, 23041, 23952, 24843, 25715,
        26568, 27403, 28220, 29020, 29802, 30568, 31317, 32050,
        32768,
    };
    // A long stall shifts the result to zero rather than wrapping.
    const uint64_t units = (static_cast<uint64_t>(dtMs) * 32u) / halfLifeMs;
    const uint32_t whole = static_cast<uint32_t>(units >> 5);
    if (whole >= 32) return 0;
    const uint32_t i = static_cast<uint32_t>(units & 31u);
    // Interpolate on the remainder the multiply discarded, recovered at the finer scale.
    const uint64_t fine = (static_cast<uint64_t>(dtMs) * 32u * 256u) / halfLifeMs;
    const uint32_t f = static_cast<uint32_t>(fine & 255u);
    const uint32_t a = kPow2Drop[i], b = kPow2Drop[i + 1];
    const uint32_t drop = a + (((b - a) * f) >> 8);
    return (65536u - drop) >> whole;
}

}  // namespace mm
