#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive graphic-equalizer effect: 16 bands as vertical bars.
/// @card GEQEffect.gif
/// Author: Andrew Tuline (WLED-SR), via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
///
/// The 16 audio bands spread across a panel's columns, each rising to its band's loudness.
/// A peak dot sits at the highest the bar recently reached and falls back slowly.
/// GEQ3D in this folder draws the same data in perspective instead.
///
/// Prior art: WLED's 2D GEQ, of the Aircoookie and Andrew Tuline lineage, by way of MoonLight.
///
/// @moreinfo
///
/// ## One frame
///
/// The buffer fades a little, which is the motion trail `fadeOut` sets.
/// Each column then reads its band, optionally smoothed against its neighbors, and fills from the floor.
/// The bar takes one color per column under `colorBars`, or a gradient running up it.
/// A per-column tracker remembers the tallest the bar reached, drawn as a dot once the bar falls below.
/// That dot decays one row every `ripple` frames, and `ripple` 0 disables it.
///
/// Reproduced from WLED: the band mapping, the smoothing weights, the fill and the falling peak.
/// Silence flattens the bars and the peaks fall away.
class GEQEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, WLED lineage, audio-reactive.
    const char* tags() const override { return "💫🐙🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    // Defaults match the WLED and MoonLight GEQ.
    /// The per-frame fade, which is the motion trail. A fast fade snaps the bars rather than smearing.
    uint8_t fadeOut    = 248;
    /// The peak dot falls one row every this many frames, and 0 removes the dot.
    uint8_t ripple     = 4;
    /// Color each bar by its column rather than by height along it.
    bool    colorBars  = false;
    /// Blend each band with its neighbors, for a smoother profile.
    bool    smoothBars = false;

    /// Publish the trail, the peak fall rate and the two coloring toggles.
    void defineControls() override {
        controls_.addControl("fadeOut", fadeOut, 0, 255);
        controls_.addControl("ripple", ripple, 0, 255);
        controls_.addControl("colorBars", colorBars);
        controls_.addControl("smoothBars", smoothBars);
    }

    /// Size one peak tracker per column, and start every peak at the floor.
    void prepare() override {
        // Cleared on every rebuild, since resize() zero-fills only on a size change.
        peaks_.resize(static_cast<size_t>(width() > 0 ? width() : 0));
        if (peaks_) std::memset(peaks_.data(), 0, peaks_.bytes());
        rippleCounter_ = 0;
    }

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

        // The peaks drop one row whenever this counter wraps `ripple`.
        bool fallThisFrame = false;
        if (ripple > 0) {
            if (++rippleCounter_ >= ripple) { rippleCounter_ = 0; fallThisFrame = true; }
        }

        for (int x = 0; x < cols; x++) {
            // Mapped over 0..cols-1 and 0..15, which keeps the last column off an out-of-range band 16.
            int band = map32(x, 0, cols - 1, 0, NUM_GEQ_CHANNELS - 1);
            if (band < 0) band = 0;
            if (band > NUM_GEQ_CHANNELS - 1) band = NUM_GEQ_CHANNELS - 1;

            int bandHeight = f->bands[band];

            // WLED's 7/3/3 weights over 12, for interior bands only. Reconstructed from its behavior.
            if (smoothBars && band > 0 && band < NUM_GEQ_CHANNELS - 1) {
                const int lastBandHeight = f->bands[band - 1];
                const int nextBandHeight = f->bands[band + 1];
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
            } else if (fallThisFrame && peaks_[x] > 0) {
                peaks_[x] = static_cast<lengthType>(peaks_[x] - 1);   // one row per ripple tick
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
    /// The spectrum's width, which the columns map onto.
    static constexpr int NUM_GEQ_CHANNELS = 16;

    /// Each column's peak-dot row, counted up from the floor, and lengthType so a tall panel fits.
    ScratchBuffer<lengthType> peaks_{*this};
    uint8_t  rippleCounter_ = 0;   ///< frames toward the next peak-fall step
};

} // namespace mm