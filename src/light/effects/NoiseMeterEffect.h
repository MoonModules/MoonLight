#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive effect: a noise field modulated by sound level.
/// @card NoiseMeterEffect.gif
/// Author: Andrew Tuline (WLED-SR), via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
///
/// A vertical meter whose height tracks the sound level and whose color is a scrolling noise field.
/// So a loud moment fills the panel bottom-up with a drifting gradient rather than a flat bar.
///
/// Prior art: WLED's Noisemeter, whose mapping and field sampling this reproduces.
///
/// @moreinfo
///
/// ## The color depends only on the row
///
/// Each lit row samples a field that both scrolls on its own phases and is modulated by the level.
/// Since nothing varies along x, the effect writes the x=0 column and extrude fans it across.
/// The meter then reads as one wide block without this effect duplicating the broadcast itself.
class NoiseMeterEffect : public EffectBase {
public:
    /// Catalog tags: WLED origin, audio-reactive.
    const char* tags() const override { return "🐙🎵🌫️"; }
    /// Writes the x=0 column, which extrude fans across x and z.
    Dim dimensions() const override { return Dim::D1; }

    // Defaults match WLED's own Noisemeter.
    /// The per-frame fade, which is the motion trail.
    uint8_t fadeRate = 240;
    /// How much of the column a given level fills.
    uint8_t width    = 128;

    /// Publish the trail and the meter's gain.
    void defineControls() override {
        controls_.addControl("fadeRate", fadeRate, 200, 254);
        controls_.addControl("width", width, 0, 255);
    }

    /// Light the column to the current level, coloring each row from the scrolling field.
    void tick() MM_NONBLOCKING override {
        const int sizeY = height();

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // latestFrame returns silence rather than null, but guard regardless

        const draw::Canvas cv = canvas();

        layer()->fadeToBlackBy(fadeRate);

        // The raw level rather than the smoothed one, which is what snaps the meter to a transient.
        const uint16_t level = f->level;
        uint32_t tmpSound2 = (static_cast<uint32_t>(level) * 2u * width) / 255u;
        if (tmpSound2 > 255u) tmpSound2 = 255u;   // capped, since this feeds a byte-ranged map

        // That proxy mapped onto the column's height, and clamped to it.
        int maxLen = static_cast<int>((tmpSound2 * static_cast<uint32_t>(sizeY)) / 255u);
        if (maxLen < 0) maxLen = 0;
        if (maxLen > sizeY) maxLen = sizeY;

        for (int y = 0; y < maxLen; y++) {
            // The row times the level walks one axis and the phase the other, so color drifts with both.
            const uint32_t coordA = static_cast<uint32_t>(y) * level + aux0_;
            const uint32_t coordB = aux1_ + static_cast<uint32_t>(y) * level;
            const uint8_t index = inoise8(coordA, coordB);
            const RGB col = colorFromPalette(*Palettes::active(), index);

            // Only the x=0 column, which extrude fans across every x and z.
            const lengthType drawY = static_cast<lengthType>(sizeY - 1 - y);
            draw::pixel(cv, {0, drawY, 0}, col);
        }

        // Two slow, detuned oscillators, so the field weaves rather than scrolling at one rate.
        aux0_ += beatsin8(5, elapsed(), 0, 10);
        aux1_ += beatsin8(4, elapsed(), 0, 10);
    }

private:
    /// The grid width, reached through an alias since the `width` control shadows the accessor.
    lengthType width_() const { return EffectBase::width(); }

    uint16_t aux0_ = 0;   ///< one scrolling-noise phase
    uint16_t aux1_ = 0;   ///< the other, detuned from it
};

} // namespace mm
