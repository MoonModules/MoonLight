#pragma once

#include "core/util/math16.h"              // peakHold, smoothFollow, map32
#include "light/effects/EffectBase.h"

namespace mm {

/// An audio analyzer with real meter ballistics: bars rise fast enough to catch a transient and fall slowly enough to read.
/// @card SpectrumEffect.gif
///
/// A peak dot marks the recent maximum and drifts down. The asymmetry is the whole point: a symmetric follower either misses the hit or flickers.
///
/// @attribution IEC 60268-17, WLED
///
/// @technical
///
/// The ballistics live in the shared `smoothFollow` and `peakHold`, so any effect wanting a meter gets the same feel, and `draw::bar` draws the columns. The band-to-column mapping follows WLED's GEQ.
class SpectrumEffect : public EffectBase {
public:
    /// Catalog tags: a showcase, and audio-reactive.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast a bar rises toward a new level.
    uint8_t attack   = 200;
    /// How fast a bar falls back.
    uint8_t release  = 30;
    /// How fast the peak dot drifts down.
    uint8_t peakDecay = 2;
    /// Draw the held peak above each bar.
    bool    showPeaks = true;
    /// Color each bar by its column rather than by height.
    bool    colorByColumn = false;

    /// Publish the ballistics, the peak's decay and the coloring.
    void defineControls() override {
        controls_.addControl("attack", attack, 1, 255);
        controls_.addControl("release", release, 1, 128);
        controls_.addControl("peakDecay", peakDecay, 0, 32);
        controls_.addControl("showPeaks", showPeaks);
        controls_.addControl("colorByColumn", colorByColumn);
    }

    /// Size one follower and one peak per column.
    void prepare() override {
        const size_t cols = static_cast<size_t>(width() > 0 ? width() : 0);
        levels_.resize(cols);
        peaks_.resize(cols);
    }

    /// Follow each band with its own ballistics, then draw its bar and peak.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (!levels_ || !peaks_) return;   // this effect's own buffers, where the grid is the Layer's

        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // no audio leaves a dark panel rather than a crash

        for (lengthType x = 0; x < w; x++) {
            // The column mapped onto one of the bands, so the analyzer fits any width.
            const uint8_t band = static_cast<uint8_t>(
                static_cast<uint32_t>(x) * kBands / static_cast<uint32_t>(w > 0 ? w : 1));
            const uint8_t mag = f->bands[band];

            // The asymmetry is the point, so rise and fall take separate rates.
            const uint8_t rate = mag > levels_[x] ? attack : release;
            levels_[x] = smoothFollow(levels_[x], mag, rate);

            // The peak rises instantly and drifts down, which is a real meter's other half.
            peaks_[x] = peakHold(peaks_[x], levels_[x], peakDecay);

            const lengthType lit = static_cast<lengthType>(
                (static_cast<uint32_t>(levels_[x]) * h) / 255u);
            const uint8_t columnHue = static_cast<uint8_t>(
                map32(x, 0, w > 1 ? w - 1 : 1, 0, 255));

            // One bar a column from the floor up, colored by height unless asked otherwise.
            draw::bar(cv, x, static_cast<lengthType>(h - 1), lit, draw::Grow::Up,
                      [&](lengthType row) {
                          const uint8_t idx = colorByColumn
                              ? columnHue
                              : static_cast<uint8_t>((static_cast<uint32_t>(row) * 255u) / (h > 1 ? h - 1 : 1));
                          return colorFromPalette(*Palettes::active(), idx, 255);
                      });

            // The held peak, drawn above where the bar reaches.
            if (showPeaks && peaks_[x] > 0) {
                const lengthType py = static_cast<lengthType>(
                    h - 1 - (static_cast<uint32_t>(peaks_[x]) * h) / 255u);
                if (py >= 0 && py < h)
                    draw::pixel(cv, {x, py, 0}, colorFromPalette(*Palettes::active(), 255, 255));
            }
        }
    }

private:
    /// The spectrum's width, which the columns map onto.
    static constexpr uint32_t kBands = 16;

    ScratchBuffer<uint8_t> levels_{*this};   ///< each column's followed bar height
    ScratchBuffer<uint8_t> peaks_{*this};    ///< and its held peak
};

}  // namespace mm
