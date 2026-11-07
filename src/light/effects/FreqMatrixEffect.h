#pragma once

#include "core/util/math16.h"            // log2Q8: the pitch's octave
#include "light/effects/EffectBase.h"

namespace mm {

/// A 1D scrolling frequency display: each frame shifts the strip and injects a new pixel at one end.
/// @card FreqMatrixEffect.gif
///
/// The new pixel's hue comes from the dominant frequency and its brightness from the volume, so pitch becomes color.
///
/// @attribution Andrew Tuline
///
/// @technical
///
/// The column is the shift register: each pass reads pixel y-1 into pixel y and writes the new color at y=0, so no history buffer is needed. It scrolls about `speed` × 60 / 255 rows a second, one step for each period elapsed, so a slow render takes several steps at once; after a pause of a quarter second it resumes rather than repainting the column in one color.
///
/// ## Pitch on a log scale
///
/// Pitch is logarithmic, so the window from `lowHz` to `highHz` is spread over the palette octave by octave: from 50 Hz to 5 kHz the bass, the voice and the cymbals each take their own part of it. A peak outside the window takes the end color.
class FreqMatrixEffect : public EffectBase {
public:
    /// Catalog tags: WLED origin, audio-reactive.
    const char* tags() const override { return "🐙🎶"; }
    /// Writes the x=0 column along y, which extrude fans across a wider layer.
    Dim dimensions() const override { return Dim::D1; }

    /// Scroll rate, 255 being about 60 rows a second.
    uint8_t speed       = 255;
    /// Brightness from the level, 128 the level itself.
    uint8_t fx          = 192;
    /// The frequency at the palette's start.
    uint16_t lowHz      = 50;
    /// The frequency at the palette's end.
    uint16_t highHz     = 5000;
    /// Let the volume modulate the scroll speed.
    bool    audioSpeed  = false;

    /// Publish the scroll rate, the brightness response and the frequency window.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("fx", fx, 0, 255);
        controls_.addControl("lowHz", lowHz, 20, 10000);   // the bands stop at 10 kHz at the default rate
        controls_.addControl("highHz", highHz, 20, 10000);
        controls_.addControl("audioSpeed", audioSpeed);
    }

    /// Scroll the column when the throttle allows, and paint the new pixel from the peak frequency.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();

        const AudioFrame* f = AudioService::latestFrame();

        // A row's period in milliseconds, 255 at about 60 rows a second; the level doubles the rate at most.
        uint32_t period = kRowsMsAt1 / (speed ? speed : 1);
        if (audioSpeed) {
            const uint32_t lvl = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
            period = period * 255u / (255u + lvl);
        }
        if (period == 0) period = 1;
        // One step per period elapsed, so the scroll keeps its speed at any frame rate.
        const uint32_t now = elapsed();
        uint32_t steps = (now - lastScrollMs_) / period;
        if (steps == 0) return;
        const uint32_t rows = height();
        if (steps > 1 && now - lastScrollMs_ >= kPauseMs) { steps = 1; lastScrollMs_ = now; }   // a pause resumes, rather than repainting the column in one color
        else { lastScrollMs_ += steps * period; if (steps > rows) steps = rows; }

        // The smoothed level, since this is a flowing scroll rather than a transient meter.
        const uint32_t level = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
        const uint32_t pixVal = level * fx / 128u;
        const uint8_t bri = static_cast<uint8_t>(pixVal > 255 ? 255 : pixVal);

        // Black unless a real tone sits above 80 Hz while there is sound; the pitch picks the color and the level its brightness.
        RGB newColor{0, 0, 0};
        if (f->peakHz > 80 && f->heard())
            newColor = colorFromPalette(*Palettes::active(), paletteIndex(f->peakHz), bri);

        // Shift the column away from the source end, then paint the rows it freed.
        draw::scroll(cv, /*axis=*/1, static_cast<int>(steps));
        for (uint32_t y = 0; y < steps; y++) draw::pixel(cv, {0, static_cast<lengthType>(y), 0}, newColor);
    }

    /// The palette index of a pitch: its place in the window from `lowHz` to `highHz`, octave by octave, the ends beyond it: @xref{pitch-on-a-log-scale}.
    uint8_t paletteIndex(uint16_t hz) const {
        const uint32_t a = log2Q8(lowHz), b = log2Q8(highHz), p = log2Q8(hz);
        const uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
        if (hi == lo || p <= lo) return 0;
        if (p >= hi) return 255;
        return static_cast<uint8_t>((p - lo) * 255u / (hi - lo));
    }

private:
    static constexpr uint32_t kRowsMsAt1 = 255u * 1000u / 60u;   ///< a row's period at speed 1, so speed 255 scrolls about 60 rows a second
    static constexpr uint32_t kPauseMs = 250;                    ///< a gap this long is a pause, which the scroll resumes from rather than replays
    uint32_t lastScrollMs_ = 0;   ///< when the column last scrolled, which is the throttle's state
};

} // namespace mm
