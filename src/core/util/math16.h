#pragma once

/// @defgroup math16 The 16-bit fixed-point tier
/// @{
/// The numeric vocabulary every effect and script writes against.
///
/// @moreinfo
///
/// ## Why a 16-bit tier alongside the 8-bit one
///
/// An 8-bit result positions to 256 levels.
/// Visibly steps on a large fixture: a wall of twelve thousand lights shows the staircase in a slow gradient or a drifting blob.
/// Effects must look smooth at every size, so the contract is 16-bit.
///
/// The 8-bit tier stays for genuinely 8-bit domains, a palette index and a hue being modulo 256 by design, and as the internal fast path.
///
/// ## The cost was measured, not assumed
///
/// Interpolating the existing 8-bit table was tried first, since it adds no bytes, and rejected.
/// Rounding the endpoints to 8 bits distorts the segments the interpolation runs between, giving around one percent of amplitude, which is worse than the classic 16-bit implementations manage.
///
/// A small quarter-wave 16-bit table with the same linear interpolation measures two orders of magnitude better, at a cost that rounds to nothing.
/// An exact implementation exists but spends eight times the table and two wide multiplies per call.
/// This is the middle that keeps large-fixture gradients smooth without either price.
///
/// ## Why the sine is signed
///
/// The unsigned form, with the midpoint at the zero crossing, was tried first to match the 8-bit sine.
/// It is the wrong call: this is the most-ported symbol in the LED world, so any snippet doing arithmetic on it breaks silently, everything offset by half scale.
/// Eleven of our own call sites were already subtracting that midpoint to undo the convention, which is the same signal from the inside.
///
/// The contract was verified against the two codebases users port from, at their main branches rather than a release.
/// The contract we bind to should be the one users will have.
/// One declares the signed return in its own header comment.
/// The other's effects add the midpoint back when they want an unsigned value, which is precisely the arithmetic that broke against our old return.
///
/// A standard name must carry the standard contract.
/// The 8-bit sine keeps its own, because palette and hue domains are genuinely unsigned.
///
/// ## How the wave is reconstructed
///
/// It is a quarter-wave lookup with linear interpolation.
/// The top bits of the angle pick the quadrant, the next six the table entry, and the low byte the position between entries.
/// Mirroring the index in odd quadrants and negating in the upper half reconstructs the full wave from a quarter of the table.
///
/// ## What the helpers exist to stop being hand-rolled
///
/// The range mapper puts the fencepost in one place.
/// Six effects hand-rolled it, each carrying its own comment about mapping to one less than the extent when the output is a grid.
/// Callers map to the extent and the pixel writer clamps, which is the form that does not lose the last column.
///
/// The narrow integer root is a binary restoring method, with no divide and no float.
/// Matters because the chip has no fast divide and effects call it per pixel for a true distance.
/// Where a comparison against a threshold will do, comparing squared values instead skips it entirely: measured on hardware that is roughly an eighth of the cost.
/// The wide form was promoted here from the audio path so both roots have one home.
///
/// The hash addresses randomness by position, which a stream generator cannot do.
/// A stream advances per call, so a device rendering one extra frame or holding a different light count diverges forever.
/// Addressing by position keeps a dissolve, a starfield or a per-pixel offset identical across devices, which is what supersync requires.
///
/// ## Why a follower needs two rates
///
/// A follower with a single rate is the wrong instrument for anything reactive.
/// It makes the attack as sluggish as the decay and rounds off the transient the effect exists to show.
/// Every meter standard separates the two, and a broadcast peak programme meter is the shape wanted here.
/// Rise almost at once, fall over a comfortable time so the eye can read the peak.
/// Equal values reduce to the single-rate form exactly, so the two-rate one is a superset rather than a second mechanism.
///
/// The falling-peak meter takes that further, rising instantly and decaying slowly.
/// A peak that eased upward would miss transients, and one that dropped instantly would show nothing to read.
///
/// The simple follower is deliberately frame-rate dependent, which is what every LED codebase uses; a caller needing true time independence scales its rate by its own step.
///
/// ## Why decay is stated as a half-life
///
/// The caller states the thing they actually mean: that half of a value is gone after so many milliseconds.
/// Frame-rate independence is then a property of the formula rather than of the caller's arithmetic, because the exponent carries the elapsed time.
/// Two short steps and one long one reach the same place.
///
/// The per-frame form does not survive a fast device.
/// A fade that keeps a fixed fraction each frame has a half-life that moves with the frame rate.
/// One setting is a long tail at sixty frames a second and an instant clear at twenty times that.
/// The effects that hit this carry a remainder by hand to stop the fade truncating to nothing.
///
/// ## Why the polar pair is 16-bit too
///
/// The 8-bit forms use an octant arctangent and an octagonal distance, taken as the larger axis plus half the smaller.
/// That is up to a tenth off a true radius and saturates at the byte's top.
/// On a small panel neither shows; on a large one the octagon reads as visible corners on what should be a circle.
/// The saturation flattens everything past that distance from the center.
///
/// The 16-bit forms address a grid in polar coordinates, which is the vocabulary behind every radial look: rings, spirals, rotation, kaleidoscopes, tunnels, radial wipes, a spectrum bent around a circle.
/// The angle feeds the sine and the palette directly, and the distance is a true Euclidean radius.
/// Both are general grid arithmetic rather than helpers for the few effects that happen to call them first.
///
/// ## Why easings are here
///
/// These are Robert Penner's curves under their industry-standard names.
/// An easing takes a progress value and bends it, so motion accelerates and settles instead of running at a constant speed.
/// Anything that travels, grows, fades or transitions reads better through one, and a linear ramp is what makes an animation look mechanical.
///
/// ## Three places the obvious arithmetic overflows
///
/// The range mapper widens every operand before the arithmetic.
/// A full-width span does not fit in a same-width subtraction, so computing the spans narrow overflows at the extremes.
/// The product of two such spans fits in the wider type for every range this maps between, an extent, a byte, a band count or a frequency.
/// Only mapping a full range onto another full range would exceed it, which no caller does.
///
/// The integer root starts from a power-of-two bound rather than the input itself.
/// The Newton step overflows when it starts at the input, which at the type's maximum made the first iteration wrap and the function return zero.
/// Halving the bit width gives a start above the true root and inside the range, so every iteration stays representable.
///
/// The distance squares in the wider unsigned type and roots there.
/// The narrow form is wrong far inside the normal range rather than only at the extremes: two coordinates of seventy thousand already sum past the narrow maximum.
/// It saturated and reported the type's top for that distance.
///
/// ## Two casts the arctangent needs
///
/// It folds into the first octant on unsigned magnitudes.
/// Negating the signed minimum has no representation, so the obvious conditional negation is undefined at exactly one input per axis; widening first keeps the fold exact across the whole range.
///
/// It subtracts from zero rather than applying unary minus.
/// The unary form is well defined on an unsigned value, being modular arithmetic.
/// Is exactly what is wanted, but one major compiler reports it and that build treats warnings as errors.
/// Subtracting from zero is the same operation with no diagnostic anywhere.
/// A quadratic core can swap in behind the same name if a field-heavy effect ever needs it.

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

/// A resolution- and framerate-independent BPM phase accumulator.
///
/// Nine effects hand-rolled this identically, and the shape is subtle enough to be worth owning once.
/// The per-tick product `dt * bpm * scale / 60000` rounds to ZERO when dt is under a millisecond (every desktop frame.
/// An ESP32 running a small fixture fast), so the animation silently freezes. The fix all nine converged on is to accumulate the RAW numerator in 64 bits and divide only at the read: which is what this does.
///
/// Usage: one member per animated quantity; call `advanceTo(nowMs, rate)` once per frame, then read as often as needed. `rate` is BPM-like: the caller's speed control, whatever its units.
class BeatPhase {
public:
    /// Accumulate this frame's contribution. Safe to call with a rate of 0 (the phase holds).
    void advanceTo(uint32_t nowMs, uint32_t rate) {
        if (!started_) { started_ = true; lastMs_ = nowMs; return; }
        const uint32_t dt = nowMs - lastMs_;       // unsigned: correct across the millis() wrap
        lastMs_ = nowMs;
        num_ += static_cast<uint64_t>(dt) * rate;
    }

    /// The phase scaled by `scale` and divided late: `phase(256)` is the uint8 angle form
    uint32_t phase(uint32_t scale) const {
        return static_cast<uint32_t>((num_ * scale) / 60000u);
    }

    /// The undivided numerator, for a caller that scales differently (NoiseEffect multiplies the
    uint64_t numerator() const { return num_; }

    /// Feed a pre-scaled product directly: for the callers whose rate already carries a factor.
    void advanceScaled(uint32_t elapsedMs, uint64_t scaledRate) {
        if (!started_) { started_ = true; lastMs_ = elapsedMs; return; }
        const uint32_t dt = elapsedMs - lastMs_;
        lastMs_ = elapsedMs;
        num_ += static_cast<uint64_t>(dt) * scaledRate;
    }

    /// Forget the accumulated phase, restarting from zero.
    void reset() { num_ = 0; lastMs_ = 0; started_ = false; }

private:
    uint64_t num_ = 0;        ///< raw dt·rate numerator; divided by 60000 only at the read
    uint32_t lastMs_ = 0;
    bool     started_ = false;
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

/// @}
}  // namespace mm
