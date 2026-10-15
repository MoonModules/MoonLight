#pragma once

#include <cstddef>
#include <cstdint>

/// CRC-16/CCITT-FALSE, a cheap well-distributed checksum used to notice that state changed.
///
/// @technical
///
/// Game of Life is the caller today, hashing its grid each generation to detect that the pattern has gone static or fallen into a short oscillation, the same CRC recurring.
/// It respawns then, instead of looping forever.
///
/// ## Which variant, and what it is not for
///
/// This is the textbook polynomial 0x1021 with an init of 0xFFFF and no reflection, the recognizable CCITT-FALSE variant.
///
/// It is no security hash, a CRC being trivially collidable; it is a fast change detector, which is all the stasis check needs.
/// The implementation is integer-only with no table, the bit-serial form being tiny and the call sites running off the hot path, once per generation rather than per pixel.

namespace mm {

/// The CRC over `len` bytes at `data`, resumable by passing a previous `crc`.
constexpr uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFFu) {
    for (size_t i = 0; i < len; i++) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace mm
