#pragma once

#include "core/util/math16.h"              // hashInt, easeInOutQuad, BeatPhase
#include "light/effects/EffectBase.h"

namespace mm {

/// Two color fields trade places pixel by pixel in an order that looks random but is computed.
/// @card DissolveEffect.gif
///
/// The transition needs no per-pixel state and no shuffled index list, so two devices rendering the same frame dissolve identically without exchanging anything.
///
/// @moreinfo
///
/// Each pixel asks a hash of its own position for a threshold and compares it against the progress. So the order is a function of position: a stream generator advances per call and drifts, and this cannot.
class DissolveEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast one transition completes.
    uint8_t bpm      = 12;
    /// How long pixels spend mid-flight; 0 gives a hard edge.
    uint8_t spread   = 60;
    /// Ease the progress instead of sweeping linearly.
    bool    eased    = true;
    /// Random order; off gives a positional wipe from the same code.
    bool    scatter  = true;

    /// Publish the transition's speed, its softness and its order.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 120);
        controls_.addControl("spread", spread, 0, 255);
        controls_.addControl("eased", eased);
        controls_.addControl("scatter", scatter);
    }

    /// Compare each pixel's own threshold against the transition's progress.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t raw = phase_.phase(65536);

        // Each sweep is a generation, which seeds the hash so no two passes share an order.
        const uint32_t generation = raw >> 16;
        uint16_t progress = static_cast<uint16_t>(raw & 0xFFFF);
        if (eased) progress = easeInOutQuad(progress);

        // The fields are palette reads a generation apart, so the effect walks the palette.
        const uint8_t hueA = static_cast<uint8_t>(generation * kHueStep);

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                // With scatter off the threshold comes from position, which wipes diagonally.
                const uint16_t threshold = scatter
                    ? hashInt(static_cast<uint32_t>(x), static_cast<uint32_t>(y), generation)
                    : static_cast<uint16_t>(((x + y) * 65535u) / (w + h > 0 ? w + h : 1));

                // `spread` widens the comparison into a ramp, so a pixel crosses over gradually.
                uint8_t mix;
                if (spread == 0) {
                    mix = progress > threshold ? 255 : 0;
                } else {
                    const int32_t band = static_cast<int32_t>(spread) * 128;
                    const int32_t d = static_cast<int32_t>(progress) - threshold;
                    mix = d <= -band ? 0 : (d >= band ? 255
                        : static_cast<uint8_t>(((d + band) * 255) / (2 * band)));
                }

                // The known delta, not the byte difference: across the wrap that runs backwards.
                const uint8_t idx = static_cast<uint8_t>(hueA + ((kHueStep * mix) >> 8));
                // Brightness dips mid-flight, so the transition shimmers rather than crossfading.
                const uint8_t bri = static_cast<uint8_t>(255 - (mix > 128 ? 255 - mix : mix) / 2);
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), idx, bri));
            }
        }
    }

private:
    /// How far the palette advances each generation.
    static constexpr int32_t kHueStep = 40;

    BeatPhase phase_;   ///< the transition clock
};

}  // namespace mm
