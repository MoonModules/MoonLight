/// @module ZstdFrameHeader

#include "doctest.h"
#include "core/util/ZstdFrameHeader.h"

using namespace mm;

// Content size 100 fits the 1-byte field (Single_Segment, FCS_flag 0): magic + descriptor(0x20) + one byte.
TEST_CASE("ZstdFrameHeader: a content size under 256 takes a 1-byte field") {
    uint8_t out[kMaxZstdFrameHeaderBytes];
    const size_t n = buildZstdFrameHeader(100, out);
    REQUIRE(n == 6);
    CHECK(out[0] == 0x28); CHECK(out[1] == 0xB5); CHECK(out[2] == 0x2F); CHECK(out[3] == 0xFD);
    CHECK(out[4] == 0x20);   // FCS_flag 0 << 6 | Single_Segment_flag (bit 5)
    CHECK(out[5] == 100);
}

// Content size 300 takes the 2-byte field (FCS_flag 1), stored as (300-256)=44, little-endian.
TEST_CASE("ZstdFrameHeader: a content size from 256 to 65791 takes a 2-byte field, offset by 256") {
    uint8_t out[kMaxZstdFrameHeaderBytes];
    const size_t n = buildZstdFrameHeader(300, out);
    REQUIRE(n == 7);
    CHECK(out[4] == 0x60);   // FCS_flag 1 << 6 | Single_Segment_flag
    CHECK(out[5] == 44); CHECK(out[6] == 0);
}

// Content size 70000 exceeds the 2-byte field's range (max 65791), so it takes the 4-byte field (FCS_flag 2).
TEST_CASE("ZstdFrameHeader: a content size over 65791 takes a 4-byte field") {
    uint8_t out[kMaxZstdFrameHeaderBytes];
    const size_t n = buildZstdFrameHeader(70000, out);
    REQUIRE(n == 9);
    CHECK(out[4] == 0xA0);   // FCS_flag 2 << 6 | Single_Segment_flag
    CHECK(out[5] == 112); CHECK(out[6] == 17); CHECK(out[7] == 1); CHECK(out[8] == 0);   // 70000 = 0x00011170, little-endian
}

// A content size past 32 bits takes the 8-byte field (FCS_flag 3), the widest this format carries.
TEST_CASE("ZstdFrameHeader: a content size over 32 bits takes an 8-byte field") {
    uint8_t out[kMaxZstdFrameHeaderBytes];
    const uint64_t big = 0x1'0000'0000ull;   // 2^32
    const size_t n = buildZstdFrameHeader(big, out);
    REQUIRE(n == 13);
    CHECK(out[4] == 0xE0);   // FCS_flag 3 << 6 | Single_Segment_flag
    for (int i = 0; i < 4; i++) CHECK(out[5 + i] == 0);
    CHECK(out[9] == 1);   // byte 4 of the little-endian u64: 2^32 = 0x1_00000000
    for (int i = 10; i < 13; i++) CHECK(out[i] == 0);
}

// Never writes more than kMaxZstdFrameHeaderBytes, whatever the content size: the caller's buffer bound.
TEST_CASE("ZstdFrameHeader: the widest header never exceeds kMaxZstdFrameHeaderBytes") {
    uint8_t out[kMaxZstdFrameHeaderBytes];
    const size_t n = buildZstdFrameHeader(0xFFFF'FFFF'FFFF'FFFFull, out);
    CHECK(n == kMaxZstdFrameHeaderBytes);
}
