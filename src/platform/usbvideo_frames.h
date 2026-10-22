#pragma once

/// The hardware-free half of USB video capture: the UVC payload headers, the mode choice and the shrink onto a picture.

#include "core/util/AreaSample.h"   // cellSpan, areaMean: the shrink every sampler shares

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

/// Whether the flags are a header's: end of header set, no error, no reserved bit.
inline bool headerFlagsOk(uint8_t flags) { return (flags & kEoh) && !(flags & (kErr | kReserved)); }

/// Whether the SCR's reserved top 5 bits are zero, the strongest single check a header carries.
inline bool headerScrOk(const uint8_t* h, uint8_t flags, uint8_t len) {
    return !(flags & kScr) || !(h[len - 1] & 0xF8);
}

/// Whether the PTS matches the frame's first header, which is constant across one frame's payloads.
inline bool headerPtsOk(const uint8_t* h, uint8_t flags, const uint8_t* pts) {
    return !pts || !(flags & kPts) || std::memcmp(h + 2, pts, 4) == 0;
}

/// Whether `h` is a payload header of exactly `want` bytes; `pts` pins it to the frame's first one.
inline bool isPayloadHeader(const uint8_t* h, size_t avail, size_t want, const uint8_t* pts) {
    if (avail < 2) return false;
    const uint8_t len = h[0], flags = h[1];
    if (len != want || len != headerLenFor(flags) || avail < len) return false;
    return headerFlagsOk(flags) && headerScrOk(h, flags, len) && headerPtsOk(h, flags, pts);
}

/// Whether headers of length `hl` recur after `first`, which is what rules out a chance match.
inline bool confirmedRun(const uint8_t* d, size_t len, size_t first, size_t hl) {
    const uint8_t* pts = (d[first + 1] & kPts) ? d + first + 2 : nullptr;
    const size_t stride = first + hl;
    size_t seen = 0;
    for (size_t at = first + stride; at + hl <= len && seen < kConfirmStrides; at += stride, seen++)
        if (!isPayloadHeader(d + at, len - at, hl, pts)) return false;
    return seen > 0 || first + stride + hl > len;   // a frame too short to confirm is taken as it is
}

/// The offset of the first confirmed header of length `hl`, or 0 when the frame carries none.
inline size_t findHeader(const uint8_t* d, size_t len, size_t hl) {
    for (size_t i = kBulkMps - hl; i + hl <= len; i += kBulkMps)
        if (isPayloadHeader(d + i, len - i, hl, nullptr) && confirmedRun(d, len, i, hl)) return i;
    return 0;
}

/// Close the gaps the headers from `hdr` on leave, answering the compacted length.
inline size_t compactFrom(uint8_t* d, size_t len, size_t hdr, size_t hl) {
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

/// Drops the payload headers a bulk frame still carries, answering the compacted length.
inline size_t stripPayloadHeaders(uint8_t* d, size_t len) {
    static constexpr size_t kLens[] = {12, 8, 6, 2};   // longest first: the more fields, the surer the match
    for (size_t hl : kLens) {
        const size_t hdr = findHeader(d, len, hl);
        if (hdr) return compactFrom(d, len, hdr, hl);
    }
    return len;
}

// --- Choosing a mode ----------------------------------------------------------------------------

/// UVC states a rate as a period in 100 ns ticks. Rounded, so 59.94 fps reads as 60 rather than 59.
inline uint8_t fpsFrom(uint32_t interval) {
    constexpr uint32_t kTicksPerSecond = 10000000;
    return interval ? static_cast<uint8_t>((kTicksPerSecond + interval / 2) / interval) : 0;
}

/// The rates a continuous range is asked for, since such a device names no rate of its own and a picture wants a familiar one.
constexpr uint8_t kOfferedRates[] = {60, 50, 30, 25, 24, 15, 10, 5};

/// The frame interval, in 100 ns ticks, that asks for `fps`.
inline uint32_t intervalFor(uint8_t fps) { return fps ? 10000000u / fps : 0; }

/// The rung of a continuous range that holds `fps`, or 0 where the range does not reach it. Rounded away from `min`, so the rate lands at or under the one asked for rather than over it.
inline uint32_t rungFor(uint8_t fps, uint32_t min, uint32_t max, uint32_t step) {
    const uint32_t want = intervalFor(fps);
    if (!want || !min || max < min || want < min || want > max) return 0;
    if (!step) return want;   // a range with no step reaches every interval in it
    const uint32_t rung = min + ((want - min + step - 1) / step) * step;
    return rung <= max ? rung : max;
}

/// Of two advertised rates, the one to ask for: the fastest at or below `wanted`, else the slowest.
inline uint8_t betterFps(uint8_t have, uint8_t row, uint8_t wanted) {
    if (!have) return row;
    const bool haveOk = have <= wanted, rowOk = row <= wanted;
    if (haveOk != rowOk) return haveOk ? have : row;
    if (haveOk) return row > have ? row : have;
    return row < have ? row : have;
}

/// Whether a mode covers the ceiling and is no bigger to decode than the best so far.
inline bool modeFits(const Mode& m, uint16_t width, uint16_t height, uint32_t area, uint32_t bestArea) {
    if (m.width < width || m.height < height) return false;   // below the ceiling it would be upscaled
    return !bestArea || area <= bestArea;                     // a bigger picture to decode every frame
}

/// The smallest mode at or above the ceiling, at the rate asked for, so decode and shrink stay cheap.
inline bool pickMode(const Mode* modes, size_t count, uint16_t& width, uint16_t& height, uint8_t& fps) {
    uint32_t bestArea = 0;
    uint16_t bw = 0, bh = 0;
    uint8_t bestFps = 0;
    for (size_t i = 0; i < count; i++) {
        const Mode& m = modes[i];
        const uint32_t area = static_cast<uint32_t>(m.width) * m.height;
        if (!modeFits(m, width, height, area, bestArea)) continue;
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

/// A JPEG's size and the MCU its luma sampling pads to: 16x16 for 4:2:0, 16x8 for 4:2:2, 8x8 for 4:4:4.
struct JpegShape {
    uint16_t width = 0;   ///< pixels across, from the frame header
    uint16_t height = 0;  ///< pixels down
    uint8_t mcuW = 8;     ///< the MCU's width
    uint8_t mcuH = 8;     ///< and its height
};

/// The shape from one frame header's fields, `s` at its precision byte and `seg` its length; false when too short to hold one component.
inline bool readFrameHeader(const uint8_t* s, size_t seg, JpegShape& out) {
    if (seg < 11) return false;   // precision, height, width, count and one component
    out.height = static_cast<uint16_t>((s[1] << 8) | s[2]);
    out.width = static_cast<uint16_t>((s[3] << 8) | s[4]);
    out.mcuW = static_cast<uint8_t>(8 * (s[7] >> 4));     // the first component's horizontal sampling
    out.mcuH = static_cast<uint8_t>(8 * (s[7] & 0x0F));   // and its vertical
    return out.width && out.height && out.mcuW && out.mcuH;
}

/// Whether `d` opens with a JPEG's start-of-image marker.
inline bool startsImage(const uint8_t* d, size_t len) { return len >= 4 && d[0] == 0xFF && d[1] == 0xD8; }
/// Whether a marker is a baseline, extended or progressive frame header (SOF0 to SOF2).
inline bool isFrameHeader(uint8_t marker) { return marker >= 0xC0 && marker <= 0xC2; }

/// The shape from a frame header, walking the segments before it; false for anything that is not a JPEG with one before its scan.
inline bool jpegShape(const uint8_t* d, size_t len, JpegShape& out) {
    if (!startsImage(d, len)) return false;
    size_t i = 2;
    while (i + 4 <= len) {
        if (d[i] != 0xFF) return false;
        const uint8_t marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }   // a fill byte before the marker
        const size_t seg = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];   // the segment's length, its own two bytes included
        if (seg < 2 || i + 2 + seg > len || marker == 0xDA) return false;   // cut short, or the scan began without a frame header
        if (isFrameHeader(marker)) return readFrameHeader(d + i + 4, seg, out);
        i += 2 + seg;
    }
    return false;
}

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

/// Each output pixel the mean of its rectangle, through the shared sampler so one fix reaches both paths.
inline void areaAverage(const uint8_t* src, size_t stride, uint16_t w, uint16_t h, const Sink& sink) {
    for (uint16_t y = 0; y < sink.height; y++) {
        const Span ry = cellSpan(y, sink.height, h);
        for (uint16_t x = 0; x < sink.width; x++) {
            const RGB mean = areaMean(src, stride, cellSpan(x, sink.width, w), ry, sink.floor);
            uint8_t* out = sink.rgb + (static_cast<size_t>(y) * sink.width + x) * 3;
            out[0] = mean.r;
            out[1] = mean.g;
            out[2] = mean.b;
        }
    }
}

}   // namespace mm::usbvideo
