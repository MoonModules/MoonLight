// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>   // std::min
#include <cstddef>     // size_t
#include <cstdint>
#include <span>

namespace mm::platform {

/// HUB75 GPIO dither: the brightness scales each code, and each pixel carries the fraction of a shown level it loses to its next refresh (first-order sigma-delta modulation in time, the frame rate control of display panels), so over the refreshes the shown levels average to the scaled value.
///
/// @technical
///
/// ## Levels and fraction bits
///
/// The encoder shows the top `bitDepth` bits of a code, so one shown level is `2^(8 - bitDepth)` codes, and the dither works in levels at every depth.
///
/// The carry keeps `fractionBits` bits of a level, 0 to 3, and 0 is off. A pixel's pattern repeats within `2^fractionBits` encodes, one a refresh while the encode keeps up, so at 2 bits it repeats at a quarter of the refresh rate or slower.
///
/// The bits are one budget. At 8 bits and brightness 64, two of them bring back the two bits the scaling takes; at 6 bits and full brightness, the two bits the planes drop; at 6 bits and 64 they share it.
///
/// ## The seed
///
/// The remainders start on a 4x4 ordered (Bayer) pattern rather than all at zero, so neighboring pixels carry over on different refreshes and a flat dim area shimmers as fine grain instead of pulsing as one. The channels of one pixel sit a column apart.
///
/// The light layer's `Dither` (powerfunctions/draw.h) narrows 16-bit samples to bytes above the platform; this one works below it, in the panel's shown levels with a carry of 0 to 3 bits, so it keeps its own table.

/// The fractions of a shown level each code holds at `brightness` and `bitDepth`, in `2^fractionBits` steps, rounded down, `bitDepth` 1 to 8 and `fractionBits` 0 to 3, as @xref{levels-and-fraction-bits|levels and fraction bits} says.
inline void hub75DitherTable(std::span<uint16_t, 256> steps, uint8_t brightness, uint8_t bitDepth, uint8_t fractionBits) {
    const uint32_t levelCodes = 1u << (8 - bitDepth);
    for (uint32_t v = 0; v < 256; v++) steps[v] = static_cast<uint16_t>((v * brightness << fractionBits) / (255 * levelCodes));
}

/// Seed the remainders of a strip `width` pixels wide, three channels a pixel, on the Bayer pattern, in `2^fractionBits` steps, `width` above 0 and `fractionBits` 0 to 3: @xref{the-seed}.
inline void hub75DitherSeed(std::span<uint8_t> remainder, size_t width, uint8_t fractionBits) {
    static constexpr uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (size_t i = 0; i < remainder.size(); i++) {
        const size_t pixel = i / 3;
        const size_t x = pixel % width + i % 3;
        const size_t y = pixel / width;
        remainder[i] = static_cast<uint8_t>(kBayer4[y & 3][x & 3] >> (4 - fractionBits));
    }
}

/// Scale `codes` in place for one refresh through the `steps` table: each code shows its whole levels and carries the rest; a level above the depth's top shows as the top, its phase kept.
inline void hub75DitherScale(std::span<uint8_t> codes, std::span<uint8_t> remainder, std::span<const uint16_t, 256> steps, uint8_t bitDepth, uint8_t fractionBits) {
    const uint8_t drop = static_cast<uint8_t>(8 - bitDepth);
    const uint32_t top = (1u << bitDepth) - 1;
    const uint32_t mask = (1u << fractionBits) - 1;
    const size_t n = std::min(codes.size(), remainder.size());
    for (size_t i = 0; i < n; i++) {
        const uint32_t sum = steps[codes[i]] + remainder[i];
        remainder[i] = static_cast<uint8_t>(sum & mask);
        codes[i] = static_cast<uint8_t>(std::min(sum >> fractionBits, top) << drop);
    }
}

}  // namespace mm::platform
