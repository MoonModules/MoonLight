#pragma once

#include "core/util/math16.h"      // sin16/cos16, atan16, dist16, isqrt, frac16
#include "light/powerfunctions/draw.h"       // pos_t, Canvas, the 2D SDF family
#include "light/util/Palette.h"

/// @defgroup shader The shader vocabulary
/// @{
/// The small set of operations every per-pixel shader is written out of, in fixed point.
///
/// A shader is one function that runs per pixel and returns a color, given that pixel's position and the time.
///
/// @attribution GLSL, Iñigo Quilez
///
/// @moreinfo
///
/// ## A shared vocabulary
///
/// What makes shader code portable is the standard names, so an effect reads the same wherever it came from.
/// This header holds the 2D operations a shader composes into a gradient, a pattern, a warped field or a set of shapes.
/// Marching rays to render 3D is a technique built on top of it, and lives beside it.
///
/// ## Everything is 16-bit fixed point
///
/// A value is a fraction of one, a distance is in sub-pixel units, and a color index is a byte because palettes are.
/// The 8-bit forms stay for genuinely 8-bit domains.
///
/// The functions keep GLSL's built-in names.

namespace mm::shader {

// --- The universal built-ins: named, so effect code states intent rather than open-coding it -----

/// Clamp to a range.
constexpr int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Linear interpolation from `a` to `b` by `t` (0..65535). GLSL's `mix`.
constexpr int32_t mix(int32_t a, int32_t b, frac16 t) {
    return a + static_cast<int32_t>((static_cast<int64_t>(b - a) * t) >> 16);
}

/// The fractional part of a 16.16 value: GLSL's `fract`, behind every repeating pattern.
constexpr frac16 fract(int32_t v) {
    return static_cast<frac16>(static_cast<uint32_t>(v) & 0xFFFF);
}

/// 0 below the edge, 65535 at or above it: a hard threshold.
constexpr frac16 step(int32_t edge, int32_t v) {
    return v < edge ? 0 : 65535;
}

/// A smooth 0→1 ramp between two edges, with zero derivative at both ends (the classic 3t²−2t³).
constexpr frac16 smoothstep(int32_t edge0, int32_t edge1, int32_t v) {
    if (edge1 == edge0) return v < edge0 ? 0 : 65535;
    int64_t t = (static_cast<int64_t>(v - edge0) << 16) / (edge1 - edge0);
    t = t < 0 ? 0 : (t > 65535 ? 65535 : t);
    // t*t*(3 - 2t) in 16.16, evaluated so the intermediates stay inside 64 bits.
    const int64_t t2 = (t * t) >> 16;
    const int64_t t3 = (t2 * t) >> 16;
    return static_cast<frac16>(clamp(static_cast<int32_t>(3 * t2 - 2 * t3), 0, 65535));
}

// --- Vectors -----------------------------------------------------------------------------------

/// Length of a 2D vector: GLSL's `length`, in sub-pixel units.
inline int32_t length(draw::pos_t x, draw::pos_t y) {
    return static_cast<int32_t>(dist16(x, y));
}

/// Rotate a point about the origin by `a`. The building block for every spinning pattern.
inline void rotate(draw::pos_t& x, draw::pos_t& y, angle16 a) {
    const int32_t c = static_cast<int32_t>(cos16(a));   // -32768..32767
    const int32_t s = static_cast<int32_t>(sin16(a));
    const int32_t nx = (static_cast<int64_t>(x) * c - static_cast<int64_t>(y) * s) >> 15;
    const int32_t ny = (static_cast<int64_t>(x) * s + static_cast<int64_t>(y) * c) >> 15;
    x = static_cast<draw::pos_t>(nx);
    y = static_cast<draw::pos_t>(ny);
}

/// Convert a pixel to shader space: centered on the grid, scaled so the short side spans -1..1.
inline void uv(lengthType px, lengthType py, lengthType w, lengthType h,
               int32_t& outX, int32_t& outY) {
    const int32_t shortSide = (w < h ? w : h);
    const int32_t s = shortSide > 0 ? shortSide : 1;
    outX = ((static_cast<int32_t>(px) * 2 - w + 1) * 65536) / s;
    outY = ((static_cast<int32_t>(py) * 2 - h + 1) * 65536) / s;
}

// --- Domain operators: transform the coordinate before the shape, so one shape becomes a pattern --

/// Tile space into cells of `size`, returning the position within the cell, centered on zero.
constexpr int32_t repeat(int32_t v, int32_t size) {
    if (size <= 0) return v;
    int32_t m = v % size;
    if (m < 0) m += size;
    return m - size / 2;
}

/// Mirror space about the origin: `abs`, which turns any shape into a symmetric pair.
constexpr int32_t mirror(int32_t v) { return v < 0 ? -v : v; }

// --- SDF combination: two shapes as distances produce a third, so a scene is composed -------------

/// Both shapes (the nearer surface wins).
constexpr int32_t opUnion(int32_t a, int32_t b) { return a < b ? a : b; }

/// Only where the shapes overlap.
constexpr int32_t opIntersect(int32_t a, int32_t b) { return a > b ? a : b; }

/// Shape `b` with shape `a` cut out of it, following Quilez's operand order deliberately.
constexpr int32_t opSubtract(int32_t a, int32_t b) { return (-a) > b ? (-a) : b; }

/// Hollow out a shape, leaving a shell of the given thickness: the outline operator.
constexpr int32_t opShell(int32_t d, int32_t thickness) {
    return (d < 0 ? -d : d) - thickness;
}

/// Grow a shape outward by `r`, rounding its corners as it goes.
constexpr int32_t opRound(int32_t d, int32_t r) { return d - r; }

// --- More shapes, completing the everyday set beside the ones effects already needed --------------

/// A box with rounded corners.
inline int32_t sdRoundBox(draw::pos_t px, draw::pos_t py, draw::pos_t cx, draw::pos_t cy,
                          draw::pos_t bx, draw::pos_t by, draw::pos_t r) {
    const int32_t qx = (px - cx < 0 ? cx - px : px - cx) - (bx - r);
    const int32_t qy = (py - cy < 0 ? cy - py : py - cy) - (by - r);
    const int32_t outX = qx > 0 ? qx : 0;
    const int32_t outY = qy > 0 ? qy : 0;
    if (outX > 0 && outY > 0)                       // past a corner: the true radial distance
        return static_cast<int32_t>(dist16(outX, outY)) - r;
    if (outX > 0 || outY > 0)                       // past one face only
        return (outX > outY ? outX : outY) - r;
    return (qx > qy ? qx : qy) - r;                 // fully inside
}

/// A regular n-sided polygon, via the polar fold.
inline int32_t sdPolygon(draw::pos_t px, draw::pos_t py, draw::pos_t cx, draw::pos_t cy,
                         draw::pos_t r, uint8_t sides) {
    if (sides < 3) return draw::sdCircle(px, py, cx, cy, r);
    const int32_t dx = px - cx, dy = py - cy;
    const int32_t dist = static_cast<int32_t>(dist16(dx, dy));
    if (dist == 0) return -r;
    // Fold the angle into one wedge, then measure to that wedge's flat edge.
    const angle16 a = atan16(dy, dx);
    const uint32_t wedge = 65536u / sides;
    uint32_t within = a % wedge;
    if (within > wedge / 2) within = wedge - within;      // mirror to the wedge's half
    const int32_t c = cos16(static_cast<angle16>(within));   // signed, per lib8tion
    if (c <= 0) return dist - r;
    // Distance to the edge is the radius divided by cos(angle from the wedge center).
    const int32_t edge = static_cast<int32_t>((static_cast<int64_t>(r) * 32768) / c);
    return dist - edge;
}

// --- Projection: a 3D point onto a 2D panel, which is one divide ----------------------------------

/// Project a 3D point onto the screen. `z` is depth from the viewer.
inline bool project(int32_t x, int32_t y, int32_t z, int32_t fov,
                    int32_t& outX, int32_t& outY) {
    if (z <= 0) return false;                       // at or behind the viewer: nothing to draw
    // A point near the plane projects arbitrarily far out, so out of range is treated as behind the viewer.
    const int64_t px = (static_cast<int64_t>(x) * fov) / z;
    const int64_t py = (static_cast<int64_t>(y) * fov) / z;
    if (px < INT32_MIN || px > INT32_MAX || py < INT32_MIN || py > INT32_MAX) return false;
    outX = static_cast<int32_t>(px);
    outY = static_cast<int32_t>(py);
    return true;
}

/// How bright something at depth `z` should be, given the distance where it fades out entirely.
constexpr uint8_t depthFade(int32_t z, int32_t far) {
    if (z <= 0 || far <= 0) return 255;
    if (z >= far) return 0;
    return static_cast<uint8_t>(((far - z) * 255) / far);
}

// --- Color ---------------------------------------------------------------------------------------

/// Iñigo Quilez's cosine palette.
inline RGB cosPalette(frac16 t, uint8_t aR, uint8_t aG, uint8_t aB,
                      uint8_t bR, uint8_t bG, uint8_t bB,
                      uint8_t cR, uint8_t cG, uint8_t cB,
                      uint8_t dR, uint8_t dG, uint8_t dB) {
    const auto ch = [t](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        const angle16 ang = static_cast<angle16>((static_cast<uint32_t>(t) * c) / 64u
                                                 + (static_cast<uint32_t>(d) << 8));
        const int32_t cosv = static_cast<int32_t>(cos16(ang));   // -32768..32767
        const int32_t v = a + ((b * cosv) >> 15);
        return static_cast<uint8_t>(clamp(v, 0, 255));
    };
    return RGB{ch(aR, bR, cR, dR), ch(aG, bG, cG, dG), ch(aB, bB, cB, dB)};
}

/// Blend two colors by `t`, the color form of `mix`.
inline RGB mixColor(RGB a, RGB b, frac16 t) {
    const uint8_t f = static_cast<uint8_t>(t >> 8);
    return RGB{static_cast<uint8_t>(a.r + (((b.r - a.r) * f) >> 8)),
               static_cast<uint8_t>(a.g + (((b.g - a.g) * f) >> 8)),
               static_cast<uint8_t>(a.b + (((b.b - a.b) * f) >> 8))};
}

// --- The shader runner ---------------------------------------------------------------------------

/// Run `fn(x, y, t)` for every pixel and write what it returns.
template <typename ShadeFn>
inline void each(const draw::Canvas& cv, angle16 t, ShadeFn fn) {
    const lengthType w = cv.dims.x, h = cv.dims.y;
    for (lengthType py = 0; py < h; py++) {
        for (lengthType px = 0; px < w; px++) {
            int32_t sx, sy;
            uv(px, py, w, h, sx, sy);
            draw::pixel(cv, {px, py, 0}, fn(sx, sy, t));
        }
    }
}

/// @}

}  // namespace mm::shader
