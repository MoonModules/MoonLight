#pragma once

#include "light/util/light_types.h"   // Coord3D, lengthType
#include "light/layers/Buffer.h" // Buffer (flat light array)
#include "core/util/color.h"          // RGB, scale8
#include "core/util/math8.h"
#include "core/util/math16.h"       // isqrt: true-distance SDFs; halfLifeKeep: the decay weight
#include "light/util/Palette.h"       // blend(RGB,RGB,amt): for blendPixel
#include "light/powerfunctions/fonts.h"         // fonts::Font: bitmap glyph tables for draw::text

#include <algorithm>             // std::reverse: in-place rotation for a wrapping scroll
#include <cstring>               // std::memmove/memcpy/memset: the scroll's run moves

/// @defgroup draw Geometry draw primitives
/// @{
/// Bounds-clipped, integer-only drawing from 1D through 3D over the flat light buffer.
///
/// The Bresenham walk and the clipping live here once, so an effect calls `line` rather than re-rolling it.
///
/// @moreinfo
///
/// ## Why the box is clipped
///
/// `addPixel` already rejects an out-of-bounds pixel, but only AFTER the distance function has run for it, which leaves two faults the clip removes.
/// A radius the caller chose makes the walk cost r squared however small the grid is, so one large number stalls the render thread.
/// And `lengthType` is 16 bits, so a radius near its ceiling puts `x1` at INT16_MAX, the increment wraps to negative, and the loop never ends at all.
///
/// ## Addressing
///
/// Index order matches the engine, and a pixel outside the grid is silently clipped, so a line running off the edge stops drawing.
///
/// ## Signed distance fields
///
/// An SDF answers how far a point is from a shape's edge: negative inside, zero on it, positive outside.
/// One number then gives a fill, an edge, an outline, a glow and a smooth blend, and the same expression serves a strand, a matrix and a volume.
///
/// ## Squared first
///
/// Measured on an S3 at 128 by 128: about 14 cycles a pixel squared, against about 108 for the true distance.
/// So every shape has a squared variant, and an effect asks for a real distance only when it needs one.
///
/// ## Rendering below the output resolution
///
/// A field's cost is per light, so computing fewer and interpolating the rest is what affords one on a large fixture.
/// The stretch costs a fixed price per output light, so it saves nothing on a field that was already cheap.
/// On a 64 by 64 layer a two-octave field goes 59 to 34 microseconds at half resolution, a curl field 212 to 71.


namespace mm::draw {


/// The surface a draw call writes to: a buffer plus the grid dimensions that address it.
///
/// Binding the two makes a mismatch unrepresentable, and applies the depth guard once rather than per effect.
struct Canvas {
    uint8_t* data = nullptr;   ///< first byte of the light array
    size_t   bytes = 0;        ///< total writable bytes (the bound every write is clipped to)
    Coord3D  dims{0, 0, 0};    ///< logical extents; z is >= 1 (see the depth guard above)
    uint8_t  cpl = 3;          ///< channels per light

    /// Build from a Buffer + the layer's logical extents, applying the depth guard.
    static Canvas of(Buffer& buf, lengthType w, lengthType h, lengthType d) {
        return Canvas{buf.data(), buf.bytes(),
                      Coord3D{w, h, static_cast<lengthType>(d > 0 ? d : 1)},
                      buf.channelsPerLight()};
    }

    /// Byte offset of a coordinate, or `bytes` when it is outside the grid.
    size_t offsetOf(Coord3D p) const {
        if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return bytes;
        return (static_cast<size_t>(p.z) * dims.y * dims.x
                + static_cast<size_t>(p.y) * dims.x + p.x) * cpl;
    }
};

/// One pixel, clipped to the grid: the Canvas form.
inline void pixel(const Canvas& cv, Coord3D p, RGB c) {
    const size_t off = cv.offsetOf(p);
    if (off + (cv.cpl < 3 ? cv.cpl : 3) > cv.bytes) return;
    if (cv.cpl >= 1) cv.data[off + 0] = c.r;
    if (cv.cpl >= 2) cv.data[off + 1] = c.g;
    if (cv.cpl >= 3) cv.data[off + 2] = c.b;
}

/// Read one pixel; black when outside the grid. The Canvas form of `get`.
inline RGB get(const Canvas& cv, Coord3D p) {
    const size_t off = cv.offsetOf(p);
    if (off + (cv.cpl < 3 ? cv.cpl : 3) > cv.bytes) return RGB{0, 0, 0};
    return RGB{cv.cpl >= 1 ? cv.data[off + 0] : uint8_t{0},
               cv.cpl >= 2 ? cv.data[off + 1] : uint8_t{0},
               cv.cpl >= 3 ? cv.data[off + 2] : uint8_t{0}};
}

namespace detail {
/// Walk the pixels of a 3D Bresenham line from a to b, calling `plot` for each.
template <typename PlotFn>
inline void walkLine(Coord3D a, Coord3D b, uint8_t shorten, PlotFn plot) {
    if (shorten == 0) return;
    if (shorten < 255) {
        const int bx = ((2 * int(b.x) - 2 * int(a.x)) * int(shorten)) / 255 + 2 * int(a.x);
        const int by = ((2 * int(b.y) - 2 * int(a.y)) * int(shorten)) / 255 + 2 * int(a.y);
        const int bz = ((2 * int(b.z) - 2 * int(a.z)) * int(shorten)) / 255 + 2 * int(a.z);
        b = {static_cast<lengthType>((bx + 1) / 2),
             static_cast<lengthType>((by + 1) / 2),
             static_cast<lengthType>((bz + 1) / 2)};
    }
    Coord3D p = a;
    const lengthType dx = b.x > a.x ? static_cast<lengthType>(b.x - a.x) : static_cast<lengthType>(a.x - b.x);
    const lengthType dy = b.y > a.y ? static_cast<lengthType>(b.y - a.y) : static_cast<lengthType>(a.y - b.y);
    const lengthType dz = b.z > a.z ? static_cast<lengthType>(b.z - a.z) : static_cast<lengthType>(a.z - b.z);
    const lengthType sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1, sz = a.z < b.z ? 1 : -1;

    if (dx >= dy && dx >= dz) {
        lengthType ey = 2 * dy - dx, ez = 2 * dz - dx;
        for (lengthType i = 0; i <= dx; i++) {
            plot(p);
            if (ey >= 0) { p.y = static_cast<lengthType>(p.y + sy); ey -= 2 * dx; }
            if (ez >= 0) { p.z = static_cast<lengthType>(p.z + sz); ez -= 2 * dx; }
            ey += 2 * dy; ez += 2 * dz; p.x = static_cast<lengthType>(p.x + sx);
        }
    } else if (dy >= dx && dy >= dz) {
        lengthType ex = 2 * dx - dy, ez = 2 * dz - dy;
        for (lengthType i = 0; i <= dy; i++) {
            plot(p);
            if (ex >= 0) { p.x = static_cast<lengthType>(p.x + sx); ex -= 2 * dy; }
            if (ez >= 0) { p.z = static_cast<lengthType>(p.z + sz); ez -= 2 * dy; }
            ex += 2 * dx; ez += 2 * dz; p.y = static_cast<lengthType>(p.y + sy);
        }
    } else {
        lengthType ex = 2 * dx - dz, ey = 2 * dy - dz;
        for (lengthType i = 0; i <= dz; i++) {
            plot(p);
            if (ex >= 0) { p.x = static_cast<lengthType>(p.x + sx); ex -= 2 * dz; }
            if (ey >= 0) { p.y = static_cast<lengthType>(p.y + sy); ey -= 2 * dz; }
            ex += 2 * dx; ey += 2 * dy; p.z = static_cast<lengthType>(p.z + sz);
        }
    }
}
}  // namespace detail

/// One pixel, clipped to the grid: the (Buffer&, dims) form.
inline void pixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return;
    const uint8_t cpl = buf.channelsPerLight();
    const size_t off = (static_cast<size_t>(p.z) * dims.y * dims.x
                        + static_cast<size_t>(p.y) * dims.x + p.x) * cpl;
    if (off + (cpl < 3 ? cpl : 3) > buf.bytes()) return;   // defends a dims/buffer mismatch
    uint8_t* d = buf.data();
    if (cpl >= 1) d[off + 0] = c.r;
    if (cpl >= 2) d[off + 1] = c.g;
    if (cpl >= 3) d[off + 2] = c.b;
}

/// A straight line a to b, clipped to the grid.
inline void line(Buffer& buf, Coord3D dims, Coord3D a, Coord3D b, RGB c, uint8_t shorten = 255) {
    detail::walkLine(a, b, shorten, [&](Coord3D p) { pixel(buf, dims, p, c); });
}

// --- Buffer read/modify helpers --------------------------------------------

/// Byte offset of a coordinate, or `bytes` when it lies outside the grid.
inline size_t offsetOf(const Buffer& buf, Coord3D dims, Coord3D p) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return buf.bytes();
    return (static_cast<size_t>(p.z) * dims.y * dims.x + static_cast<size_t>(p.y) * dims.x + p.x)
           * buf.channelsPerLight();
}

/// Read the RGB at a pixel, black when out of bounds or under three channels.
inline RGB get(const Buffer& buf, Coord3D dims, Coord3D p) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return {0, 0, 0};
    const uint8_t* d = buf.data();
    return {d[off + 0], d[off + 1], d[off + 2]};
}

/// Blend a color into a pixel by an amount, in place.
inline void blendPixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c, uint8_t amt) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return;
    uint8_t* d = buf.data();
    const RGB cur{d[off + 0], d[off + 1], d[off + 2]};
    const RGB out = blend(cur, c, amt);
    d[off + 0] = out.r; d[off + 1] = out.g; d[off + 2] = out.b;
}

/// Add light to a pixel, saturating so a bright one cannot wrap to dark.
inline void addPixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return;
    uint8_t* d = buf.data();
    d[off + 0] = qadd8(d[off + 0], c.r);
    d[off + 1] = qadd8(d[off + 1], c.g);
    d[off + 2] = qadd8(d[off + 2], c.b);
}

/// Fade every channel toward black by an amount over 255.
inline void fade(Buffer& buf, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t i = 0; i < n; i++) d[i] = scale8(d[i], keep);
}

/// Blur one axis in place, as a single carryover pass.
inline void blurAxis(uint8_t* d, size_t cpl, size_t len, size_t stride,
                     size_t lineCount, size_t lineStride, uint8_t amt) {
    if (len < 2 || cpl < 3) return;                 // nothing to seep along a 1-pixel (or sub-RGB) axis
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    const uint8_t seep = static_cast<uint8_t>(amt >> 1);
    for (size_t l = 0; l < lineCount; l++) {
        uint8_t* base = d + l * lineStride;
        uint8_t cr = 0, cg = 0, cb = 0;             // carryover (the seep flowing forward), starts black
        size_t off = 0, prev = 0;
        for (size_t i = 0; i < len; i++, off += stride) {
            uint8_t* px = base + off;
            const uint8_t pr = scale8(px[0], seep), pg = scale8(px[1], seep), pb = scale8(px[2], seep);
            px[0] = qadd8(scale8(px[0], keep), cr);  // keep self + receive prev pixel's forward seep
            px[1] = qadd8(scale8(px[1], keep), cg);
            px[2] = qadd8(scale8(px[2], keep), cb);
            if (i) {                                 // seep back into the previous pixel (deferred add)
                uint8_t* pv = base + prev;
                pv[0] = qadd8(pv[0], pr); pv[1] = qadd8(pv[1], pg); pv[2] = qadd8(pv[2], pb);
            }
            cr = pr; cg = pg; cb = pb; prev = off;
        }
        uint8_t* last = base + prev;                 // the final forward seep lands on the last pixel
        last[0] = qadd8(last[0], cr); last[1] = qadd8(last[1], cg); last[2] = qadd8(last[2], cb);
    }
}

/// Box blur the whole buffer.
inline void blur(Buffer& buf, Coord3D dims, uint8_t amt) {
    if (amt == 0) return;
    uint8_t* d = buf.data();
    const size_t cpl = buf.channelsPerLight();
    const size_t w = dims.x > 0 ? static_cast<size_t>(dims.x) : 0;
    const size_t h = dims.y > 0 ? static_cast<size_t>(dims.y) : 0;
    const size_t z = dims.z > 0 ? static_cast<size_t>(dims.z) : 0;
    if (w == 0 || h == 0 || z == 0) return;
    if (static_cast<size_t>(w * h * z) * cpl > buf.bytes()) return;   // dims/buffer mismatch guard
    // x-pass: each (y,z) line is `w` pixels, stride cpl; lines start every w·cpl bytes, h·z of them.
    blurAxis(d, cpl, w, cpl, h * z, w * cpl, amt);
    // z outside, since the column starts are not contiguous across z blocks.
    for (size_t zz = 0; zz < z; zz++)
        blurAxis(d + zz * h * w * cpl, cpl, h, w * cpl, w, cpl, amt);
    // z-pass (3D only): each (x,y) line is `z` pixels, stride w·h·cpl; w·h lines stepping by cpl.
    if (z > 1) blurAxis(d, cpl, z, w * h * cpl, w * h, cpl, amt);
}

/// Fill every light with one color.
inline void fill(Buffer& buf, RGB c) {
    const uint8_t cpl = buf.channelsPerLight();
    if (cpl == 0) return;   // a 0-channel buffer has no color to write; guards off += 0 spinning
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t off = 0; off + cpl <= n; off += cpl) {
        if (cpl >= 1) d[off + 0] = c.r;
        if (cpl >= 2) d[off + 1] = c.g;
        if (cpl >= 3) d[off + 2] = c.b;
    }
}

/// Blit one glyph at a grid position.
inline void glyph(Buffer& buf, Coord3D dims, const fonts::Font& font, char ch, lengthType x, lengthType y, RGB c) {
    if (ch < 32 || ch > 126) return;
    const uint8_t idx = static_cast<uint8_t>(ch - 32);
    const uint8_t* rows = font.rows + static_cast<size_t>(idx) * font.height;
    for (uint8_t ry = 0; ry < font.height; ry++) {
        const uint8_t bits = rows[ry];
        // Column rx is bit (7 - rx); reading (rx + 8-width) mirrors a 'b' into a 'd'.
        for (uint8_t rx = 0; rx < font.width; rx++)
            if ((bits >> (7 - rx)) & 0x01)
                pixel(buf, dims, {static_cast<lengthType>(x + rx), static_cast<lengthType>(y + ry), 0}, c);
    }
}

/// Draw a string, answering the first line's pixel width.
inline lengthType text(Buffer& buf, Coord3D dims, const fonts::Font& font, const char* str,
                       lengthType x, lengthType y, RGB c) {
    if (!str) return 0;
    lengthType cx = x, cy = y;
    lengthType firstLineWidth = 0;   // frozen at the first '\n' so a multi-line string still reports line 1
    bool onFirstLine = true;
    for (const char* p = str; *p; p++) {
        if (*p == '\n') {
            if (onFirstLine) { firstLineWidth = static_cast<lengthType>(cx - x); onFirstLine = false; }
            cx = x; cy = static_cast<lengthType>(cy + font.height); continue;
        }
        glyph(buf, dims, font, *p, cx, cy, c);
        cx = static_cast<lengthType>(cx + font.width);
    }
    return onFirstLine ? static_cast<lengthType>(cx - x) : firstLineWidth;
}

// ---- Canvas overloads --------------------------------------------------------------------------

// These carry their own implementations, so a pair can drift: the Canvas blur already did once.

/// Fade every channel toward black. Canvas form; the Buffer form forwards here.
inline void fade(const Canvas& cv, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    for (size_t i = 0; i < cv.bytes; i++) cv.data[i] = scale8(cv.data[i], keep);
}

/// Decay every sample toward black by a half-life: after `halfLifeMs`, half of it is gone.
inline void decay(const Canvas& cv, uint32_t halfLifeMs, uint32_t dtMs) {
    const uint32_t keep = mm::halfLifeKeep(dtMs, halfLifeMs);
    if (keep >= 65536) return;                 // nothing elapsed, or no half-life asked for
    if (keep == 0) { std::memset(cv.data, 0, cv.bytes); return; }
    for (size_t i = 0; i < cv.bytes; i++)
        cv.data[i] = static_cast<uint8_t>((static_cast<uint32_t>(cv.data[i]) * keep) >> 16);
}

/// The same decay over a 16-bit plane, which is the form a trail uses at any framerate.
inline void decay16(uint16_t* data, size_t n, uint32_t halfLifeMs, uint32_t dtMs) {
    if (!data) return;
    const uint32_t keep = mm::halfLifeKeep(dtMs, halfLifeMs);
    if (keep >= 65536) return;
    if (keep == 0) { std::memset(data, 0, n * sizeof(uint16_t)); return; }
    for (size_t i = 0; i < n; i++)
        data[i] = static_cast<uint16_t>((static_cast<uint32_t>(data[i]) * keep) >> 16);
}

/// Fill every light with one color, leaving any channel beyond RGB untouched.
inline void fill(const Canvas& cv, RGB c) {
    if (cv.cpl == 0) return;   // a 0-channel buffer has no color to write
    for (size_t off = 0; off + cv.cpl <= cv.bytes; off += cv.cpl) {
        if (cv.cpl >= 1) cv.data[off + 0] = c.r;
        if (cv.cpl >= 2) cv.data[off + 1] = c.g;
        if (cv.cpl >= 3) cv.data[off + 2] = c.b;
    }
}

/// Read-modify-write lerp of one pixel toward `c` by `amt`.
inline void blendPixel(const Canvas& cv, Coord3D p, RGB c, uint8_t amt) {
    const RGB cur = get(cv, p);
    pixel(cv, p, blend(cur, c, amt));
}

/// Saturating additive pixel: light adds, so this never wraps to black.
inline void addPixel(const Canvas& cv, Coord3D p, RGB c) {
    const RGB cur = get(cv, p);
    pixel(cv, p, RGB{qadd8(cur.r, c.r), qadd8(cur.g, c.g), qadd8(cur.b, c.b)});
}

/// Separable box blur over every axis with extent above 1, so one call covers 1D through 3D.
inline void blur(const Canvas& cv, uint8_t amt) {
    if (amt == 0 || cv.cpl == 0) return;
    const size_t cpl = cv.cpl;
    const size_t w = cv.dims.x > 0 ? static_cast<size_t>(cv.dims.x) : 0;
    const size_t h = cv.dims.y > 0 ? static_cast<size_t>(cv.dims.y) : 0;
    const size_t z = cv.dims.z > 0 ? static_cast<size_t>(cv.dims.z) : 0;
    if (w == 0 || h == 0 || z == 0) return;
    if (w * h * z * cpl > cv.bytes) return;                 // dims/buffer mismatch guard
    blurAxis(cv.data, cpl, w, cpl, h * z, w * cpl, amt);    // x
    for (size_t zz = 0; zz < z; zz++)                       // y, per z-slice
        blurAxis(cv.data + zz * h * w * cpl, cpl, h, w * cpl, w, cpl, amt);
    if (z > 1) blurAxis(cv.data, cpl, z, w * h * cpl, w * h, cpl, amt);   // z
}

/// A straight line on a Canvas, sharing the Buffer form's Bresenham walker.
inline void line(const Canvas& cv, Coord3D a, Coord3D b, RGB c, uint8_t shorten = 255) {
    detail::walkLine(a, b, shorten, [&](Coord3D p) { pixel(cv, p, c); });
}

// --- Blob field (metaballs) --------------------------------------------------------------------

/// One blob's orbit: where it sits at time `t` on a grid of `w` by `h`.
struct BlobPath {
    uint8_t speedMul;   ///< multiplies the shared time base, so blobs drift apart
    uint8_t phaseX;     ///< phase offset on x, keeping the paths from coinciding
    uint8_t phaseY;   ///< the vertical path's phase offset
};

/// Evaluate blob centers for this frame into `outX`/`outY` (caller-sized to `count`).
inline void blobCenters(const BlobPath* paths, uint8_t count, uint8_t t, lengthType w, lengthType h,
                        int16_t* outX, int16_t* outY) {
    for (uint8_t b = 0; b < count; b++) {
        const uint8_t tb = static_cast<uint8_t>(t * paths[b].speedMul);
        outX[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + paths[b].phaseX)) * w) >> 8);
        outY[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + paths[b].phaseY)) * h) >> 8);
    }
}

/// `r2` is a squared radius in WHOLE PIXELS, not sub-pixel units.
inline uint32_t blobField(lengthType x, lengthType y, const int16_t* bx, const int16_t* by,
                          uint8_t count, int32_t r2) {
    uint32_t field = 0;
    for (uint8_t b = 0; b < count; b++) {
        const int32_t dx = static_cast<int32_t>(x) - bx[b];
        const int32_t dy = static_cast<int32_t>(y) - by[b];
        const int32_t d2 = dx * dx + dy * dy + 1;
        field += static_cast<uint32_t>((r2 * 64) / d2);
    }
    return field;
}

// --- Scrolling ------------------------------------------------------------------------------

// One memmove per contiguous run, since x is adjacent in memory and a row is adjacent along y.

/// Move the grid `delta` steps along `axis` (0 = x, 1 = y, 2 = z). Positive delta moves toward increasing coordinates. Vacated cells go dark unless `wrap` is set.
inline void scroll(const Canvas& cv, uint8_t axis, int delta, bool wrap = false) {
    if (delta == 0 || cv.data == nullptr) return;
    const lengthType extent = axis == 0 ? cv.dims.x : (axis == 1 ? cv.dims.y : cv.dims.z);
    if (extent <= 1) return;                       // nothing to move along a degenerate axis

    // Before the modulo, which would turn a whole-extent shift into a no-op.
    if (!wrap && (delta >= extent || delta <= -extent)) { fill(cv, RGB{0, 0, 0}); return; }

    // A shift keeps its sign, since the direction decides which end goes dark.
    int shift = delta % extent;
    if (shift == 0) return;                        // a full turn when wrapping; nothing to do
    if (wrap && shift < 0) shift += extent;

    // Every axis is equally-spaced lines with a fixed step, so one loop covers all three.
    const size_t cpl = cv.cpl;
    const size_t rowBytes = static_cast<size_t>(cv.dims.x) * cpl;
    const size_t sliceBytes = static_cast<size_t>(cv.dims.y) * rowBytes;
    // Two nested counts, since along Y the (z, x) columns are not evenly spaced by one stride.
    size_t step = 0, outer = 0, outerStride = 0, inner = 1, innerStride = 0;
    switch (axis) {
        case 0:  step = cpl;        outer = static_cast<size_t>(cv.dims.y) * cv.dims.z; outerStride = rowBytes;
                 inner = 1;         innerStride = 0;                                                            break;
        case 1:  step = rowBytes;   outer = cv.dims.z;                                  outerStride = sliceBytes;
                 inner = cv.dims.x; innerStride = cpl;                                                          break;
        default: step = sliceBytes; outer = cv.dims.y;                                  outerStride = rowBytes;
                 inner = cv.dims.x; innerStride = cpl;                                                          break;
    }
    const size_t lineCount = outer * inner;

    // The largest run held is one line, which is at most the grid's longest axis.
    for (size_t line = 0; line < lineCount; line++) {
        uint8_t* base = cv.data + (line / inner) * outerStride + (line % inner) * innerStride;
        // Skip the line rather than the scroll: a mismatch costs that line, not every later one.
        if (base + static_cast<size_t>(extent - 1) * step + cpl > cv.data + cv.bytes) continue;

        if (step == cpl) {
            // Contiguous run: the whole line is one memmove.
            uint8_t* p = base;
            const size_t n = static_cast<size_t>(extent) * cpl;
            const size_t off = static_cast<size_t>(shift > 0 ? shift : 0) * cpl;
            if (wrap) {
                // Three reversals: the standard in-place rotation, with no scratch buffer.
                std::reverse(p, p + n - off);
                std::reverse(p + n - off, p + n);
                std::reverse(p, p + n);
            } else if (shift > 0) {
                std::memmove(p + off, p, n - off);   // toward increasing x: the low end goes dark
                std::memset(p, 0, off);
            } else {
                const size_t back = static_cast<size_t>(-shift) * cpl;
                std::memmove(p, p + back, n - back); // toward decreasing x: the high end goes dark
                std::memset(p + n - back, 0, back);
            }
        } else {
            // Far end first, so a forward shift does not overwrite a source it has yet to read.
            if (wrap) {
                // No scratch, so any channel count rotates whole, and O(extent) not O(shift*extent).
                const auto swapCells = [&](int i, int j) {
                    uint8_t* a = base + static_cast<size_t>(i) * step;
                    uint8_t* b = base + static_cast<size_t>(j) * step;
                    for (size_t k = 0; k < cpl; k++) { const uint8_t tmp = a[k]; a[k] = b[k]; b[k] = tmp; }
                };
                const auto reverseRange = [&](int lo, int hi) {
                    while (lo < hi) { swapCells(lo, hi); lo++; hi--; }
                };
                reverseRange(0, extent - 1 - shift);
                reverseRange(extent - shift, extent - 1);
                reverseRange(0, extent - 1);
            } else if (shift > 0) {
                // Far end first, so a source cell is read before the shift overwrites it.
                for (int c = extent - 1; c >= 0; c--) {
                    const int src = c - shift;
                    uint8_t* dst = base + static_cast<size_t>(c) * step;
                    if (src >= 0) std::memcpy(dst, base + static_cast<size_t>(src) * step, cpl);
                    else          std::memset(dst, 0, cpl);
                }
            } else {
                // Negative shift walks the other way, near end first, for the same reason.
                for (int c = 0; c < extent; c++) {
                    const int src = c - shift;                  // shift < 0, so src > c
                    uint8_t* dst = base + static_cast<size_t>(c) * step;
                    if (src < extent) std::memcpy(dst, base + static_cast<size_t>(src) * step, cpl);
                    else              std::memset(dst, 0, cpl);
                }
            }
        }
    }
}

// --- Rectangles and bars -----------------------------------------------------------------------

// The color is a callback, since every call site varies it along the run.

/// Direction a bar grows from its origin. Named rather than a signed delta.
enum class Grow : uint8_t { Right, Left, Up, Down };

/// A run of `len` cells from `(x, y)` along `dir`, colored per cell by `colorAt(i)` where `i` is the distance from the origin. Clipped per cell, so a bar longer than the grid stops.
template <typename ColorFn>
inline void bar(const Canvas& cv, lengthType x, lengthType y, lengthType len, Grow dir,
                ColorFn colorAt) {
    for (lengthType i = 0; i < len; i++) {
        lengthType px = x, py = y;
        switch (dir) {
            case Grow::Right: px = static_cast<lengthType>(x + i); break;
            case Grow::Left:  px = static_cast<lengthType>(x - i); break;
            case Grow::Up:    py = static_cast<lengthType>(y - i); break;   // row 0 is the top
            case Grow::Down:  py = static_cast<lengthType>(y + i); break;
        }
        pixel(cv, {px, py, 0}, colorAt(i));
    }
}

/// A bar in one flat color.
inline void bar(const Canvas& cv, lengthType x, lengthType y, lengthType len, Grow dir, RGB c) {
    bar(cv, x, y, len, dir, [c](lengthType) { return c; });
}

/// Filled axis-aligned rectangle from `(x, y)`, `w` by `h`, colored per row by `colorAt(row)`.
template <typename ColorFn>
inline void fillRect(const Canvas& cv, lengthType x, lengthType y, lengthType w, lengthType h,
                     ColorFn colorAt) {
    for (lengthType row = 0; row < h; row++)
        bar(cv, x, static_cast<lengthType>(y + row), w, Grow::Right,
            [&](lengthType) { return colorAt(row); });
}

/// A filled rectangle in one flat color.
inline void fillRect(const Canvas& cv, lengthType x, lengthType y, lengthType w, lengthType h, RGB c) {
    fillRect(cv, x, y, w, h, [c](lengthType) { return c; });
}

/// The outline of an axis-aligned rectangle: four bars, corners written once each.
inline void rect(const Canvas& cv, lengthType x, lengthType y, lengthType w, lengthType h, RGB c) {
    if (w <= 0 || h <= 0) return;
    const lengthType right = static_cast<lengthType>(x + w - 1);
    const lengthType bottom = static_cast<lengthType>(y + h - 1);
    bar(cv, x, y, w, Grow::Right, c);                                    // top
    if (h == 1) return;                                                  // a 1-row rect is one bar
    bar(cv, x, bottom, w, Grow::Right, c);                               // bottom
    if (h <= 2) return;                                                  // no interior rows to join
    const lengthType inner = static_cast<lengthType>(h - 2);
    bar(cv, x, static_cast<lengthType>(y + 1), inner, Grow::Down, c);    // left
    if (w > 1) bar(cv, right, static_cast<lengthType>(y + 1), inner, Grow::Down, c);
}

// --- Circles ---------------------------------------------------------------------------------

// Bresenham's midpoint, for a shape that sits on the grid; the SDF forms are for one that moves.

/// The outline of a circle, integer center and radius, using the midpoint algorithm. Each of the eight octants is mirrored from one computed arc, so the rim is exact and symmetric.
inline void circle(const Canvas& cv, lengthType cx, lengthType cy, lengthType r, RGB c) {
    if (r < 0) return;
    if (r == 0) { pixel(cv, {cx, cy, 0}, c); return; }
    lengthType x = 0, y = r;
    int d = 3 - 2 * r;                     // the midpoint decision variable
    while (x <= y) {
        // Eight-way symmetry: one computed point paints eight rim cells.
        pixel(cv, {static_cast<lengthType>(cx + x), static_cast<lengthType>(cy + y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - x), static_cast<lengthType>(cy + y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx + x), static_cast<lengthType>(cy - y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - x), static_cast<lengthType>(cy - y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx + y), static_cast<lengthType>(cy + x), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - y), static_cast<lengthType>(cy + x), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx + y), static_cast<lengthType>(cy - x), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - y), static_cast<lengthType>(cy - x), 0}, c);
        if (d < 0) { d += 4 * x + 6; }
        else       { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/// A filled disc: the same midpoint walk, drawing a horizontal span per scanline instead of points, colored per row by `colorAt(dyFromCenter)`.
template <typename ColorFn>
inline void fillCircle(const Canvas& cv, lengthType cx, lengthType cy, lengthType r, ColorFn colorAt) {
    if (r < 0) return;
    lengthType x = 0, y = r;
    int d = 3 - 2 * r;
    while (x <= y) {
        // Two span pairs per step: the arc is walked in x, and each point fixes the width of a row.
        bar(cv, static_cast<lengthType>(cx - x), static_cast<lengthType>(cy + y),
            static_cast<lengthType>(2 * x + 1), Grow::Right, colorAt(y));
        bar(cv, static_cast<lengthType>(cx - x), static_cast<lengthType>(cy - y),
            static_cast<lengthType>(2 * x + 1), Grow::Right, colorAt(static_cast<lengthType>(-y)));
        bar(cv, static_cast<lengthType>(cx - y), static_cast<lengthType>(cy + x),
            static_cast<lengthType>(2 * y + 1), Grow::Right, colorAt(x));
        bar(cv, static_cast<lengthType>(cx - y), static_cast<lengthType>(cy - x),
            static_cast<lengthType>(2 * y + 1), Grow::Right, colorAt(static_cast<lengthType>(-x)));
        if (d < 0) { d += 4 * x + 6; }
        else       { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/// A filled disc in one flat color.
inline void fillCircle(const Canvas& cv, lengthType cx, lengthType cy, lengthType r, RGB c) {
    fillCircle(cv, cx, cy, r, [c](lengthType) { return c; });
}

// --- Anti-aliased line ------------------------------------------------------------------------

// Wu 1991, at roughly twice the writes, so Bresenham stays the default.

/// A 2D anti-aliased line between two integer endpoints (Wu 1991), z taken from `a`.
inline void lineAA(const Canvas& cv, Coord3D a, Coord3D b, RGB c) {
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    const bool steep = (y1 > y0 ? y1 - y0 : y0 - y1) > (x1 > x0 ? x1 - x0 : x0 - x1);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }       // walk the major axis
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }

    const int dx = x1 - x0;
    const int dy = y1 - y0;
    // A zero-length line is a point; without this the gradient divides by zero.
    if (dx == 0) { pixel(cv, a, c); return; }

    // Fixed-point gradient in 16.16, so the walk stays integer.
    const int32_t gradient = (static_cast<int32_t>(dy) << 16) / dx;
    int32_t inter = (static_cast<int32_t>(y0) << 16);

    for (int x = x0; x <= x1; x++) {
        const int32_t whole = inter >> 16;
        const uint8_t frac = static_cast<uint8_t>((inter >> 8) & 0xFF);
        const uint8_t weightNear = static_cast<uint8_t>(255 - frac);   // `near` is a Windows macro
        // The two cells straddling the true line, weighted by how close it passes to each.
        if (steep) {
            addPixel(cv, {static_cast<lengthType>(whole), static_cast<lengthType>(x), a.z},
                     RGB{scale8(c.r, weightNear), scale8(c.g, weightNear), scale8(c.b, weightNear)});
            addPixel(cv, {static_cast<lengthType>(whole + 1), static_cast<lengthType>(x), a.z},
                     RGB{scale8(c.r, frac), scale8(c.g, frac), scale8(c.b, frac)});
        } else {
            addPixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(whole), a.z},
                     RGB{scale8(c.r, weightNear), scale8(c.g, weightNear), scale8(c.b, weightNear)});
            addPixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(whole + 1), a.z},
                     RGB{scale8(c.r, frac), scale8(c.g, frac), scale8(c.b, frac)});
        }
        inter += gradient;
    }
}

namespace sprites {
/// A small movable bitmap: palette-indexed pixels (index 0 = the transparent key, the classic key-color scheme), frames stacked vertically (frame f = rows [f*h, (f+1)*h)). Carried as constexpr data, the fonts.h shape. Full alpha (Porter-Duff over) stays deferred until a consumer needs it.
/// A transparent index is what classic sprites used and what LED walls need.
struct Sprite {
    const uint8_t* pixels;    ///< palette-indexed bytes, one per pixel per frame
    const RGB* palette;       ///< the sprite's own colors; index 0 is never read
    uint8_t w, h, frames, paletteCount;   ///< the sprite's extents and its palette size
};
}  // namespace sprites

/// Blit one sprite frame at pixel (x, y).
inline void sprite(const Canvas& cv, const sprites::Sprite& s, uint8_t frame,
                   lengthType x, lengthType y, uint8_t scale = 1, bool flipX = false) {
    if (!s.pixels || !s.palette || s.w == 0 || s.h == 0 || s.frames == 0 || scale == 0) return;
    if (frame >= s.frames) frame = static_cast<uint8_t>(s.frames - 1);
    const uint8_t* rows = s.pixels + static_cast<size_t>(frame) * s.w * s.h;
    for (uint8_t ry = 0; ry < s.h; ry++) {
        for (uint8_t rx = 0; rx < s.w; rx++) {
            // Mirrors the read, not the write, so the sprite lands with the same footprint.
            const uint8_t sx0 = flipX ? static_cast<uint8_t>(s.w - 1 - rx) : rx;
            const uint8_t idx = rows[static_cast<size_t>(ry) * s.w + sx0];
            if (idx == 0 || idx >= s.paletteCount) continue;
            const RGB c = s.palette[idx];
            for (uint8_t sy = 0; sy < scale; sy++)
                for (uint8_t sx = 0; sx < scale; sx++)
                    pixel(cv, {static_cast<lengthType>(x + rx * scale + sx),
                               static_cast<lengthType>(y + ry * scale + sy), 0}, c);
        }
    }
}

/// Blit one glyph on a Canvas: the Canvas form of `glyph`. Same MSB-first column order and the same clipping.
inline void glyph(const Canvas& cv, const fonts::Font& font, char ch, lengthType x, lengthType y, RGB c) {
    if (ch < 32 || ch > 126) return;
    const uint8_t idx = static_cast<uint8_t>(ch - 32);
    const uint8_t* rows = font.rows + static_cast<size_t>(idx) * font.height;
    for (uint8_t ry = 0; ry < font.height; ry++) {
        const uint8_t bits = rows[ry];
        for (uint8_t rx = 0; rx < font.width; rx++)
            if ((bits >> (7 - rx)) & 0x01)
                pixel(cv, {static_cast<lengthType>(x + rx), static_cast<lengthType>(y + ry), 0}, c);
    }
}

/// Draw a NUL-terminated string on a Canvas.
inline lengthType text(const Canvas& cv, const fonts::Font& font, const char* str,
                       lengthType x, lengthType y, RGB c) {
    if (!str) return 0;
    lengthType cx = x, cy = y;
    lengthType firstLineWidth = 0;
    bool onFirstLine = true;
    for (const char* p = str; *p; p++) {
        if (*p == '\n') {
            if (onFirstLine) { firstLineWidth = static_cast<lengthType>(cx - x); onFirstLine = false; }
            cx = x; cy = static_cast<lengthType>(cy + font.height); continue;
        }
        glyph(cv, font, *p, cx, cy, c);
        cx = static_cast<lengthType>(cx + font.width);
    }
    return onFirstLine ? static_cast<lengthType>(cx - x) : firstLineWidth;
}

/// Byte offset of a coordinate on a Canvas, or `bytes` when it is outside the grid. The Canvas form of `offsetOf`.
inline size_t offsetOf(const Canvas& cv, Coord3D p) { return cv.offsetOf(p); }

// ---- Sub-pixel positioning ----------------------------------------------------------------------

/// A position in 24.8 fixed point: 256 sub-units to the pixel, so a shift gives the pixel and the low byte the fraction. int32 covers ±8 million pixels.
using pos_t = int32_t;

/// One pixel = this many sub-units. `pixels << kSubShift` converts, `sub >> kSubShift` decodes.
inline constexpr uint8_t kSubShift = 8;
inline constexpr int32_t kSubOne = 1 << kSubShift;

/// Convert a whole-pixel coordinate to sub-pixel space.
inline constexpr pos_t toSub(lengthType px) { return static_cast<pos_t>(px) << kSubShift; }

/// Decode a sub-pixel coordinate to the pixel that contains it. Uses an arithmetic shift, which floors toward negative infinity.
inline constexpr lengthType toPixel(pos_t sub) { return static_cast<lengthType>(sub >> kSubShift); }

/// Draw a point at a FRACTIONAL position, spreading its light across the neighboring pixels by how much of each it covers (Xiaolin Wu, SIGGRAPH 1991.
inline void splat(const Canvas& cv, pos_t x, pos_t y, pos_t z, RGB c) {
    const lengthType px = toPixel(x), py = toPixel(y), pz = toPixel(z);
    // Masking rather than subtracting, which is correct for negatives too.
    const uint16_t fx = static_cast<uint16_t>(x & (kSubOne - 1));
    const uint16_t fy = static_cast<uint16_t>(y & (kSubOne - 1));
    const uint16_t fz = static_cast<uint16_t>(z & (kSubOne - 1));

    for (uint8_t corner = 0; corner < 8; corner++) {
        const bool dx = corner & 1, dy = corner & 2, dz = corner & 4;
        // A corner on a degenerate axis is its partner's pixel, so skip it.
        if (dx && cv.dims.x <= 1 && fx == 0) continue;
        if (dy && cv.dims.y <= 1 && fy == 0) continue;
        if (dz && cv.dims.z <= 1 && fz == 0) continue;

        const uint32_t wx = dx ? fx : (kSubOne - fx);
        const uint32_t wy = dy ? fy : (kSubOne - fy);
        const uint32_t wz = dz ? fz : (kSubOne - fz);
        // The product of the per-axis coverages, normalized back to 0..255.
        const uint32_t w = (wx * wy * wz) >> (2 * kSubShift);
        if (w == 0) continue;

        const Coord3D p{static_cast<lengthType>(px + (dx ? 1 : 0)),
                        static_cast<lengthType>(py + (dy ? 1 : 0)),
                        static_cast<lengthType>(pz + (dz ? 1 : 0))};
        addPixel(cv, p, RGB{static_cast<uint8_t>((c.r * w) >> kSubShift),
                            static_cast<uint8_t>((c.g * w) >> kSubShift),
                            static_cast<uint8_t>((c.b * w) >> kSubShift)});
    }
}

/// 2D convenience: the z axis sits at pixel 0.
inline void splat(const Canvas& cv, pos_t x, pos_t y, RGB c) { splat(cv, x, y, 0, c); }

// --- Gather (reading the grid as a texture) ----------------------------------------------------

// Wrapping rather than clamping, which is what makes a tunnel or a scroll seamless.

/// Bilinear sample at a sub-pixel coordinate, wrapping at the grid edges.
inline RGB sampleWrap(const Canvas& cv, pos_t x, pos_t y, lengthType z = 0) {
    if (cv.dims.x <= 0 || cv.dims.y <= 0) return RGB{0, 0, 0};
    // Floor to the containing pixel and keep the fraction for the blend.
    const int32_t x0 = toPixel(x), y0 = toPixel(y);
    const uint8_t fx = static_cast<uint8_t>(x & (kSubOne - 1));
    const uint8_t fy = static_cast<uint8_t>(y & (kSubOne - 1));
    // Positive modulo: a negative coordinate wraps to the far edge rather than off the grid.
    const auto wrap = [](int32_t v, lengthType n) {
        const int32_t m = v % n;
        return static_cast<lengthType>(m < 0 ? m + n : m);
    };
    const lengthType xa = wrap(x0, cv.dims.x), xb = wrap(x0 + 1, cv.dims.x);
    const lengthType ya = wrap(y0, cv.dims.y), yb = wrap(y0 + 1, cv.dims.y);

    const RGB c00 = get(cv, {xa, ya, z}), c10 = get(cv, {xb, ya, z});
    const RGB c01 = get(cv, {xa, yb, z}), c11 = get(cv, {xb, yb, z});
    const auto mix = [](uint8_t a, uint8_t b, uint8_t t) {
        return static_cast<uint8_t>(a + (((static_cast<int32_t>(b) - a) * t) >> 8));
    };
    return RGB{mix(mix(c00.r, c10.r, fx), mix(c01.r, c11.r, fx), fy),
               mix(mix(c00.g, c10.g, fx), mix(c01.g, c11.g, fx), fy),
               mix(mix(c00.b, c10.b, fx), mix(c01.b, c11.b, fx), fy)};
}

/// How a sample outside the grid is read: wrapped around, or held at the edge.
enum class Edge : uint8_t {
    Wrap,    ///< the far side comes back around: seamless for a scroll, a tunnel, a torus flow
    Clamp,   ///< the edge pixel repeats: what a flow leaving the grid should smear against
};

/// Bilinear sample at a sub-pixel coordinate, holding the edge pixel outside the grid.
inline RGB sampleClamp(const Canvas& cv, pos_t x, pos_t y, lengthType z = 0) {
    if (cv.dims.x <= 0 || cv.dims.y <= 0) return RGB{0, 0, 0};
    const int32_t x0 = toPixel(x), y0 = toPixel(y);
    const uint8_t fx = static_cast<uint8_t>(x & (kSubOne - 1));
    const uint8_t fy = static_cast<uint8_t>(y & (kSubOne - 1));
    const auto clamp = [](int32_t v, lengthType n) {
        return static_cast<lengthType>(v < 0 ? 0 : (v >= n ? n - 1 : v));
    };
    const lengthType xa = clamp(x0, cv.dims.x), xb = clamp(x0 + 1, cv.dims.x);
    const lengthType ya = clamp(y0, cv.dims.y), yb = clamp(y0 + 1, cv.dims.y);
    const RGB c00 = get(cv, {xa, ya, z}), c10 = get(cv, {xb, ya, z});
    const RGB c01 = get(cv, {xa, yb, z}), c11 = get(cv, {xb, yb, z});
    const auto mix = [](uint8_t a, uint8_t b, uint8_t tt) {
        return static_cast<uint8_t>(a + (((static_cast<int32_t>(b) - a) * tt) >> 8));
    };
    return RGB{mix(mix(c00.r, c10.r, fx), mix(c01.r, c11.r, fx), fy),
               mix(mix(c00.g, c10.g, fx), mix(c01.g, c11.g, fx), fy),
               mix(mix(c00.b, c10.b, fx), mix(c01.b, c11.b, fx), fy)};
}

/// Sample under either edge rule, so a caller carries the choice as data rather than a branch.
inline RGB sampleEdge(const Canvas& cv, pos_t x, pos_t y, Edge edge, lengthType z = 0) {
    return edge == Edge::Wrap ? sampleWrap(cv, x, y, z) : sampleClamp(cv, x, y, z);
}

/// Move every pixel of `src` along a velocity field and write the result into `dst`.
template <typename Rule>
inline void advect(const Canvas& dst, const Canvas& src, Rule&& rule, Edge edge = Edge::Wrap) {
    if (!dst.data || !src.data) return;
    if (dst.dims.x != src.dims.x || dst.dims.y != src.dims.y) return;   // two shapes, no meaning
    for (lengthType z = 0; z < dst.dims.z; z++) {
        for (lengthType y = 0; y < dst.dims.y; y++) {
            for (lengthType x = 0; x < dst.dims.x; x++) {
                pos_t vx = 0, vy = 0;
                rule(x, y, z, vx, vy);
                // Backward: where what is here now came from, hence the subtraction.
                const pos_t sx = toSub(x) - vx;
                const pos_t sy = toSub(y) - vy;
                pixel(dst, {x, y, z}, sampleEdge(src, sx, sy, edge, z));
            }
        }
    }
}

/// Advect a 16-BIT plane: the form a trail uses, and the reason it survives.
template <typename Rule>
inline void advect16(uint16_t* dst, const uint16_t* src, lengthType w, lengthType h, lengthType d,
                     Rule&& rule, Edge edge = Edge::Wrap) {
    if (!dst || !src || w <= 0 || h <= 0 || d <= 0) return;
    const auto fold = [&](int32_t v, lengthType n) -> lengthType {
        if (edge == Edge::Wrap) { const int32_t m = v % n; return static_cast<lengthType>(m < 0 ? m + n : m); }
        return static_cast<lengthType>(v < 0 ? 0 : (v >= n ? n - 1 : v));
    };
    for (lengthType z = 0; z < d; z++) {
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                pos_t vx = 0, vy = 0;
                rule(x, y, z, vx, vy);
                const pos_t sx = toSub(x) - vx, sy = toSub(y) - vy;      // BACKWARD, as advect()
                const int32_t x0 = toPixel(sx), y0 = toPixel(sy);
                const uint32_t fx = static_cast<uint32_t>(sx & (kSubOne - 1));
                const uint32_t fy = static_cast<uint32_t>(sy & (kSubOne - 1));
                const lengthType xa = fold(x0, w), xb = fold(x0 + 1, w);
                const lengthType ya = fold(y0, h), yb = fold(y0 + 1, h);
                const size_t slice = static_cast<size_t>(z) * h * w;
                const size_t i00 = (slice + static_cast<size_t>(ya) * w + xa) * 3;
                const size_t i10 = (slice + static_cast<size_t>(ya) * w + xb) * 3;
                const size_t i01 = (slice + static_cast<size_t>(yb) * w + xa) * 3;
                const size_t i11 = (slice + static_cast<size_t>(yb) * w + xb) * 3;
                const size_t o = (slice + static_cast<size_t>(y) * w + x) * 3;
                for (uint8_t c = 0; c < 3; c++) {
                    // Two lerps at full width: the top edge, the bottom edge, then between them.
                    const uint32_t a = src[i00 + c] + (((static_cast<int32_t>(src[i10 + c]) - src[i00 + c]) * static_cast<int32_t>(fx)) >> kSubShift);
                    const uint32_t b = src[i01 + c] + (((static_cast<int32_t>(src[i11 + c]) - src[i01 + c]) * static_cast<int32_t>(fx)) >> kSubShift);
                    dst[o + c] = static_cast<uint16_t>(a + (((static_cast<int32_t>(b) - static_cast<int32_t>(a)) * static_cast<int32_t>(fy)) >> kSubShift));
                }
            }
        }
    }
}

/// Combine two colors by taking the brighter channel.
inline RGB combineMax(RGB a, RGB b) {
    return RGB{a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b};
}


// ---- Signed distance fields ---------------------------------------------------------------------

/// Squared distance from a point to a circle's center, minus the squared radius. Negative inside, zero on the rim, positive outside.
inline int32_t sdCircleSq(pos_t px, pos_t py, pos_t cx, pos_t cy, pos_t r) {
    // Widened, a squared sub-unit coordinate overflowing 32 bits on a large grid.
    const int64_t dx = px - cx, dy = py - cy;
    const int64_t d2 = dx * dx + dy * dy;
    const int64_t r2 = static_cast<int64_t>(r) * r;
    const int64_t diff = d2 - r2;
    // Saturate rather than wrap: a clamped magnitude stays usable as a falloff input.
    if (diff > INT32_MAX) return INT32_MAX;
    if (diff < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(diff);
}

/// True signed distance to a circle's edge, in sub-pixel units. Costs a square root.
inline int32_t sdCircle(pos_t px, pos_t py, pos_t cx, pos_t cy, pos_t r) {
    const int64_t dx = px - cx, dy = py - cy;
    const uint64_t d2 = static_cast<uint64_t>(dx * dx + dy * dy);
    const uint32_t d = isqrt(static_cast<uint32_t>(d2 > UINT32_MAX ? UINT32_MAX : d2));
    return static_cast<int32_t>(d) - r;
}

/// Signed distance to an axis-aligned box centered at (cx, cy) with half-extents (bx, by).
inline int32_t sdBox(pos_t px, pos_t py, pos_t cx, pos_t cy, pos_t bx, pos_t by) {
    const pos_t qx = (px - cx < 0 ? cx - px : px - cx) - bx;
    const pos_t qy = (py - cy < 0 ? cy - py : py - cy) - by;
    const pos_t outX = qx > 0 ? qx : 0;
    const pos_t outY = qy > 0 ? qy : 0;
    const pos_t outside = outX > outY ? outX : outY;
    if (outside > 0) return outside;
    // Fully inside: the distance to the NEAREST face is the larger (least negative) overshoot.
    return qx > qy ? qx : qy;
}

/// Signed distance to a line segment a→b, minus `thickness`.
inline int32_t sdSegment(pos_t px, pos_t py, pos_t ax, pos_t ay, pos_t bx, pos_t by, pos_t thickness) {
    const int64_t pax = px - ax, pay = py - ay;
    const int64_t bax = bx - ax, bay = by - ay;
    const int64_t len2 = bax * bax + bay * bay;
    // A zero-length segment falls back to the point distance, rather than dividing by zero.
    int64_t hx = pax, hy = pay;
    if (len2 > 0) {
        int64_t tNum = pax * bax + pay * bay;      // projection along the segment, scaled by len2
        if (tNum < 0) tNum = 0;
        if (tNum > len2) tNum = len2;
        hx = pax - (bax * tNum) / len2;
        hy = pay - (bay * tNum) / len2;
    }
    const uint64_t d2 = static_cast<uint64_t>(hx * hx + hy * hy);
    const uint32_t d = isqrt(static_cast<uint32_t>(d2 > UINT32_MAX ? UINT32_MAX : d2));
    return static_cast<int32_t>(d) - thickness;
}

/// Smooth minimum of two distances: the operator that makes two shapes flow into each other.
inline int32_t smin(int32_t a, int32_t b, int32_t k) {
    if (k <= 0) return a < b ? a : b;
    // Polynomial smooth min: h = clamp(0.5 + 0.5*(b-a)/k), mix(b, a, h) - k*h*(1-h).
    const int32_t diff = b - a;
    int32_t h = 128 + (static_cast<int64_t>(diff) * 128) / k;   // 0..256 in 8-bit fixed point
    if (h < 0) h = 0;
    if (h > 256) h = 256;
    // Both terms widen before multiplying: either wrap returns a value larger than both inputs, inverting the blend.
    const int64_t mixed = b + ((static_cast<int64_t>(a) - b) * h) / 256;
    const int64_t bump  = (static_cast<int64_t>(k) * h * (256 - h)) / (256 * 256);
    return static_cast<int32_t>(mixed - bump);
}

/// Turn a signed distance into coverage: 255 well inside, 0 well outside, a ramp across the edge.
inline uint8_t coverage(int32_t d, pos_t edge = kSubOne) {
    if (edge <= 0) return d <= 0 ? 255 : 0;
    if (d <= -edge) return 255;
    if (d >= edge) return 0;
    // Map [-edge, +edge] onto [255, 0].
    return static_cast<uint8_t>(((edge - d) * 255) / (2 * edge));
}


/// Clip an SDF shape's bounding box to the canvas, so the walk covers only pixels that can be lit: @xref{why-the-box-is-clipped}.
inline void clipBox(const Canvas& cv, lengthType& x0, lengthType& x1,
                    lengthType& y0, lengthType& y1) {
    // An EMPTY canvas is stated rather than left to arithmetic, which would reach the same answer by accident.
    if (cv.dims.x <= 0 || cv.dims.y <= 0) { x1 = -1; y1 = -1; return; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<lengthType>(cv.dims.x - 1)) x1 = static_cast<lengthType>(cv.dims.x - 1);
    if (y1 > static_cast<lengthType>(cv.dims.y - 1)) y1 = static_cast<lengthType>(cv.dims.y - 1);
}

// --- Filled shapes with soft edges: the SDF forms, shading the boundary by coverage ---------------

/// A filled disc with an anti-aliased edge, additive so overlapping discs brighten.
inline void disc(const Canvas& cv, pos_t cx, pos_t cy, pos_t r, RGB c, lengthType z = 0) {
    if (r <= 0) return;
    lengthType x0 = toPixel(cx - r) - 1, x1 = toPixel(cx + r) + 1;
    lengthType y0 = toPixel(cy - r) - 1, y1 = toPixel(cy + r) + 1;
    clipBox(cv, x0, x1, y0, y1);
    for (lengthType y = y0; y <= y1; y++) {
        for (lengthType x = x0; x <= x1; x++) {
            // The pixel's center against the edge: sampling the corner drifts the shape as it grows.
            const int32_t d = sdCircle(toSub(x) + kSubOne / 2, toSub(y) + kSubOne / 2, cx, cy, r);
            const uint8_t cov = coverage(d);
            if (cov == 0) continue;
            addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
        }
    }
}

/// A circle OUTLINE of a given stroke width, centered on the radius.
inline void ring(const Canvas& cv, pos_t cx, pos_t cy, pos_t r, pos_t thickness, RGB c,
                 lengthType z = 0) {
    if (r <= 0 || thickness <= 0) return;
    const pos_t half = thickness / 2;
    lengthType x0 = toPixel(cx - r - half) - 1, x1 = toPixel(cx + r + half) + 1;
    lengthType y0 = toPixel(cy - r - half) - 1, y1 = toPixel(cy + r + half) + 1;
    clipBox(cv, x0, x1, y0, y1);
    for (lengthType y = y0; y <= y1; y++) {
        for (lengthType x = x0; x <= x1; x++) {
            // The ring is the band around the circle's line, so the distance is taken absolute.
            const int32_t d = sdCircle(toSub(x) + kSubOne / 2, toSub(y) + kSubOne / 2, cx, cy, r);
            const int32_t off = (d < 0 ? -d : d) - half;
            const uint8_t cov = coverage(off);
            if (cov == 0) continue;
            addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
        }
    }
}

/// A line with WIDTH and sub-pixel endpoints, antialiased along both edges and round-capped.
inline void strokeLine(const Canvas& cv, pos_t x0, pos_t y0, pos_t x1, pos_t y1, pos_t thickness,
                       RGB c, lengthType z = 0) {
    if (thickness <= 0) return;
    const pos_t half = thickness / 2;
    const int64_t dx = static_cast<int64_t>(x1) - x0, dy = static_cast<int64_t>(y1) - y0;
    const int64_t lenSq = dx * dx + dy * dy;
    // A zero-length stroke is a dot, which is what a fully retracted hand should still draw.
    if (lenSq == 0) { disc(cv, x0, y0, half, c, z); return; }

    const pos_t loX = (x0 < x1 ? x0 : x1) - half, hiX = (x0 > x1 ? x0 : x1) + half;
    const pos_t loY = (y0 < y1 ? y0 : y1) - half, hiY = (y0 > y1 ? y0 : y1) + half;
    for (lengthType y = toPixel(loY) - 1; y <= toPixel(hiY) + 1; y++) {
        for (lengthType x = toPixel(loX) - 1; x <= toPixel(hiX) + 1; x++) {
            const pos_t px = toSub(x) + kSubOne / 2, py = toSub(y) + kSubOne / 2;
            // Projected onto the segment and clamped to its ends, which is what rounds the caps. `t` is the position along the segment, 0..kSubOne.
            const int64_t vx = static_cast<int64_t>(px) - x0, vy = static_cast<int64_t>(py) - y0;
            int64_t t = ((vx * dx + vy * dy) * kSubOne) / lenSq;
            if (t < 0) t = 0;
            if (t > kSubOne) t = kSubOne;
            const pos_t nx = static_cast<pos_t>(x0 + ((dx * t) >> kSubShift));
            const pos_t ny = static_cast<pos_t>(y0 + ((dy * t) >> kSubShift));
            // The distance to that point against the stroke's half-width, the edge rule every shape uses.
            const uint8_t cov = coverage(sdCircle(px, py, nx, ny, half));
            if (cov == 0) continue;
            addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
        }
    }
}

/// The volumetric form: a filled sphere, shaded the same way.
inline void sphere(const Canvas& cv, pos_t cx, pos_t cy, pos_t cz, pos_t r, RGB c) {
    if (r <= 0) return;
    const lengthType x0 = toPixel(cx - r) - 1, x1 = toPixel(cx + r) + 1;
    const lengthType y0 = toPixel(cy - r) - 1, y1 = toPixel(cy + r) + 1;
    const lengthType z0 = toPixel(cz - r) - 1, z1 = toPixel(cz + r) + 1;
    for (lengthType z = z0; z <= z1; z++) {
        for (lengthType y = y0; y <= y1; y++) {
            for (lengthType x = x0; x <= x1; x++) {
                const int64_t dx = toSub(x) + kSubOne / 2 - cx;
                const int64_t dy = toSub(y) + kSubOne / 2 - cy;
                const int64_t dz = toSub(z) + kSubOne / 2 - cz;
                const uint64_t d2 = static_cast<uint64_t>(dx * dx + dy * dy + dz * dz);
                const uint32_t dist = isqrt(static_cast<uint32_t>(d2 > UINT32_MAX ? UINT32_MAX : d2));
                const uint8_t cov = coverage(static_cast<int32_t>(dist) - r);
                if (cov == 0) continue;
                addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
            }
        }
    }
}

// --- Rendering below the output resolution: the fieldScale lever -----------------------------------

/// One destination column's blend: the two source columns and the weight between them.
struct UpscaleTap { uint16_t a, b; uint16_t w; };

/// Bilinearly stretch a smaller 16-bit plane over a larger one.
inline void upscale16(uint16_t* dst, lengthType dw, lengthType dh, lengthType dd,
                      const uint16_t* src, lengthType sw, lengthType sh, lengthType sd,
                      UpscaleTap* taps, size_t tapCount) {
    if (!dst || !src || dw <= 0 || dh <= 0 || dd <= 0 || sw <= 0 || sh <= 0 || sd <= 0) return;
    if (!taps || tapCount < static_cast<size_t>(dw)) return;
    // The standard alignment, computed once per axis: a divide per sample cost more than the field it stretched.
    const auto axis = [](lengthType d, lengthType dn, lengthType sn) -> int32_t {
        if (dn <= 1) return 0;
        const int64_t num = (static_cast<int64_t>(d) * 2 + 1) * sn - dn;
        return static_cast<int32_t>((num << 15) / dn);          // 16.16, may be negative at the edge
    };
    const auto clamp = [](int32_t v, lengthType n) -> size_t {
        return static_cast<size_t>(v < 0 ? 0 : (v >= n ? n - 1 : v));
    };
    // One entry per destination column: the two source columns it blends and the weight between.
    UpscaleTap* row = taps;
    for (lengthType x = 0; x < dw; x++) {
        const int32_t fx = axis(x, dw, sw);
        row[x] = {static_cast<uint16_t>(clamp(fx >> 16, sw)),
                  static_cast<uint16_t>(clamp((fx >> 16) + 1, sw)),
                  static_cast<uint16_t>(fx & 0xFFFF)};
    }
    const bool interpZ = sd > 1;
    for (lengthType z = 0; z < dd; z++) {
        const int32_t fz = interpZ ? axis(z, dd, sd) : 0;
        const int32_t z0 = fz >> 16;
        const uint32_t wz = static_cast<uint32_t>(fz & 0xFFFF);
        const size_t za = clamp(z0, sd), zb = interpZ ? clamp(z0 + 1, sd) : za;
        for (lengthType y = 0; y < dh; y++) {
            const int32_t fy = axis(y, dh, sh);
            const int32_t y0 = fy >> 16;
            const uint32_t wy = static_cast<uint32_t>(fy & 0xFFFF);
            const size_t ya = clamp(y0, sh), yb = clamp(y0 + 1, sh);
            for (lengthType x = 0; x < dw; x++) {
                const UpscaleTap tap = row[x];
                const uint32_t wx = tap.w;
                const size_t xa = tap.a, xb = tap.b;
                const size_t slA = za * sh * sw, slB = zb * sh * sw;
                const size_t o = ((static_cast<size_t>(z) * dh + y) * dw + x) * 3;
                for (uint8_t c = 0; c < 3; c++) {
                    const auto at = [&](size_t sl, size_t yy, size_t xx) -> uint32_t {
                        return src[(sl + yy * sw + xx) * 3 + c];
                    };
                    // Signed: an unsigned difference wraps wherever the field descends, which lit whole cells.
                    const auto lerp = [](uint32_t a, uint32_t b, uint32_t w) -> uint32_t {
                        return static_cast<uint32_t>(static_cast<int64_t>(a)
                             + (((static_cast<int64_t>(b) - static_cast<int64_t>(a))
                                 * static_cast<int64_t>(w)) >> 16));
                    };
                    // Two rows of the near slice, then the far one, then between them.
                    const uint32_t n0 = lerp(at(slA, ya, xa), at(slA, ya, xb), wx);
                    const uint32_t n1 = lerp(at(slA, yb, xa), at(slA, yb, xb), wx);
                    uint32_t v = lerp(n0, n1, wy);
                    if (interpZ) {
                        const uint32_t f0 = lerp(at(slB, ya, xa), at(slB, ya, xb), wx);
                        const uint32_t f1 = lerp(at(slB, yb, xa), at(slB, yb, xb), wx);
                        v = lerp(v, lerp(f0, f1, wy), wz);
                    }
                    dst[o + c] = static_cast<uint16_t>(v);
                }
            }
        }
    }
}

// --- Narrowing 16-bit state to the wire: dithering moves the error rather than adding levels -----

/// How the low half of a 16-bit sample is disposed of on the way to a byte.
enum class Dither : uint8_t {
    None,      ///< truncate: the fastest, and what bands
    Ordered,   ///< a 4x4 Bayer threshold on the pixel's position: stateless, spatial
    Temporal,  ///< carry the error into the next frame: needs a byte per channel, and is smoothest
};

/// The 4x4 Bayer matrix, scaled to a 0..255 threshold. The standard recurrence, in its usual order.
inline constexpr uint8_t kBayer4[16] = {
      8, 136,  40, 168,
    200,  72, 232, 104,
     56, 184,  24, 152,
    248, 120, 216,  88,
};

/// Narrow one 16-bit sample to a byte, dithered.
inline uint8_t quantize(uint16_t v, Dither mode, uint8_t& carry,
                        lengthType x = 0, lengthType y = 0, lengthType z = 0) {
    switch (mode) {
        case Dither::Ordered: {
            // Depth rotates the matrix rather than indexing a third dimension, which a panel pays nothing for.
            const size_t cell = (static_cast<size_t>(y & 3) * 4) + (x & 3) + (static_cast<size_t>(z & 3) * 5);
            const uint8_t thr = kBayer4[cell & 15];
            const uint8_t hi = static_cast<uint8_t>(v >> 8);
            // Round up when the discarded low byte beats the threshold, saturating rather than wrapping.
            return (static_cast<uint8_t>(v & 0xFF) > thr && hi < 255) ? static_cast<uint8_t>(hi + 1) : hi;
        }
        case Dither::Temporal: {
            // This frame's remainder is added to the next, so the sequence averages to the true value.
            const uint32_t x16 = static_cast<uint32_t>(v) + carry;
            const uint32_t hi = x16 >> 8;
            const uint8_t out = static_cast<uint8_t>(hi > 255 ? 255 : hi);
            const uint32_t used = static_cast<uint32_t>(out) << 8;
            carry = static_cast<uint8_t>(x16 > used ? (x16 - used > 255 ? 255 : x16 - used) : 0);
            return out;
        }
        case Dither::None:
        default:
            return static_cast<uint8_t>(v >> 8);
    }
}

/// A 16-bit plane onto the canvas: the one narrowing step, dithered.
inline void blit16(const Canvas& cv, const uint16_t* p, lengthType w, lengthType h, lengthType d,
                   uint8_t* carry) {
    if (!p) return;
    std::size_t i = 0;
    uint8_t dummy = 0;
    for (lengthType z = 0; z < d; z++)
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++, i += 3) {
                const auto q = [&](std::size_t c) {
                    uint8_t& st = carry ? carry[i + c] : dummy;
                    return quantize(p[i + c], carry ? Dither::Temporal : Dither::None, st, x, y, z);
                };
                pixel(cv, {x, y, z}, RGB{q(0), q(1), q(2)});
            }
}


// --- Velocity rules: plain stateless functions answering which way the medium moves at a point -----

/// A steady wind: everything moves the same way at the same speed.
inline void flowWind(angle16 direction, int32_t speed, pos_t& vx, pos_t& vy) {
    vx = static_cast<pos_t>((static_cast<int32_t>(cos16(direction)) * speed) >> 15);
    vy = static_cast<pos_t>((static_cast<int32_t>(sin16(direction)) * speed) >> 15);
}

/// Out from a center, or into it when `speed` is negative. The fountain and the drain.
inline void flowRadial(lengthType x, lengthType y, lengthType cx, lengthType cy,
                       int32_t speed, pos_t& vx, pos_t& vy) {
    const int32_t dx = static_cast<int32_t>(x) - cx, dy = static_cast<int32_t>(y) - cy;
    if (dx == 0 && dy == 0) { vx = vy = 0; return; }        // the center has no direction to go
    const angle16 a = atan16(dy, dx);
    flowWind(a, speed, vx, vy);
}

/// Around a center and outward at once: the spiral, which is the two above added.
inline void flowSpiral(lengthType x, lengthType y, lengthType cx, lengthType cy,
                       int32_t angular, int32_t radial, pos_t& vx, pos_t& vy) {
    const int32_t dx = static_cast<int32_t>(x) - cx, dy = static_cast<int32_t>(y) - cy;
    if (dx == 0 && dy == 0) { vx = vy = 0; return; }
    const angle16 a = atan16(dy, dx);
    pos_t rx, ry, tx, ty;
    flowWind(a, radial, rx, ry);                            // outward
    flowWind(static_cast<angle16>(a + 16384), angular, tx, ty);   // and a quarter turn from it
    vx = rx + tx;
    vy = ry + ty;
}

/// @}

}  // namespace mm::draw
