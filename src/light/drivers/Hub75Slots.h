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
///       for each column x          -> one bus word: the six color bits for
///                                     (x, r) and (x, r + rows), addressed to
///                                     the row in the latch; the last column's
///                                     word also latches row r, dark
///
/// Every word on the bus is a clock, so the latch rides the last column's word: a word of its own would shift one pixel more than the panel holds.
///
/// ## The address lags the data by one row
///
/// A panel lights where output-enable, the addressed row and the latched row meet, and the latch holds the row last strobed.
/// So row r's words name row r - 1, the latch word moves the address to r in the dark, and row 0's words name the last row.
///
/// ## Brightness is time
///
/// A panel's chips only switch a column on or off, so plane p lights 2^p as long as plane 0, and brightness shortens every plane's window rather than scaling values.
/// Below about a quarter brightness, the lowest plane of a 64-column row rounds to nothing.
///
/// ## The bus word
///
/// Address, latch and OE share the color bits' word, since the panel wants them at once; `Hub75Layout` names each line's bit.
/// OE is active low and a peripheral idles its lines low, so the OE bit means lit and the platform inverts the pin.
///
/// ## Prior art
///
/// mrcodetastic/ESP32-HUB75-MatrixPanel-DMA, hzeller/rpi-rgb-led-matrix and ESPHome's hub75.

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
    /// Output enable, SET where the panel is lit: the platform inverts the pin for the panel's active-low OE.
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
    /// Bit planes per frame, 2 to 6. Every plane costs a full scan pass.
    uint8_t  bitDepth = 4;
    /// How long each plane is lit, 0 to 255 of its full window: the global brightness, as time.
    uint8_t  brightness = 255;

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
        // At 64 columns plane 0 of 6 still lights 2 words; a deeper plane rounds to nothing until planes repeat (backlog).
        if (bitDepth < 2 || bitDepth > 6) return false;
        if (scanRate == 0 || height % scanRate != 0) return false;
        return rowsPerScan() % 2 == 0;
    }

    // Each plane stored ONCE: storing plane p 2^p times is a megabyte for one 64x64 panel.
    /// Slots one encoded frame occupies.
    size_t frameSlots() const {
        const uint16_t pairs = rowsPerScan() / 2;   // color passes per address step
        return static_cast<size_t>(bitDepth) * scanRows() * width * pairs;
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
            // Lit words: the top plane fills the row less its latch, each lower plane half the one above; 32 bits hold 2,048 words at 6 bits.
            const uint32_t span = 255u << (geo.bitDepth - 1);
            const uint32_t litWords =
                (static_cast<uint32_t>(rowWords - 1) * geo.brightness * (1u << plane) + span / 2) / span;
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

/// @}

}  // namespace mm
