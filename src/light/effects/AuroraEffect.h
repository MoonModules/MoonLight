#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Several noise fields, each on its own clock, read in polar coordinates and composited into curtains of light.
/// @card AuroraEffect.gif
///
/// Nothing is simulated: layers of one field at different scales interfere, and that is what reads as curtains folding through one another. `polarTable` and `polarTable16` choose the polar address table and its precision.
///
/// @attribution Stefan Petrick
///
/// @moreinfo
///
/// `PolarLut` addresses the field around the center, `OscillatorBank` keeps the layers' phases related, and `fbm8` over gradient noise is the field. One fbm runs per layer per pixel.
class AuroraEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️🌫️🎡"; }
    /// Volumetric: the curtains have depth.
    Dim dimensions() const override { return Dim::D3; }

    /// The compositing ceiling, and the width of the oscillator bank.
    static constexpr uint8_t kMaxLayers = 4;

    /// Master rate, every layer scales from it; 0 freezes it.
    uint8_t speed    = 30;
    /// Noise cells across the grid; low is broad, high is fine.
    uint8_t scale    = 40;
    /// How many fields are composited, and the main cost knob.
    uint8_t layers   = 3;
    /// How far the field displaces its own sample angle.
    uint8_t warp     = 60;
    /// How much the radius shears the angle, giving the curtains their lean.
    uint8_t twist    = 40;
    /// Kaleidoscope wedges; 1 leaves the composition unfolded.
    uint8_t segments = 1;
    /// How much of the field lights; low is cloud, high is sharp.
    uint8_t contrast = 140;
    /// Detail within each layer, multiplying the cost knob.
    uint8_t octaves  = 2;

    /// The polar address: the table, its precision, and how a volumetric fixture maps to angle and radius.
    PolarLut::Controls polar;

    /// Publish the layers, the field's shape, the visibility window and the polar address.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 120);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("layers", layers, 1, kMaxLayers);
        controls_.addControl("warp", warp, 0, 255);
        controls_.addControl("twist", twist, 0, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("contrast", contrast, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
        PolarLut::addControls(controls_, polar);
    }

    /// Build the polar address table for the current geometry.
    void prepare() override {
        // Built here rather than in tick(), so the render path never allocates.
        lut_.prepareFor(polar, width(), height(), EffectBase::depth());
    }

    /// Composite every layer through the visibility window, one pixel at a time.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();
        const uint8_t n = layers < 1 ? 1 : (layers > kMaxLayers ? kMaxLayers : layers);

        // Three oscillators a layer: drift, breathing scale and rotation, at rates sharing no multiple.
        for (uint8_t i = 0; i < kMaxLayers; i++) {
            const uint16_t rate = static_cast<uint16_t>(speed) * (7 + i * 5) / 10;
            bank_.set(static_cast<uint8_t>(i * 3 + 0),
                      {.rate = rate, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
            bank_.set(static_cast<uint8_t>(i * 3 + 1),
                      {.rate = static_cast<uint16_t>(rate / 3), .low = 192, .high = 320,
                       .phaseOffset = static_cast<angle16>(i * 12000), .wave = Wave::Sine});
            bank_.set(static_cast<uint8_t>(i * 3 + 2),
                      {.rate = static_cast<uint16_t>(rate / 5), .low = -8192, .high = 8192,
                       .phaseOffset = static_cast<angle16>(i * 20000), .wave = Wave::Sine});
        }
        bank_.advanceTo(elapsed());

        const bool table = lut_.ready();
        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;

        // Hoist everything constant across the frame: the pixel loop should read, not compute.
        uint32_t drift[kMaxLayers];
        uint32_t layerScale[kMaxLayers];
        int32_t  rotate[kMaxLayers];
        for (uint8_t i = 0; i < n; i++) {
            drift[i]      = bank_.unitValue(static_cast<uint8_t>(i * 3 + 0));
            layerScale[i] = static_cast<uint32_t>(bank_.value(static_cast<uint8_t>(i * 3 + 1))) * scale / 256u;
            rotate[i]     = bank_.value(static_cast<uint8_t>(i * 3 + 2));
            if (layerScale[i] == 0) layerScale[i] = 1;
        }

        // The window sits at `contrast` up the field's own range, measured on the previous frame.
        const uint8_t lowEnd = floor_ < peak_ ? floor_ : static_cast<uint8_t>(peak_ > 8 ? peak_ - 8 : 0);
        const uint8_t threshold = static_cast<uint8_t>(
            lowEnd + (static_cast<uint32_t>(peak_ - lowEnd) * contrast) / 272u);
        uint8_t frameMax = 0, frameMin = 255;

        std::size_t idx = 0;
        for (lengthType z = 0; z < dep; z++)
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, idx++) {
                angle16 baseAngle;
                uint32_t r;
                // The depth the field samples along, which is what separates a cylinder's slices.
                int32_t along;
                if (table) {
                    baseAngle = lut_.angle(idx);
                    r = lut_.radiusPixels(idx);
                    along = lut_.mapping() == PolarLut::Mapping::Spherical
                          ? static_cast<int32_t>(lut_.pitch(idx) >> 6)
                          : static_cast<int32_t>(z) - cz;
                } else {
                    // The same address the table would have held, under the same mapping.
                    const auto m = PolarLut::mappingOf(polar);
                    const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                        static_cast<int32_t>(y) - cy,
                                                        static_cast<int32_t>(z) - cz);
                    baseAngle = ad.angle;
                    r = ad.radius;
                    along = m == PolarLut::Mapping::Spherical ? static_cast<int32_t>(ad.pitch >> 6)
                                                              : static_cast<int32_t>(z) - cz;
                }

                // The strongest layer wins: a sum would average the curtains into an even haze.
                uint8_t best = 0;
                uint8_t winner = 0;
                for (uint8_t i = 0; i < n; i++) {
                    // Each layer leans its own way and turns at its own rate.
                    angle16 a = static_cast<angle16>(baseAngle + (r * twist) + rotate[i]);
                    a = kaleido(a, segments);

                    const uint32_t fx = (static_cast<uint32_t>(a) >> 6) * layerScale[i] / 16u;
                    const uint32_t fy = (r * layerScale[i]) + (drift[i] >> 6) + i * 4096u;
                    // The field's third axis, which is 0 on a panel and reduces to the 2D sample.
                    const uint32_t fz = static_cast<uint32_t>(along * static_cast<int32_t>(layerScale[i]));

                    // The field displaces its own sample angle, so a curtain folds over itself.
                    const uint8_t v = warp > 0 ? warp8(fx, fy, fz, static_cast<uint16_t>(warp) * 4, octaves)
                                               : fbm8(fx, fy, fz, octaves);
                    if (v > best) { best = v; winner = i; }
                }

                // Below the threshold is dark, and above it stretches over the full range.
                uint8_t bri = 0;
                if (best > threshold) {
                    const uint32_t span = peak_ > threshold ? peak_ - threshold : 1u;
                    const uint32_t over = static_cast<uint32_t>(best - threshold) * 255u / span;
                    bri = static_cast<uint8_t>(over > 255u ? 255u : over);
                }
                if (best > frameMax) frameMax = best;
                if (best < frameMin) frameMin = best;

                // Which layer won picks the palette region, and nothing else enters the index.
                const uint8_t index = static_cast<uint8_t>((winner * 255u) / n);
                draw::pixel(cv, {x, y, z}, colorFromPalette(*Palettes::active(), index, bri));
            }
        }

        // Eased rather than assigned, so one bright frame cannot make the composition flinch.
        peak_  = static_cast<uint8_t>((peak_ * 7u + (frameMax < 32 ? 32 : frameMax)) / 8u);
        floor_ = static_cast<uint8_t>((floor_ * 7u + frameMin) / 8u);
    }

private:
    PolarLut                        lut_{*this};
    OscillatorBank<kMaxLayers * 3>  bank_;
    uint8_t                         peak_ = 200;   ///< the field's high water mark, eased per frame
    uint8_t                         floor_ = 40;   ///< and its low, so the window spans what is there
};

}  // namespace mm
