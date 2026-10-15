#pragma once

#include "core/util/math16.h"              // BeatPhase, sin16: the shared time base and oscillator
#include "light/effects/EffectBase.h"

namespace mm {

/// A circle and a box orbit and melt into each other, drawn as signed distance fields rather than outlines.
/// @card SdfShapesEffect.gif
///
/// One distance per pixel yields three looks at once: a smooth fill, an outline, and a glow falling off into the field. The two shapes flow together like mercury rather than merely overlapping.
///
/// @attribution Iñigo Quilez
///
/// @technical
///
/// ## What one number buys
///
/// Each pixel asks the distance to a circle and to a box, and reads the fill, the outline and the glow off that one value. A rasterizer would need separate code for each.
///
/// ## The dimension proof
///
/// `length(p) - r` is a circle on a matrix and a sphere in a volume, from identical code, so z collapses on a 2D layer. Measured on an S3 at 128×128: 20 fps, 728 cycles per pixel, the palette lookup dominating.
class SdfShapesEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// Orbit speed.
    uint8_t bpm = 12;
    /// Circle radius, as a fraction of the short side.
    uint8_t radius = 60;
    /// Box half-extent, on the same scale.
    uint8_t boxSize = 45;
    /// The melt radius, where 0 unions the shapes hard.
    uint8_t blend = 90;
    /// Outline width, where 0 fills the shape.
    uint8_t outline = 0;
    /// Tint the surrounding field by its distance from the shape.
    bool    glow = true;

    /// Publish the orbit, both shapes, the melt and the two draw styles.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 120);
        controls_.addControl("radius", radius, 0, 255);
        controls_.addControl("boxSize", boxSize, 0, 255);
        controls_.addControl("blend", blend, 0, 255);
        controls_.addControl("outline", outline, 0, 64);
        controls_.addControl("glow", glow);
    }

    /// Ask each pixel its distance to both shapes, then read the fill, outline and glow off it.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const angle16 t = static_cast<angle16>(phase_.phase(65536));

        // The short side sets the scale, so the composition looks the same on any aspect ratio.
        const lengthType shortSide = w < h ? w : h;
        const draw::pos_t scale = draw::toSub(shortSide);

        // Two orbits in antiphase, so the melt happens at the crossing rather than a fixed point.
        const draw::pos_t cxA = midX(w) + oscillate(t, scale / 4);
        const draw::pos_t cyA = midY(h) + oscillate(static_cast<angle16>(t + 16384), scale / 6);
        const draw::pos_t cxB = midX(w) - oscillate(t, scale / 4);
        const draw::pos_t cyB = midY(h) - oscillate(static_cast<angle16>(t + 16384), scale / 6);

        const draw::pos_t r  = (scale * radius) / 1020;    // /1020 = /255/4: 255 is a quarter of the side
        const draw::pos_t bs = (scale * boxSize) / 1020;
        const int32_t k = static_cast<int32_t>((scale * blend) / 2040);
        const draw::pos_t outlineW = draw::toSub(1) * outline / 8;

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                const draw::pos_t px = draw::toSub(x), py = draw::toSub(y);
                // True distances, since the outline width and the glow falloff need a real one.
                const int32_t dCircle = draw::sdCircle(px, py, cxA, cyA, r);
                const int32_t dBox    = draw::sdBox(px, py, cxB, cyB, bs, bs);
                int32_t d = draw::smin(dCircle, dBox, k);

                // The same distance folded, which is negative only in a band around the edge.
                if (outlineW > 0) d = (d < 0 ? -d : d) - outlineW;

                const uint8_t cov = draw::coverage(d);
                // Written black rather than skipped: the Layer holds last frame, path and all.
                if (cov == 0 && !glow) { draw::pixel(cv, {x, y, 0}, RGB{0, 0, 0}); continue; }

                // The index rides the distance, so the shape reads as a lit body with a falling field.
                const int32_t dPix = d >> draw::kSubShift;
                const uint8_t idx = static_cast<uint8_t>((t >> 8) + static_cast<uint8_t>(dPix * 4));
                const uint8_t bri = glow ? (cov > 0 ? cov : glowFalloff(dPix)) : cov;
                if (bri == 0) { draw::pixel(cv, {x, y, 0}, RGB{0, 0, 0}); continue; }

                const RGB c = colorFromPalette(*Palettes::active(), idx, bri);
                draw::pixel(cv, {x, y, 0}, c);
            }
        }
    }

private:
    /// A signed swing around zero, from the shared 16-bit sine.
    static draw::pos_t oscillate(angle16 a, draw::pos_t amp) {
        const int32_t s = static_cast<int32_t>(sin16(a));   // -32768..32767
        return static_cast<draw::pos_t>((static_cast<int64_t>(s) * amp) / 32768);
    }
    static draw::pos_t midX(lengthType w) { return draw::toSub(w) / 2; }
    static draw::pos_t midY(lengthType h) { return draw::toSub(h) / 2; }

    /// The field outside the shape: bright near the edge and dark further out, on a cheap falloff.
    static uint8_t glowFalloff(int32_t dPixels) {
        if (dPixels <= 0) return 255;
        if (dPixels > 16) return 0;
        return static_cast<uint8_t>(255 / (1 + dPixels * dPixels / 2));
    }

    BeatPhase phase_;   ///< the orbit clock
};

}  // namespace mm
