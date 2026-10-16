#pragma once

/// Parses an FSEQ sequence file's fixed header, compression flags and sparse-range table.
///
/// FSEQ is the Falcon Player/xLights animation file format: a flat, per-frame byte stream with no pixel/RGB semantics of its own.
/// This header is pure and platform-agnostic: it reads caller-supplied byte buffers only, no `platform::` calls, no I/O.
///
/// @technical
///
/// ## Prior art
///
/// FPP's `FSEQ_Sequence_File_Format.txt` (github.com/FalconChristmas/fpp/blob/master/docs/FSEQ_Sequence_File_Format.txt).
/// Cryptkeeper/fseq-file-format (github.com/Cryptkeeper/fseq-file-format) is an example-driven third-party write-up of the same format.
/// `core/util/FirmwareImage.h` is the structural template this header follows: a truncation-safe parser over a caller-supplied buffer, no I/O of its own.
///
/// ## Mapping bytes onto pixels is the caller's job
///
/// FSEQ carries no pixel/RGB semantics of its own, just a flat numbered byte stream per frame.
/// Mapping a frame's bytes onto a light buffer is `FseqPlayerEffect`'s job, not this parser's.
///
/// ## Compression
///
/// A v2 file may compress channel data per-frame.
/// `parseHeader` reports the type a file declares, zlib (2) or zstd (1) alike; whether a build can decode it is the caller's judgment, not this file's.
/// A caller owns the live decompressor state across frames; this parser never touches it.
///
/// ## Sparse ranges
///
/// A v2 file may carry only some of a show's channels.
/// The sparse range table lists which absolute channel each contiguous slice of a frame's raw bytes belongs to; `expandSparseFrame` does that placement.
/// A frame with no sparse ranges (the common case) maps 1:1 onto the buffer in channel order and never needs that function.
///
/// ## Why bytes 20 and 21 are read whole, not nibble-split
///
/// An earlier version of this parser packed the compression block count across the high nibble of byte 20 and all of byte 21.
/// A real xLights export disproved that.
/// `compressionBlockCount * 8 + 32` only lands on that file's own `variableHeaderOffset` when byte 20 and byte 21 are each read as a whole, independent byte.
///
/// ## `parseHeader` never reads past `len`
///
/// A buffer too short for a field or table the header claims to have returns `Truncated`.
/// `out` is left however far parsing got, the same truncation-safe contract as `FirmwareImage.h`'s `identify()`.
///
/// ## `parseHeader`'s two non-fatal errors
///
/// `TooManySparseRanges` and `UnsupportedCompression` are returned only once `out` is otherwise fully populated (clipped to `kMaxSparseRanges` in the first case).
/// A caller may still use the rest of the header, e.g. to show a status line, rather than discard it outright.
///
/// ## Why `frameByteOffset` returns `size_t`, not `uint32_t`
///
/// `frameIndex * channelCount` overflows a 32-bit accumulator well within a real show's frame count once channel count is in the thousands.
///
/// ## The layout `expandSparseFrame`'s `rawFrame` and `out` each assume
///
/// `rawFrame` holds `hdr.sparseRangeCount` ranges back-to-back with no padding between them.
/// Each range's bytes land at `out[startChannel .. startChannel+channelCount)`, clipped to `outCap`.
/// Gaps between ranges are left untouched, so `out` must already be zeroed by the caller.
/// That is one `std::memset` per frame, done once, rather than this function re-zeroing the parts it doesn't write on every call.

#include <cstddef>
#include <cstdint>

namespace mm::fseq {

/// Why a header failed to parse, or (for the non-fatal cases) why it was accepted with a caveat.
enum class ParseError : uint8_t {
    None,
    BadMagic,                 ///< first 4 bytes are not "PSEQ"/"FSEQ"
    UnsupportedVersion,       ///< major version is neither 1 nor 2
    UnsupportedCompression,   ///< the compression-type byte names no recognized value, not a malformed file
    TooManySparseRanges,      ///< non-fatal: the table was clipped to kMaxSparseRanges and parsed anyway
    Truncated,                ///< buffer ends before a field/table this header claims to have
};

/// A file's declared compression, read from header byte 20 (v2 only).
enum class CompressionType : uint8_t { None = 0, Zstd = 1, Zlib = 2 };

/// One contiguous slice of a sparse frame, placed at an absolute channel offset by `expandSparseFrame`.
struct SparseRange {
    uint32_t startChannel = 0;   ///< the absolute (unsparsed) channel this slice starts at
    uint32_t channelCount = 0;   ///< how many raw bytes belong to this slice
};

/// Bound on parsed sparse ranges: large enough for any real show, small enough to hold inline.
constexpr uint8_t kMaxSparseRanges = 32;

/// The fields of an FSEQ header this player actually needs; metadata blocks are not retained.
struct FseqHeader {
    uint16_t channelDataOffset = 0;      ///< byte offset of frame 0 within the file
    uint8_t  minorVersion = 0;           ///< the file's minor version, carried for diagnostics only
    uint8_t  majorVersion = 0;           ///< 1 or 2; anything else is UnsupportedVersion
    uint32_t channelCount = 0;           ///< bytes per frame (sum of sparse-range counts, if any)
    uint32_t frameCount = 0;             ///< total frames in the file
    uint8_t  stepTimeMs = 0;             ///< milliseconds per frame, the player's tick pacing
    CompressionType compressionType = CompressionType::None;   ///< None, Zlib or Zstd, as declared; usability is the caller's call
    uint8_t  compressionBlockCount = 0;  ///< v2 only; used to locate the sparse range table, not for seeking
    uint8_t  sparseRangeCount = 0;       ///< entries actually populated in sparseRanges (<= kMaxSparseRanges)
    SparseRange sparseRanges[kMaxSparseRanges] = {};   ///< the sparse range table, if sparseRangeCount > 0
};

namespace detail {
inline uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint32_t rdU24(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}
} // namespace detail

/// Whether `buf` opens with the FSEQ magic, legacy "FSEQ" accepted alongside current "PSEQ".
inline bool isValidMagic(const uint8_t* buf) {
    return (buf[0] == 'P' || buf[0] == 'F') && buf[1] == 'S' && buf[2] == 'E' && buf[3] == 'Q';
}

/// Parse the v2-only compression type, block count and sparse-range table into `out`; `buf`/`len` are the whole header buffer, unchanged from `parseHeader`.
inline ParseError parseV2Tail(const uint8_t* buf, size_t len, FseqHeader& out) {
    // Byte 20 is the whole compression-type byte; byte 21 is the whole block count (see @technical above).
    const uint8_t compressionByte = buf[20];
    out.compressionBlockCount = buf[21];
    const uint8_t sparseCountRaw = buf[22];

    const size_t sparseTableOffset = 32 + static_cast<size_t>(out.compressionBlockCount) * 8;
    const uint8_t sparseCount = sparseCountRaw > kMaxSparseRanges ? kMaxSparseRanges : sparseCountRaw;
    const size_t sparseTableEnd = sparseTableOffset + static_cast<size_t>(sparseCount) * 6;
    if (len < sparseTableEnd) return ParseError::Truncated;

    for (uint8_t i = 0; i < sparseCount; i++) {
        const uint8_t* entry = buf + sparseTableOffset + static_cast<size_t>(i) * 6;
        out.sparseRanges[i].startChannel = detail::rdU24(entry);
        out.sparseRanges[i].channelCount = detail::rdU24(entry + 3);
    }
    out.sparseRangeCount = sparseCount;

    switch (compressionByte) {
        case 0: out.compressionType = CompressionType::None; break;
        case 1: out.compressionType = CompressionType::Zstd; break;
        case 2: out.compressionType = CompressionType::Zlib; break;
        default: return ParseError::UnsupportedCompression;   // a genuinely reserved byte value
    }

    if (sparseCountRaw > kMaxSparseRanges) return ParseError::TooManySparseRanges;
    return ParseError::None;
}

/// Parse the fixed header, compression flags and (v2) sparse-range table out of `buf`.
inline ParseError parseHeader(const uint8_t* buf, size_t len, FseqHeader& out) {
    out = FseqHeader{};
    if (len < 32) return ParseError::Truncated;
    if (!isValidMagic(buf)) return ParseError::BadMagic;

    out.channelDataOffset = detail::rdU16(buf + 4);
    out.minorVersion = buf[6];
    out.majorVersion = buf[7];
    out.channelCount = detail::rdU32(buf + 10);
    out.frameCount = detail::rdU32(buf + 14);
    out.stepTimeMs = buf[18];

    if (out.majorVersion != 1 && out.majorVersion != 2) return ParseError::UnsupportedVersion;
    if (out.majorVersion == 1) return ParseError::None;   // v1 has no compression/sparse fields
    return parseV2Tail(buf, len, out);
}

/// The byte offset of frame `frameIndex`'s channel data within the file.
inline size_t frameByteOffset(const FseqHeader& hdr, uint32_t frameIndex) {
    return static_cast<size_t>(hdr.channelDataOffset) +
           static_cast<size_t>(frameIndex) * static_cast<size_t>(hdr.channelCount);
}

/// Place a sparse frame's concatenated raw bytes at their absolute channel offsets in `out`.
inline size_t expandSparseFrame(const FseqHeader& hdr, const uint8_t* rawFrame, size_t rawLen,
                                 uint8_t* out, size_t outCap) {
    size_t rawPos = 0;
    for (uint8_t i = 0; i < hdr.sparseRangeCount && rawPos < rawLen; i++) {
        const SparseRange& r = hdr.sparseRanges[i];
        size_t avail = rawLen - rawPos;
        size_t n = static_cast<size_t>(r.channelCount) < avail ? r.channelCount : avail;
        if (r.startChannel < outCap) {
            size_t room = outCap - r.startChannel;
            size_t copy = n < room ? n : room;
            for (size_t b = 0; b < copy; b++) out[r.startChannel + b] = rawFrame[rawPos + b];
        }
        rawPos += n;
    }
    return rawPos;
}

} // namespace mm::fseq
