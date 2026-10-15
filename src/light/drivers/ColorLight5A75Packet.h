#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

/// The ColorLight 5A-75 wire format, in one home, which `PanelCardDriver` builds frames against.
///
/// @technical
///
/// This is the same shape as `ArtNetPacket.h` and `DdpPacket.h`, with one structural difference.
/// The format has no receiver in the tree, the cards being the receiver, so there is no build-and-parse round trip to test against.
/// The unit tests pin the built bytes against the layout instead.
///
/// ## Raw Ethernet, not UDP
///
/// These are complete layer-2 frames sent below IP: no address, no port, no DHCP lease.
/// That is why the driver works on a link that never got one.
///
/// ## The overloaded EtherType, the format's defining quirk
///
/// A normal frame carries a two-byte EtherType at offset 12 and 13.
/// Here byte 12 is the packet type and byte 13 is already the first payload byte, so every packet's body starts at offset 13 rather than 14.
/// A row packet therefore reads on the wire as EtherType `0x55` followed by the row's high byte.
/// That is not because the row is deliberately encoded there, but because it is simply the first payload byte and lands in that position.
/// Getting this wrong shifts every field by one, and the cards silently discard the frame.
///
/// ## The MACs are fixed constants, not real addresses
///
/// The destination is `11:22:33:44:55:66` and the source `22:22:33:44:55:66`.
/// The cards filter on that destination, so a frame sent to broadcast, or from the device's own MAC, is dropped with no visible error.
/// The source is not this device's MAC.
///
/// ## The frame layout
///
/// Every packet type shares the first thirteen bytes:
///
/// | Bytes | Meaning |
/// |-------|---------|
/// | 0 to 5 | destination MAC, always the fixed constant |
/// | 6 to 11 | source MAC, always the fixed constant |
/// | 12 | packet type: `0x55` row data, `0x01` sync, `0x0A` brightness, `0x07` discovery |
/// | 13 onward | payload, per type |
///
/// ## The row-data payload
///
/// Row data, type `0x55`, carries its payload from offset 13:
///
/// | Bytes | Meaning |
/// |-------|---------|
/// | 13 to 14 | row number, big-endian |
/// | 15 to 16 | pixel offset within the row, big-endian |
/// | 17 to 18 | pixel count in this packet, big-endian |
/// | 19 | `0x08`, a constant whose purpose is undocumented |
/// | 20 | `0x88`, likewise |
/// | 21 onward | pixel data, three bytes per pixel |
///
/// ## The sync and brightness packets
///
/// Sync, type `0x01`, is 112 bytes with its body zeroed except in four places.
/// Byte 13 is `0x07`, marking a PC or netcard sender where hardware writes `0x00`.
/// Byte 35 is the brightness, byte 36 a constant `0x05`, and bytes 38 to 40 the brightness again.
///
/// Brightness, type `0x0A`, is 77 bytes, zeroed except for the brightness three times at bytes 13 to 15 and `0xFF` at byte 16.
///
/// ## The order within a frame
///
/// Brightness first, then every row ascending, then sync last, the sync being what latches the buffered rows onto the panels.
///
/// The brightness packet is reported not to work on older card firmware, so the driver treats it as advisory and never depends on it having landed.
///
/// No configuration packets are sent: the cards are configured by the vendor's own tool and keep that in flash, so a sender only streams pixels.
///
/// ## What a builder requires of its caller
///
/// Each builder writes into a caller-owned buffer and returns the bytes written, so the buffer must be large enough before the call.
/// A row frame needs the row prefix plus three bytes a pixel, which the largest-frame constant covers.
/// A sync frame needs its own fixed 112 bytes, and a brightness frame its 77.
///
/// Pixel bytes are copied verbatim, because channel order is the caller's business and the driver's `Correction` has already applied it.
/// Nothing here reorders them, and nothing here should start to.
///
/// ## Why a row packet carries 497 pixels
///
/// The largest frame is 1512 bytes measured from the destination MAC, so it includes the 14-byte Ethernet header.
/// Its payload is 1498 bytes, which sits just inside the standard 1500-byte MTU: 497 pixels is the most that fits, and one more would not.
///
/// FPP packs rows the same way, while Harald Kubota's write-up sends 128 pixels a packet instead.
/// Both work against the cards, and the larger packet is fewer frames for the same wall.

/// The fixed destination MAC; the cards filter on it, so it is not arbitrary.
constexpr uint8_t kColorLightDestMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
/// The fixed source MAC.
constexpr uint8_t kColorLightSrcMac[6]  = {0x22, 0x22, 0x33, 0x44, 0x55, 0x66};

/// Byte 12 is the type; byte 13 begins the payload (see the EtherType note above).
constexpr size_t COLORLIGHT_TYPE_OFFSET = 12;
constexpr size_t COLORLIGHT_DATA_OFFSET = 13;

constexpr uint8_t COLORLIGHT_TYPE_ROW        = 0x55;
constexpr uint8_t COLORLIGHT_TYPE_SYNC       = 0x01;
constexpr uint8_t COLORLIGHT_TYPE_BRIGHTNESS = 0x0A;

/// The row header (row, offset, count, two constants) is 8 bytes, so pixels start at 13 + 8 = 21.
constexpr size_t COLORLIGHT_ROW_HEADER = 8;
constexpr size_t COLORLIGHT_ROW_PREFIX = COLORLIGHT_DATA_OFFSET + COLORLIGHT_ROW_HEADER;  // 21

constexpr uint16_t COLORLIGHT_MAX_PIXELS_PER_PACKET = 497;
constexpr uint8_t COLORLIGHT_BYTES_PER_PIXEL = 3;

/// The largest frame built: 1512 bytes from the destination MAC, a 1498-byte payload inside the MTU.
constexpr size_t COLORLIGHT_MAX_FRAME =
    COLORLIGHT_ROW_PREFIX + COLORLIGHT_MAX_PIXELS_PER_PACKET * COLORLIGHT_BYTES_PER_PIXEL;  // 1512

constexpr size_t COLORLIGHT_SYNC_FRAME = 112;
constexpr size_t COLORLIGHT_BRIGHTNESS_FRAME = 77;

/// Constants the cards require, reproduced from the observed layout, their purpose undocumented.
constexpr uint8_t kUnknownRowConst0 = 0x08;   // row payload byte 6
constexpr uint8_t kUnknownRowConst1 = 0x88;   // row payload byte 7
constexpr uint8_t kSyncFromNetcard  = 0x07;   // sync payload byte 0: sender is a PC/netcard
constexpr uint8_t kUnknownSyncConst = 0x05;   // sync payload byte 23

/// Write the 12-byte MAC pair plus the packet-type byte, everything after which is payload.
inline void buildColorLightHeader(uint8_t* out, uint8_t packetType) {
    std::memcpy(out, kColorLightDestMac, 6);
    std::memcpy(out + 6, kColorLightSrcMac, 6);
    out[COLORLIGHT_TYPE_OFFSET] = packetType;
}

/// Build one row-data frame from `pixelOffset` within `row`, returning the frame length.
inline size_t buildColorLightRowPacket(uint8_t* out, uint16_t row,
                                       uint16_t pixelOffset, uint16_t pixelCount,
                                       const uint8_t* pixels) {
    buildColorLightHeader(out, COLORLIGHT_TYPE_ROW);
    uint8_t* data = out + COLORLIGHT_DATA_OFFSET;
    data[0] = static_cast<uint8_t>(row >> 8);
    data[1] = static_cast<uint8_t>(row & 0xFF);
    data[2] = static_cast<uint8_t>(pixelOffset >> 8);
    data[3] = static_cast<uint8_t>(pixelOffset & 0xFF);
    data[4] = static_cast<uint8_t>(pixelCount >> 8);
    data[5] = static_cast<uint8_t>(pixelCount & 0xFF);
    data[6] = kUnknownRowConst0;
    data[7] = kUnknownRowConst1;
    const size_t dataBytes = static_cast<size_t>(pixelCount) * COLORLIGHT_BYTES_PER_PIXEL;
    if (pixels && dataBytes) std::memcpy(out + COLORLIGHT_ROW_PREFIX, pixels, dataBytes);
    return COLORLIGHT_ROW_PREFIX + dataBytes;
}

/// Build the sync frame that latches every row sent since the last one, returning its length.
inline size_t buildColorLightSyncPacket(uint8_t* out, uint8_t brightness) {
    std::memset(out, 0, COLORLIGHT_SYNC_FRAME);
    buildColorLightHeader(out, COLORLIGHT_TYPE_SYNC);
    uint8_t* data = out + COLORLIGHT_DATA_OFFSET;
    data[0]  = kSyncFromNetcard;
    data[22] = brightness;
    data[23] = kUnknownSyncConst;
    data[25] = brightness;
    data[26] = brightness;
    data[27] = brightness;
    return COLORLIGHT_SYNC_FRAME;
}

/// Build the brightness frame, one level on each of the three channels, returning its length.
inline size_t buildColorLightBrightnessPacket(uint8_t* out, uint8_t brightness) {
    std::memset(out, 0, COLORLIGHT_BRIGHTNESS_FRAME);
    buildColorLightHeader(out, COLORLIGHT_TYPE_BRIGHTNESS);
    uint8_t* data = out + COLORLIGHT_DATA_OFFSET;
    data[0] = brightness;
    data[1] = brightness;
    data[2] = brightness;
    data[3] = 0xFF;
    return COLORLIGHT_BRIGHTNESS_FRAME;
}

}  // namespace mm
