#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive brush strokes: lines whose 3D endpoints oscillate on the beat over a fading field.
/// @card PaintBrushEffect.gif
///
/// Each stroke is shortened to its band's length, so the moving tip sweeps a curve, and each drum hit stretches every stroke to its full span before it settles back. The field fades each frame, so the lines leave brush strokes rather than redrawing cleanly, and silence fades the field to black.
///
/// @attribution troyhacks
///
/// @technical
///
/// Each line takes one band, and its Euclidean span scaled by that band becomes the shorten amount. The lines are crisp, since `draw::line` has no anti-aliased form.
///
/// ## Motion that glides
///
/// The six endpoint coordinates are an oscillator bank, its phase accumulated frame by frame, so the bass raising the rate speeds the sweep from where it is; elapsed time times rate would jump by the elapsed minutes in whole cycles on every rate change.
///
/// The bass and the lengths read the bands' meter ballistic, so a stroke swells with its band rather than twitching with every block.
class PaintBrushEffect : public EffectBase {
public:
    /// Catalog tags: MoonModules origin, audio-reactive.
    const char* tags() const override { return "🌙🎶"; }
    /// Volumetric: the endpoints oscillate on all three axes.
    Dim dimensions() const override { return Dim::D3; }

    /// The phase spread between the oscillating endpoints.
    uint8_t oscillatorOffset = 6 * 160 / 255;
    /// How many parallel strokes are animated.
    uint8_t numLines   = 255;
    /// How fast the background decays, higher for shorter strokes.
    uint8_t fadeRate   = 40;
    /// A stroke draws only if longer than this, so quiet bands stay off.
    uint8_t minLength  = 0;
    /// Random hue per line instead of a per-band gradient.
    bool    color_chaos = false;
    /// Random per-frame phase jitter.
    bool    phase_chaos = false;

    /// Publish the oscillators, the line count, the trail and the two chaos switches.
    void defineControls() override {
        controls_.addControl("oscillatorOffset", oscillatorOffset, 0, 16);
        controls_.addControl("numLines", numLines, 2, 255);
        controls_.addControl("fadeRate", fadeRate, 0, 128);
        controls_.addControl("minLength", minLength);   // the full range, so a slider covers it
        controls_.addControl("color_chaos", color_chaos);
        controls_.addControl("phase_chaos", phase_chaos);
    }

    /// Fade the field, then draw each line between its two oscillating endpoints: @xref{motion-that-glides}.
    void tick() MM_NONBLOCKING override {
        const lengthType cols = width(), rows = height(), depth = this->depth();

        const draw::Canvas cv = canvas();

        // Advance the hue, then fade the field toward black, which is the trail.
        aux0Hue++;
        layer()->fadeToBlackBy(fadeRate);

        // One jitter value, shared by every endpoint this frame.
        aux1Chaos = phase_chaos ? rng_.next8() : 0;

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // a silence frame is non-null in practice, but guard before dereferencing
        const uint32_t ms = elapsed();
        // The smoothed bass added to every oscillator's rate, and the bank advanced from the phase it reached.
        const uint32_t base = f->bandsSmoothed[0] / kBands;
        for (uint8_t k = 0; k < kAxes; k++)
            osc_.set(k, Oscillator{static_cast<uint16_t>(oscillatorOffset * (k + 1u) + base), 0, 65535, static_cast<angle16>(k * (65536u / kAxes)), Wave::Sine});   // a sixth of a turn apart, so the strokes have length from the first frame
        osc_.advanceTo(ms);
        // A drum hit stretches every stroke to its span, then the stretch halves every kSurgeHalfLifeMs.
        surge_ = static_cast<uint8_t>((static_cast<uint32_t>(surge_) * halfLifeKeep(ms - lastMs_, kSurgeHalfLifeMs)) >> 16);
        lastMs_ = ms;
        if (f->seq != lastSeq_ && f->onset != 0) surge_ = 255;   // once a frame, by its number
        lastSeq_ = f->seq;

        // The loop stops below `numLines`, so the map's input high is one less or the top is never reached.
        const int lineHi = numLines - 1;

        for (size_t i = 0; i < numLines; i++) {
            const uint8_t bin = static_cast<uint8_t>(
                map(static_cast<int>(i), 0, lineHi, 0, kBands - 1));
            const uint8_t band = f->bandsSmoothed[bin];
            if (band == 0) continue;   // a silent band draws no stroke

            // Each band leads its line's oscillators by its own small angle, so the lines of different bands sit apart.
            const lengthType x1 = axis(0, band, cols), x2 = axis(1, band, cols);
            const lengthType y1 = axis(2, band, rows), y2 = axis(3, band, rows);

            lengthType z1 = 0, z2 = 0;
            int length;
            if (depth > 1) {
                z1 = axis(4, band, depth);
                z2 = axis(5, band, depth);
                length = isqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) + (z2 - z1) * (z2 - z1));
            } else {
                length = isqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
            }

            // The band, or the hit's stretch while it lasts, scales the span into the shorten amount, clamped before the cast against a wrap.
            length = map8(band > surge_ ? band : surge_, 0, static_cast<uint8_t>(length > 255 ? 255 : length));

            if (length > MAX(1, minLength)) {
                const uint8_t index = color_chaos
                    ? static_cast<uint8_t>(i * 255 / numLines + (aux0Hue & 0xFF))
                    : static_cast<uint8_t>(map(static_cast<int>(i), 0, lineHi, 0, 255));
                const RGB color = colorFromPalette(*Palettes::active(), index, 255);
                draw::line(cv, {x1, y1, z1}, {x2, y2, z2}, color, static_cast<uint8_t>(length));
            }
        }
    }

    /// Test seam: oscillator `k`'s endpoint across `len` for a band of `band`, as this frame draws it.
    lengthType endpointForTest(uint8_t k, uint8_t band, lengthType len) const { return axis(k, band, len); }

private:
    /// The spectrum's width, tied to the array so the band map stays in sync with it.
    static constexpr int kBands = static_cast<int>(sizeof(AudioFrame::bands) / sizeof(AudioFrame::bands[0]));
    static constexpr uint8_t kAxes = 6;                  ///< x, y and z of both endpoints
    static constexpr uint32_t kSurgeHalfLifeMs = 120;    ///< how fast a hit's stretch settles
    static constexpr uint32_t kLeadPerBand = 16;         ///< a band unit's lead, as an angle: a full band leads by a sixteenth of a turn

    uint16_t aux0Hue = 0;    ///< the running hue, advanced each frame
    uint8_t  aux1Chaos = 0;  ///< this frame's phase jitter, 0 unless `phase_chaos`
    Random8  rng_{0xB17EB00Bu};   ///< fixed-seed, so the goldens reproduce
    OscillatorBank<kAxes> osc_;   ///< the endpoints' phases, accumulated so a rate change glides
    uint32_t lastMs_ = 0;         ///< the last frame's time, which the stretch decays over
    uint8_t  surge_ = 0;          ///< the last hit's stretch, 255 on the hit
    uint16_t lastSeq_ = 0;        ///< the frame the last hits came from, so a frame read on two ticks gives its hits once

    /// Oscillator `k`'s endpoint coordinate across `len`, led by `band` as a fixed angle, so a rate change moves no line's lead, and shifted by this frame's jitter.
    lengthType axis(uint8_t k, uint8_t band, lengthType len) const MM_NONBLOCKING {
        const uint32_t lead = static_cast<uint32_t>(band) * kLeadPerBand;
        const angle16 theta = static_cast<angle16>(osc_.phase(k) - lead + (static_cast<uint32_t>(aux1Chaos) << 8));
        const uint32_t u = static_cast<uint32_t>(sin16(theta) + 32768);   // 0..65535
        return static_cast<lengthType>(u * static_cast<uint32_t>(len > 0 ? len - 1 : 0) / 65535u);
    }

    /// Kept local so the loop above reads like the MoonLight source it follows.
    static constexpr int MAX(int a, int b) { return a > b ? a : b; }

    /// The standard integer map, guarding a zero input span.
    static constexpr int map(int i, int inlo, int inhi, int outlo, int outhi) {
        return inhi == inlo ? outlo : outlo + (i - inlo) * (outhi - outlo) / (inhi - inlo);
    }

    /// Integer square root: the true Euclidean length, where `dist8` is an octagonal approximation.
    static int isqrt(int n) {
        if (n <= 0) return 0;
        unsigned int x = static_cast<unsigned int>(n), res = 0, bit = 1u << 30;
        while (bit > x) bit >>= 2;
        while (bit != 0) {
            if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
            else res >>= 1;
            bit >>= 2;
        }
        return static_cast<int>(res);
    }
};

} // namespace mm
