// SPDX-License-Identifier: GPL-3.0-or-later

/// Packs the three lanes' MoonLight slot streams into 32-bit GPIO data words, one word per pixel clock, for the CPU dump loop.
///
/// Each lane's frame is encoded by MoonLight's hub75Encode() (platform/Hub75Encode.h) into 16-bit little-endian slot words; the repack keeps only the six color bits of each and scatters them to the GPIO color lines through lookup tables (@xref{repack|repack}).
///
/// The repack assumes MoonLight's default Hub75Layout (@xref{default-layout|default layout}).
///
/// @technical
///
/// ## Repack
///
/// The three lanes share address, latch and OE and run identical geometry in lockstep, so at any slot index the three slot words differ only in their six color bits. The repack merges them into one 32-bit GPIO data word per pixel clock: bit (gpio) = level for that GPIO's color line.
///
/// Only the color bits survive. The slot word's address, latch and OE bits are stripped: the dump loop drives the address and the latch itself in the dark interval between rows, and the hardware PWM pulser drives OE, which is the point of the hybrid design (the encoder's OE bits describe a timed pulse the PWM owns, so they stay out of the GPIO stream).
///
/// The scatter is done with lookup tables built once from the pin map: three 64-entry color tables (one per lane, 6 color bits in). The repack loop is three table lookups and two ORs per word.
///
/// ## Default layout
///
/// The default Hub75Layout is r1=0, g1=1, b1=2, r2=3, g2=4, b2=5, a=8, b=9, c=10, d=11, e=12, lat=13, oe=14. The engine encodes with the default layout, and hub75_rpi_common.h pins it with a static_assert, so a layout change fails at compile time instead of scrambling the repack.
///
/// A remapped layout would need the tables rebuilt from it, which the fixed board wiring does not call for, so the default layout is the right one.

#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/rpi/hub75_board_pins.h"   // kHub75Lanes: the board's lane count

namespace mm::platform {

/// Lookup tables for the repack, built once by buildGpioRepackTables() from the pin map in hub75_board_pins.h, so a wiring change is a data change rather than a code change.
struct GpioRepackTables {
    /// The GPIO mask for a lane's six color pins, indexed by lane and then by the six color bits as in the slot word.
    uint32_t color[kHub75Lanes][64];
};

/// Builds the tables from the pin map into `out`, a pure function of hub75_board_pins.h; `out` is a parameter so the 768-byte tables are built in the engine's member, not returned through a stack temporary.
void buildGpioRepackTables(GpioRepackTables& out);

/// Repacks `slots` 16-bit slot words a lane into `slots` 32-bit GPIO words; `laneSlots[c]` points at lane c's little-endian slot stream, lanes beyond `lanes` count as all-zero (panels stay dark); the address, latch and OE bits are ignored.
void repackSlotsToGpio(const uint16_t* const laneSlots[kHub75Lanes], int lanes,
                       size_t slots, const GpioRepackTables& tables, uint32_t* gpioOut);

/// Reference implementation of one word's pack, bit by bit with no tables; the unit test checks the tables against it; the hot path skips it.
uint32_t packGpioWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int lanes);

}  // namespace mm::platform
