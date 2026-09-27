#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Effect that paints moving brush-stroke lines across the layer.
/// @card PaintBrushEffect.gif
/// Author: @TroyHacks (WLED MoonModules, GPLv3), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h
///
/// Lines whose endpoints oscillate in 3D on the beat, each shortened by a band's magnitude.
/// That shortening is what makes the strokes curve and sweep.
/// The field fades each frame, so the lines leave brush strokes rather than redrawing cleanly.
///
/// Prior art: MoonLight's PaintBrush, whose six endpoints and length feedback this reproduces.
///
/// @moreinfo
///
/// ## What a line does
///
/// Each line takes one band, and its Euclidean span scaled by that band becomes the shorten amount.
/// A line draws only when it runs longer than `minLength`, so quiet bands stay dark.
/// Silence leaves no lines at all, and the field fades to black.
///
/// The soft anti-alias control is omitted, since `draw::line` is crisp and has no Xiaolin-Wu form.
class PaintBrushEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, MoonModules, audio-reactive.
    const char* tags() const override { return "💫🌙🎶"; }
    /// Volumetric: the endpoints oscillate on all three axes.
    Dim dimensions() const override { return Dim::D3; }

    /// The phase-spread multiplier between the six endpoint oscillators.
    uint8_t oscillatorOffset = 6 * 160 / 255;
    /// How many lines are animated in parallel.
    uint8_t numLines   = 255;
    /// How fast the background decays, which is the brush-stroke trail.
    uint8_t fadeRate   = 40;
    /// A line draws only when it runs longer than this.
    uint8_t minLength  = 0;
    /// Vary the hue per line rather than running a gradient across the bands.
    bool    color_chaos = false;
    /// Jitter every endpoint's phase once a frame.
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

    /// Fade the field, then draw each line between its two oscillating endpoints.
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
        // The bass term, added to every oscillator's rate.
        const uint8_t base = static_cast<uint8_t>(f->bands[0] / kBands);

        // The loop stops below `numLines`, so the map's input high is one less or the top is never reached.
        const int lineHi = numLines - 1;

        for (size_t i = 0; i < numLines; i++) {
            const uint8_t bin = static_cast<uint8_t>(
                map(static_cast<int>(i), 0, lineHi, 0, kBands - 1));
            const uint8_t band = f->bands[bin];

            // Generated full-range then mapped, since beatsin8's 8-bit range truncates a wide grid.
            const lengthType x1 = osc(static_cast<uint8_t>(oscillatorOffset * 1 + base), ms, band, cols);
            const lengthType x2 = osc(static_cast<uint8_t>(oscillatorOffset * 2 + base), ms, band, cols);
            const lengthType y1 = osc(static_cast<uint8_t>(oscillatorOffset * 3 + base), ms, band, rows);
            const lengthType y2 = osc(static_cast<uint8_t>(oscillatorOffset * 4 + base), ms, band, rows);

            lengthType z1 = 0, z2 = 0;
            int length;
            if (depth > 1) {
                z1 = osc(static_cast<uint8_t>(oscillatorOffset * 5 + base), ms, band, depth);
                z2 = osc(static_cast<uint8_t>(oscillatorOffset * 6 + base), ms, band, depth);
                length = isqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) + (z2 - z1) * (z2 - z1));
            } else {
                length = isqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
            }

            // The band scales the span into the shorten amount, clamped before the cast against a wrap.
            length = map8(band, 0, static_cast<uint8_t>(length > 255 ? 255 : length));

            if (length > MAX(1, minLength)) {
                const uint8_t index = color_chaos
                    ? static_cast<uint8_t>(i * 255 / numLines + (aux0Hue & 0xFF))
                    : static_cast<uint8_t>(map(static_cast<int>(i), 0, lineHi, 0, 255));
                const RGB color = colorFromPalette(*Palettes::active(), index, 255);
                draw::line(cv, {x1, y1, z1}, {x2, y2, z2}, color, static_cast<uint8_t>(length));
            }
        }
    }

private:
    /// The spectrum's width, tied to the array so the band map stays in sync with it.
    static constexpr int kBands = static_cast<int>(sizeof(AudioFrame::bands) / sizeof(AudioFrame::bands[0]));

    uint16_t aux0Hue = 0;    ///< the running hue, advanced each frame
    uint8_t  aux1Chaos = 0;  ///< this frame's phase jitter, 0 unless `phase_chaos`
    Random8  rng_{0xB17EB00Bu};   ///< fixed-seed, so the goldens reproduce

    /// One endpoint on an axis, generated full-range then mapped so a wide grid keeps its sweep.
    lengthType osc(uint8_t bpm, uint32_t ms, uint8_t timebase, lengthType len) const {
        const uint8_t s = beatsin8(bpm, ms, 0, 255, timebase, aux1Chaos);
        return static_cast<lengthType>(map(s, 0, 255, 0, len > 0 ? len - 1 : 0));
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
