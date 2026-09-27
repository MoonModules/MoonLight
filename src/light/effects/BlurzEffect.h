#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive effect: blurred dots positioned by frequency band.
/// @card BlurzEffect.gif
/// Author: Andrew Tuline (WLED-SR), with enhancements by @softhack007, via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
///
/// Each frame lights one dot colored by a band's magnitude, then blurs the whole strip.
/// The dot bleeds into a soft smear that drifts and fades.
/// A band cursor advances each frame, so the color cycles through the spectrum over sixteen.
///
/// Prior art: WLED's Blurz, by way of MoonLight.
///
/// @moreinfo
///
/// ## Where the dot lands is the lever
///
/// With `freqMap` its position tracks the dominant frequency, so the spectrum scrolls with pitch.
/// With `geqScanner` it sweeps steadily across the strip instead.
/// With neither it jumps to a random position each frame, which is the classic look.
///
/// The effect is a strip in spirit, declared 2D so it spans a panel as a flat run of pixels.
/// The dot and the blur work over the whole pixel count either way.
class BlurzEffect : public EffectBase {
public:
    /// Catalog tags: WLED lineage, audio-reactive.
    const char* tags() const override { return "🐙🎶"; }
    /// A flat run of pixels, spanning a panel as readily as a strip.
    Dim dimensions() const override { return Dim::D2; }

    // Defaults match WLED and MoonLight: fast enough that each dot stays distinct rather than washing.
    /// How fast the trail fades each frame.
    uint8_t fadeRate   = 48;
    /// How far the blur spreads each dot into its halo.
    uint8_t blur       = 127;
    /// Position the dot by the dominant frequency rather than scanning or jumping.
    bool    freqMap    = false;
    /// Sweep the dot steadily rather than jumping it, when `freqMap` is off.
    bool    geqScanner = false;

    /// Publish the trail, the blur and the two positioning modes.
    void defineControls() override {
        controls_.addControl("fadeRate", fadeRate, 1, 255);
        controls_.addControl("blur", blur, 1, 255);
        controls_.addControl("freqMap", freqMap);
        controls_.addControl("geqScanner", geqScanner);
    }

    /// Arm the one-shot clear and rest both cursors, so a rebuild starts from a known state.
    void prepare() override {
        firstFrame_ = true;
        freqBand_   = 0;
        scanPos_    = 0;
    }

    /// Fade the trail, place this frame's dot, blur the strip, then re-stamp the dot's core.
    void tick() MM_NONBLOCKING override {
        const int cols = width();
        const int rows = height();

        const draw::Canvas cv = canvas();

        // The strip is the flat run of every pixel, addressed by one linear index.
        const int maxLen = static_cast<int>(nrOfLights());

        // A one-shot wipe on the first frame after a rebuild.
        if (firstFrame_) { draw::fill(cv, RGB{0, 0, 0}); firstFrame_ = false; }

        // The per-frame fade is what gives the blurred dot its decaying trail.
        layer()->fadeToBlackBy(fadeRate);

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;

        // One band a frame, wrapping across the spectrum.
        freqBand_ = static_cast<uint8_t>((freqBand_ + 1) % 16);

        // Decide where the dot lands this frame.
        int segLoc;
        if (freqMap && f->peakHz > 0) {
            // The dominant frequency mapped along the strip on a log scale, so pitch drives the dot.
            constexpr float kMaxFreqLog10 = 4.0424f;   ///< the top of the mapped range
            constexpr float kLoLog10      = 1.78f;     ///< about 60 Hz, below the mic's own floor
            const float lp = log10f(static_cast<float>(f->peakHz));
            const int freqLocn = static_cast<int>(
                roundf((lp - kLoLog10) * static_cast<float>(maxLen) / (kMaxFreqLog10 - kLoLog10)));
            segLoc = freqLocn;
        } else if (geqScanner) {
            // A steady sweep, one pixel a frame, independent of what the audio is doing.
            segLoc = scanPos_;
            scanPos_ = static_cast<int>((scanPos_ + 1) % maxLen);
        } else {
            // The classic look: the dot jumps to a random position each frame.
            segLoc = static_cast<int>(rng_.next16() % static_cast<uint16_t>(maxLen));
        }

        // Clamped, since the frequency map can land outside the strip.
        if (segLoc < 0) segLoc = 0;
        if (segLoc > maxLen - 1) segLoc = maxLen - 1;

        // Colored by the band's magnitude, where a value past 255 wraps the palette rather than clamping.
        const int denom = (maxLen - 1) > 1 ? (maxLen - 1) : 1;   // max(1, maxLen-1)
        const int pixColor = (2 * static_cast<int>(f->bands[freqBand_]) * 240) / denom;
        const RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(pixColor));

        // The linear index addressed as a position on the flat run.
        const int dx = segLoc % cols;
        const int dy = segLoc / cols;

        // The radius scales with the fixture, since a single pixel vanishes on a large grid.
        const int minDim = cols < rows ? cols : rows;
        const int r = minDim > 32 ? minDim / 32 : 0;   // one pixel on a small grid, a blob above
        for (int oy = -r; oy <= r; oy++)
            for (int ox = -r; ox <= r; ox++)
                draw::pixel(cv, {static_cast<lengthType>(dx + ox), static_cast<lengthType>(dy + oy), 0}, c);

        // The blur over the whole buffer, which is the defining smear.
        draw::blur(cv, blur);

        // Re-stamped on top: the blur spreads the dot into a halo but also dilutes its center.
        for (int oy = -r; oy <= r; oy++)
            for (int ox = -r; ox <= r; ox++)
                draw::addPixel(cv, {static_cast<lengthType>(dx + ox), static_cast<lengthType>(dy + oy), 0}, c);
    }

private:
    bool    firstFrame_ = true;   ///< arms the one-shot wipe after a rebuild
    uint8_t freqBand_   = 0;      ///< the band cursor, advanced each frame
    int     scanPos_    = 0;      ///< the sweep's position
    Random8 rng_;                 ///< the random dot's placement
};

} // namespace mm