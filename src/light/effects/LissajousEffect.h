#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"   // FrameTime: the shared time scale

namespace mm {

/// A Lissajous curve traced across the grid, palette-colored along its length, with a fading trail.
/// @card LissajousEffect.gif
///
/// Sample points sweep an x and a y oscillator whose frequencies and shared phase drift, so the closed curve continuously morphs and rotates.
///
/// @attribution Andrew Tuline
///
/// @moreinfo
///
/// The phase stays wide and only the final oscillator argument narrows to a byte. That one truncation closes the figure, so keeping it exact keeps the shape exact.
class LissajousEffect : public EffectBase {
public:
    /// Catalog tags: WLED lineage.
    const char* tags() const override { return "🐙"; }
    /// The curve is traced across a plane.
    Dim dimensions() const override { return Dim::D2; }

    /// The x-axis sweep frequency, which sets the curve's lobe count.
    uint8_t xFrequency = 64;
    /// How fast the trail fades each frame.
    uint8_t fadeRate   = 128;
    /// How fast the curve's phase advances.
    uint8_t speed      = 128;

    /// Publish the figure's shape, its trail and its speed.
    void defineControls() override {
        controls_.addControl("xFrequency", xFrequency, 0, 255);
        controls_.addControl("fadeRate", fadeRate, 0, 255);
        controls_.addControl("speed", speed, 0, 255);
    }

    /// Sweep both oscillators across the samples, painting each onto the grid.
    void tick() MM_NONBLOCKING override {
        const int w = width();
        const int h = height();

        const draw::Canvas cv = canvas();

        // The Layer scales this rate by the elapsed frame, so the tail holds its length.
        layer()->fadeToBlackBy(fadeRate);

        // Kept wide, so the high bits survive into the half-phase below.
        const uint32_t ms = elapsed();
        const uint16_t phase = static_cast<uint16_t>(ms * speed / 256);
        const uint16_t halfPhase = static_cast<uint16_t>(phase / 2);

        for (int i = 0; i < 256; i++) {
            // The cast here is the one truncation point, which is what closes the figure.
            const uint8_t sx = sin8(static_cast<uint8_t>(halfPhase + (i * xFrequency) / 64));
            const uint8_t sy = cos8(static_cast<uint8_t>(halfPhase + i * 2));

            // The oscillator mapped onto the grid, where a single-cell axis has only index 0.
            const int lx = (w < 2) ? 0 : (((2 * sx) * (2 * (w - 1))) / 511 + 1) / 2;
            const int ly = (h < 2) ? 0 : (((2 * sy) * (2 * (h - 1))) / 511 + 1) / 2;

            const uint8_t colorIndex = static_cast<uint8_t>(ms / 100 + i);
            draw::pixel(cv, {static_cast<lengthType>(lx), static_cast<lengthType>(ly), 0},
                        colorFromPalette(*Palettes::active(), colorIndex, 255));
        }
    }

};

} // namespace mm
