#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// A texture mapped onto the inside of an infinite tube, so the viewer appears to fly down it forever.
/// @card TunnelEffect.gif
///
/// Nothing is 3D: the angle around the center is one texture coordinate and the reciprocal of the distance is the other, which is perspective for the price of a divide.
///
/// @attribution Stefan Petrick, Iñigo Quilez
///
/// @moreinfo
///
/// The reciprocal grows without bound toward the center, so the texture compresses to a vanishing point, and adding time to it pulls the wall toward the viewer. The divide is the expensive part of the per-pixel cost.
class TunnelEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️🌫️🎡"; }
    /// Volumetric: the wall recedes through depth.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the tunnel flies past.
    uint8_t bpm      = 20;
    /// Texture scale along the tunnel; higher is finer rings.
    uint8_t depth    = 60;
    /// Rotation per unit depth, so the tunnel corkscrews.
    uint8_t twist    = 40;
    /// Kaleidoscope the wall, where 1 leaves it plain.
    uint8_t segments = 1;
    /// Wall texture detail, and the cost knob.
    uint8_t octaves  = 2;
    /// Darken toward the vanishing point, so it reads as receding.
    bool    vignette = true;

    /// The polar address: the table, its precision, and how a volumetric fixture maps to angle and radius.
    PolarLut::Controls polar;

    /// Publish the flight, the wall's texture and the polar address.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 120);
        controls_.addControl("depth", depth, 1, 255);
        controls_.addControl("twist", twist, 0, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("vignette", vignette);
        PolarLut::addControls(controls_, polar);
    }
    /// Build the polar address table for the current geometry.
    void prepare() override {
        // Built here rather than in tick(), so the render path never allocates.
        lut_.prepareFor(polar, width(), height(), EffectBase::depth());
    }


    /// Map the wall per pixel from its angle and the reciprocal of its distance.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = EffectBase::depth();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);

        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;

        // Read from a table, or computed per pixel where the memory cannot be spared.
        const bool table = lut_.ready();

        std::size_t i = 0;
        for (lengthType z = 0; z < dep; z++)
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, i++) {
                uint32_t r;
                angle16 a;
                // How far along the tube this light sits, so each slice shows its own ring.
                int32_t along;
                if (table) {
                    a = lut_.angle(i);
                    r = lut_.radiusPixels(i);
                    along = lut_.mapping() == PolarLut::Mapping::Spherical
                          ? static_cast<int32_t>(lut_.pitch(i) >> 6)
                          : static_cast<int32_t>(z) - cz;
                } else {
                    // The same address the table would have held, under the same mapping.
                    const auto m = PolarLut::mappingOf(polar);
                    const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                        static_cast<int32_t>(y) - cy,
                                                        static_cast<int32_t>(z) - cz);
                    a = ad.angle;
                    r = ad.radius;
                    along = m == PolarLut::Mapping::Spherical ? static_cast<int32_t>(ad.pitch >> 6)
                                                              : static_cast<int32_t>(z) - cz;
                }

                // 1/r is the depth coordinate, and the +1 keeps the center pixel off a divide by zero.
                const uint32_t depthCoord = (static_cast<uint32_t>(depth) * 4096u) / (r + 1);

                // Rotating by depth turns each ring a little more than the one behind it.
                a = static_cast<angle16>(a + ((depthCoord * twist) >> 6));
                a = kaleido(a, segments);

                // Sampled in angle and depth, with time pulling that depth toward the viewer.
                const uint32_t u = (static_cast<uint32_t>(a) >> 5);
                // Depth down the tube adds to the texture's own, and is 0 on a panel.
                const uint32_t v = depthCoord + (t >> 5) + static_cast<uint32_t>(along * 256);
                const uint8_t tex = fbm8(u, v, octaves);

                // Vignette by distance so the center reads as far away rather than merely small.
                uint8_t bri = 255;
                if (vignette) {
                    const uint32_t maxR = static_cast<uint32_t>(cx > cy ? cx : cy) + 1;
                    const uint32_t rel = r > maxR ? 255u : (r * 255u) / maxR;
                    bri = static_cast<uint8_t>(rel < 20 ? 20 : rel);
                }

                draw::pixel(cv, {x, y, z}, colorFromPalette(*Palettes::active(), tex, bri));
            }
        }
    }

private:
    PolarLut  lut_{*this};   ///< the per-pixel angle and radius
    BeatPhase phase_;        ///< the flight clock
};

}  // namespace mm
