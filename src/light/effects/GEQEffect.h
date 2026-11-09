#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/AudioBarsBase.h"

namespace mm {

/// A flat graphic equalizer: the audio bands rise as vertical bars from the bottom.
/// @card GEQEffect.gif
///
/// The bands spread across the columns, the third octaves by default or the sixteen or the piano's keys with `bands`, each rising to its loudness, with optional smoothing, per-bar palette coloring and falling peak markers. A peak dot sits at the highest the bar recently reached and falls back slowly. @ref GEQ3DEffect draws the same data in perspective.
///
/// @attribution Andrew Tuline, softhack007
///
/// @technical
///
/// A per-column tracker remembers the tallest the bar reached, drawn as a dot once the bar falls below it.
class GEQEffect : public AudioBarsBase {
public:
    /// Catalog tags: WLED origin, audio-reactive.
    const char* tags() const override { return "🐙🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast bars fade each frame, which is the motion trail.
    uint8_t fadeOut    = 248;
    /// Falling-peak marker decay; 0 removes the marker.
    uint8_t ripple     = 4;
    /// Color each bar from the palette by band instead of by row.
    bool    colorBars  = false;
    /// Blend neighboring bands for smoother bar heights.
    bool    smoothBars = false;

    /// Publish the trail, the peak fall rate, the two coloring toggles and the bars.
    void defineControls() override {
        controls_.addControl("fadeOut", fadeOut, 0, 255);
        controls_.addControl("ripple", ripple, 0, 255);
        controls_.addControl("colorBars", colorBars);
        controls_.addControl("smoothBars", smoothBars);
        addBandsSelect();
    }

    /// Size one peak tracker per column, and start every peak at the floor.
    void prepare() override {
        AudioBarsBase::prepare();
        // Cleared on every rebuild, since resize() zero-fills only on a size change.
        peaks_.resize(static_cast<size_t>(width() > 0 ? width() : 0));
        if (peaks_) std::memset(peaks_.data(), 0, peaks_.bytes());
        rippleMs_ = lastTickMs_ = 0;
    }

    /// Column `x`'s peak-dot row, for tests.
    lengthType peakForTest(int x) const { return peaks_ && x >= 0 && static_cast<size_t>(x) < peaks_.count() ? peaks_[x] : 0; }

    /// Fade the trail, then fill each column to its band and drop its peak dot.
    void tick() MM_NONBLOCKING override {
        const int cols = width();
        const int rows = height();
        if (!peaks_) return;   // nothing allocated yet, so nothing to draw

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // latestFrame returns silence rather than null, but guard regardless

        const draw::Canvas cv = canvas();

        // The motion trail: dim the whole buffer each frame.
        layer()->fadeToBlackBy(fadeOut);

        // The peaks drop a row every `ripple` 60 Hz reference frames of elapsed time, so they fall alike at any frame rate.
        const uint32_t now = elapsed();
        uint32_t falls = 0;
        if (ripple > 0) {
            const uint32_t period = ripple * 1000u / 60u + 1u;
            rippleMs_ += now - lastTickMs_;
            falls = rippleMs_ / period;
            rippleMs_ -= falls * period;
        }
        lastTickMs_ = now;

        const AudioBars bars = barsOf(*f, bands);
        const int nBars = bars.n;
        for (int x = 0; x < cols; x++) {
            // Mapped over 0..cols-1 and 0..n-1, which keeps the last column off an out-of-range bar.
            int band = map32(x, 0, cols - 1, 0, nBars - 1);
            if (band < 0) band = 0;
            if (band > nBars - 1) band = nBars - 1;

            int bandHeight = bars.mag[band];

            // WLED's 7/3/3 weights over 12, for interior bands only. Reconstructed from its behavior.
            if (smoothBars && band > 0 && band < nBars - 1) {
                const int lastBandHeight = bars.mag[band - 1];
                const int nextBandHeight = bars.mag[band + 1];
                bandHeight = (7 * bandHeight + 3 * lastBandHeight + 3 * nextBandHeight) / 12;
                if (bandHeight < 0)   bandHeight = 0;
                if (bandHeight > 255) bandHeight = 255;
            }

            // The band magnitude mapped onto the row count.
            int barHeight = map32(bandHeight, 0, 255, 0, rows);
            if (barHeight < 0)    barHeight = 0;
            if (barHeight > rows) barHeight = rows;

            // Rise instantly to a new high, otherwise fall slowly.
            if (barHeight > peaks_[x]) {
                peaks_[x] = static_cast<lengthType>(barHeight);
            } else if (falls > 0) {
                peaks_[x] = static_cast<lengthType>(static_cast<uint32_t>(peaks_[x]) > falls ? peaks_[x] - static_cast<lengthType>(falls) : 0);   // a row per ripple period passed
            }

            // Filled from the floor up, taking either the column's hue or the height along the bar.
            const uint8_t columnIndex = static_cast<uint8_t>(map32(x, 0, cols - 1, 0, 255));
            draw::bar(cv, static_cast<lengthType>(x), static_cast<lengthType>(rows - 1),
                      static_cast<lengthType>(barHeight), draw::Grow::Up, [&](lengthType h) {
                          const uint8_t colorIndex = colorBars
                              ? columnIndex
                              : static_cast<uint8_t>(map32(h, 0, rows - 1, 0, 255));
                          return colorFromPalette(*Palettes::active(), colorIndex);
                      });

            // The remembered peak, drawn as one pixel where it stands above the live bar.
            if (ripple > 0 && peaks_[x] > 0 && peaks_[x] > barHeight) {
                const int y = rows - peaks_[x];      // peaks_[x] rows up from the floor
                if (y >= 0 && y < rows) {
                    // The top of the palette, so the dot reads as the crest.
                    const RGB peakCol = colorFromPalette(*Palettes::active(), 255);
                    draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, peakCol);
                }
            }
        }
    }

private:

    /// Each column's peak-dot row, counted up from the floor, and lengthType so a tall panel fits.
    ScratchBuffer<lengthType> peaks_{*this};
    uint32_t rippleMs_ = 0;        ///< the time toward the next peak-fall step
    uint32_t lastTickMs_ = 0;      ///< elapsed() at the last tick
};

} // namespace mm