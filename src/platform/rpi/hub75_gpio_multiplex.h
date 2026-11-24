// SPDX-License-Identifier: GPL-3.0-or-later

/// The multiplex mapping table for HUB75 panels: each entry maps a logical pixel to the position the panel is driven at, with the driven geometry (stretch and scan divisor). Normal is first, Stripe second; why a table: @xref{why-a-table|Why a table}; what multiplexing is: @xref{multiplexed-panels|Multiplexed panels}.
///
/// Stripe is the type the Pi 4 sign's panels need, written from hzeller's algorithm (@xref{the-stripe-mapping|The Stripe mapping}, @xref{license-caveat|License caveat}) and rendered right on the panel (@xref{verified-on-the-panel|Verified on the panel}).
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Multiplexed panels
///
/// A direct panel lights, at address step s, the rows the encoder placed at step s. Some outdoor panels wire their rows through an internal multiplexer instead, so the panel is driven as a shorter, wider panel than it looks.
///
/// hzeller's README puts it as "by default their output looks jumbled up" and "they require some pixel-mapping", and names the types (0 direct, 1 Stripe, 2 Checkered, 3 Spiral, 4 ZStripe, ... 24 in all).
///
/// The Pi 4 sign's panels need type 1, Stripe (`--led-multiplexing=1` in its working hzeller card).
///
/// ## Why a table
///
/// One mapping is chosen from a table: the engine asks the table for the panel position to driven position mapping and the driven geometry. Normal is first, Stripe second: Stripe is one mapping among hzeller's 24, and the default is direct.
///
/// A further mapping is a further entry: its position function, its stretch (the driven columns per logical column) and its scan divisor (the address steps are rows/divisor).
///
/// A mapping whose driven geometry does not fit a stretch and a divisor, one that reshapes the panel some other way, needs the entry to grow a geometry function instead of two numbers; the table holds no such mapping.
///
/// ## The Stripe mapping
///
/// The algorithm of hzeller's StripeMultiplexMapper (lib/multiplex-mappers.cc, the "Stripe" entry), reimplemented in new code from the algorithm.
///
/// Stripe has stretch factor 2: a panel of R rows x C columns is driven as R/2 rows x 2C columns, and the scan is the driven panel's, R/4 address steps, so the sign's 32x64 panel is driven as 16 rows x 128 columns at 1/8 scan, on three address lines (ABC) instead of four. Logical pixel (x, y) lands at driven:
///
/// ```
/// fold = (y % (R/2)) < R/4
/// driven_x = fold ? x + C : x
/// driven_y = (y / (R/2)) * (R/4) + y % (R/4)
/// ```
///
/// For the sign's 32x64 panel: rows 0..7 and 16..23 fold to columns 64..127, rows 8..15 and 24..31 stay at columns 0..63, and the address is (y/16)*8 + y%8.
///
/// Each driven row holds two logical rows side by side (driven row 0 is logical row 8 on columns 0..63 and logical row 0 on columns 64..127), and each address step lights driven rows s and s+8, so four logical rows a step. The driven frame holds the same pixel count as the logical one, only reshaped.
///
/// A 32-row panel scans 1/16 direct and 1/8 under Stripe, a 64-row one 1/32 direct and 1/16 under Stripe, since Stripe halves the rows first.
///
/// ## License caveat
///
/// hzeller's per-file headers are GPL-2.0-only although its README says "or any later version", so no hzeller source is copied, transliterated or adapted here: the Stripe mapping is new code written from the algorithm, credited to rpi-rgb-led-matrix (see docs/reference/attributions.md).
///
/// This applies to the Pi 4 GPIO side only; the Pi 5 PIO side has no multiplexing.
///
/// ## Verified on the panel
///
/// The Stripe mapping is the algorithm from the mapper the sign's hzeller build runs, and the bench-v5 lab run showed it from this engine: the test pattern renders right (red rows step top to bottom, green columns left to right, held red row 0 lights the top) and a rotating wireframe renders clean and sharp, with no spreading, tearing or ghosting.
///
/// Verified here means rendered right on the panel; the license caveat stands.

#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/Hub75Gpio.h"   // Hub75GpioMultiplexing: the wirings the table holds, and their names

namespace mm::platform {

/// One panel's driven position: the column and row the logical pixel is driven at, under whichever mapping the table entry names.
struct Hub75MultiplexPos {
    uint32_t x = 0;   ///< the driven column
    uint8_t  y = 0;   ///< the driven row
};

/// The direct mapping: the driven position is the logical one.
inline Hub75MultiplexPos hub75GpioNormalMap(uint16_t x, uint8_t y, uint8_t /*rows*/,
                                            uint16_t /*cols*/) {
    return Hub75MultiplexPos{x, y};
}

/// Logical pixel (x, y) of a panel of `rows` x `cols` under Stripe (`rows` a multiple of 4; a panel too short to fold maps to itself): @xref{the-stripe-mapping|The Stripe mapping}.
inline Hub75MultiplexPos hub75GpioStripeMap(uint16_t x, uint8_t y, uint8_t rows, uint16_t cols) {
    if (rows < 4) return Hub75MultiplexPos{x, y};
    const uint8_t half = static_cast<uint8_t>(rows / 2);
    const uint8_t quarter = static_cast<uint8_t>(rows / 4);
    const bool fold = (y % half) < quarter;
    Hub75MultiplexPos p;
    p.x = fold ? static_cast<uint32_t>(x) + cols : x;
    p.y = static_cast<uint8_t>((y / half) * quarter + y % quarter);
    return p;
}

/// One multiplex mapping: its name (the control's option text), its position function and its driven geometry. Normal is the identity at stretch 1; Stripe folds at stretch 2, scan rows/4.
struct Hub75MultiplexEntry {
    const char* name;   ///< the control's option text
    Hub75MultiplexPos (*map)(uint16_t x, uint8_t y, uint8_t rows, uint16_t cols);   ///< the position function
    uint8_t stretch;       ///< the driven columns per logical column
    uint8_t scanDivisor;   ///< the address steps are rows/divisor
};

/// The table, in the control's order: normal first, Stripe second; the engine, the helpers below and the control's count all read it.
inline constexpr Hub75MultiplexEntry kHub75MultiplexEntries[kHub75MultiplexCount] = {
    {kHub75GpioMultiplexNames[kHub75MultiplexNormal], hub75GpioNormalMap, 1, 2},
    {kHub75GpioMultiplexNames[kHub75MultiplexStripe], hub75GpioStripeMap, 2, 4},
};

static_assert(kHub75MultiplexEntries[kHub75MultiplexCount - 1].stretch != 0, "every Hub75GpioMultiplexing has a table entry");

/// The entry for `multiplexing`; an unknown type is direct, so a caller always gets an entry, and the engine refuses an unknown type at open.
inline const Hub75MultiplexEntry& hub75GpioMultiplexEntry(uint8_t multiplexing) {
    return multiplexing < kHub75MultiplexCount
               ? kHub75MultiplexEntries[multiplexing]
               : kHub75MultiplexEntries[0];
}

/// Logical pixel (x, y) of a panel of `rows` x `cols` under `multiplexing`, through the table: the identity for normal, the fold for Stripe.
inline Hub75MultiplexPos hub75GpioMultiplexMap(uint8_t multiplexing, uint16_t x, uint8_t y,
                                               uint8_t rows, uint16_t cols) {
    return hub75GpioMultiplexEntry(multiplexing).map(x, y, rows, cols);
}

/// The scan a panel of `rows` wires: rows/2 direct, rows/4 under Stripe, the driven panel's scan (@xref{the-stripe-mapping|The Stripe mapping}).
inline uint8_t hub75GpioMultiplexScanRate(uint8_t multiplexing, uint8_t rows) {
    const uint8_t divisor = hub75GpioMultiplexEntry(multiplexing).scanDivisor;
    return rows >= divisor ? static_cast<uint8_t>(rows / divisor) : 1;
}

/// The driven panel's height: the rows, or half of them under Stripe.
inline uint8_t hub75GpioMultiplexDrivenRows(uint8_t multiplexing, uint8_t rows) {
    return static_cast<uint8_t>(rows / hub75GpioMultiplexEntry(multiplexing).stretch);
}

/// The driven panel's width: the columns, or double under Stripe; 32 bits, since a panel of 40,000 columns driven at stretch 2 is 80,000 wide.
inline uint32_t hub75GpioMultiplexDrivenCols(uint8_t multiplexing, uint16_t cols) {
    return static_cast<uint32_t>(cols) * hub75GpioMultiplexEntry(multiplexing).stretch;
}

}  // namespace mm::platform
