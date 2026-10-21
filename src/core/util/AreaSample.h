#pragma once

/// Sampling a picture's rectangle onto one output cell: the proportional span and its mean in linear light.

#include "core/util/color.h"    // RGB
#include "core/util/math16.h"   // isqrt: the root the linear-light mean undoes its squares with

#include <cstddef>
#include <cstdint>

namespace mm {

/// The source pixels one output cell covers, `hi` one past the last.
struct Span {
    uint16_t lo = 0;
    uint16_t hi = 0;
};

/// Taps an axis of one cell, so a cell's sum stays well inside 32 bits whatever the source size.
constexpr uint32_t kAreaTapsPerAxis = 4;

/// The source span output cell `i` of `out` covers, the proportional split every sampler shares.
inline Span cellSpan(uint32_t i, uint32_t out, uint32_t src) {
    uint32_t lo = i * src / out, hi = (i + 1u) * src / out;
    if (hi <= lo) hi = lo + 1;   // a source smaller than the output: neighboring cells share a pixel
    return Span{static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)};
}

/// The rectangle's mean in linear light (square, average, root: gamma 2 for the display curve), from at most four reads per axis.
inline RGB areaMean(const uint8_t* rgb, size_t stride, Span cx, Span ry) {
    const uint32_t sx = (cx.hi - cx.lo + kAreaTapsPerAxis - 1u) / kAreaTapsPerAxis;
    const uint32_t sy = (ry.hi - ry.lo + kAreaTapsPerAxis - 1u) / kAreaTapsPerAxis;
    uint32_t sum[3] = {0, 0, 0}, taps = 0;
    for (uint32_t y = ry.lo; y < ry.hi; y += sy) {
        const uint8_t* row = rgb + static_cast<size_t>(y) * stride;
        for (uint32_t x = cx.lo; x < cx.hi; x += sx) {
            const uint8_t* p = row + static_cast<size_t>(x) * 3u;
            for (int c = 0; c < 3; c++) sum[c] += static_cast<uint32_t>(p[c]) * p[c];
            taps++;
        }
    }
    if (!taps) return RGB{0, 0, 0};
    return RGB{static_cast<uint8_t>(isqrt(sum[0] / taps)), static_cast<uint8_t>(isqrt(sum[1] / taps)),
               static_cast<uint8_t>(isqrt(sum[2] / taps))};
}

}   // namespace mm
