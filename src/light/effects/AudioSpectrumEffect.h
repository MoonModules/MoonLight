#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// The 16 mic frequency bands spread across X, each column lit bottom-up by its magnitude.
/// @card AudioSpectrumEffect.gif
///
/// The classic equalizer display. The bands scale to any width, so a wide grid gives each several columns and a narrow one samples. On a grid at least three rows tall, the bottom row becomes a horizontal level bar and the spectrum bars sit above it. On a single row the bars collapse to per-column brightness.
///
/// @attribution Andrew Tuline
///
/// @moreinfo
///
/// This effect is the audio test instrument, so it shows the raw level rather than a smoothed one, which lags by tens of milliseconds.
class AudioSpectrumEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The coloring choices, in option order.
    enum ColorMode : uint8_t { kColorHeight, kColorPerBand, kColorModeCount };

    /// Bars colored by height, the VU look, or per band, a rainbow.
    uint8_t colorMode = kColorPerBand;

    /// Publish the coloring choice.
    void defineControls() override {
        static constexpr const char* kColorOptions[] = {"height", "per-band"};
        controls_.addSelect("colorMode", colorMode, kColorOptions, kColorModeCount);
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
                if (colorMode == kColorPerBand) {
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
