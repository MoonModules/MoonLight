/// @module FseqFile
/// @also FseqPlayerEffect

/// Pins the FSEQ header/table parser against hand-built bytes: truncation at every offset, and sparse-range placement across a gap between two ranges.

#include "doctest.h"

#include "light/util/FseqFile.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace mm::fseq;

namespace {

void putU16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void putU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void putU24(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 3; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

/// A v1 header: no compression block table, no sparse range table, legacy bytes 20-31 unused.
std::vector<uint8_t> makeV1(uint16_t channelDataOffset, uint32_t channelCount, uint32_t frameCount,
                             uint8_t stepTime) {
    std::vector<uint8_t> b(channelDataOffset, 0);
    std::memcpy(b.data(), "PSEQ", 4);
    putU16(b, 4, channelDataOffset);
    b[6] = 0;   // minor
    b[7] = 1;   // major
    putU32(b, 10, channelCount);
    putU32(b, 14, frameCount);
    b[18] = stepTime;
    return b;
}

/// A v2 header, with an explicit compression block count and sparse range table; `sparse` entries are laid out right after `compressionBlockCount * 8` reserved bytes.
std::vector<uint8_t> makeV2(uint16_t channelDataOffset, uint32_t channelCount, uint32_t frameCount,
                             uint8_t stepTime, uint8_t compressionType, uint8_t compressionBlockCount,
                             const std::vector<SparseRange>& sparse) {
    const size_t sparseTableOffset = 32 + static_cast<size_t>(compressionBlockCount) * 8;
    const size_t minLen = sparseTableOffset + sparse.size() * 6;
    const size_t len = channelDataOffset > minLen ? channelDataOffset : minLen;
    std::vector<uint8_t> b(len, 0);
    std::memcpy(b.data(), "PSEQ", 4);
    putU16(b, 4, channelDataOffset);
    b[6] = 0;   // minor
    b[7] = 2;   // major
    putU32(b, 10, channelCount);
    putU32(b, 14, frameCount);
    b[18] = stepTime;
    // A whole byte each, not nibble-split, matching a real xLights v2 export.
    b[20] = compressionType;
    b[21] = compressionBlockCount;
    b[22] = static_cast<uint8_t>(sparse.size());
    for (size_t i = 0; i < sparse.size(); i++) {
        const size_t at = sparseTableOffset + i * 6;
        putU24(b, at, sparse[i].startChannel);
        putU24(b, at + 3, sparse[i].channelCount);
    }
    return b;
}

} // namespace

TEST_CASE("a valid v1 header parses the fields the player needs") {
    const auto buf = makeV1(/*channelDataOffset=*/32, /*channelCount=*/450, /*frameCount=*/600,
                             /*stepTime=*/25);
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::None);
    CHECK(hdr.channelDataOffset == 32);
    CHECK(hdr.majorVersion == 1);
    CHECK(hdr.channelCount == 450);
    CHECK(hdr.frameCount == 600);
    CHECK(hdr.stepTimeMs == 25);
    CHECK(hdr.compressionType == CompressionType::None);
    CHECK(hdr.sparseRangeCount == 0);
}

TEST_CASE("a v2 header with no compression and no sparse ranges behaves like v1") {
    const auto buf = makeV2(32, 450, 600, 25, /*compressionType=*/0, /*compressionBlockCount=*/0, {});
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::None);
    CHECK(hdr.compressionType == CompressionType::None);
    CHECK(hdr.sparseRangeCount == 0);
}

TEST_CASE("a legacy FSEQ magic is accepted alongside the current PSEQ magic") {
    auto buf = makeV1(32, 10, 1, 25);
    std::memcpy(buf.data(), "FSEQ", 4);
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::None);
}

TEST_CASE("a wrong magic is refused") {
    auto buf = makeV1(32, 10, 1, 25);
    std::memcpy(buf.data(), "ZZZZ", 4);
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::BadMagic);
}

TEST_CASE("an unknown major version is refused") {
    auto buf = makeV1(32, 10, 1, 25);
    buf[7] = 3;
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::UnsupportedVersion);
}

TEST_CASE("zstd compression is its own error, not a generic failure") {
    // Type 1 is zstd: no decoder exists in this build, so it is reported distinctly from a malformed file.
    const auto buf = makeV2(32, 100, 10, 25, /*compressionType=*/1, 0, {});
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::UnsupportedCompression);
    CHECK(hdr.compressionType == CompressionType::Zstd);
}

TEST_CASE("zlib compression parses cleanly, since a ROM decoder exists for it") {
    const auto buf = makeV2(32, 100, 10, 25, /*compressionType=*/2, 0, {});
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::None);
    CHECK(hdr.compressionType == CompressionType::Zlib);
}

TEST_CASE("a reserved compression type byte is treated as unsupported rather than silently accepted") {
    const auto buf = makeV2(32, 100, 10, 25, /*compressionType=*/7, 0, {});
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::UnsupportedCompression);
}

TEST_CASE("sparse ranges land in the header in file order") {
    const std::vector<SparseRange> ranges = {{0, 100}, {500, 50}};
    const auto buf = makeV2(32 + 2 * 6, 150, 10, 25, 0, 0, ranges);
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::None);
    REQUIRE(hdr.sparseRangeCount == 2);
    CHECK(hdr.sparseRanges[0].startChannel == 0);
    CHECK(hdr.sparseRanges[0].channelCount == 100);
    CHECK(hdr.sparseRanges[1].startChannel == 500);
    CHECK(hdr.sparseRanges[1].channelCount == 50);
}

TEST_CASE("a compression block table displaces the sparse range table, and parsing follows it") {
    // compressionBlockCount=3 means 24 reserved bytes between the fixed header and the sparse table.
    const std::vector<SparseRange> ranges = {{10, 5}};
    const auto buf = makeV2(32 + 3 * 8 + 6, 50, 10, 25, 2, /*compressionBlockCount=*/3, ranges);
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::None);
    CHECK(hdr.compressionBlockCount == 3);
    REQUIRE(hdr.sparseRangeCount == 1);
    CHECK(hdr.sparseRanges[0].startChannel == 10);
    CHECK(hdr.sparseRanges[0].channelCount == 5);
}

TEST_CASE("a real xLights v2 export's header parses with the right block count") {
    // Pinned against a real export that once misread compressionBlockCount as 336 instead of 21; padded to 200 bytes since parseHeader needs the whole compression-block table.
    std::vector<uint8_t> head(200, 0);
    const uint8_t fixed[32] = {
        'P', 'S', 'E', 'Q',
        0xcf, 0x00,            // channelDataOffset = 207
        0x00, 0x02,            // minor=0, major=2
        0xc8, 0x00,            // variableHeaderOffset = 200
        0xc0, 0x5d, 0x00, 0x00, // channelCount = 24000
        0x34, 0x08, 0x00, 0x00, // frameCount = 2100
        0x1d,                  // stepTimeMs = 29
        0x00,
        0x02,                  // compressionType = 2 (zlib)
        0x15,                  // compressionBlockCount = 21
        0x00,                  // sparseRangeCount = 0
        0x00,
        0, 0, 0, 0, 0, 0, 0, 0 // unique id, unused
    };
    std::memcpy(head.data(), fixed, sizeof(fixed));
    FseqHeader hdr;
    CHECK(parseHeader(head.data(), head.size(), hdr) == ParseError::None);
    CHECK(hdr.compressionType == CompressionType::Zlib);
    CHECK(hdr.compressionBlockCount == 21);
    CHECK(hdr.channelCount == 24000);
    CHECK(hdr.frameCount == 2100);
    CHECK(hdr.channelDataOffset == 207);
    CHECK(32 + static_cast<int>(hdr.compressionBlockCount) * 8 == 200);   // == variableHeaderOffset
}

TEST_CASE("more sparse ranges than the cap are clipped, not overflowed, and reported non-fatally") {
    std::vector<SparseRange> ranges;
    for (uint8_t i = 0; i < kMaxSparseRanges + 5; i++) ranges.push_back({static_cast<uint32_t>(i * 10), 1});
    const auto buf = makeV2(32 + static_cast<uint16_t>(ranges.size() * 6), 300, 10, 25, 0, 0, ranges);
    FseqHeader hdr;
    // The buffer only reserves room for the clipped count: parseHeader must not read past that.
    std::vector<uint8_t> clippedBuf(buf.begin(), buf.begin() + static_cast<long>(32 + static_cast<size_t>(kMaxSparseRanges) * 6));
    CHECK(parseHeader(clippedBuf.data(), clippedBuf.size(), hdr) == ParseError::TooManySparseRanges);
    CHECK(hdr.sparseRangeCount == kMaxSparseRanges);
}

TEST_CASE("a buffer truncated at every interesting point reports Truncated rather than reading past it") {
    const std::vector<SparseRange> ranges = {{0, 100}, {500, 50}};
    const auto full = makeV2(32 + 2 * 6 + 10, 150, 10, 25, 0, 0, ranges);
    for (size_t n : {size_t{0}, size_t{1}, size_t{31}, size_t{32}, size_t{37}, size_t{40}}) {
        CAPTURE(n);
        FseqHeader hdr;
        const auto err = parseHeader(full.data(), n, hdr);
        if (n < 32) { CHECK(err == ParseError::Truncated); continue; }
        // n==32..43 is mid-sparse-table (table spans [32, 44)); every one of those must truncate.
        CHECK(err == ParseError::Truncated);
    }
    // One byte short of the full sparse table still truncates; the exact end succeeds.
    FseqHeader hdrShort;
    CHECK(parseHeader(full.data(), 32 + 2 * 6 - 1, hdrShort) == ParseError::Truncated);
    FseqHeader hdrExact;
    CHECK(parseHeader(full.data(), 32 + 2 * 6, hdrExact) == ParseError::None);
}

TEST_CASE("channelDataOffset overlapping the v1 fixed header is refused") {
    auto buf = makeV1(/*channelDataOffset=*/32, 10, 1, 25);
    putU16(buf, 4, 20);   // claims frame 0 starts inside the 32-byte fixed header
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::Truncated);
}

TEST_CASE("channelDataOffset overlapping a v2 file's compression/sparse tables is refused") {
    const std::vector<SparseRange> ranges = {{0, 100}};
    auto buf = makeV2(32 + 6, 150, 10, 25, 0, 0, ranges);   // tables end at 32+6=38
    putU16(buf, 4, 35);   // claims frame 0 starts inside the sparse range table
    FseqHeader hdr;
    CHECK(parseHeader(buf.data(), buf.size(), hdr) == ParseError::Truncated);
}

TEST_CASE("frameByteOffset uses size_t arithmetic, so a large show does not overflow a 32-bit accumulator") {
    FseqHeader hdr;
    hdr.channelDataOffset = 32;
    hdr.channelCount = 300000;   // a large 3D voxel show
    CHECK(frameByteOffset(hdr, 0) == 32);
    CHECK(frameByteOffset(hdr, 1) == 32u + 300000u);
    // frameIndex * channelCount alone exceeds UINT32_MAX by frame ~14310: size_t keeps this exact.
    const uint32_t bigFrame = 20000;
    const size_t expected = size_t{32} + size_t{bigFrame} * size_t{300000};
    CHECK(expected > 0xFFFFFFFFULL);
    CHECK(frameByteOffset(hdr, bigFrame) == expected);
}

TEST_CASE("validateBlockRange accepts a well-formed entry and computes its decompressed length") {
    uint32_t frameCount = 0;
    size_t decompLen = 0;
    CHECK(validateBlockRange(/*blockFirstFrame=*/10, /*frameCount=*/100, /*hasNext=*/true,
                              /*nextFirstFrame=*/40, /*channelCount=*/50, frameCount, decompLen));
    CHECK(frameCount == 30);
    CHECK(decompLen == 30u * 50u);

    // The last entry has no next: its span runs to the file's own frameCount.
    CHECK(validateBlockRange(40, 100, /*hasNext=*/false, 0, 50, frameCount, decompLen));
    CHECK(frameCount == 60);
}

TEST_CASE("validateBlockRange refuses a block table that does not strictly advance") {
    uint32_t frameCount = 0;
    size_t decompLen = 0;
    // The next entry equal to this one would underflow the frame-count subtraction.
    CHECK_FALSE(validateBlockRange(40, 100, /*hasNext=*/true, /*nextFirstFrame=*/40, 50, frameCount, decompLen));
    // The next entry going backwards is refused the same way.
    CHECK_FALSE(validateBlockRange(40, 100, /*hasNext=*/true, /*nextFirstFrame=*/10, 50, frameCount, decompLen));
}

TEST_CASE("validateBlockRange refuses a block starting at or past the file's own frameCount") {
    uint32_t frameCount = 0;
    size_t decompLen = 0;
    CHECK_FALSE(validateBlockRange(100, 100, false, 0, 50, frameCount, decompLen));
    CHECK_FALSE(validateBlockRange(150, 100, false, 0, 50, frameCount, decompLen));
}

// validateBlockRange's overflow guard only trips on a 32-bit size_t (two uint32_t values can't multiply past 64-bit); only real ESP32 hardware can exercise that branch.

TEST_CASE("expandSparseFrame places each range at its absolute channel offset, leaving gaps alone") {
    FseqHeader hdr;
    hdr.sparseRangeCount = 2;
    hdr.sparseRanges[0] = {0, 3};     // channels 0..2
    hdr.sparseRanges[1] = {10, 2};    // channels 10..11, a gap at 3..9
    const uint8_t raw[5] = {1, 2, 3, 9, 8};   // ranges concatenated with no padding
    uint8_t out[16];
    std::memset(out, 0xAA, sizeof(out));      // a sentinel: gaps must stay untouched by this call
    const size_t consumed = expandSparseFrame(hdr, raw, sizeof(raw), out, sizeof(out));
    CHECK(consumed == 5);
    CHECK(out[0] == 1); CHECK(out[1] == 2); CHECK(out[2] == 3);
    CHECK(out[3] == 0xAA);   // the gap: caller's own zeroing (or lack of it) is preserved
    CHECK(out[10] == 9); CHECK(out[11] == 8);
}

TEST_CASE("expandSparseFrame clips to the output capacity rather than overflowing it") {
    FseqHeader hdr;
    hdr.sparseRangeCount = 1;
    hdr.sparseRanges[0] = {14, 4};   // would write channels 14..17 into a 16-byte buffer
    const uint8_t raw[4] = {1, 2, 3, 4};
    uint8_t out[16];
    std::memset(out, 0, sizeof(out));
    expandSparseFrame(hdr, raw, sizeof(raw), out, sizeof(out));
    CHECK(out[14] == 1);
    CHECK(out[15] == 2);   // bytes 3 and 4 fell outside outCap and were dropped, not overrun
}
