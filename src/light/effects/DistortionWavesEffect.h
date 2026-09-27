#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

/// Interference effect: overlaid moving waves distorting the field.
/// @card DistortionWavesEffect.gif
/// Author: ldirko & blazoncek (WLED port), https://editor.soulmatelights.com/gallery/1089-distorsion-waves , via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
///
/// Two sine waves whose summed value drives the hue, giving a flowing moire field.
/// They run at independent frequencies and slightly different time rates, so they beat.
///
/// Prior art: WLED's Distortion Waves, by way of MoonLight.
///
/// @moreinfo
///
/// ## Integer throughout
///
/// Angles are bytes and the sines return bytes, which the effect averages into a hue.
/// The vertical wave's time runs about 1.3 times the horizontal, approximated in integers.
/// That hue then indexes the active palette, where the source swept hue directly.
class DistortionWavesEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight and WLED origin.
    const char* tags() const override { return "💫"; }
    /// A plane, which extrude lifts through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The horizontal wave's frequency.
    uint8_t freq_x = 3;
    /// The vertical wave's frequency.
    uint8_t freq_y = 3;
    /// How fast the field moves, where 0 freezes it.
    uint8_t speed = 50;

    /// Publish both wave frequencies and the speed.
    void defineControls() override {
        controls_.addControl("freq_x", freq_x, 1, 8);
        controls_.addControl("freq_y", freq_y, 1, 8);
        controls_.addControl("speed", speed, 0, 100);
    }

    /// Sum the two waves per pixel and read the palette at their average.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const uint8_t cpl = channelsPerLight();

        // A speed of 0 freezes, and BeatPhase still tracks time so resuming does not jump.
        phase_.advanceTo(elapsed(), speed);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));
        // From the same accumulator at its own scale, since the wrapped value jumps at every wrap.
        const uint8_t ty = static_cast<uint8_t>(phase_.phase(333));

        for (lengthType y = 0; y < h; y++) {
            const uint8_t sy = sin8(static_cast<uint8_t>(static_cast<uint8_t>(y) * freq_y + ty));
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint8_t sx = sin8(static_cast<uint8_t>(static_cast<uint8_t>(x) * freq_x + t));
                // The two sines averaged, where their interference is what makes the pattern move.
                const uint8_t hue = static_cast<uint8_t>((static_cast<uint16_t>(sx) + sy) >> 1);
                const RGB c = colorFromPalette(*Palettes::active(), hue);
                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    BeatPhase phase_;   ///< the shared time base both waves read
};

} // namespace mm
