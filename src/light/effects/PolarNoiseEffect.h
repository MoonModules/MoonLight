#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// A warped noise field addressed by angle and radius, folded into a kaleidoscope.
/// @card PolarNoiseEffect.gif
///
/// Cost scales with `octaves` and `warp`: at `warp` above 0 and `octaves` 2 it is roughly 4 noise samples per pixel. On a large wall set `octaves` to 1 or `warp` to 0, which degrades to a plain polar noise that still reads well.
///
/// @attribution Stefan Petrick, Iñigo Quilez, Polar coordinates
///
/// @guide
///
/// ## polarTable
///
/// Each pixel's angle and radius come from a table at 2 bytes a pixel.
///
/// ## polarTable16
///
/// The table at full precision, 4 bytes a pixel.
///
/// @moreinfo
///
/// Three power functions compose it: `PolarLut` addresses the grid by angle and radius. `warp8` displaces the sample coordinate by a second field. `kaleido` folds the angle into mirrored wedges. The polar address never changes between frames, and computing it per pixel measured 39% of a frame on an S3.
class PolarNoiseEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️🌫️🎡"; }
    /// Volumetric: the field turns through depth.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the field drifts.
    uint8_t bpm      = 8;
    /// Noise cells across the grid; low is broad, high is fine detail.
    uint8_t scale    = 40;
    /// Kaleidoscope wedges; 1 disables the fold.
    uint8_t segments = 6;
    /// Domain-warp strength; 0 gives a plain field.
    uint8_t warp     = 90;
    /// Fbm octaves, and the main cost knob.
    uint8_t octaves  = 2;
    /// How much the radius shears the angle, setting the spiral.
    uint8_t twist    = 30;

    /// The polar address: the table (2 bytes a pixel, buying back the per-pixel atan16 and dist16), its precision, and the volumetric mapping.
    PolarLut::Controls polar;

    /// Publish the drift, the field's shape, the fold and the polar address.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("warp", warp, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("twist", twist, 0, 255);
        PolarLut::addControls(controls_, polar);
    }
    /// Build the polar address table for the current geometry.
    void prepare() override {
        // Built here rather than in tick(), so the render path never allocates.
        lut_.prepareFor(polar, width(), height(), EffectBase::depth());
    }


    /// Sample the warped field per pixel, folded into wedges around the center.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();

        // A sawtooth, so the field breathes outward rather than rocking back and forth.
        drift_.set(0, {.rate = bpm, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
        drift_.advanceTo(elapsed());
        const uint32_t t = drift_.unitValue(0);

        // Read from a table, or computed per pixel where it could not be allocated.
        const bool table = lut_.ready();
        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;

        std::size_t i = 0;
        for (lengthType z = 0; z < dep; z++)
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, i++) {
                // The polar address, and the radius in the pixel units the field is scaled in.
                angle16 a;
                uint32_t r;
                // The axis sampled through the fixture, which is 0 on a panel.
                int32_t along;
                if (table) {
                    a = lut_.angle(i);
                    r = lut_.radiusPixels(i);
                    along = lut_.mapping() == PolarLut::Mapping::Spherical
                          ? static_cast<int32_t>(lut_.pitch(i) >> 6)
                          : static_cast<int32_t>(z) - cz;
                } else {
                    // The same address the table would hold, so the composition matches either way.
                    const auto m = PolarLut::mappingOf(polar);
                    const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                        static_cast<int32_t>(y) - cy,
                                                        static_cast<int32_t>(z) - cz);
                    a = ad.angle;
                    r = ad.radius;
                    along = m == PolarLut::Mapping::Spherical ? static_cast<int32_t>(ad.pitch >> 6)
                                                              : static_cast<int32_t>(z) - cz;
                }

                // Shearing the angle by the radius turns concentric rings into spiral arms.
                a = static_cast<angle16>(a + (r * twist));

                // Folded before sampling, so the symmetry costs one modulo rather than a pass.
                a = kaleido(a, segments);

                // Sampled in angle and radius, with time on the radius so the pattern breathes.
                const uint32_t fx = (static_cast<uint32_t>(a) >> 6) * scale / 16u;
                const uint32_t fy = (r * scale) + (t >> 6);
                const uint32_t fz = static_cast<uint32_t>(along * static_cast<int32_t>(scale));

                const uint8_t v = warp > 0 ? warp8(fx, fy, fz, static_cast<uint16_t>(warp) * 4, octaves)
                                           : fbm8(fx, fy, fz, octaves);

                const RGB c = colorFromPalette(*Palettes::active(), v, 255);
                draw::pixel(cv, {x, y, z}, c);
            }
        }
    }

private:
    PolarLut            lut_{*this};   ///< the per-pixel angle and radius
    OscillatorBank<1>   drift_;        ///< the field's forward drift
};

}  // namespace mm
