#pragma once

/// The hardware-free half of USB video capture: the UVC payload headers, the mode choice and the shrink onto a picture.

#include "core/util/math16.h"   // isqrt: the tree's integer root, which the box average needs

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::usbvideo {

/// One mode a grabber advertises, as its frame list reports it.
struct Mode {
    uint16_t width = 0;    ///< frame width in pixels
    uint16_t height = 0;   ///< frame height in pixels
    uint8_t fps = 0;       ///< frames a second at that size
};

// --- UVC bulk payload headers (UVC 1.5 2.4.3.3) ---------------------------------------------------

constexpr uint8_t kPts = 0x04, kScr = 0x08, kEoh = 0x80, kErr = 0x40, kReserved = 0x10;
constexpr size_t kBulkMps = 512;        ///< high-speed bulk packet size, the stride headers sit on
constexpr size_t kConfirmStrides = 3;   ///< a short header carries few bits, so one match proves nothing

/// A header's own length from its flags: 2 bytes, plus 4 for a PTS and 6 for an SCR.
inline size_t headerLenFor(uint8_t flags) {
    return 2u + ((flags & kPts) ? 4u : 0u) + ((flags & kScr) ? 6u : 0u);
}

/// Whether `h` is a payload header of exactly `want` bytes; `pts` pins it to the frame's first one.
inline bool isPayloadHeader(const uint8_t* h, size_t avail, size_t want, const uint8_t* pts) {
    if (avail < 2) return false;
    const uint8_t len = h[0], flags = h[1];
    if (len != want || len != headerLenFor(flags) || avail < len) return false;
    if (!(flags & kEoh) || (flags & (kErr | kReserved))) return false;
    // The SCR's top 5 bits are reserved and zero, the strongest single check a header carries.
    if ((flags & kScr) && (h[len - 1] & 0xF8)) return false;
    return !pts || !(flags & kPts) || std::memcmp(h + 2, pts, 4) == 0;
}

/// Whether headers of length `hl` really recur after `first`, which is what rules out a chance match.
inline bool confirmedRun(const uint8_t* d, size_t len, size_t first, size_t hl) {
    const uint8_t* pts = (d[first + 1] & kPts) ? d + first + 2 : nullptr;
    const size_t stride = first + hl;
    size_t seen = 0;
    for (size_t at = first + stride; at + hl <= len && seen < kConfirmStrides; at += stride, seen++)
        if (!isPayloadHeader(d + at, len - at, hl, pts)) return false;
    return seen > 0 || first + stride + hl > len;   // a frame too short to confirm is taken as it is
}

/// Drops the payload headers a bulk frame still carries, answering the compacted length.
inline size_t stripPayloadHeaders(uint8_t* d, size_t len) {
    static constexpr size_t kLens[] = {12, 8, 6, 2};   // longest first: the more fields, the surer the match
    size_t hdr = 0, hl = 0;
    for (size_t cand : kLens) {
        for (size_t i = kBulkMps - cand; i + cand <= len; i += kBulkMps) {
            if (!isPayloadHeader(d + i, len - i, cand, nullptr) || !confirmedRun(d, len, i, cand)) continue;
            hdr = i;
            hl = cand;
            break;
        }
        if (hdr) break;
    }
    if (!hdr) return len;
    const size_t payload = hdr + hl;
    uint8_t pts[4] = {};   // copied: the compaction overwrites the header it came from
    const bool hasPts = (d[hdr + 1] & kPts) != 0;
    if (hasPts) std::memcpy(pts, d + hdr + 2, sizeof pts);
    size_t w = hdr, r = hdr;
    while (isPayloadHeader(d + r, len - r, hl, hasPts ? pts : nullptr)) {
        r += hl;
        const size_t run = payload - hl < len - r ? payload - hl : len - r;
        std::memmove(d + w, d + r, run);
        w += run;
        r += run;
    }
    std::memmove(d + w, d + r, len - r);   // whatever follows a missing header is left as delivered
    return w + (len - r);
}

// --- Choosing a mode ----------------------------------------------------------------------------

/// UVC states a rate as a period in 100 ns ticks. Rounded, so 59.94 fps reads as 60 rather than 59.
inline uint8_t fpsFrom(uint32_t interval) {
    constexpr uint32_t kTicksPerSecond = 10000000;
    return interval ? static_cast<uint8_t>((kTicksPerSecond + interval / 2) / interval) : 0;
}

/// Of two advertised rates, the one to ask for: the fastest at or below `wanted`, else the slowest.
inline uint8_t betterFps(uint8_t have, uint8_t row, uint8_t wanted) {
    if (!have) return row;
    const bool haveOk = have <= wanted, rowOk = row <= wanted;
    if (haveOk != rowOk) return haveOk ? have : row;
    if (haveOk) return row > have ? row : have;
    return row < have ? row : have;
}

/// The smallest mode at or above the ceiling, at the rate asked for, so decode and shrink stay cheap.
inline bool pickMode(const Mode* modes, size_t count, uint16_t& width, uint16_t& height, uint8_t& fps) {
    uint32_t bestArea = 0;
    uint16_t bw = 0, bh = 0;
    uint8_t bestFps = 0;
    for (size_t i = 0; i < count; i++) {
        const Mode& m = modes[i];
        if (m.width < width || m.height < height) continue;   // below the ceiling it would be upscaled
        const uint32_t area = static_cast<uint32_t>(m.width) * m.height;
        if (bestArea && area > bestArea) continue;            // a bigger picture to decode every frame
        // The rates accumulate per resolution, not per area: two shapes can share an area.
        if (m.width != bw || m.height != bh) bestFps = 0;
        bestArea = area;
        bw = m.width;
        bh = m.height;
        bestFps = betterFps(bestFps, m.fps, fps);
    }
    if (!bestArea) return false;   // nothing listed, or nothing big enough: ask for what was wanted
    width = bw;
    height = bh;
    if (bestFps) fps = bestFps;
    return true;
}

// --- The decoded frame --------------------------------------------------------------------------

/// `v` rounded up to a multiple of `to`, the MCU boundary a decoder pads a frame's axis to.
inline uint32_t alignUp(uint32_t v, uint8_t to) { return (v + to - 1u) / to * to; }

/// The row stride in bytes the decoder wrote, or 0 when `outSize` matches no layout its MCU allows.
inline size_t strideFor(uint16_t w, uint16_t h, uint8_t mcuW, uint8_t mcuH, uint32_t outSize) {
    if (!w || !h || !mcuW || !mcuH) return 0;
    if (outSize == static_cast<uint32_t>(w) * h * 3u) return static_cast<size_t>(w) * 3u;
    const uint32_t padW = alignUp(w, mcuW), padH = alignUp(h, mcuH);
    if (outSize == padW * padH * 3u) return static_cast<size_t>(padW) * 3u;
    return 0;
}

/// The source range an output cell covers, the proportional split the Picture effect's ranges use.
inline void cellRange(uint32_t i, uint32_t out, uint32_t src, uint32_t& lo, uint32_t& hi) {
    lo = i * src / out;
    hi = (i + 1u) * src / out;
    if (hi <= lo) hi = lo + 1;   // a source smaller than the output: neighbouring cells share a pixel
}

/// Taps an axis of one cell on the scattered path, so a cell's sum stays well inside 32 bits.
constexpr uint32_t kTapsPerAxis = 4;

/// Each output pixel the linear-light mean of its rectangle, sampled at a stride: a scattered read
/// beats a sequential one here, since the arithmetic dominates and a full walk is 2.7x more of it.
inline void areaAverage(const uint8_t* src, size_t stride, uint16_t w, uint16_t h, uint16_t ow,
                        uint16_t oh, uint8_t* dst) {
    for (uint16_t y = 0; y < oh; y++) {
        uint32_t ylo = 0, yhi = 0;
        cellRange(y, oh, h, ylo, yhi);
        const uint32_t sy = (yhi - ylo + kTapsPerAxis - 1) / kTapsPerAxis;
        for (uint16_t x = 0; x < ow; x++) {
            uint32_t xlo = 0, xhi = 0;
            cellRange(x, ow, w, xlo, xhi);
            const uint32_t sx = (xhi - xlo + kTapsPerAxis - 1) / kTapsPerAxis;
            uint32_t sum[3] = {0, 0, 0}, taps = 0;
            for (uint32_t py = ylo; py < yhi; py += sy) {
                const uint8_t* row = src + static_cast<size_t>(py) * stride;
                for (uint32_t px = xlo; px < xhi; px += sx) {
                    const uint8_t* p = row + static_cast<size_t>(px) * 3;
                    for (int c = 0; c < 3; c++) sum[c] += static_cast<uint32_t>(p[c]) * p[c];
                    taps++;
                }
            }
            uint8_t* out = dst + (static_cast<size_t>(y) * ow + x) * 3;
            for (int c = 0; c < 3; c++) out[c] = static_cast<uint8_t>(isqrt(sum[c] / taps));
        }
    }
}

}   // namespace mm::usbvideo
