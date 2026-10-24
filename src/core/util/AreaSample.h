#pragma once

/// Sampling a picture's rectangle onto one output cell: the proportional span and its mean in linear light.

#include "core/util/color.h"    // RGB
#include "core/util/math16.h"   // isqrt: the root the linear-light mean undoes its squares with

#include <cstddef>
#include <cstdint>

namespace mm {

/// The source pixels one output cell covers, `hi` one past the last.
struct Span {
    uint16_t lo = 0;   ///< the first source pixel the cell covers
    uint16_t hi = 0;   ///< one past the last, so hi - lo is the count
};

/// Taps an axis of one cell, so a cell's sum stays well inside 32 bits whatever the source size.
constexpr uint32_t kAreaTapsPerAxis = 4;

/// The source span output cell `i` of `out` covers, the proportional split every sampler shares.
inline Span cellSpan(uint32_t i, uint32_t out, uint32_t src) {
    uint32_t lo = i * src / out, hi = (i + 1u) * src / out;
    if (hi <= lo) hi = lo + 1;   // a source smaller than the output: neighboring cells share a pixel
    return Span{static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)};
}

/// Where a sampler writes: the buffer, the picture size it fills, and the floor below which a source byte reads as black.
struct Sink {
    uint8_t* rgb = nullptr;   ///< the destination, `width` x `height` x 3 bytes, top row first
    uint16_t width = 0;       ///< the picture's width, which a sampler fills exactly
    uint16_t height = 0;      ///< the picture's height, likewise
    uint8_t floor = 0;        ///< source bytes at or below this read as 0; 0 passes every byte through
};

/// The rectangle's mean in linear light (square, average, root: gamma 2 for the display curve), from at most four reads per axis. A byte at or below `floor` counts as 0, which a noisy source's black needs.
inline RGB areaMean(const uint8_t* rgb, size_t stride, Span cx, Span ry, uint8_t floor = 0) {
    const uint32_t sx = (cx.hi - cx.lo + kAreaTapsPerAxis - 1u) / kAreaTapsPerAxis;
    const uint32_t sy = (ry.hi - ry.lo + kAreaTapsPerAxis - 1u) / kAreaTapsPerAxis;
    uint32_t sum[3] = {0, 0, 0}, taps = 0;
    for (uint32_t y = ry.lo; y < ry.hi; y += sy) {
        const uint8_t* row = rgb + static_cast<size_t>(y) * stride;
        for (uint32_t x = cx.lo; x < cx.hi; x += sx) {
            const uint8_t* p = row + static_cast<size_t>(x) * 3u;
            for (int c = 0; c < 3; c++) {
                const uint32_t v = p[c] > floor ? p[c] : 0u;
                sum[c] += v * v;
            }
            taps++;
        }
    }
    if (!taps) return RGB{0, 0, 0};
    return RGB{static_cast<uint8_t>(isqrt(sum[0] / taps)), static_cast<uint8_t>(isqrt(sum[1] / taps)),
               static_cast<uint8_t>(isqrt(sum[2] / taps))};
}

}   // namespace mm
