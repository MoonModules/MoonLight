#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

/// @defgroup ddp_packet DDP wire format
/// @{
/// The one place the Distributed Display Protocol layout lives, shared by the sender and the receiver.
///
/// @moreinfo
///
/// ## The high-throughput choice
///
/// A short header and a large payload carry 480 lights a packet against ArtNet's 170.
/// Per-packet cost dominates the wire time, measured around 280 microseconds over Ethernet and 1140 over WiFi.
///
/// ## The packet
///
/// A ten-byte header precedes the data, and the multi-byte fields are big-endian.
/// It carries flags with a version and a push bit, a sequence, a data type, a destination, then the offset and length.
///
/// ## Validation is deliberately thin
///
/// The two-bit version is the only magic, so a stray datagram can parse with a garbage offset.
/// The receiver's bound check absorbs that, and the real discriminator is the dedicated port rather than the header.

constexpr uint16_t DDP_PORT = 4048;
constexpr size_t DDP_HEADER_SIZE = 10;
constexpr size_t DDP_MAX_PAYLOAD = 1440;  // 480 RGB / 360 RGBW lights; divisible by 3 and 4
/// The flags bit marking a frame's last packet.
constexpr uint8_t DDP_FLAG_PUSH = 0x01;

/// Build a data packet; `push` marks a frame's last packet, which a double-buffering receiver shows on.
inline size_t buildDdpPacket(uint8_t* outBuf, uint32_t offset, bool push,
                             const uint8_t* data, uint16_t dataLen) {
    outBuf[0] = static_cast<uint8_t>(0x40 | (push ? DDP_FLAG_PUSH : 0x00));
    outBuf[1] = 0;                       // sequence unused
    outBuf[2] = 0x01;                    // RGB
    outBuf[3] = 0x01;                    // default display
    outBuf[4] = static_cast<uint8_t>(offset >> 24);
    outBuf[5] = static_cast<uint8_t>(offset >> 16);
    outBuf[6] = static_cast<uint8_t>(offset >> 8);
    outBuf[7] = static_cast<uint8_t>(offset & 0xFF);
    outBuf[8] = static_cast<uint8_t>(dataLen >> 8);
    outBuf[9] = static_cast<uint8_t>(dataLen & 0xFF);
    std::memcpy(outBuf + DDP_HEADER_SIZE, data, dataLen);
    return DDP_HEADER_SIZE + dataLen;
}

/// Parse and validate a data packet, zero-copy, reporting its push flag when asked; the version and length are the only checks.
inline bool parseDdpPacket(const uint8_t* pkt, size_t len, uint32_t& offsetOut,
                           const uint8_t*& dataOut, uint16_t& dataLenOut, bool* pushOut = nullptr) {
    if (!pkt || len < DDP_HEADER_SIZE) return false;
    if ((pkt[0] & 0xC0) != 0x40) return false;   // version must be 01
    const uint16_t dataLen = static_cast<uint16_t>((pkt[8] << 8) | pkt[9]);
    if (dataLen == 0 || dataLen > len - DDP_HEADER_SIZE) return false;
    offsetOut = (static_cast<uint32_t>(pkt[4]) << 24) | (static_cast<uint32_t>(pkt[5]) << 16)
              | (static_cast<uint32_t>(pkt[6]) << 8) | pkt[7];
    dataOut = pkt + DDP_HEADER_SIZE;
    dataLenOut = dataLen;
    if (pushOut) *pushOut = (pkt[0] & DDP_FLAG_PUSH) != 0;
    return true;
}

/// @}

} // namespace mm
