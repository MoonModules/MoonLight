#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive effect: colors the layer from the 16-band FFT spectrum.
/// @card AudioSpectrumEffect.gif
/// Author: projectMM original, on the WLED-SR GEQ / spectrum-analyzer concept (Andrew Tuline), via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
///
/// The classic equalizer display: sixteen bands across x, each column lit from the bottom up.
/// The bands scale to any width, so a wide grid gives each several columns and a narrow one samples.
///
/// @moreinfo
///
/// ## The bottom row is a level meter
///
/// On a grid at least three rows tall, the bottom row becomes a horizontal level bar.
/// The spectrum bars then sit above it, and a shorter grid gives its whole height to them.
/// On a single row the bars collapse to per-column brightness instead.
///
/// This effect is the audio test instrument, so it shows the raw level rather than a smoothed one.
/// The smoothed value lags by tens of milliseconds, which is the calm look every other effect wants.
class AudioSpectrumEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// Color by height for the VU look, or per band, which reads as a spectrum at a glance.
    uint8_t colorMode = 1;

    /// Publish the coloring choice.
    void defineControls() override {
        static constexpr const char* kColorOptions[] = {"height", "per-band"};
        controls_.addSelect("colorMode", colorMode, kColorOptions, 2);
    }

    /// Draw the level meter, then one bar per column from its band's magnitude.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        const lengthType h = height();

        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* f = AudioService::latestFrame();

        // A tall enough grid reserves its bottom row for the level meter.
        const bool levelRow = (h >= 3);
        const lengthType specH = levelRow ? static_cast<lengthType>(h - 1) : h;

        if (levelRow) {
            const lengthType y = static_cast<lengthType>(h - 1);   // the bottom row
            // The raw level, as the bars above use, since a lag is wrong for judging response.
            const uint16_t vu = f->level;
            const lengthType litW = static_cast<lengthType>(
                static_cast<uint32_t>(vu > 255 ? 255 : vu) * w / 255u);
            // Green to red across the width, which is the VU-meter look.
            draw::bar(cv, 0, y, litW, draw::Grow::Right, [w](lengthType x) {
                const uint8_t frac = static_cast<uint8_t>(
                    static_cast<uint32_t>(x) * 255u / (w > 1 ? w : 1));
                return RGB{frac, static_cast<uint8_t>(255 - frac), 0};
            });
        }

        for (lengthType x = 0; x < w; x++) {
            // This column mapped onto one of the sixteen bands, at any width.
            const uint8_t band = static_cast<uint8_t>(
                static_cast<uint32_t>(x) * 16u / static_cast<uint32_t>(w));
            const uint8_t mag = f->bands[band];

            // The bar's height over the spectrum area, where a single row lights whole.
            const lengthType lit = (h == 1)
                ? 1
                : static_cast<lengthType>(static_cast<uint32_t>(mag) * specH / 255u);

            // The bands spread across the whole wheel, so each column takes its own color.
            const uint8_t bandHue = static_cast<uint8_t>(band * 16);

            // The bars sit above the level row when one is reserved.
            const lengthType specBottom = static_cast<lengthType>(levelRow ? h - 2 : h - 1);
            draw::bar(cv, x, specBottom, lit, draw::Grow::Up, [&](lengthType row) {
                uint8_t r, g, b;
                if (colorMode == 1) {
                    // The column's own hue at full brightness, or dimmed by magnitude on one row.
                    const uint8_t v = (h == 1) ? mag : 255;
                    const RGB c = colorFromPalette(*Palettes::active(), bandHue, v);
                    r = c.r; g = c.g; b = c.b;
                } else {
                    // Over the spectrum's own rows: the full height stops the top short of red.
                    const uint8_t frac = (specH > 1)
                        ? static_cast<uint8_t>(static_cast<uint32_t>(row) * 255u / (specH - 1))
                        : mag;
                    r = (specH > 1) ? frac : static_cast<uint8_t>(static_cast<uint32_t>(frac) * mag / 255u);
                    g = (specH > 1) ? static_cast<uint8_t>(255 - frac)
                                    : static_cast<uint8_t>(static_cast<uint32_t>(255 - frac) * mag / 255u);
                    b = 0;
                }
                return RGB{r, g, b};
            });
        }
    }
};

} // namespace mm
