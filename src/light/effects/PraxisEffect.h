#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// An algorithmic palette pattern driven by two beat oscillators, a macro and a micro mutator, whose frequencies and ranges reshape the hue field over time.
/// @card PraxisEffect.gif
///
/// Each pixel's hue comes from the two mutators combined with its own position, so the pattern continually stretches, shears and rolls across the grid.
///
/// @attribution MONSOONO
///
/// @moreinfo
///
/// The micro mutator divides the spatial term, so it sets the pattern's spatial frequency. The macro mutator multiplies the cross term instead, which warps the field, and a steadily advancing hue base scrolls it through the palette.
class PraxisEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the pattern advances.
    uint8_t speed = 4;

    /// The coarse mutator's beat rate.
    uint8_t macroMutatorFreq = 3;
    /// The coarse mutator's range minimum.
    uint8_t macroMutatorMin  = 250;
    /// The coarse mutator's range maximum.
    uint8_t macroMutatorMax  = 255;
    /// The fine mutator's beat rate.
    uint8_t microMutatorFreq = 4;
    /// The fine mutator's range minimum.
    uint8_t microMutatorMin  = 200;
    /// The fine mutator's range maximum.
    uint8_t microMutatorMax  = 255;

    /// Publish the hue scroll and both mutators.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 64);
        controls_.addControl("macroMutatorFreq", macroMutatorFreq, 0, 15);
        controls_.addControl("macroMutatorMin", macroMutatorMin, 0, 255);
        controls_.addControl("macroMutatorMax", macroMutatorMax, 0, 255);
        controls_.addControl("microMutatorFreq", microMutatorFreq, 0, 15);
        controls_.addControl("microMutatorMin", microMutatorMin, 0, 255);
        controls_.addControl("microMutatorMax", microMutatorMax, 0, 255);
    }

    /// Sweep both mutators, then color each pixel from them and its own position.
    void tick() MM_NONBLOCKING override {
        const int w = width();
        const int h = height();

        const draw::Canvas cv = canvas();

        const uint32_t now = elapsed();

        // The macro mutator sweeps a wide range, where the micro one sweeps a byte.
        const uint16_t macro = beatsin16(macroMutatorFreq, now,
                                         static_cast<uint16_t>(macroMutatorMin << 8),
                                         static_cast<uint16_t>(macroMutatorMax << 8));
        const uint16_t micro = beatsin16(microMutatorFreq, now,
                                         microMutatorMin, microMutatorMax);

        // Widened before the multiply, since the product overflows after a few hours of uptime.
        const uint32_t huebase = static_cast<uint32_t>(static_cast<uint64_t>(now) * speed / 320);
        const int64_t  microDiv = static_cast<int64_t>(micro) + 1;  // guards a divide by zero

        // The cross term grows with the square of the grid, so it accumulates wide before the divide.
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int64_t spatial = static_cast<int64_t>(x)
                                      + static_cast<int64_t>(y) * static_cast<int64_t>(macro) * static_cast<int64_t>(x);
                const uint32_t hue = huebase + static_cast<uint32_t>(spatial / microDiv);
                const RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(hue), 255);
                draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, c);
            }
        }
    }

};

} // namespace mm
