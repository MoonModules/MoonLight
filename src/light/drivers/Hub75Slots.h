#pragma once

#include "platform/Hub75Bus.h"   // Hub75Layout, Hub75Geometry: the bus contract the port reads too

#include <cstddef>   // size_t
#include <cstdint>

namespace mm {

/// HUB75 encode: the frame the driver hands a port, named for the wire unit it builds (one pixel clock is one SLOT). A pure data transform on the bus contract (platform/Hub75Bus.h); sibling of ParallelSlots.h.
///
/// @attribution mrcodetastic, hzeller, ESPHome
///
/// @technical
///
/// ## The wire layout
///
/// One encoded frame, outermost first:
///
///   for each bit plane p (0 = least significant)
///     for each scan row r
///       for each column x          -> one bus word: the six color bits for
///                                     (x, r) and (x, r + rows), addressed to
///                                     the row in the latch; the last column's
///                                     word also latches row r, dark
///
/// Every word on the bus is a clock, so the latch rides the last column's word: a word of its own would shift one pixel more than the panel holds.
///
/// ## The address lags the data by one row
///
/// A panel lights where output-enable, the addressed row and the latched row meet, and the latch holds the row last strobed. So row r's words name row r - 1, the latch word moves the address to r in the dark, and row 0's words name the last row.
///
/// The lit window follows the data in the latch, not the plane being clocked in. Row 0's words light the last row of the plane before, so they take that plane's window, and plane 0's take the top plane's, the one the buffer wraps from. With the current plane's window instead, the last row of each half-panel weighed its bits double and its top bit almost nothing: a line at the seam and along the bottom edge, which a uniform white hides.
///
/// ## Brightness is time
///
/// A panel's chips only switch a column on or off, so plane p lights 2^p as long as plane 0 and brightness shortens every plane's window.
///
/// Below about a quarter brightness, the lowest plane of a 64-column row rounds to nothing.

/// Encode one RGB frame into @xref{the-wire-layout|bit planes}, @xref{the-address-lags-the-data-by-one-row|the address and the lit window a row late}, @xref{brightness-is-time|brightness as time}; bytes written, or 0 on a bad geometry: a dark frame beats half a frame.
inline size_t hub75Encode(const uint8_t* rgb, uint8_t* out,
                          const Hub75Geometry& geo, const Hub75Layout& lay = {}) {
    if (!rgb || !out) return 0;
    if (!geo.valid()) return 0;

    const uint16_t rows = geo.scanRows();
    const uint16_t pairs = geo.rowsPerScan() / 2;   // color passes per address step
    const uint16_t pairSpan = geo.height / 2;       // upper row pairs with row + span
    // Only the first `addrBits` are read, so a 1/8 panel never touches D or E.
    const uint8_t addr[5] = {lay.a, lay.b, lay.c, lay.d, lay.e};
    uint8_t addrBits = 3;                       // 1/8 scan
    if (geo.scanRate > 8) addrBits = 4;         // 1/16
    if (geo.scanRate > 16) addrBits = 5;        // 1/32

    // A row's address lines as one mask, built once per scan row rather than bit by bit on every word.
    auto addrMask = [&](uint16_t row) {
        uint16_t m = 0;
        for (uint8_t bit = 0; bit < addrBits; bit++)
            if ((row >> bit) & 1) m |= static_cast<uint16_t>(1u << addr[bit]);
        return m;
    };
    const size_t rowWords = static_cast<size_t>(geo.width) * pairs;

    size_t w = 0;
    for (uint8_t plane = 0; plane < geo.bitDepth; plane++) {
        // Depth < 8 keeps the HIGH bits: dropping those would dim every bright pixel.
        const uint8_t shift = static_cast<uint8_t>(8 - geo.bitDepth + plane);
        for (uint16_t r = 0; r < rows; r++) {
            // The row in the LATCH while these words clock in: the previous one, and for row 0 the last row of the previous plane or frame.
            const uint16_t latched = static_cast<uint16_t>((r + rows - 1) % rows);
            // Lit words name the latched row; the latch word strobes row r and moves the address to it, in the dark.
            const uint16_t darkWord = addrMask(latched);
            const uint16_t litWord = static_cast<uint16_t>(darkWord | (1u << lay.oe));
            const uint16_t latchWord = static_cast<uint16_t>(addrMask(r) | (1u << lay.lat));
            // The plane whose data the latch holds while these words clock in, the previous one for row 0: its window is the one these words light.
            const uint8_t shownPlane = r == 0 ? static_cast<uint8_t>((plane + geo.bitDepth - 1) % geo.bitDepth) : plane;
            // Lit words: the top plane fills the row less its latch, each lower plane half the one above; 32 bits hold 2,048 words at 6 bits.
            const uint32_t span = 255u << (geo.bitDepth - 1);
            const uint32_t litWords =
                (static_cast<uint32_t>(rowWords - 1) * geo.brightness * (1u << shownPlane) + span / 2) / span;
            for (uint16_t pair = 0; pair < pairs; pair++)
            for (uint16_t x = 0; x < geo.width; x++) {
                // Pair p of step r is row r + p*scanRate, paired half a panel below.
                const uint16_t upRow = static_cast<uint16_t>(r + pair * geo.scanRate);
                const size_t up = (static_cast<size_t>(upRow) * geo.width + x) * 3;
                const size_t lo = (static_cast<size_t>(upRow + pairSpan) * geo.width + x) * 3;
                uint16_t word = 0;
                if ((rgb[up + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r1);
                if ((rgb[up + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g1);
                if ((rgb[up + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b1);
                if ((rgb[lo + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r2);
                if ((rgb[lo + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g2);
                if ((rgb[lo + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b2);
                // The last word latches; the rest are lit inside this plane's window and dark after it.
                const size_t i = static_cast<size_t>(pair) * geo.width + x;
                word |= (i == rowWords - 1) ? latchWord : (i < litWords ? litWord : darkWord);
                // Little-endian, matching how the peripheral latches a 16-bit bus word.
                out[w++] = static_cast<uint8_t>(word & 0xFF);
                out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
            }
        }
    }
    // The frame ends on the last row's latch word, dark, and row 0's data words name that last row: the loop closes with every row lit once.
    return w;
}

}  // namespace mm
