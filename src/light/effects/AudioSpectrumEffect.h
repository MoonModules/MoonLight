#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// The 16 mic frequency bands spread across X, each column lit bottom-up by its magnitude.
/// @card AudioSpectrumEffect.gif
///
/// The classic equalizer display. The bands scale to any width, so a wide grid gives each several columns and a narrow one samples. On a grid at least three rows tall, the bottom rows can be a horizontal level bar under the spectrum, as tall as a bar is wide, with a square at its right end that flashes red on each detected hit. On a single row the bars collapse to per-column brightness.
///
/// @attribution Andrew Tuline, troyhacks
///
/// @guide
///
/// ## Comparing audio engines
///
/// The sweep colors with the level bar off draw what WLED-MM's GEQ PPA draws: the same bars, heights and color sweep, so two boards side by side differ only in their audio. Point one at the other's WLED audio sync to see both engines through this one effect.
///
/// @technical
///
/// This effect is the audio test instrument, so it shows the raw level rather than a smoothed one, which lags by tens of milliseconds. Bar `i` spans columns `i*w/16` up to `(i+1)*w/16` and rises `band*h/255` rows, as GEQ PPA computes them, so the two compare pixel for pixel.
class AudioSpectrumEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The coloring choices, in option order.
    enum ColorMode : uint8_t { kColorHeight, kColorPerBand, kColorSweep, kColorModeCount };

    /// By height (a VU), per band, or sweep: GEQ PPA's drifting rainbow.
    uint8_t colorMode = kColorPerBand;
    /// A level bar under the bars, and a square that flashes red on a hit.
    bool levelBar = true;

    /// Publish the coloring choice and the level bar.
    void defineControls() override {
        static constexpr const char* kColorOptions[] = {"height", "per-band", "sweep"};
        controls_.addSelect("colorMode", colorMode, kColorOptions, kColorModeCount);
        controls_.addControl("levelBar", levelBar);
    }

    /// Draw the level meter, then one bar per column from its band's magnitude.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        const lengthType h = height();

        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* f = AudioService::latestFrame();

        // The level meter takes the bottom rows, as many as a bar has columns, and never more than the spectrum keeps.
        const lengthType barW = static_cast<lengthType>(w >= 16 ? w / 16 : 1);
        const lengthType levelRows = levelBar ? std::min<lengthType>(barW, static_cast<lengthType>((h - 1) / 2)) : 0;
        const lengthType specH = static_cast<lengthType>(h - levelRows);

        const uint32_t ms = elapsed();
        if (f->onset) onsetUntilMs_ = ms + kOnsetFlashMs;
        if (levelRows) drawLevelMeter(cv, f->level, specH, ms < onsetUntilMs_);

        // The bars sit on the level meter, or on the floor without one.
        const lengthType specBottom = static_cast<lengthType>(specH - 1);
        for (uint8_t band = 0; band < 16; band++) {
            const uint8_t mag = f->bands[band];
            // The bar's height over the spectrum area, where a single row lights whole.
            const lengthType lit = (h == 1) ? 1 : static_cast<lengthType>(static_cast<uint32_t>(mag) * specH / 255u);
            const RGB sweep = colorMode == kColorSweep ? sweepColor(band, ms) : RGB{0, 0, 0};
            // This band's columns, which a narrow grid rounds to none for some bands.
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(band) * w / 16u);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(band + 1) * w / 16u);
            for (lengthType x = x0; x < x1; x++)
                draw::bar(cv, x, specBottom, lit, draw::Grow::Up,
                          [&](lengthType row) { return barColor(band, mag, row, specH, h == 1, sweep); });
        }
    }

private:
    /// How long the square stays red after a hit: an onset lasts one audio block, 23 ms, too short for the eye.
    static constexpr uint32_t kOnsetFlashMs = 100;
    uint32_t onsetUntilMs_ = 0;   ///< until when the square shows the last hit

    /// The level meter on the rows from `top` down: the raw level green to red, since a lag is wrong for judging response, and at its right end a square, red while a hit shows.
    void drawLevelMeter(const draw::Canvas& cv, uint16_t level, lengthType top, bool hit) MM_NONBLOCKING {
        const lengthType side = static_cast<lengthType>(height() - top);
        const lengthType w = width() > side ? static_cast<lengthType>(width() - side) : 0;   // the meter stops short of the square
        const lengthType litW = static_cast<lengthType>(static_cast<uint32_t>(level > 255 ? 255 : level) * w / 255u);
        for (lengthType y = top; y < height(); y++) {
            draw::bar(cv, 0, y, litW, draw::Grow::Right, [w](lengthType x) {
                const uint8_t frac = static_cast<uint8_t>(static_cast<uint32_t>(x) * 255u / (w > 1 ? w : 1));
                return RGB{frac, static_cast<uint8_t>(255 - frac), 0};
            });
            if (hit) draw::bar(cv, w, y, static_cast<lengthType>(width() - w), draw::Grow::Right, [](lengthType) { return RGB{255, 0, 0}; });
        }
    }

    /// GEQ PPA's color: three sines a third of a cycle apart, each bar 32 ms further along, one cycle a second.
    static RGB sweepColor(uint8_t band, uint32_t ms) MM_NONBLOCKING {
        const uint32_t timebase = band * 32u;
        return RGB{beatsin8(60, ms, 0, 255, timebase, 0), beatsin8(60, ms, 0, 255, timebase, 85),
                   beatsin8(60, ms, 0, 255, timebase, 170)};
    }

    /// One pixel of a bar by the coloring choice; on a single row the bar's brightness carries its magnitude.
    RGB barColor(uint8_t band, uint8_t mag, lengthType row, lengthType specH, bool oneRow, RGB sweep) const MM_NONBLOCKING {
        if (colorMode == kColorSweep)
            return oneRow ? RGB{scale8(sweep.r, mag), scale8(sweep.g, mag), scale8(sweep.b, mag)} : sweep;
        if (colorMode == kColorPerBand)   // the bands spread across the whole wheel
            return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 16), oneRow ? mag : 255);
        // Height: over the spectrum's own rows, the full height stops the top short of red.
        if (specH <= 1) return RGB{static_cast<uint8_t>(mag * mag / 255u), static_cast<uint8_t>((255u - mag) * mag / 255u), 0};
        const uint8_t frac = static_cast<uint8_t>(static_cast<uint32_t>(row) * 255u / (specH - 1));
        return RGB{frac, static_cast<uint8_t>(255 - frac), 0};
    }
};

} // namespace mm
