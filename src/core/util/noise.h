#pragma once

#include <cstdint>

/// A smooth, deterministic pseudo-random field: the staple organic-motion source for effects.
///
/// @technical
///
/// The value varies smoothly across space, so neighboring coordinates give similar results, unlike a raw hash.
/// Sample it across a grid for clouds, plasma or fire-like fields, and scroll a coordinate to animate.
/// One gradient set and one interpolation serve all three dimensions.
///
/// ## The algorithm
///
/// This is Perlin's improved noise.
/// A pseudo-random gradient sits at every lattice corner, chosen by a hash from the twelve cube-edge directions.
/// Its dot product with the offset from the corner, a quintic fade on the fraction, and a linear blend across the corners give the value.
///
/// Gradient noise is zero at every lattice point and has no value bias toward the corners.
/// That is why it reads as smooth and isotropic where value noise, which picks a random value per corner, reads blocky and axis-aligned at low frequency.
///
/// Two dimensions is three with the last offset at zero.
/// One takes gradients of its own, since the cube-edge set projected onto a single axis is zero for six codes of sixteen.
/// The cost scales with the corners: two, four and eight dot products.
///
/// ## Why the gradients are a table
///
/// A table rather than the usual select expression, because the selects compile to branches and a core without a branch predictor pays for each one.
/// The select form was forty-four branches per two-dimensional sample on this target; the table is loads and small multiplies.
///
/// ## Why the core works a slice at a time
///
/// One instantiation per arity compiles to straight-line code over exactly its corners.
/// A slice is two or four corners with the blends between them, and three dimensions is that slice at two depths blended by the depth fade.
///
/// One body with eight corners in flight spilled half its registers on a sixteen-register window, fifty-two stack stores a sample.
/// The slice form keeps at most four.
///
/// ## Why the scale is per arity
///
/// Halving regardless is right for two and three dimensions and wrong for one, which then spans only the middle half of the range.
/// Measured, a one-dimensional field covered the central half of the byte range and read washed out beside the same field sampled in two.
///
/// ## The three standard compositions
///
/// One sample is a smooth blur; the looks people recognize come from composing samples, and each composition is a few lines over the base rather than a new field generator.
///
/// Summing octaves at doubling frequency and halving amplitude turns the blur into cloud, terrain or smoke structure: large shapes with fine detail on them.
/// The same sum over the absolute deviation creases the field, and those creases read as billows and flame.
/// Sampling at a coordinate that noise itself displaced is the warp, which produces the flowing, marbled look.
///
/// Cost is stated per call because it decides whether an effect fits.
/// Each octave is one sample, so three octaves cost three, and a warp costs its own probes plus the field it then samples.
/// On a large fixture that multiplies by the pixel count.
///
/// ## What a domain warp is for
///
/// A warp displaces the sample coordinate by a noise field and then samples there, the strength saying how far that reaches in the same units as the coordinates.
/// It is the primitive behind the flowing, marbled look.
/// The field stops looking like a texture laid on the grid and starts looking like something moving through it, for two extra samples.
///
/// ## Why the volumetric warp takes three probes
///
/// It uses three rather than two, each offset by its own constant so the axes displace independently.
/// Sampling one field three times would move everything along a diagonal.
///
/// Its octave count has no default, unlike the flat form's.
/// With one, the two four-argument calls would both be viable and every existing call ambiguous, so requiring it makes the arity say which field the caller means.
///
/// Both share one body, so the two cannot drift apart.
/// Each still compiles to exactly its own arity: the flat instantiation walks four corners and never touches the third axis.
/// Calling the volumetric body with a zero third coordinate would have been simpler and measured nearly twice as slow.
/// Every probe and the inner sum then walk eight corners to reach the same answer.
///
/// ## Why curl noise cannot clump
///
/// It is the perpendicular gradient of a scalar field, from Bridson's curl-noise paper.
/// Taking the gradient of a potential and turning it a quarter turn gives a field whose divergence is zero by construction.
/// Whatever flows into a region flows out again.
///
/// That is what separates it from sampling noise straight into a velocity, where the field has sources and sinks.
/// Anything carried by such a field collects in the sinks and drains from the sources, which looks like clumping rather than flow.
///
/// The output is scaled by a strength in the caller's own units, and the sampling distance for the central difference is in the same coordinates as the field.
/// Too small and the difference is quantization noise; too large and the curl is of a blurrier field than the one being sampled.
/// The default is a sixteenth of a cell.
///
/// ## The output uses its full range
///
/// The dot product is signed offsets added, so the blend spans about a cell rather than the tighter bound a unit vector suggests.
/// An exact search over every gradient choice at every fraction gives half a cell in one dimension, a cell in two and slightly more in three.
///
/// Each tier halves the raw blend onto its range, so the midpoint is the field's mean and both ends are reached.
/// The clamp is a guard that only the three-dimensional extreme touches.
///
/// ## How a coordinate is read
///
/// A coordinate is fixed point scaled however the caller likes: its high byte selects the noise cell and its low byte the interpolation position within.
/// A larger step per pixel is therefore finer noise, with more cells across the grid, and a smaller step is broader and smoother.
///
/// ## What the lattice hash costs
///
/// Each coordinate is multiplied by its own odd constant and the results xored, which shares work across a cell's corners: six multiplies per three-dimensional sample rather than twenty-four.
/// Then one shift, one multiply, and the top nibble of the product, its best-mixed part.
///
/// Four bits is all a gradient needs, so that is the whole hash.
/// A byte-wide avalanche spent three multiplies per corner on bits nothing read.
/// Checked against it the sixteen codes are uniform to well under a percent, with adjacent corners as independent as the avalanche managed.
/// A linear pre-mix with a single multiply was not: adjacent corners correlated six-fold, which reads as lattice patterns.
///
/// Both tiers share the hash, so the 16-bit field is the 8-bit one at finer resolution rather than an unrelated field.
///
/// The implementation is written fresh and integer throughout: the hash and the fade table are ours, and the gradient trick is Perlin's.

namespace mm {
namespace noise {

/// A corner's gradient index, four bits, which is all a gradient needs.
constexpr uint32_t corner(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (z * 0xcb1ab31fu);
    h ^= h >> 16;
    h *= 0x7feb352du;
    return h >> 28;
}

/// Perlin's twelve cube-edge gradients, padded to sixteen so a nibble indexes them by mask.
inline constexpr int8_t kGradient[16][3] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
    {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
    {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1},
};

/// Dot product of corner gradient `h` (0..15) with the offset, over the axes the arity samples.
template <int Dims>
constexpr int32_t grad(uint32_t h, int32_t x, int32_t y, int32_t z) {
    // Its own gradients: six cube-edge ones have no first component and would leave a cell flat.
    if constexpr (Dims == 1) return (h & 1u) ? -x : x;
    const int8_t* g = kGradient[h];
    int32_t d = g[0] * x;
    if constexpr (Dims > 1) d += g[1] * y;
    if constexpr (Dims > 2) d += g[2] * z;
    return d;
}

/// The quintic fade as a table, because the polynomial needs a wide intermediate per sample.
struct FadeTable {
    uint32_t v[257];
    constexpr FadeTable() : v{} {
        for (uint64_t i = 0; i <= 256; i++) {
            // (6i⁵ − 3840i⁴ + 655360i³) / 2^24 == (6t⁵ − 15t⁴ + 10t³) · 65536 for t = i/256.
            const uint64_t i3 = i * i * i;
            v[i] = static_cast<uint32_t>((6u * i3 * i * i - 3840u * i3 * i + 655360u * i3) >> 24);
        }
    }
};
inline constexpr FadeTable kFade{};

/// fade for an 8-bit fraction: a table lookup.
constexpr uint32_t fade8(uint8_t t) { return kFade.v[t]; }

/// fade for a 16-bit fraction: the table on the high byte, linear on the low byte.
constexpr uint32_t fade16(uint16_t t) {
    const uint32_t a = kFade.v[t >> 8], b = kFade.v[(t >> 8) + 1];
    return a + (((b - a) * (t & 0xFFu)) >> 8);
}

/// The two tiers as policies for the one core below: the fraction width, the fade of a fraction.
/// The blend a→b by a 0.16 weight sized to the tier's operand range.
struct Tier8 {
    /// How many fraction bits a coordinate carries at this tier.
    static constexpr int kFrac = 8;
    /// The quintic fade for this tier's fraction width.
    static constexpr uint32_t fade(int32_t f) { return fade8(static_cast<uint8_t>(f)); }
    /// Operands stay within ±2^11 (three offsets of a 256 cell), so the product fits 32 bits.
    static constexpr int32_t blend(int32_t a, int32_t b, uint32_t w) {
        return a + (((b - a) * static_cast<int32_t>(w)) >> 16);
    }
};
struct Tier16 {
    /// How many fraction bits a coordinate carries at this tier.
    static constexpr int kFrac = 16;
    /// The quintic fade for this tier's fraction width.
    static constexpr uint32_t fade(int32_t f) { return fade16(static_cast<uint16_t>(f)); }
    /// Operands reach 2^18 against a 2^16 weight: a widening multiply (two on a 32-bit core).
    static constexpr int32_t blend(int32_t a, int32_t b, uint32_t w) {
        return a + static_cast<int32_t>((static_cast<int64_t>(b - a) * static_cast<int64_t>(w)) >> 16);
    }
};

/// The core: gradient noise over the corners of one lattice cell, returning a raw blend.
template <class Tier, int Dims>
constexpr int32_t layer(uint32_t ix, uint32_t iy, uint32_t iz, int32_t fx, int32_t fy, int32_t fz, uint32_t wx, uint32_t wy) {
    constexpr int32_t kCell = 1 << Tier::kFrac;
    int32_t v = Tier::blend(grad<Dims>(corner(ix, iy, iz),     fx,         fy, fz),
                            grad<Dims>(corner(ix + 1, iy, iz), fx - kCell, fy, fz), wx);
    if constexpr (Dims > 1) {
        v = Tier::blend(v, Tier::blend(grad<Dims>(corner(ix, iy + 1, iz),     fx,         fy - kCell, fz),
                                       grad<Dims>(corner(ix + 1, iy + 1, iz), fx - kCell, fy - kCell, fz), wx), wy);
    }
    return v;
}

template <class Tier, int Dims>
constexpr int32_t raw(uint32_t x, uint32_t y, uint32_t z) {
    constexpr int32_t kCell = 1 << Tier::kFrac;
    const uint32_t ix = x >> Tier::kFrac, iy = y >> Tier::kFrac, iz = z >> Tier::kFrac;
    const int32_t fx = static_cast<int32_t>(x & (kCell - 1));
    const int32_t fy = static_cast<int32_t>(y & (kCell - 1));
    const int32_t fz = static_cast<int32_t>(z & (kCell - 1));
    const uint32_t wx = Tier::fade(fx);
    const uint32_t wy = Dims > 1 ? Tier::fade(fy) : 0;
    int32_t v = layer<Tier, Dims>(ix, iy, iz, fx, fy, fz, wx, wy);
    if constexpr (Dims > 2) v = Tier::blend(v, layer<Tier, Dims>(ix, iy, iz + 1, fx, fy, fz - kCell, wx, wy), Tier::fade(fz));
    return v;
}

/// Map a raw blend onto the tier's unsigned range, centered on the midpoint.
template <class Tier, int Dims>
constexpr uint32_t out(int32_t raw) {
    constexpr int32_t kCell = 1 << Tier::kFrac;
    const int32_t v = kCell / 2 + (Dims == 1 ? raw : (raw >> 1));
    return static_cast<uint32_t>(v < 0 ? 0 : (v > kCell - 1 ? kCell - 1 : v));
}

/// Linear interpolation between two field values, for a caller blending those rather than gradients.
constexpr uint16_t lerp16(uint16_t a, uint16_t b, uint16_t t) {
    const int32_t delta = static_cast<int32_t>(b) - static_cast<int32_t>(a);
    // Wide on purpose: the narrow product is signed overflow on about a quarter of samples.
    return static_cast<uint16_t>(static_cast<int32_t>(a)
                                 + static_cast<int32_t>((static_cast<int64_t>(delta) * t) >> 16));
}

/// Octave sums narrow, and this is what widens them back.
inline constexpr uint16_t kFbmGain[9] = {256, 256, 343, 391, 417, 430, 437, 440, 442};

/// Re-widen an octave sum around the midpoint. `mid` is 128 at the 8-bit tier, 32768 at 16-bit.
constexpr int32_t fbmWiden(int32_t v, int32_t mid, uint8_t octaves) {
    const uint16_t g = kFbmGain[octaves > 8 ? 8 : octaves];
    return mid + (((v - mid) * g) >> 8);
}

}  // namespace noise

/// 1D gradient noise: x is a 16.0 fixed coordinate (high byte = cell, low byte = position).
constexpr uint8_t inoise8(uint32_t x) {
    return static_cast<uint8_t>(noise::out<noise::Tier8, 1>(noise::raw<noise::Tier8, 1>(x, 0, 0)));
}

/// 2D gradient noise over the 4 cell corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y) {
    return static_cast<uint8_t>(noise::out<noise::Tier8, 2>(noise::raw<noise::Tier8, 2>(x, y, 0)));
}

/// 3D gradient noise over the 8 cube corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y, uint32_t z) {
    return static_cast<uint8_t>(noise::out<noise::Tier8, 3>(noise::raw<noise::Tier8, 3>(x, y, z)));
}

/// 1D gradient noise at 16 bits. `x` is 16.16 fixed point: the whole part selects the cell, the fraction interpolates within it.
constexpr uint16_t inoise16(uint32_t x) {
    return static_cast<uint16_t>(noise::out<noise::Tier16, 1>(noise::raw<noise::Tier16, 1>(x, 0, 0)));
}

/// 2D gradient noise at 16 bits: the common case for a panel.
constexpr uint16_t inoise16(uint32_t x, uint32_t y) {
    return static_cast<uint16_t>(noise::out<noise::Tier16, 2>(noise::raw<noise::Tier16, 2>(x, y, 0)));
}

/// 3D gradient noise at 16 bits. `z` is the axis a 2D effect uses as time, so the field evolves in place instead of scrolling past.
constexpr uint16_t inoise16(uint32_t x, uint32_t y, uint32_t z) {
    return static_cast<uint16_t>(noise::out<noise::Tier16, 3>(noise::raw<noise::Tier16, 3>(x, y, z)));
}

/// Fractal Brownian motion at 16 bits: `octaves` samples at doubling frequency, halving amplitude.
inline uint16_t fbm16(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 32768;                     // no octaves: flat mid-field
    // Wide so each octave keeps its full width: a narrow accumulator bands exactly as this tier avoids.
    uint64_t sum = 0;
    uint32_t norm = 0, amp = 32768;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint64_t>(inoise16(x, y)) * amp;
        norm += amp;
        x <<= 1; y <<= 1;                               // double the frequency
        amp >>= 1;                                      // halve the contribution
    }
    if (!norm) return 32768;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 32768, octaves);
    return static_cast<uint16_t>(widened < 0 ? 0 : (widened > 65535 ? 65535 : widened));
}

/// The same sum with a third axis, so a volumetric fixture samples a real field.
inline uint16_t fbm16(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 32768;
    uint64_t sum = 0;
    uint32_t norm = 0, amp = 32768;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint64_t>(inoise16(x, y, z)) * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    if (!norm) return 32768;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 32768, octaves);
    return static_cast<uint16_t>(widened < 0 ? 0 : (widened > 65535 ? 65535 : widened));
}

/// Fractal Brownian motion: `octaves` samples at doubling frequency, halving amplitude, returned normalized to 0..255. octaves=1 is plain noise; 3-4 is the usual cloud look.
inline uint8_t fbm8(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 128;                       // no octaves: flat mid-field
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint32_t>(inoise8(x, y)) * amp;
        norm += amp;
        x <<= 1; y <<= 1;                               // double the frequency
        amp >>= 1;                                      // halve the contribution
    }
    if (!norm) return 128;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 128, octaves);
    return static_cast<uint8_t>(widened < 0 ? 0 : (widened > 255 ? 255 : widened));
}

/// 3D fbm: the same sum with a z axis, so a 2D effect can use z as time for a field that evolves in place rather than scrolling past.
inline uint8_t fbm8(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 128;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint32_t>(inoise8(x, y, z)) * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    if (!norm) return 128;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 128, octaves);
    return static_cast<uint8_t>(widened < 0 ? 0 : (widened > 255 ? 255 : widened));
}

/// Turbulence: fbm over |noise - 128|, which creases the field where it crosses the midpoint. The creases are what read as billowing smoke and flame rather than soft cloud.
inline uint8_t turbulence8(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 0;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        const int16_t v = static_cast<int16_t>(inoise8(x, y)) - 128;
        sum  += static_cast<uint32_t>(v < 0 ? -v : v) * 2u * amp;
        norm += amp;
        x <<= 1; y <<= 1;
        amp >>= 1;
    }
    const uint32_t r = norm ? sum / norm : 0;
    return static_cast<uint8_t>(r > 255 ? 255 : r);
}

/// 3D turbulence: the same creased sum with a z axis.
inline uint8_t turbulence8(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 0;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        const int16_t v = static_cast<int16_t>(inoise8(x, y, z)) - 128;
        sum  += static_cast<uint32_t>(v < 0 ? -v : v) * 2u * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    const uint32_t r = norm ? sum / norm : 0;
    return static_cast<uint8_t>(r > 255 ? 255 : r);
}

/// Domain warp: displace the sample coordinate by a noise field, then sample there. `strength` is how far the displacement reaches, in the same fixed-point units as the coordinates.

/// 3D domain warp: the same displacement with a z axis, so the field flows through a volume. Three probes, each offset by its own constant, so the axes displace independently.
template <int Dims>
inline uint8_t warpImpl(uint32_t x, uint32_t y, uint32_t z, uint16_t strength, uint8_t octaves) {
    // Offset so the axes displace independently, rather than all along one diagonal.
    int32_t dx, dy, dz = 0;
    if constexpr (Dims > 2) {
        dx = (static_cast<int32_t>(inoise8(x, y, z)) - 128) * strength / 128;
        dy = (static_cast<int32_t>(inoise8(x + 0x9E37u, y + 0x7C15u, z)) - 128) * strength / 128;
        // The third probe rides that axis itself, so a flat fixture displaces nothing along it.
        if (z) dz = (static_cast<int32_t>(inoise8(x + 0x6A09u, y + 0xBB67u, z)) - 128) * strength / 128;
    } else {
        dx = (static_cast<int32_t>(inoise8(x, y)) - 128) * strength / 128;
        dy = (static_cast<int32_t>(inoise8(x + 0x9E37u, y + 0x7C15u)) - 128) * strength / 128;
    }
    // Added unsigned: the signed form overflows on any large scaled coordinate, and wrapping is wanted.
    const uint32_t sx = x + static_cast<uint32_t>(dx);
    const uint32_t sy = y + static_cast<uint32_t>(dy);
    if constexpr (Dims > 2) {
        return fbm8(sx, sy, z + static_cast<uint32_t>(dz), octaves);
    } else {
        return fbm8(sx, sy, octaves);
    }
}

/// 3D domain warp: the field flows through a volume rather than through a plane.
inline uint8_t warp8(uint32_t x, uint32_t y, uint32_t z, uint16_t strength, uint8_t octaves) {
    return warpImpl<3>(x, y, z, strength, octaves);
}

/// Displace the sample coordinate by a noise field, then sample there.
inline uint8_t warp8(uint32_t x, uint32_t y, uint16_t strength, uint8_t octaves = 1) {
    return warpImpl<2>(x, y, 0u, strength, octaves);
}

/// Curl of a noise potential: a velocity field that cannot pile up or thin out.
inline void curl16(uint32_t x, uint32_t y, uint32_t z, int32_t strength,
                   int32_t& vx, int32_t& vy, uint32_t eps = 4096) {
    // Two central differences, turned a quarter turn, so flow runs along the contours.
    const int32_t dy = static_cast<int32_t>(inoise16(x, y + eps, z))
                     - static_cast<int32_t>(inoise16(x, y - eps, z));
    const int32_t dx = static_cast<int32_t>(inoise16(x + eps, y, z))
                     - static_cast<int32_t>(inoise16(x - eps, y, z));
    // Scaled to keep the result near the strength, widened for the product, clamped coming back.
    const auto scaled = [](int32_t d, int32_t s) -> int32_t {
        const int64_t v = (static_cast<int64_t>(d) * s) >> 15;
        return static_cast<int32_t>(v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : v));
    };
    vx = scaled(dy, strength);
    vy = scaled(-dx, strength);
}

/// The 2D form: the same field at a fixed z, which is what a panel wants.
inline void curl16(uint32_t x, uint32_t y, int32_t strength, int32_t& vx, int32_t& vy,
                   uint32_t eps = 4096) {
    curl16(x, y, 0u, strength, vx, vy, eps);
}

}  // namespace mm
