#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

/// Domain logic with no ESP header, so the bit shapes are host-testable without an ESP32.
///
/// @technical
///
/// The platform owns only the peripheral that expands wire bytes with these shapes, through `rmtWs2812SetBitTiming`.
///
/// ## The symbol layout
///
/// The layout matches ESP-IDF's `rmt_symbol_word_t`, documented here so no `driver/rmt_*.h` leaks into the light domain.
/// One 32-bit word is two 16-bit halves, each a duration and level pair with the level in the top bit:
///
/// | Bits | Meaning |
/// |------|---------|
/// | 0 to 14 | duration0, in RMT ticks |
/// | 15 | level0, 0 or 1 |
/// | 16 to 30 | duration1, in RMT ticks |
/// | 31 | level1, 0 or 1 |
///
/// One WS2812 data bit is one symbol: high for the bit's high ticks, then low for the rest of the cell.
/// So the first half is the high time at level 1, and the second the remainder of the period at level 0.

/// Pack two duration and level pairs into one RMT symbol word.
constexpr uint32_t makeRmtSymbol(uint16_t dur0, uint8_t lvl0,
                                 uint16_t dur1, uint8_t lvl1) {
    return (static_cast<uint32_t>(dur0 & 0x7FFF))
         | (static_cast<uint32_t>(lvl0 & 1) << 15)
         | (static_cast<uint32_t>(dur1 & 0x7FFF) << 16)
         | (static_cast<uint32_t>(lvl1 & 1) << 31);
}

} // namespace mm
