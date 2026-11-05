#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// A spectrogram scrolling down the grid: each band a column, the newest moment on top, its color and brightness from the band's magnitude.
/// @card WaterfallEffect.gif
///
/// A held note draws a line down the grid, a sweep a diagonal, a drum hit a sharp edge across it, so the music's shape over the last seconds stays in view. The 31 third octaves when the mic measures them, the sixteen otherwise.
///
/// @technical
///
/// The grid is the history: each step scrolls every row down one and paints the newest bands into the top row, so no buffer is kept. Of `n` bands, column `x` shows band `x*n/w`.
class WaterfallEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the history flows down, a step every 256 - speed milliseconds.
    uint8_t speed = 230;

    /// Publish the flow speed.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
    }

    /// Scroll the history when its step is due, then paint the newest bands on top.
    void tick() MM_NONBLOCKING override {
        // The steps due since the last, the fraction carried, so a row is the same time on any device and at any frame rate.
        const uint32_t period = static_cast<uint32_t>(256 - speed);
        const uint32_t now = elapsed();
        const uint32_t due = (now - lastStepMs_) / period;
        if (due == 0) return;
        lastStepMs_ += due * period;

        const draw::Canvas cv = canvas();
        const lengthType w = width();
        const lengthType h = height();
        const AudioFrame* f = AudioService::latestFrame();
        const bool thirds = f->thirdsCount > 0;
        const uint32_t n = thirds ? f->thirdsCount : 16u;   // the thirds the rate carries, 28 at the default 24 kHz

        const uint32_t height32 = h > 0 ? static_cast<uint32_t>(h) : 0u;
        const lengthType rows = static_cast<lengthType>(due < height32 ? due : height32);
        draw::scroll(cv, /*axis=*/1, rows);
        for (lengthType y = 0; y < rows; y++)
            for (lengthType x = 0; x < w; x++) {
                const uint32_t band = static_cast<uint32_t>(x) * n / w;
                const uint8_t mag = thirds ? f->thirds[band] : f->bands[band];
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), mag, mag));
            }
    }

private:
    uint32_t lastStepMs_ = 0;   ///< when the history last moved, the step's state
};

} // namespace mm
