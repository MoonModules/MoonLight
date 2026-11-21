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
///
/// ## Hold words
///
/// A plane lights `2^p × (pass - 1) / 2^(bitDepth-1)` words of its row's pass, rounded, so a narrow row's lowest plane, `(row - 1) / 2^(bitDepth-1)` words, rounds to zero. The encoder leads each (plane, row) group with hold words to prevent that: clocks with color 0, addressed to the latched row and lit inside the window like any word, which the row's own words shift out before the latch.
///
/// A row of `2^(bitDepth-2)` words or fewer stretches to a pass of `2^(bitDepth-1) + 1` words, where the lowest plane lights exactly one word and every plane doubles exactly. A wider row takes none and keeps the rounding: at 6 bits a 17-word row lights 1, 1, 2, 4, 8, 16 words and a 64-word row 2, 4, 8, 16, 32, 63.
///
/// The stretch is a function of the row width and the depth, so the frame size holds still when the brightness changes. It costs refresh on a narrow row, since every plane's pass grows: at 6 bits a row of 16 words clocks 33, and at 8 bits a row of 64 clocks 129.

/// Which bus bit each HUB75 line occupies in @xref{the-bus-word|the 16-bit bus word} per slot; the platform layer maps bit to GPIO. The encoder reads these as compile-time constants, and a port's pin order follows them.
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

/// The deepest `bitDepth` the encoder takes. A port may take fewer, see `kHub75PortMaxBitDepth`.
constexpr uint8_t kHub75EncodeMaxBitDepth = 8;

/// The deepest `bitDepth` the ESP32 port takes, until the repeated-planes card (backlog) lifts it: the lit-window weights cost top brightness, and plane 0 of a 64-column row lights 2 words at 6 bits, half that at 7. The driver and the port read it.
constexpr uint8_t kHub75PortMaxBitDepth = 6;

/// Hold words for a row of `rowWords` shift words at `bitDepth` planes, the one home of the formula (@xref{hold-words|Technical → Hold words}): 0 for an empty row, and for a wide one whose lowest plane already lights a word.
inline size_t hub75HoldWords(size_t rowWords, uint8_t bitDepth) {
    if (rowWords == 0 || bitDepth < 2 || bitDepth > kHub75EncodeMaxBitDepth) return 0;
    // The lowest plane's window rounds to zero up to 2^(bitDepth-2) row words; above that the rounding stands.
    if (rowWords > (static_cast<size_t>(1) << (bitDepth - 2))) return 0;
    const size_t pass = (static_cast<size_t>(1) << (bitDepth - 1)) + 1;
    return pass - rowWords;
}

/// The geometry one encode needs. `scanRate` is the panel's own and is NOT derivable from the height. Two panels of identical dimensions can scan differently, which is why it is a control rather than a calculation.
struct Hub75Geometry {
    /// Bytes on the wire per pixel clock: one 16-bit bus word, sixteen rather than eight since the control lines live at bits 8-14.
    static constexpr size_t kBytesPerSlot = 2;
    /// The largest frame, RGB or encoded, a geometry may need: what a 32-bit size_t holds.
    static constexpr uint64_t kMaxFrameBytes = 0xFFFFFFFFu;

    /// Panel width in pixels.
    uint16_t width = 64;
    /// Panel height in pixels.
    uint16_t height = 64;
    /// The panel's own scan rate, meaning 1/N, up to 32 for the encoder's five address lines; the ESP32 port takes 8, 16 or 32.
    uint8_t  scanRate = 16;
    /// Bit planes per frame, 2 to `kHub75EncodeMaxBitDepth`. Every plane costs a full scan pass.
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
        // The encoder's ceiling: above it the plane shift in hub75Encode goes negative and the cast wraps. A port limits the depth it takes below that, the ESP32 port at `kHub75PortMaxBitDepth`.
        if (bitDepth < 2 || bitDepth > kHub75EncodeMaxBitDepth) return false;
        // The encoder has five address lines, so 32 steps: above that rows alias.
        if (scanRate == 0 || scanRate > 32 || height % scanRate != 0) return false;
        if (rowsPerScan() % 2 != 0) return false;
        // The RGB frame and the encoded frame fit 32 bits on every target, counted in 64, so a 32-bit size_t holds every size the encode and its buffers use.
        const uint64_t rgbBytes = static_cast<uint64_t>(width) * height * 3;
        const uint64_t encodedBytes = static_cast<uint64_t>(bitDepth) * scanRows() * (static_cast<uint64_t>(rowWords()) + holdWords()) * kBytesPerSlot;
        return rgbBytes <= kMaxFrameBytes && encodedBytes <= kMaxFrameBytes;
    }

    /// Shift words one (plane, row) group clocks into the panel: the width times the color passes per address step.
    size_t rowWords() const {
        return static_cast<size_t>(width) * (rowsPerScan() / 2);
    }

    /// Hold words the encoder leads each group with, so a narrow row's lowest plane lights a whole word (Hub75Encode.h): a function of the row width and the depth, 0 for an empty row, see @xref{hold-words|Technical → Hold words}.
    size_t holdWords() const { return hub75HoldWords(rowWords(), bitDepth); }

    /// Words one (plane, row) group holds on the wire: the shift words plus the hold words. A wide row needs no hold words, so the pass is the row.
    size_t passWords() const { return rowWords() + holdWords(); }

    /// Slots one encoded frame occupies, each plane stored once since storing plane p 2^p times costs 258,048 bytes at 6 bits on a 64x64 panel against 24,576; each group is `passWords()` wide, hold words included.
    size_t frameSlots() const {
        return static_cast<size_t>(bitDepth) * scanRows() * passWords();
    }

    /// Bytes one encoded frame occupies: the one home for the size, which the platform asks for rather than recomputing.
    size_t frameBytes() const { return frameSlots() * kBytesPerSlot; }
};

}  // namespace mm
