#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Author: MoonLight original (rotating spiral)
/// Effect winding a lit spiral up a conical layout.
/// @card SpiralEffect.gif
///
/// Each pixel's hue comes from its angle plus its radius times `twist`, walked by the clock.
/// So the arms wind outward, and turning `twist` up tightens them.
class SpiralEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, David Jupijn and Rising Step.
    const char* tags() const override { return "💫🦅🖌️🎡"; }
    /// Iterates y and x, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the spiral turns.
    uint8_t bpm = 40;
    /// How tightly the arms wind, as hue per unit radius.
    uint8_t twist = 4;
    /// Walks every arm around the palette.
    uint8_t hue_shift = 0;

    /// The polar address: the table, its precision, and how a volumetric fixture maps to angle and radius.
    PolarLut::Controls polar;

    /// Publish the rotation, the winding and the polar address.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("twist", twist, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
        PolarLut::addControls(controls_, polar);
    }
    /// Build the polar address table for the current geometry.
    void prepare() override {
        // Built here rather than in tick(), so the render path never allocates.
        lut_.prepareFor(polar, width(), height(), depth());
    }


    /// Walk the spiral's phase, then color each pixel from its own angle and radius.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        uint32_t now = elapsed();
        // BeatPhase keeps its numerator wide until the read, so a fast frame cannot round to zero.
        phase_.advanceTo(now, bpm);
        uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        int16_t cx = static_cast<int16_t>(w >> 1);
        int16_t cy = static_cast<int16_t>(h >> 1);

        // Read from a table, or computed per pixel where the memory cannot be spared.
        const bool table = lut_.ready();

        std::size_t i = 0;
        for (lengthType y = 0; y < h; y++) {
            int16_t dy = static_cast<int16_t>(y) - cy;
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++, i++) {
                int16_t dx = static_cast<int16_t>(x) - cx;
                // A true radius, since the 8-bit approximation is an octagon with visible corners.
                uint8_t angle, dist;
                if (table) {
                    angle = static_cast<uint8_t>(lut_.angle(i) >> 8);
                    dist  = static_cast<uint8_t>(lut_.radiusPixels(i));
                } else {
                    angle = static_cast<uint8_t>(atan16(dy, dx) >> 8);
                    dist  = static_cast<uint8_t>(dist16(dx, dy));
                }
                uint8_t hue = static_cast<uint8_t>(
                    angle + static_cast<uint8_t>(dist * twist) - t + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue);

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    PolarLut lut_{*this};   ///< the per-pixel angle and radius
    BeatPhase phase_;       ///< the rotation clock
};

} // namespace mm
