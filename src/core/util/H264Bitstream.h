#pragma once
/// Reading an Annex B H.264 stream: where its NAL units and its frames begin.
/// Domain-neutral, because both the light domain's RTP packetiser and the platform layer's encoder reader answer the same question of the same bytes.
/// @technical
/// ## Where one frame ends and the next begins
/// An Annex B stream carries no frame delimiter, so the boundary is inferred from the slice header.
/// `first_mb_in_slice` is the first exp-Golomb value in it, and a leading 1 bit encodes zero, which marks the first slice of a picture.
/// A picture split into several slices therefore opens once, at the slice sitting at macroblock zero.
/// Parameter sets and SEI precede the frame they describe, so they ride with the frame that follows rather than closing the one before.
/// An access unit therefore starts at the first of those introducing NALs rather than at the slice, and a decoder receives the sets that describe a picture together with it.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mm::h264 {

/// No offset, since zero is a valid one.
static constexpr size_t kNoOffset = static_cast<size_t>(-1);

/// The bytes of Annex B start code at `p`, 3 or 4, or 0 where a NAL begins elsewhere.
inline size_t startCodeLen(const uint8_t* p, size_t len) {
    if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (len >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}

/// True where the NAL header at `nal` opens a new access unit: a VCL slice whose first macroblock is zero.
inline bool opensAccessUnit(const uint8_t* nal, size_t len) {
    if (!nal || len < 2) return false;
    const uint8_t type = static_cast<uint8_t>(nal[0] & 0x1F);
    if (type != 1 && type != 5) return false;      // a non-VCL NAL never opens a frame
    return (nal[1] & 0x80) != 0;                   // first_mb_in_slice == 0
}

/// True for a NAL that introduces the frame after it: a parameter set, SEI, or access unit delimiter.
inline bool precedesAccessUnit(const uint8_t* nal, size_t len) {
    if (!nal || len < 1) return false;
    const uint8_t type = static_cast<uint8_t>(nal[0] & 0x1F);
    return type == 6 || type == 7 || type == 8 || type == 9;   // SEI, SPS, PPS, AUD
}

/// Every access unit start in an Annex B buffer, appended to `starts` in order, the last marking the tail still arriving.
inline void findAccessUnits(const uint8_t* buf, size_t len, std::vector<size_t>* starts) {
    if (!buf || !starts) return;
    size_t scan = 0;
    size_t leading = kNoOffset;    // the first introducing NAL since the last frame began
    while (scan + 3 < len) {
        const size_t sc = startCodeLen(buf + scan, len - scan);
        if (sc == 0) { scan++; continue; }
        const size_t hdr = scan + sc;
        if (hdr + 1 >= len) break;                 // the slice header has yet to arrive
        if (precedesAccessUnit(buf + hdr, len - hdr)) {
            if (leading == kNoOffset) leading = scan;      // the frame starts HERE, not at its slice
        } else if (opensAccessUnit(buf + hdr, len - hdr)) {
            starts->push_back(leading == kNoOffset ? scan : leading);
            leading = kNoOffset;
        } else {
            leading = kNoOffset;   // a non-VCL NAL that introduces nothing breaks the run
        }
        scan = hdr + 1;
    }
}

/// True where the access unit at `buf` carries an IDR, which a client decodes from.
inline bool hasKeyframe(const uint8_t* buf, size_t len) {
    if (!buf) return false;
    size_t scan = 0;
    while (scan + 3 < len) {
        const size_t sc = startCodeLen(buf + scan, len - scan);
        if (sc == 0) { scan++; continue; }
        const size_t hdr = scan + sc;
        if (hdr >= len) break;
        if ((buf[hdr] & 0x1F) == 5) return true;
        scan = hdr + 1;
    }
    return false;
}

}  // namespace mm::h264
