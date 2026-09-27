#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Palette-cycling diagonal rainbow effect, and the default the tests reach for.
/// @card RainbowEffect.gif
/// Author: FastLED rainbow (Mark Kriegsman), via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_FastLED.h
///
/// A pixel's hue comes from its own x plus y, walked by the clock, so the bands run diagonally.
class RainbowEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Writes the z=0 slice, which extrude duplicates through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the rainbow cycles, where a whole one a second reads too fast.
    uint8_t speed = 20;

    /// Publish the cycle speed.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
    }

    /// Walk the phase, then color each pixel by its diagonal position.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // One hue cycle a beat, widened since a 32-bit product overflows within minutes.
        uint32_t phase = static_cast<uint32_t>(
            static_cast<uint64_t>(elapsed()) * speed * 256 / 60000
        );

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                // The hue varies with x plus y and the clock, which runs the bands diagonally.
                uint8_t hue = static_cast<uint8_t>(
                    (static_cast<uint32_t>(x + y) * 256 / (w + h)) + phase
                );

                RGB c = colorFromPalette(*Palettes::active(), hue);
                size_t offset = (static_cast<size_t>(y) * w + x) * cpl;
                if (cpl >= 1) buf[offset + 0] = c.r;
                if (cpl >= 2) buf[offset + 1] = c.g;
                if (cpl >= 3) buf[offset + 2] = c.b;
            }
        }
    }
};

} // namespace mm
