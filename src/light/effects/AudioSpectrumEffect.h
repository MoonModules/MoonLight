#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// The mic's frequency bands spread across X, every third octave it measures or the sixteen, each column lit bottom-up by its magnitude.
/// @card AudioSpectrumEffect.gif
///
/// The classic equalizer display. The bands scale to any width, so a wide grid gives each several columns and a narrow one samples. On a single row the bars collapse to per-column brightness.
///
/// `readouts` draws the rest of the frame with the bars, each mark a bar wide: along the top, white over the dominant frequency; at the right edge, the height where the gate opens, white while open and red while the room is gated. On a grid at least three rows tall it adds a level meter under the bars, as tall as a bar is wide, with a square that flashes red on each hit.
///
/// @attribution Andrew Tuline, troyhacks
///
/// @guide
///
/// ## Comparing audio engines
///
/// The sweep colors with sixteen bands and the readouts off draw what WLED-MM's GEQ PPA draws: the same bars and heights, and its sweep's timing through the palette, so two boards side by side differ only in their audio. Point one at the other's WLED audio sync to see both engines through this one effect.
///
/// @technical
///
/// This effect is the audio test instrument, so it shows the raw level rather than a smoothed one, which lags by tens of milliseconds. A frame from audio sync carries the sixteen alone, so the third octaves fall back to them.
///
/// Of `n` bars, bar `i` spans columns `i*w/n` up to `(i+1)*w/n` and rises `band*h/255` rows, as GEQ PPA computes them for sixteen, so the two compare pixel for pixel.
///
/// The floor needs no line of its own: the window's bottom never sinks under it, so the bars stand on the floor.
class AudioSpectrumEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The coloring choices, in option order.
    enum ColorMode : uint8_t { kColorHeight, kColorPerBand, kColorSweep, kColorModeCount };

    /// The palette by height (a VU), per band, or as GEQ PPA's sweep.
    uint8_t colorMode = kColorPerBand;
    /// The level meter, hits, gate and peak, drawn with the bars.
    bool readouts = true;
    /// The bands to draw, in option order.
    enum Bands : uint8_t { kBands16, kBandsAll, kBandsCount };
    /// Every third octave the rate carries, or WLED-MM's sixteen.
    uint8_t bands = kBandsAll;
    /// The sweep's swings through the palette a minute.
    uint8_t speed = 60;

    /// Publish the coloring choice, the readouts and the band count.
    void defineControls() override {
        static constexpr const char* kColorOptions[] = {"height", "per-band", "sweep"};
        controls_.addSelect("colorMode", colorMode, kColorOptions, kColorModeCount);
        // defineControls reruns on every control change, so picking sweep shows its speed.
        controls_.addControl("speed", speed, 1, 255);
        controls_.setHidden(controls_.count() - 1, colorMode != kColorSweep);
        controls_.addControl("readouts", readouts);
        static constexpr const char* kBandOptions[] = {"16", "all"};
        controls_.addSelect("bands", bands, kBandOptions, kBandsCount);
    }

    /// Draw the level meter, one bar per column from its band's magnitude, then the gate and the peak over them.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        const lengthType h = height();

        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* f = AudioService::latestFrame();
        const bool thirds = bands == kBandsAll && f->thirdsCount > 0;
        const uint8_t n = thirds ? f->thirdsCount : 16;   // the bands the rate carries, 28 at the default 24 kHz

        // The level meter takes the bottom rows, as many as a bar has columns, and never more than the spectrum keeps.
        const lengthType barW = static_cast<lengthType>(w >= n ? w / n : 1);
        const lengthType levelRows = readouts ? std::min<lengthType>(barW, static_cast<lengthType>((h - 1) / 2)) : 0;
        const lengthType specH = static_cast<lengthType>(h - levelRows);

        const uint32_t ms = elapsed();
        if (f->onset) onsetUntilMs_ = ms + kOnsetFlashMs;
        if (levelRows) drawLevelMeter(cv, f->level, specH, ms < onsetUntilMs_);

        // The bars sit on the level meter, or on the floor without one.
        const lengthType specBottom = static_cast<lengthType>(specH - 1);
        for (uint8_t band = 0; band < n; band++) {
            const uint8_t mag = thirds ? f->thirds[band] : f->bands[band];
            // The bar's height over the spectrum area, where a single row lights whole.
            const lengthType lit = (h == 1) ? 1 : static_cast<lengthType>(static_cast<uint32_t>(mag) * specH / 255u);
            // The bar's one color, once per bar rather than per pixel; height varies it per row instead.
            const RGB whole = colorMode == kColorSweep ? sweepColor(band, ms, speed)
                            : colorMode == kColorPerBand ? colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 256u / n), 255)
                            : RGB{0, 0, 0};
            // This band's columns, which a narrow grid rounds to none for some bands.
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(band) * w / n);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(band + 1) * w / n);
            for (lengthType x = x0; x < x1; x++)
                draw::bar(cv, x, specBottom, lit, draw::Grow::Up,
                          [&](lengthType row) { return barColor(mag, row, specH, h == 1, whole); });
        }
        if (!readouts || h == 1) return;   // a single row has no room over its bars
        if (f->gate) drawGate(cv, f->gate, f->gateOpen, specH, barW);
        // The dominant frequency, while there is sound to have one.
        if (f->peakHz && (f->gate == 0 || f->gateOpen)) drawPeak(cv, f->peakHz, thirds, n, barW);
    }

    /// Where `hz` sits across `n` bars, in bars: the third-octave index, mapped between the sixteen when only those show.
    static float peakBar(uint16_t hz, bool thirds, uint8_t n) MM_NONBLOCKING {
        const float third = 17.0f + 3.0f * std::log2(static_cast<float>(hz) / 1000.0f);   // band 17 is 1 kHz
        if (thirds) return std::fmin(std::fmax(third, 0.0f), static_cast<float>(n - 1));
        if (third <= kSixteenOfThirds[0]) return 0.0f;
        for (uint8_t i = 0; i + 1 < 16; i++)
            if (third < kSixteenOfThirds[i + 1])
                return i + (third - kSixteenOfThirds[i]) / static_cast<float>(kSixteenOfThirds[i + 1] - kSixteenOfThirds[i]);
        return 15.0f;
    }

private:
    /// How long the square stays red after a hit: an onset lasts one audio block, 21 ms at 24 kHz, too short for the eye.
    static constexpr uint32_t kOnsetFlashMs = 100;
    uint32_t onsetUntilMs_ = 0;   ///< until when the square shows the last hit

    /// The level meter on the rows from `top` down: the raw level across the palette, since a lag is wrong for judging response, and at its right end a square, red while a hit shows.
    void drawLevelMeter(const draw::Canvas& cv, uint16_t level, lengthType top, bool hit) MM_NONBLOCKING {
        const lengthType side = static_cast<lengthType>(height() - top);
        const lengthType w = width() > side ? static_cast<lengthType>(width() - side) : 0;   // the meter stops short of the square
        const lengthType litW = static_cast<lengthType>(static_cast<uint32_t>(level > 255 ? 255 : level) * w / 255u);
        for (lengthType y = top; y < height(); y++) {
            draw::bar(cv, 0, y, litW, draw::Grow::Right, [w](lengthType x) {
                return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(static_cast<uint32_t>(x) * 255u / (w > 1 ? w : 1)), 255);
            });
            if (hit) draw::bar(cv, w, y, static_cast<lengthType>(width() - w), draw::Grow::Right, [](lengthType) { return RGB{255, 0, 0}; });
        }
    }

    /// The dominant frequency as a white mark `barW` wide along the top row, centered where it sits on the bars' axis.
    void drawPeak(const draw::Canvas& cv, uint16_t hz, bool thirds, uint8_t n, lengthType barW) MM_NONBLOCKING {
        const float center = (peakBar(hz, thirds, n) + 0.5f) * width() / n;
        const lengthType x = static_cast<lengthType>(std::fmin(std::fmax(center - barW / 2.0f, 0.0f), static_cast<float>(width() - barW)));
        draw::bar(cv, x, 0, barW, draw::Grow::Right, [](lengthType) { return RGB{255, 255, 255}; });
    }

    /// The row a bar of the gate's height reaches, as a mark `barW` wide at the right edge: white while open, red while shut.
    void drawGate(const draw::Canvas& cv, uint8_t gate, bool open, lengthType specH, lengthType barW) MM_NONBLOCKING {
        const lengthType rise = static_cast<lengthType>(static_cast<uint32_t>(gate) * specH / 255u);
        const lengthType y = static_cast<lengthType>(specH - (rise ? rise : 1));
        const RGB color = open ? RGB{255, 255, 255} : RGB{255, 0, 0};
        draw::bar(cv, static_cast<lengthType>(width() - barW), y, barW, draw::Grow::Right, [color](lengthType) { return color; });
    }

    /// GEQ PPA's sweep through the palette, `bpm` swings a minute, each bar 32 ms further along; GEQ PPA's own is 60.
    static RGB sweepColor(uint8_t band, uint32_t ms, uint8_t bpm) MM_NONBLOCKING {
        return colorFromPalette(*Palettes::active(), beatsin8(bpm, ms, 0, 255, band * 32u, 0), 255);
    }

    /// One pixel of a bar by the coloring choice, each from the palette; on a single row the bar's brightness carries its magnitude.
    RGB barColor(uint8_t mag, lengthType row, lengthType specH, bool oneRow, RGB whole) const MM_NONBLOCKING {
        if (colorMode != kColorHeight)   // sweep and per-band: one color per bar
            return oneRow ? RGB{scale8(whole.r, mag), scale8(whole.g, mag), scale8(whole.b, mag)} : whole;
        // Height: the palette up the spectrum's own rows, so the full height reaches the palette's end; a single row reads it at the magnitude.
        if (specH <= 1) return colorFromPalette(*Palettes::active(), mag, mag);
        return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(static_cast<uint32_t>(row) * 255u / (specH - 1)), 255);
    }
};

} // namespace mm
