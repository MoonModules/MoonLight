// SPDX-License-Identifier: GPL-3.0-or-later

/// Packs three MoonLight slot streams into 32-bit wide PIO words, one word per pixel clock, through lookup tables built once from the pin map.
///
/// Each lane's frame is encoded separately by `hub75Encode()` (platform/Hub75Encode.h) into 16-bit little-endian slot words; the lanes share address, latch and OE, so at any slot index the three slot words differ only in their six color bits. The wide word is read by the PIO program (see hub75.pio and hub75_pio_pins.h): wide bit (gpio - 2) is the level for that GPIO, OE inverted (set = blanked).
///
/// The repack loop is four table lookups and three ORs per word. The pin layout is in @xref{layout|Technical → Layout}, the tables in @xref{tables|Technical → Tables}, the control bits in @xref{control-bits|Technical → Control bits}.
///
/// @technical
///
/// ## Layout
///
/// The repack assumes MoonLight's DEFAULT Hub75Layout (r1=0, g1=1, b1=2, r2=3, g2=4, b2=5, a=8, b=9, c=10, d=11, e=12, lat=13, oe=14). The engine always encodes with the default layout, and a remapped layout needs the tables rebuilt from it; the board wiring is fixed, so the default layout is the right one.
///
/// hub75_rpi_common.h pins the default with a static_assert, so a layout change fails at compile time instead of scrambling the repack.
///
/// ## Tables
///
/// The scatter is done with lookup tables built once from the pin map: three 64-entry color tables (one per lane, 6 color bits in) and one 128-entry control table (7 control bits in). A wiring change is a data change in hub75_board_pins.h, not a code change.
///
/// `buildRepackTables` fills an out-parameter because a RepackTables is 1,280 bytes, and returning it by value materializes a temporary that size on the caller's stack, over the project's 512-byte frame limit (Hub75PioEngine::open measured 1,424 bytes on Clang when it returned by value). Building in place keeps the tables in the engine's member storage, off every stack.
///
/// ## Control bits
///
/// The control bits (address, latch, OE) are taken from lane 0's word. The three lanes' control bits are identical by construction (same geometry, same brightness). Lanes beyond `lanes` are treated as all-zero, so their panels, if connected, stay dark.
///
/// ## Latch blanking
///
/// `applyLatchBlanking` follows the ESP32 style: it clears the lit state (sets the OE pin bit, which blanks) on the `blankWords` words before each row's latch word. `rowWords` is the number of slots per (plane, row) group, the pass: laneWidth * pairs plus the encoder's hold words on a narrow row, so the blanking lands on the group's own words. A value of 0 leaves the encoder's output untouched.
///
/// Only the top plane(s) at high brightness are affected, since lower planes are already dark at the end of their rows, and the top plane's lit window shortens by `blankWords` pixel clocks.

#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/rpi/hub75_board_pins.h"   // kHub75Lanes: the board's lane count

namespace mm::platform {

/// Lookup tables for the repack, built once by buildRepackTables() from the pin map in hub75_board_pins.h. See @xref{tables|Technical → Tables}.
struct RepackTables {
    /// Wide-word mask for a lane's six color pins, indexed [lane][6 color bits as in the slot word].
    uint32_t color[kHub75Lanes][64];
    /// Wide-word mask for A-E, LAT and OE, indexed by the 7 control bits of the slot word (bits 8..14); the table sets the OE pin bit when the slot OE bit is CLEAR.
    uint32_t control[128];
};

/// Builds the tables from the pin map into `out`, a pure function of hub75_board_pins.h; `out` is a parameter because the tables are too large to return by value (@xref{tables|Technical → Tables}).
void buildRepackTables(RepackTables& out);

/// Repacks `slots` 16-bit slot words per lane into `slots` 32-bit wide words; `laneSlots[c]` is lane c's little-endian slot stream, as hub75Encode writes it. See @xref{control-bits|Technical → Control bits}.
void repackSlots(const uint16_t* const laneSlots[kHub75Lanes], int lanes,
                 size_t slots, const RepackTables& tables, uint32_t* wideOut);

/// Optional latch blanking on `wide`: blanks `blankWords` words before each row's latch word, `rowWords` slots per (plane, row) group. See @xref{latch-blanking|Technical → Latch blanking}.
void applyLatchBlanking(uint32_t* wide, size_t slots, size_t rowWords,
                        int blankWords);

/// Reference implementation of one word's pack, written out bit by bit with no tables; the unit test checks the tables against it, and the hot path leaves it unused.
uint32_t packWideWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int lanes);

}  // namespace mm::platform
