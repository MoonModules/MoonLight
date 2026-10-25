#pragma once

#include <cstddef>   // size_t
#include <cstdint>

namespace mm {

/// The HUB75 bus contract between the light driver and a port: which bit of the bus word carries each panel line, and the geometry that sizes one frame.
///
/// @technical
///
/// ## The bus word
///
/// Address, latch and OE share the color bits' word, since the panel wants them at once; `Hub75Layout` names each line's bit. OE is active low and a peripheral idles its lines low, so the OE bit means lit and the platform inverts the pin.

/// Which bus bit each HUB75 line occupies in @xref{the-bus-word|the 16-bit bus word} per slot; the platform layer maps bit to GPIO. Defaults are the conventional order, and a board that wires the panel differently overrides them.
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
    /// Bytes on the wire per pixel clock: one 16-bit bus word, sixteen rather than eight since the control lines live at bits 8-14.
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

    /// Address steps one plane walks: the panel's scanRate, so a 1/16 panel steps 16 addresses whatever its height.
    uint16_t scanRows() const { return scanRate; }

    /// Rows driven per address step: four on a 64-row 1/16 panel, since encoding only the first pair leaves three quarters dark.
    uint16_t rowsPerScan() const { return scanRate ? height / scanRate : 0; }

    /// Is this geometry encodable? Each step drives an upper and a lower row together, so an odd count leaves one unpaired.
    bool valid() const {
        if (width == 0 || height == 0) return false;
        // At 64 columns plane 0 of 6 still lights 2 words; a deeper plane rounds to nothing until planes repeat (backlog).
        if (bitDepth < 2 || bitDepth > 6) return false;
        if (scanRate == 0 || height % scanRate != 0) return false;
        return rowsPerScan() % 2 == 0;
    }

    /// Slots one encoded frame occupies, each plane stored once since storing plane p 2^p times is a megabyte for one 64x64 panel.
    size_t frameSlots() const {
        const uint16_t pairs = rowsPerScan() / 2;   // color passes per address step
        return static_cast<size_t>(bitDepth) * scanRows() * width * pairs;
    }

    /// Bytes one encoded frame occupies: the one home for the size, which the platform asks for rather than recomputing.
    size_t frameBytes() const { return frameSlots() * kBytesPerSlot; }
};

}  // namespace mm
