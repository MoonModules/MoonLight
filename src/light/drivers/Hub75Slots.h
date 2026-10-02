#pragma once

#include <cstddef>   // size_t
#include <cstdint>

namespace mm {

/// @defgroup Hub75Slots HUB75 scan encoder: the bit-plane wire format
/// @{
///
/// HUB75 encode: the contract between the driver and a HUB75 port, named for the wire unit it builds.
/// One pixel clock is one SLOT. Sibling of ParallelSlots.h, which does the same job for WS2812.
/// A pure data transform with no platform include, so unit_Hub75Slots.cpp pins it without an ESP32.
///
/// @moreinfo
///
/// ## The wire layout
///
/// One encoded frame, outermost first:
///
///   for each bit plane p (0 = least significant)
///     for each scan row r
///       for each column x          -> one bus word per column: the six color
///                                     bits for (x, r) and (x, r + rows), lit,
///                                     ADDRESSED TO THE PREVIOUS ROW
///       one blanking word           -> OE high (dark), latch high, addressed
///                                     to row r: the row that data belongs to
///
/// ## The address lags the data by one row
///
/// A panel lights where three things meet: output-enable low, the row the address lines select, and the row held in the output LATCH.
/// The latch holds the row last strobed, which is the previous one. So while row r's color bits clock in, the address must still name row r - 1, or the previous row's data lights on row r's LEDs.
/// The blanking word is where the address moves to r: dark, and latching r at the same time.
/// Row 0's data words therefore name the LAST scan row, which is what lets the frame loop through the DMA wrap with every row lit exactly once.
/// Sending the data words with their own row as the address displaced the whole picture one scan row down and put the last row on the first. On a wall that reads as one bright line.
///
/// ## Address rides with the data
///
/// The row address and the control lines share the bus word that carries the color bits.
/// The peripheral clocks one word per slot, and a panel wants address and data at once.
/// `Hub75Layout` says which bus bit each line sits on, the one thing a board's wiring changes.
/// How a panel scans, and why brightness is time, is on the driver's page under "HUB75, details".
///
/// ## Prior art
///
/// mrcodetastic/ESP32-HUB75-MatrixPanel-DMA, hzeller/rpi-rgb-led-matrix and ESPHome's hub75.
/// The scan and bit-plane structure is the panel's, and the encoder is written from that behavior.

/// Which bus bit each HUB75 line occupies. The peripheral drives one 16-bit bus word per slot.
/// Every line is a bit position rather than a GPIO here, and the platform layer maps bit to GPIO.
///
/// Defaults are the conventional order and cost nothing to override. A board that wires the panel differently changes these, and the encoder is unchanged.
struct Hub75Layout {
    /// Color bits for the upper half-panel.
    uint8_t r1 = 0, g1 = 1, b1 = 2;
    /// Color bits for the lower half-panel.
    uint8_t r2 = 3, g2 = 4, b2 = 5;
    /// Row address bits; a 1/8 panel uses only a, b and c.
    uint8_t a = 8, b = 9, c = 10, d = 11, e = 12;
    /// Latch: moves the shift register to the output drivers.
    uint8_t lat = 13;
    /// Output enable, active LOW, so high is dark.
    uint8_t oe = 14;
};

/// The geometry one encode needs. `scanRate` is the panel's own and is NOT derivable from the height. Two panels of identical dimensions can scan differently, which is why it is a control rather than a calculation.
struct Hub75Geometry {
    // Sixteen rather than eight: the control lines live at bits 8-14 (Hub75Layout).
    /// Bytes on the wire per pixel clock: one 16-bit bus word.
    static constexpr size_t kBytesPerSlot = 2;

    /// Panel width in pixels.
    uint16_t width = 64;
    /// Panel height in pixels.
    uint16_t height = 64;
    /// The panel's own scan rate: 8, 16 or 32, meaning 1/8, 1/16 or 1/32.
    uint8_t  scanRate = 16;
    /// Bit planes per frame, 2 to 4. Every plane costs a full scan pass.
    uint8_t  bitDepth = 4;

    // The panel's scanRate IS that count: a 1/16 panel steps 16 addresses whatever its height.
    /// Address steps one plane walks.
    uint16_t scanRows() const { return scanRate; }

    // FOUR on a 64-row 1/16 panel: encoding only the first pair leaves three quarters dark.
    /// Rows driven per address step.
    uint16_t rowsPerScan() const { return scanRate ? height / scanRate : 0; }

    // Each step drives an upper and a lower row together, so an odd count leaves one unpaired.
    /// Is this geometry encodable?
    bool valid() const {
        if (width == 0 || height == 0) return false;
        // Capped at 4 until the planes are weighted: an unweighted 5th buys nothing visible.
        if (bitDepth < 2 || bitDepth > 4) return false;
        if (scanRate == 0 || height % scanRate != 0) return false;
        return rowsPerScan() % 2 == 0;
    }

    // Each plane stored ONCE: storing plane p 2^p times is a megabyte for one 64x64 panel.
    /// Slots one encoded frame occupies.
    size_t frameSlots() const {
        const uint16_t pairs = rowsPerScan() / 2;   // color passes per address step
        return static_cast<size_t>(bitDepth) * scanRows() *
               (static_cast<size_t>(width) * pairs + 1);
    }

    // The one home for the size: the platform asks rather than recomputing it.
    /// Bytes one encoded frame occupies.
    size_t frameBytes() const { return frameSlots() * kBytesPerSlot; }
};

// Returns bytes written, or 0 on an unusable geometry: a half-encoded frame is worse than a dark one.
/// Encode one rendered RGB frame into a HUB75 bit-plane buffer.
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

    size_t w = 0;
    for (uint8_t plane = 0; plane < geo.bitDepth; plane++) {
        // Depth < 8 keeps the HIGH bits: dropping those would dim every bright pixel.
        const uint8_t shift = static_cast<uint8_t>(8 - geo.bitDepth + plane);
        for (uint16_t r = 0; r < rows; r++) {
            // The row in the LATCH while these words clock in: the previous one, and for row 0 the last row of the previous plane or frame.
            const uint16_t lit = static_cast<uint16_t>((r + rows - 1) % rows);
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
                // The address rides EVERY column word, and it names the row in the latch: these words are lit, and what they light is the previous row's data.
                for (uint8_t bit = 0; bit < addrBits; bit++) {
                    if ((lit >> bit) & 1) word |= static_cast<uint16_t>(1u << addr[bit]);
                }
                // Little-endian, matching how the peripheral latches a 16-bit bus word.
                out[w++] = static_cast<uint8_t>(word & 0xFF);
                out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
            }
            // Dark, latching row r, and the address moves to r here, in the dark: the next row's data words then light row r with row r's data.
            uint16_t blank = static_cast<uint16_t>(1u << lay.oe) |
                             static_cast<uint16_t>(1u << lay.lat);
            for (uint8_t bit = 0; bit < addrBits; bit++) {
                if ((r >> bit) & 1) blank |= static_cast<uint16_t>(1u << addr[bit]);
            }
            out[w++] = static_cast<uint8_t>(blank & 0xFF);
            out[w++] = static_cast<uint8_t>((blank >> 8) & 0xFF);
        }
    }
    // The frame ends on the last row's blanking word, dark, and row 0's data words name that last row: the loop closes with every row lit once.
    return w;
}

/// @}

}  // namespace mm
