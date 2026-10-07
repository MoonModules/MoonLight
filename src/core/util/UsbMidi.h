#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::usbmidi {

/// @defgroup UsbMidi USB-MIDI event packets
/// @{
/// MIDI over USB travels in 4-byte event packets, as the USB MIDI 1.0 class lays them out.
/// The first byte holds the cable in its high nibble and the Code Index Number (CIN) in its low one; the other three carry the message, padded with zeros.

/// How many MIDI bytes a packet with this CIN carries, 0 for the reserved ones.
inline uint8_t length(uint8_t cin) {
    static constexpr uint8_t kLength[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    return kLength[cin & 0x0F];
}

/// The channel or system message in one packet, written to `m` with its length; 0 for a SysEx piece, which a desk's input never needs.
inline uint8_t decode(const uint8_t p[4], uint8_t m[3]) {
    const uint8_t cin = p[0] & 0x0F;
    if (cin >= 0x4 && cin <= 0x7) return 0;
    const uint8_t len = length(cin);
    for (uint8_t i = 0; i < len; i++) m[i] = p[1 + i];
    return len;
}

/// One channel message of up to three bytes as one packet, its CIN the status nibble; 0 when it is not one.
inline size_t encodeChannel(const uint8_t* msg, size_t len, uint8_t (*out)[4], size_t max) {
    if (max == 0 || len > 3 || msg[0] >= 0xF0) return 0;
    out[0][0] = static_cast<uint8_t>(msg[0] >> 4);
    for (uint8_t i = 0; i < 3; i++) out[0][1 + i] = i < len ? msg[i] : 0;
    return 1;
}

/// A SysEx message three bytes a packet: CIN 4 starts or continues it, and CIN 5, 6 or 7 ends it with that many bytes.
inline size_t encodeSysex(const uint8_t* msg, size_t len, uint8_t (*out)[4], size_t max) {
    const size_t packets = (len + 2) / 3;
    if (packets > max || msg[len - 1] != 0xF7) return 0;
    for (size_t k = 0; k < packets; k++) {
        const size_t at = k * 3, left = len - at;
        const bool last = k + 1 == packets;
        out[k][0] = last ? static_cast<uint8_t>(0x4 + left) : 0x4;
        for (uint8_t i = 0; i < 3; i++) out[k][1 + i] = i < left ? msg[at + i] : 0;
    }
    return packets;
}

/// Cut one complete MIDI message into packets on cable 0, a SysEx one included; the count written, 0 when it does not fit or is not a message.
inline size_t encode(const uint8_t* msg, size_t len, uint8_t (*out)[4], size_t max) {
    if (!msg || len == 0 || !(msg[0] & 0x80)) return 0;
    return msg[0] == 0xF0 ? encodeSysex(msg, len, out, max) : encodeChannel(msg, len, out, max);
}

/// @}
}  // namespace mm::usbmidi
