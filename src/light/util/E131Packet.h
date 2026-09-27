#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

/// @defgroup e131_packet E1.31 wire format
/// @{
/// The one place the streaming-ACN layout lives, shared by the sender and the receiver.
///
/// A unit test round-trips build against parse, and the shape follows ArtNet's: constants plus two free functions.
///
/// @moreinfo
///
/// ## Three nested layers
///
/// A 126-byte header precedes the channel data, and every multi-byte field is big-endian.
/// The root layer carries the packet identifier and the sender's stable component id.
/// The framing layer carries the source name, the priority, the sequence and the universe.
/// The DMP layer carries the property count and the start code, and the channel data follows it.

constexpr uint16_t E131_PORT = 5568;
constexpr size_t E131_HEADER_SIZE = 126;
constexpr size_t E131_CID_LENGTH = 16;

namespace detail {
inline void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
inline uint16_t getU16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
} // namespace detail

/// Build a data packet; priority and sync address stay at their defaults until a consumer needs them.
inline size_t buildE131Packet(uint8_t* outBuf, uint16_t universe, uint8_t sequence,
                              const uint8_t cid[E131_CID_LENGTH],
                              const uint8_t* data, uint16_t dataLen) {
    const size_t totalLen = E131_HEADER_SIZE + dataLen;
    std::memset(outBuf, 0, E131_HEADER_SIZE);

    // Root layer
    detail::putU16(outBuf + 0, 0x0010);                       // preamble size
    // postamble size stays 0
    std::memcpy(outBuf + 4, "ASC-E1.17\0\0\0", 12);
    detail::putU16(outBuf + 16, static_cast<uint16_t>(0x7000 | (totalLen - 16)));
    outBuf[21] = 0x04;                                        // vector 0x00000004
    std::memcpy(outBuf + 22, cid, E131_CID_LENGTH);

    // Framing layer
    detail::putU16(outBuf + 38, static_cast<uint16_t>(0x7000 | (totalLen - 38)));
    outBuf[43] = 0x02;                                        // vector 0x00000002
    std::memcpy(outBuf + 44, "MoonLight", 9);                 // source name (NUL-padded)
    outBuf[108] = 100;                                        // priority (spec default)
    // sync address stays 0
    outBuf[111] = sequence;
    // options stay 0
    detail::putU16(outBuf + 113, universe);

    // DMP layer
    detail::putU16(outBuf + 115, static_cast<uint16_t>(0x7000 | (totalLen - 115)));
    outBuf[117] = 0x02;                                       // vector: set property
    outBuf[118] = 0xA1;                                       // address & data type
    // first property address stays 0x0000
    outBuf[122] = 0x01;                                       // address increment
    detail::putU16(outBuf + 123, static_cast<uint16_t>(1 + dataLen));
    // start code stays 0x00

    std::memcpy(outBuf + E131_HEADER_SIZE, data, dataLen);
    return totalLen;
}

/// Parse and validate a data packet, zero-copy; priority, sequence and sync are ignored, last write winning.
inline bool parseE131Packet(const uint8_t* pkt, size_t len, uint16_t& universeOut,
                            const uint8_t*& dataOut, uint16_t& dataLenOut) {
    if (!pkt || len < E131_HEADER_SIZE) return false;
    if (std::memcmp(pkt + 4, "ASC-E1.17\0\0\0", 12) != 0) return false;
    if (pkt[18] != 0 || pkt[19] != 0 || pkt[20] != 0 || pkt[21] != 0x04) return false;
    if (pkt[40] != 0 || pkt[41] != 0 || pkt[42] != 0 || pkt[43] != 0x02) return false;
    if (pkt[117] != 0x02) return false;
    if (pkt[125] != 0x00) return false;   // only DMX start code 0 carries light data
    const uint16_t propCount = detail::getU16(pkt + 123);
    if (propCount < 1) return false;
    const uint16_t dataLen = static_cast<uint16_t>(propCount - 1);
    if (dataLen == 0 || dataLen > len - E131_HEADER_SIZE) return false;
    universeOut = detail::getU16(pkt + 113);
    dataOut = pkt + E131_HEADER_SIZE;
    dataLenOut = dataLen;
    return true;
}

/// @}

} // namespace mm
