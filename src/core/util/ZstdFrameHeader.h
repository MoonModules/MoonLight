#pragma once

#include <cstddef>
#include <cstdint>

/// A minimal zstd frame header for a headerless block of known size, built fresh because FSEQ's zstd blocks omit the frame header FPP/xLights' own writer leaves out.
///
/// @technical
///
/// `Single_Segment_flag` set means window size equals content size, so no separate window descriptor byte is needed, the smallest header a known-size block can carry.
///
/// Its content-size field is 1 byte for 0-255 (Single_Segment only), 2 bytes (+256 offset) for 256-65791, or 4 or 8 bytes beyond that, per RFC 8878 Table 4.

namespace mm {

/// Bytes a header can ever need: magic(4) + descriptor(1) + content-size field(up to 8).
constexpr size_t kMaxZstdFrameHeaderBytes = 13;

/// Writes a minimal valid zstd frame header for `contentSize` into `out` (>= kMaxZstdFrameHeaderBytes); returns bytes written.
inline size_t buildZstdFrameHeader(uint64_t contentSize, uint8_t* out) {
    out[0] = 0x28; out[1] = 0xB5; out[2] = 0x2F; out[3] = 0xFD;   // RFC 8878 magic, little-endian on the wire

    // Three sequential, non-nested checks: each re-derives fieldValue from contentSize, so the last one to fire wins.
    uint8_t fcsFlag = 0;
    uint64_t fieldValue = contentSize;
    size_t fieldWidth = 1;
    if (contentSize > 255) { fcsFlag = 1; fieldValue = contentSize - 256; fieldWidth = 2; }
    if (contentSize > 65791) { fcsFlag = 2; fieldValue = contentSize; fieldWidth = 4; }
    if (contentSize > 0xFFFFFFFFu) { fcsFlag = 3; fieldValue = contentSize; fieldWidth = 8; }

    // Single_Segment_flag set (bit 5): window size equals content size, so no window descriptor byte follows.
    out[4] = static_cast<uint8_t>((fcsFlag << 6) | (1 << 5));
    for (size_t i = 0; i < fieldWidth; i++) out[5 + i] = static_cast<uint8_t>(fieldValue >> (8 * i));
    return 5 + fieldWidth;
}

} // namespace mm
