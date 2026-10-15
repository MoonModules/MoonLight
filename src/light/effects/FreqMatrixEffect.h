#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"

namespace mm {

/// A 1D scrolling frequency display: each frame shifts the strip and injects a new pixel at one end.
/// @card FreqMatrixEffect.gif
///
/// The new pixel's hue comes from the dominant frequency and its brightness from the volume, so pitch becomes color.
///
/// @attribution Andrew Tuline
///
/// @moreinfo
///
/// The column is the shift register: each pass reads pixel y-1 into pixel y and writes the new color at y=0, so no history buffer is needed. The volume is a 0..255 integer, so WLED's 0.25 threshold is 64 and its 256.0 divisor is 2560.
class FreqMatrixEffect : public EffectBase {
public:
    /// Catalog tags: WLED origin, audio-reactive.
    const char* tags() const override { return "🐙🎶"; }
    /// Writes the x=0 column along y, which extrude fans across a wider layer.
    Dim dimensions() const override { return Dim::D1; }

    /// Scroll rate.
    uint8_t speed       = 255;
    /// Sound-effect intensity, scaling the injected brightness.
    uint8_t fx          = 128;
    /// The lower edge of the frequency window mapped across the hue range.
    uint8_t lowBin      = 18;
    /// The upper edge of that window.
    uint8_t highBin     = 48;
    /// Input gain.
    uint8_t sensitivity = 30;
    /// Let the volume modulate the scroll speed.
    bool    audioSpeed  = false;

    /// Publish the scroll rate, the brightness response and the frequency window.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("fx", fx, 0, 255);
        controls_.addControl("lowBin", lowBin, 0, 255);
        controls_.addControl("highBin", highBin, 0, 255);
        controls_.addControl("sensitivity", sensitivity, 10, 100);
        controls_.addControl("audioSpeed", audioSpeed);
    }

    /// Scroll the column when the throttle allows, and paint the new pixel from the peak frequency.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // the silence frame is non-null in practice, but guard for safety

        // The throttle period in milliseconds, so a high speed scrolls almost every frame.
        uint32_t period = static_cast<uint32_t>(256 - speed);
        if (audioSpeed) {
            const uint32_t lvl = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
            period = period > (lvl >> 2) ? period - (lvl >> 2) : 1;       // louder scrolls faster
        }
        if (period == 0) period = 1;
        const uint32_t now = elapsed();
        if (now - lastScrollMs_ < period) return;                         // not time to scroll
        lastScrollMs_ = now;

        // The smoothed volume, since this is a flowing scroll rather than a transient meter.
        const uint32_t level = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
        uint32_t pixVal = (level * fx * sensitivity) / 2560u;
        if (pixVal > 255) pixVal = 255;
        const uint8_t bri = static_cast<uint8_t>(pixVal);

        // Black unless a real tone sits above 80 Hz and the volume above a quarter scale.
        RGB newColor{0, 0, 0};
        if (f->peakHz > 80 && level > 64) {
            // The frequency window mapped onto a palette index, WLED's own limits.
            const int upperLimit = 80 + 42 * static_cast<int>(highBin);
            const int lowerLimit = 80 + 3 * static_cast<int>(lowBin);
            int idx;
            if (lowerLimit != upperLimit)
                idx = map32(static_cast<int>(f->peakHz), lowerLimit, upperLimit, 0, 255);
            else
                idx = static_cast<int>(f->peakHz);
            if (idx < 0) idx = -idx;                     // a peak below the window still maps
            const uint8_t i = static_cast<uint8_t>(idx & 0xFF);
            newColor = colorFromPalette(*Palettes::active(), i, bri);
        }

        // Shift the column away from the source end, then paint the new color at its head.
        draw::scroll(cv, /*axis=*/1, 1);
        draw::pixel(cv, {0, 0, 0}, newColor);
    }

private:
    uint32_t lastScrollMs_ = 0;   ///< when the column last scrolled, which is the throttle's state
};

} // namespace mm
