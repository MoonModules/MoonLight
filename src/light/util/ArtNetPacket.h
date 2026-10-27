#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

/// The one place the ArtNet packet layouts live, so a sender and a receiver cannot drift apart.
///
/// A unit test round-trips build against parse, and the header sits above both subfolders because the protocol is neutral between them.
///
/// @technical
///
/// ## The data packet
///
/// An 18-byte header then the channel data: the magic string, the opcode, a protocol version, a sequence and physical port, the universe, and the data length.
/// The multi-byte fields run in a mix of orders, which is the protocol's own inconsistency rather than ours.
///
/// ## Discovery
///
/// A controller broadcasts a poll and every node answers with a reply carrying its address, names and bound universe.
/// That reply is what makes the device appear in a controller's output list.

constexpr uint16_t ARTNET_PORT = 6454;
constexpr size_t MAX_CHANNELS_PER_UNIVERSE = 510; // 170 RGB lights
constexpr size_t ARTNET_HEADER_SIZE = 18;
constexpr size_t ARTNET_POLL_REPLY_SIZE = 239;

/// Build a data packet into `outBuf`, which holds the header and the data rounded up to even, an odd length padded with a zero channel as Art-Net 4 asks; answers the total size.
inline size_t buildArtDmxPacket(uint8_t* outBuf, uint16_t universe, uint8_t sequence,
                                const uint8_t* data, uint16_t dataLen) {
    // "Art-Net\0" header
    std::memcpy(outBuf, "Art-Net", 8); // includes null terminator

    // OpCode: OpDmx = 0x5000 (little-endian)
    outBuf[8] = 0x00;
    outBuf[9] = 0x50;

    // Protocol version: 14 (big-endian)
    outBuf[10] = 0x00;
    outBuf[11] = 0x0e;

    // Sequence
    outBuf[12] = sequence;

    // Physical port
    outBuf[13] = 0;

    // Universe (little-endian)
    outBuf[14] = static_cast<uint8_t>(universe & 0xFF);
    outBuf[15] = static_cast<uint8_t>(universe >> 8);

    // Length (big-endian), even
    const uint16_t wireLen = static_cast<uint16_t>(dataLen + (dataLen & 1));
    outBuf[16] = static_cast<uint8_t>(wireLen >> 8);
    outBuf[17] = static_cast<uint8_t>(wireLen & 0xFF);

    // DMX data
    std::memcpy(outBuf + ARTNET_HEADER_SIZE, data, dataLen);
    if (wireLen != dataLen) outBuf[ARTNET_HEADER_SIZE + dataLen] = 0;

    return ARTNET_HEADER_SIZE + wireLen;
}

/// Parse and validate a data packet, zero-copy, reporting its sequence when asked (0 is a sender without one); the version is deliberately not checked.
inline bool parseArtDmxPacket(const uint8_t* pkt, size_t len, uint16_t& universeOut,
                              const uint8_t*& dataOut, uint16_t& dataLenOut, uint8_t* sequenceOut = nullptr) {
    if (!pkt || len < ARTNET_HEADER_SIZE) return false;
    if (std::memcmp(pkt, "Art-Net", 8) != 0) return false;
    if (pkt[8] != 0x00 || pkt[9] != 0x50) return false;   // OpDmx only
    const uint16_t dataLen = static_cast<uint16_t>((pkt[16] << 8) | pkt[17]);
    if (dataLen == 0 || dataLen > len - ARTNET_HEADER_SIZE) return false;
    universeOut = static_cast<uint16_t>(pkt[14] | (pkt[15] << 8));
    dataOut = pkt + ARTNET_HEADER_SIZE;
    dataLenOut = dataLen;
    if (sequenceOut) *sequenceOut = pkt[12];
    return true;
}

/// True when the packet is a poll, a controller asking which nodes are out there.
inline bool isArtPoll(const uint8_t* pkt, size_t len) {
    return pkt && len >= 14 && std::memcmp(pkt, "Art-Net", 8) == 0
        && pkt[8] == 0x00 && pkt[9] == 0x20;   // OpPoll, little-endian
}

/// Build the minimal reply a controller reads: our address, names, MAC and one bound output port.
inline size_t buildArtPollReply(uint8_t* outBuf, const uint8_t ip[4],
                                const uint8_t mac[6], const char* shortName,
                                const char* longName, uint16_t universeStart) {
    std::memset(outBuf, 0, ARTNET_POLL_REPLY_SIZE);
    std::memcpy(outBuf, "Art-Net", 8);
    outBuf[8] = 0x00; outBuf[9] = 0x21;                   // OpPollReply, little-endian
    std::memcpy(outBuf + 10, ip, 4);
    outBuf[14] = 0x36; outBuf[15] = 0x19;                 // port 6454, little-endian
    // The port address split across three fields, which together re-assemble the universe.
    outBuf[18] = static_cast<uint8_t>((universeStart >> 8) & 0x7F);
    outBuf[19] = static_cast<uint8_t>((universeStart >> 4) & 0x0F);
    outBuf[20] = 0x00; outBuf[21] = 0xFF;                 // OEM: unknown/generic
    // Short name (18 bytes incl NUL) and long name (64 bytes incl NUL).
    std::strncpy(reinterpret_cast<char*>(outBuf + 26), shortName, 17);
    std::strncpy(reinterpret_cast<char*>(outBuf + 44), longName, 63);
    outBuf[173] = 1;                                      // NumPorts (lo) = 1
    outBuf[174] = 0x80;                                   // PortTypes[0]: can output DMX
    outBuf[182] = 0x80;                                   // GoodOutput[0]: outputting
    outBuf[190] = static_cast<uint8_t>(universeStart & 0x0F);  // SwOut[0]
    // Style at 200 stays 0x00 = StNode.
    std::memcpy(outBuf + 201, mac, 6);
    return ARTNET_POLL_REPLY_SIZE;
}

} // namespace mm
