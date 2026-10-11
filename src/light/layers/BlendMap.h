#pragma once

#include "light/layers/Buffer.h"
#include "light/layers/MappingLUT.h"

#include <cstring>

namespace mm {

/// @defgroup BlendMap Composing a layer into the frame
/// @{
/// The one pass that reads a layer's logical buffer and writes it, mapped and blended, into the physical frame.
///
/// @moreinfo
///
/// The pass runs once per enabled layer each frame, driven by the Drivers container, which reads each layer's own `blendMode` and `opacity` controls along with the stack order.
///
/// `clearFirst` clears the destination before writing, which the first and bottom layer of a composite does.
/// That is what keeps physical cells with no source, such as a sparse layout's lattice gaps, black.
/// It applies on the mapped path only: the identity path writes every byte it covers, so it has no unwritten cell to clear.
/// Later layers pass false and blend onto the frame accumulated below.
/// A single layer passes `Overwrite` at full opacity with a clear, which takes the exact fast path this had before composition existed.
///
/// ## The combine math is integer-only
///
/// The hot-path per-light rule applies, so one specialised loop is chosen once before the per-light loop and there is no per-pixel mode check.
///
/// | Mode | What it does |
/// |------|--------------|
/// | Overwrite | a plain copy with no read-back. A dense grid with no LUT is a `memcpy`; a single-write LUT copies per mapped light |
/// | Additive | Porter and Duff's plus: the destination plus the source scaled by opacity, summed with saturation at 255 |
/// | Alpha | the textbook 8-bit alpha-over, the source and destination weighted by opacity, divided by 255 through the fast reciprocal. Full opacity collapses to a plain overwrite at no blend cost |
/// | Multiply, Screen, Darken, Lighten, Difference, Exclusion | the W3C separable modes: a per-channel function, mixed with the backdrop by opacity as alpha is |
///
/// A non-overwriting LUT, one folding several logical lights onto a single physical cell, routes through the additive accumulate path so overlaps sum with clamping rather than last-writer-wins.
///
/// ## How a light finds its destination
///
/// A dense-grid layer has no LUT, so its buffer blends one to one with the source index equal to the physical index and no lookup at all.
/// A layer with a LUT maps each logical light to its physical destinations first.
/// Physical indices come from the LUT, which is built in range from the shared Layouts, so they address the destination in bounds by construction.
///
/// ## Why every mapped access is bounded
///
/// A reshape rebuilds the mapping and the driver's output buffer in separate steps of one `prepareTree` sweep, Layouts first, then the Layer, then Drivers.
/// A render tick can land between them, holding the new mapping's physical indices and the old, smaller buffer.
/// Unbounded, that writes past the end and corrupts the heap, and the failure then surfaces in an unrelated allocation later, which is what made resizing a layout look intermittently fatal.
///
/// This is a bound rather than a fix for the ordering.
/// The window is still there and the frame drawn inside it is briefly wrong, but it cannot corrupt memory, which is the property that matters.
///
/// ## What Overwrite defers to
///
/// Overwrite is the default, for a single layer or the bottom of a composite, and it defers to the LUT's own overwrites flag.
/// A mapping writing each physical cell once, a mirror, a shuffle or a sparse box, plain-copies.
/// A mapping folding several logical lights onto one cell accumulates additively within the layer, with clamping, while cross-layer Additive and Alpha remain the explicit ops.

/// How a layer's pixels combine into the destination during composition.
enum class BlendOp : uint8_t {
    Overwrite,   ///< `dst = src` (replace; the first/bottom layer, fastest, no read-back)
    Alpha,       ///< `dst = src*opacity + dst*(255-opacity)` (opacity-weighted over)
    Additive,    ///< `dst = clamp(dst + src*opacity/255)` (adds light, never dims)
    Multiply,    ///< `src*dst/255`: darkens, white keeps the backdrop and black hides it
    Screen,      ///< `255 - (255-src)*(255-dst)/255`: lightens, the inverse of multiply
    Darken,      ///< the smaller of source and backdrop
    Lighten,     ///< the larger of source and backdrop
    Difference,  ///< `|src - dst|`
    Exclusion,   ///< `src + dst - 2*src*dst/255`: difference with less contrast
};

/// Fast 8-bit divide by 255, exact over the full 16-bit range, avoiding a real divide on the hot path.
inline uint8_t div255(uint16_t x) { return static_cast<uint8_t>((x + (x >> 8) + 1) >> 8); }

/// The per-channel blend functions B(source, backdrop), by their specification names.
namespace blendfn {
/// The source wins, which alpha mixes with the backdrop by opacity.
struct Normal     { uint8_t operator()(uint8_t s, uint8_t) const { return s; } };
/// The product, scaled back to 8 bits.
struct Multiply   { uint8_t operator()(uint8_t s, uint8_t d) const { return div255(static_cast<uint16_t>(s) * d); } };
/// The complement of the product of the complements.
struct Screen     { uint8_t operator()(uint8_t s, uint8_t d) const { return static_cast<uint8_t>(255 - div255(static_cast<uint16_t>(255 - s) * (255 - d))); } };
/// The smaller channel.
struct Darken     { uint8_t operator()(uint8_t s, uint8_t d) const { return s < d ? s : d; } };
/// The larger channel.
struct Lighten    { uint8_t operator()(uint8_t s, uint8_t d) const { return s > d ? s : d; } };
/// The distance between the channels.
struct Difference { uint8_t operator()(uint8_t s, uint8_t d) const { return s > d ? static_cast<uint8_t>(s - d) : static_cast<uint8_t>(d - s); } };
// The product is rounded rather than floored, since doubling a floored product doubles its error; this stays in 0 to 255 for every pair.
/// The sum less twice the product.
struct Exclusion  { uint8_t operator()(uint8_t s, uint8_t d) const { return static_cast<uint8_t>(s + d - 2 * div255(static_cast<uint16_t>(s * d + 127))); } };
}  // namespace blendfn

namespace detail {
/// One channel blended by `B`, then mixed with the backdrop by `opacity`, as alpha mixes the source.
template <class B>
struct SeparableMix {
    B blend;           ///< the mode's per-channel function
    uint8_t opacity;   ///< how much of the result replaces the backdrop, 255 for all of it
    uint8_t operator()(uint8_t s, uint8_t d) const {
        const uint8_t b = blend(s, d);
        return opacity == 255 ? b
                              : div255(static_cast<uint16_t>(b) * opacity + static_cast<uint16_t>(d) * (255 - opacity));
    }
};

/// The dense grid: one byte onto the byte at the same index.
template <class Mix>
inline void blendDense(const Buffer& src, Buffer& dst, Mix mix) {
    const size_t n = src.bytes() < dst.bytes() ? src.bytes() : dst.bytes();
    const uint8_t* s = src.data();
    uint8_t* d = dst.data();
    for (size_t i = 0; i < n; i++) d[i] = mix(s[i], d[i]);
}

/// Through the LUT: each logical light onto every physical light it maps to, bounded by both buffers.
template <class Mix>
inline void blendMapped(const Buffer& src, Buffer& dst, const MappingLUT& lut, uint8_t channelsPerLight, Mix mix) {
    const nrOfLightsType logCount = lut.logicalCount();
    const size_t dstLights = channelsPerLight ? dst.bytes() / channelsPerLight : 0;
    const size_t srcLights = channelsPerLight ? src.bytes() / channelsPerLight : 0;
    for (nrOfLightsType li = 0; li < logCount; li++) {
        if (li >= srcLights) break;
        const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
        lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
            if (physIdx >= dstLights) return;
            uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
            for (uint8_t c = 0; c < channelsPerLight; c++) dstLight[c] = mix(srcLight[c], dstLight[c]);
        });
    }
}
}  // namespace detail

// One instance per function, so each mode gets its own tight loop and none checks the mode per light.
/// Blend `src` into `dst` by `B`, then mix with the backdrop by `opacity`, through the LUT when there is one.
template <class B>
inline void blendSeparable(const Buffer& src, Buffer& dst, const MappingLUT& lut, uint8_t channelsPerLight,
                           uint8_t opacity, B blend) {
    const detail::SeparableMix<B> mix{blend, opacity};
    if (lut.hasLUT()) detail::blendMapped(src, dst, lut, channelsPerLight, mix);
    else detail::blendDense(src, dst, mix);
}

/// Dispatch a separable mode once per layer, so the per-light loop is the mode's own.
inline void blendSeparable(BlendOp op, const Buffer& src, Buffer& dst, const MappingLUT& lut,
                           uint8_t channelsPerLight, uint8_t opacity) {
    switch (op) {
        case BlendOp::Multiply:   blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Multiply{}); break;
        case BlendOp::Screen:     blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Screen{}); break;
        case BlendOp::Darken:     blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Darken{}); break;
        case BlendOp::Lighten:    blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Lighten{}); break;
        case BlendOp::Difference: blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Difference{}); break;
        case BlendOp::Exclusion:  blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Exclusion{}); break;
        default:                  blendSeparable(src, dst, lut, channelsPerLight, opacity, blendfn::Normal{}); break;
    }
}

/// Blend one layer's `src` into `dst` through its LUT, once per frame.
inline void blendMap(const Buffer& src, Buffer& dst, const MappingLUT& lut,
                     uint8_t channelsPerLight,
                     BlendOp op = BlendOp::Overwrite, uint8_t opacity = 255,
                     bool clearFirst = true) {
    // No LUT is an identity map, the dense-grid case: blend one to one with no lookup.
    if (!lut.hasLUT()) {
        const size_t n = src.bytes() < dst.bytes() ? src.bytes() : dst.bytes();
        const uint8_t* s = src.data();
        uint8_t* d = dst.data();
        if (op == BlendOp::Overwrite && opacity == 255) {
            std::memcpy(d, s, n);
            return;
        }
        const bool full = (opacity == 255);
        if (op == BlendOp::Additive) {
            for (size_t i = 0; i < n; i++) {
                uint16_t sv = full ? s[i] : div255(static_cast<uint16_t>(s[i]) * opacity);
                uint16_t sum = static_cast<uint16_t>(d[i]) + sv;
                d[i] = sum > 255 ? 255 : static_cast<uint8_t>(sum);
            }
            return;
        }
        blendSeparable(op, src, dst, lut, channelsPerLight, opacity);   // alpha and the separable modes
        return;
    }

    if (clearFirst) dst.clear();   // bottom layer: cells with no source stay black
    const nrOfLightsType logCount = lut.logicalCount();
    const bool full = (opacity == 255);

    // Every mapped access below is bounded by these: a LUT entry is valid only against its own buffer.
    const size_t dstLights = channelsPerLight ? dst.bytes() / channelsPerLight : 0;
    const size_t srcLights = channelsPerLight ? src.bytes() / channelsPerLight : 0;

    // Overwrite defers to the LUT's own overwrites() flag, so a folding mapping accumulates.
    const bool effectiveAdditive = (op == BlendOp::Additive) ||
                                   (op == BlendOp::Overwrite && !lut.overwrites());

    // --- Plain overwrite (replace) — single-write LUT; copy, no read-back. ---
    if (op == BlendOp::Overwrite && full && lut.overwrites()) {
        for (nrOfLightsType li = 0; li < logCount; li++) {
            if (li >= srcLights) break;
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
                if (physIdx >= dstLights) return;
                uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
                for (uint8_t c = 0; c < channelsPerLight; c++) dstLight[c] = srcLight[c];
            });
        }
        return;
    }

    // --- Additive with clamp; opacity scales the source. full-opacity skips the scale. ---
    if (effectiveAdditive) {
        for (nrOfLightsType li = 0; li < logCount; li++) {
            if (li >= srcLights) break;
            const uint8_t* srcLight = src.data() + static_cast<size_t>(li) * channelsPerLight;
            lut.forEachDestination(li, [&](nrOfLightsType physIdx) {
                if (physIdx >= dstLights) return;
                uint8_t* dstLight = dst.data() + static_cast<size_t>(physIdx) * channelsPerLight;
                for (uint8_t c = 0; c < channelsPerLight; c++) {
                    uint16_t s = full ? srcLight[c] : div255(static_cast<uint16_t>(srcLight[c]) * opacity);
                    uint16_t sum = static_cast<uint16_t>(dstLight[c]) + s;
                    dstLight[c] = sum > 255 ? 255 : static_cast<uint8_t>(sum);
                }
            });
        }
        return;
    }

    // --- Alpha (over) and the separable modes, each mixed with the backdrop by opacity. ---
    blendSeparable(op, src, dst, lut, channelsPerLight, opacity);
}

/// @}
} // namespace mm
