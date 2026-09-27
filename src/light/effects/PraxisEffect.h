#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Algorithmic palette-pattern effect driven by two beat oscillators.
/// @card PraxisEffect.gif
/// Author: MONSOONO / @Flavourdynamics (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// Each pixel's hue comes from two oscillating mutators combined with its own position.
/// So the pattern continually stretches, shears and rolls across the grid.
///
/// Prior art: MoonLight's Praxis, whose two-mutator model this reproduces.
///
/// @moreinfo
///
/// ## What each mutator does
///
/// The micro mutator divides the spatial term, so it sets the pattern's spatial frequency.
/// The macro mutator multiplies the cross term instead, which is what warps the field.
/// A steadily advancing hue base then scrolls the whole thing through the palette.
class PraxisEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the hue scrolls, which is this port's own addition to the source.
    uint8_t speed = 4;

    // Defaults and ranges match MoonLight's own Praxis.
    /// The macro mutator's beat frequency.
    uint8_t macroMutatorFreq = 3;
    /// Its low end, scaled into the wide sweep.
    uint8_t macroMutatorMin  = 250;
    /// And its high end.
    uint8_t macroMutatorMax  = 255;
    /// The micro mutator's beat frequency.
    uint8_t microMutatorFreq = 4;
    /// Its low end.
    uint8_t microMutatorMin  = 200;
    /// And its high end.
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
