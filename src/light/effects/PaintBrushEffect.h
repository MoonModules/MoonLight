#pragma once

#include "light/effects/AudioEffectBase.h"   // the analysis, while this effect reads it

namespace mm {

/// Audio-reactive brush strokes: lines whose 3D endpoints oscillate on the beat over a fading field.
/// @card PaintBrushEffect.gif
///
/// Each stroke is shortened to its band's length, so the moving tip sweeps a curve. The field fades each frame, so the lines leave brush strokes rather than redrawing cleanly, and silence fades the field to black. `surge` stretches every stroke to its full span on each drum hit.
///
/// @attribution troyhacks
///
/// @technical
///
/// Up to sixteen lines, one band each, as troyhacks's Paintbrush draws them: its Euclidean span scaled by that band becomes the shorten amount, a span under `minLength` draws nothing, and on a flat grid `antiAlias` draws the strokes with Wu's line. On a grid with depth the endpoints oscillate in z too, with crisp 3D lines.
///
/// ## The scatter, or the glide
///
/// The original places each endpoint at elapsed time times its rate, and the raw bass adds to every rate, so each step of the bass throws the endpoints to new places, by more the longer the effect runs: the fading field fills with crossed strokes, which is its look. Each band leads its line by the angle its value in milliseconds is at the rate.
///
/// `glide` makes the endpoints an oscillator bank instead, its phase accumulated frame by frame, so the bass speeds the sweep from where it is; the leads take the base rate, so a bass change moves none, and the bass and the lengths read the bands' meter ballistic, so a stroke swells with its band.
///
/// ## The original's frame rate as time
///
/// The original fades, drifts its hue and jitters once a frame, at the 30 frames a second troyhacks's board runs it at. Here the fade is a rate per 60 Hz reference frame, which the Layer scales by elapsed time, so `fadeRate` 25 is the original's 80; the hue and the jitter step 30 times a second.
///
/// The strokes draw every frame, so a faster render sharpens them and changes nothing else.
class PaintBrushEffect : public AudioEffectBase {
public:
    /// Catalog tags: MoonModules origin, audio-reactive.
    const char* tags() const override { return "🌙🎶"; }
    /// Volumetric: the endpoints oscillate on all three axes.
    Dim dimensions() const override { return Dim::D3; }

    /// The base sweep in cycles a minute, multiplied per endpoint.
    uint8_t oscillatorOffset = 10;
    /// How many strokes are drawn, one band each.
    uint8_t numLines   = 16;
    /// How fast the background decays, higher for shorter strokes.
    uint8_t fadeRate   = 25;
    /// The span a stroke's endpoints must exceed for it to draw.
    uint8_t minLength  = 0;
    /// Random hue per line instead of a per-band gradient.
    bool    color_chaos = false;
    /// Smooth the strokes on a flat grid.
    bool    antiAlias = true;
    /// Random per-frame phase jitter.
    bool    phase_chaos = false;
    /// Stretch every stroke to its span on each drum hit.
    bool    surge = false;
    /// Sweep the strokes smoothly instead of scattering them on the bass.
    bool    glide = false;

    /// Publish the oscillators, the line count, the trail, the switches and the surge.
    void defineControls() override {
        controls_.addControl("oscillatorOffset", oscillatorOffset, 1, 15);
        controls_.addControl("numLines", numLines, 1, kBands);
        controls_.addControl("fadeRate", fadeRate, 5, 64);
        controls_.addControl("minLength", minLength, 0, 31);
        controls_.addControl("color_chaos", color_chaos);
        controls_.addControl("antiAlias", antiAlias);
        controls_.addControl("phase_chaos", phase_chaos);
        controls_.addControl("surge", surge);
        controls_.addControl("glide", glide);
    }

    /// Fade the field, then draw each line between its two oscillating endpoints: @xref{the-scatter-or-the-glide}.
    void tick() MM_NONBLOCKING override {
        const lengthType cols = width(), rows = height(), depth = this->depth();

        const draw::Canvas cv = canvas();
        const AudioFrame* f = AudioService::latestFrame();
        const uint32_t ms = elapsed();
        // The bass added to every endpoint's rate; the angle from elapsed time, the original's scatter, or from the bank's accumulated phase.
        const uint8_t* mags = glide ? f->bandsSmoothed : f->bands;
        const uint32_t bass = mags[0] / kBands;
        for (uint8_t k = 0; k < kAxes; k++) {
            const uint32_t rate = oscillatorOffset * (k + 1u) + bass;
            osc_.set(k, Oscillator{static_cast<uint16_t>(rate), 0, 65535, 0, Wave::Sine});
            leadRate_[k] = glide ? rate - bass : rate;
            if (!glide) phase_[k] = static_cast<angle16>(static_cast<uint64_t>(ms) * rate * 65536u / 60000u);
        }
        osc_.advanceTo(ms);
        if (glide) for (uint8_t k = 0; k < kAxes; k++) phase_[k] = osc_.phase(k);
        // A drum hit stretches every stroke to its span, then the stretch halves every kSurgeHalfLifeMs, read from the time since the hit so no tick rounds it.
        if (surge && f->seq != lastSeq_ && f->onset != 0) { lastHitMs_ = ms; hitSeen_ = true; }   // once a frame, by its number
        surge_ = hitSeen_ ? static_cast<uint8_t>((255u * halfLifeKeep(ms - lastHitMs_, kSurgeHalfLifeMs)) >> 16) : 0;
        lastSeq_ = f->seq;

        // The trail, a rate the Layer scales by elapsed time; the hue drift and the jitter run on the original's 30 frames a second: @xref{the-originals-frame-rate-as-time}.
        layer()->fadeToBlackBy(fadeRate);
        const uint32_t frame30 = static_cast<uint32_t>(static_cast<uint64_t>(ms) * kOriginalHz / 1000u);
        aux0Hue = static_cast<uint16_t>(frame30);
        if (frame30 != jitterFrame_) { jitterFrame_ = frame30; aux1Chaos = phase_chaos ? rng_.next8() : 0; }   // one jitter value a frame of the original, shared by every endpoint

        // The loop stops below `numLines`, so the map's input high is one less or the top is never reached.
        const int lineHi = numLines - 1;

        for (size_t i = 0; i < numLines; i++) {
            const uint8_t bin = static_cast<uint8_t>(
                map(static_cast<int>(i), 0, lineHi, 0, kBands - 1));
            const uint8_t band = mags[bin];
            if (band == 0) continue;   // a silent band draws no stroke

            // Each band leads its line's oscillators by its own small angle, so the lines of different bands sit apart.
            const lengthType x1 = axis(0, band, cols), x2 = axis(1, band, cols);
            const lengthType y1 = axis(2, band, rows), y2 = axis(3, band, rows);

            lengthType z1 = 0, z2 = 0;
            if (depth > 1) {
                z1 = axis(4, band, depth);
                z2 = axis(5, band, depth);
            }
            const int span = static_cast<int>(isqrt(static_cast<uint32_t>((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) + (z2 - z1) * (z2 - z1))));   // the true length, where dist8 is octagonal
            if (span <= MAX(1, minLength)) continue;

            // The band, or the hit's stretch while it lasts, scales the span into the shorten amount, clamped before the cast against a wrap.
            const uint8_t length = map8(band > surge_ ? band : surge_, 0, static_cast<uint8_t>(span > 255 ? 255 : span));
            const uint8_t index = color_chaos
                ? static_cast<uint8_t>(i * 255 / numLines + (aux0Hue & 0xFF))
                : static_cast<uint8_t>(map(static_cast<int>(i), 0, lineHi, 0, 255));
            const RGB color = colorFromPalette(*Palettes::active(), index, 255);
            if (antiAlias && depth <= 1) draw::lineAA(cv, {x1, y1, 0}, {x2, y2, 0}, color, length);
            else draw::line(cv, {x1, y1, z1}, {x2, y2, z2}, color, length);
        }
    }

    /// How much of the last hit's stretch is left, for tests.
    uint8_t surgeForTest() const { return surge_; }

    /// Test seam: oscillator `k`'s endpoint across `len` for a band of `band`, as this frame draws it.
    lengthType endpointForTest(uint8_t k, uint8_t band, lengthType len) const { return axis(k, band, len); }

private:
    /// The spectrum's width, tied to the array so the band map stays in sync with it.
    static constexpr int kBands = static_cast<int>(sizeof(AudioFrame::bands) / sizeof(AudioFrame::bands[0]));
    static constexpr uint8_t kAxes = 6;                  ///< x, y and z of both endpoints
    static constexpr uint32_t kSurgeHalfLifeMs = 120;    ///< how fast a hit's stretch settles

    uint16_t aux0Hue = 0;    ///< the running hue, 30 steps a second
    uint8_t  aux1Chaos = 0;  ///< this frame's phase jitter, 0 unless `phase_chaos`
    Random8  rng_{0xB17EB00Bu};   ///< fixed-seed, so the goldens reproduce
    OscillatorBank<kAxes> osc_;   ///< the endpoints' phases, accumulated so a rate change glides
    angle16  phase_[kAxes] = {};  ///< this frame's endpoint angles, from time or from the bank
    uint32_t leadRate_[kAxes] = {};   ///< the rate a band's milliseconds lead its endpoint at
    uint32_t lastHitMs_ = 0;      ///< elapsed() at the last hit, which the stretch decays from
    bool     hitSeen_ = false;    ///< whether a hit came since the effect started
    uint8_t  surge_ = 0;          ///< the last hit's stretch, 255 on the hit
    uint16_t lastSeq_ = 0;        ///< the frame the last hits came from, so a frame read on two ticks gives its hits once
    static constexpr uint32_t kOriginalHz = 30;      ///< the original's frame rate on troyhacks's board, which its look is judged by
    uint32_t jitterFrame_ = UINT32_MAX;               ///< the original's frame the jitter was drawn for

    /// Endpoint `k`'s coordinate across `len`, led by the angle `band` milliseconds are at its lead rate, and shifted by this frame's jitter.
    lengthType axis(uint8_t k, uint8_t band, lengthType len) const MM_NONBLOCKING {
        const uint32_t lead = static_cast<uint32_t>(band) * leadRate_[k] * 65536u / 60000u;
        const angle16 theta = static_cast<angle16>(phase_[k] - lead + (static_cast<uint32_t>(aux1Chaos) << 8));
        const uint32_t u = static_cast<uint32_t>(sin16(theta) + 32768);   // 0..65535
        return static_cast<lengthType>(u * static_cast<uint32_t>(len > 0 ? len - 1 : 0) / 65535u);
    }

    /// Kept local so the loop above reads like the MoonLight source it follows.
    static constexpr int MAX(int a, int b) { return a > b ? a : b; }

    /// The standard integer map, guarding a zero input span.
    static constexpr int map(int i, int inlo, int inhi, int outlo, int outhi) {
        return inhi == inlo ? outlo : outlo + (i - inlo) * (outhi - outlo) / (inhi - inlo);
    }

};

} // namespace mm
