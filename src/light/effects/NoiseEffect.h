#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: a gradient-noise field through the palette, drifting across the fixture or morphing in place.
/// @card NoiseEffect.gif
/// Author: FastLED inoise field (Mark Kriegsman), and MoonLight's Noise2D for the morph form.
///
/// The plainest way to turn a noise field into light, and the reference the others vary on.
/// One control decides what moves, which is the whole character of the effect.
///
/// @moreinfo
///
/// ## Drift against morph is an argument over the third axis
///
/// Drift scrolls the sample coordinates, so the field slides across the fixture like weather.
/// Each axis scrolls at its own rate, so it flows rather than translating rigidly.
/// A volumetric fixture spends the third axis on the light's own depth, so its slices differ.
///
/// Morph holds the coordinates still and spends that axis on time instead.
/// The field then changes in place without going anywhere, which on a panel is the plasma wash.
/// A volumetric fixture has no axis left for depth, so every slice shows the same field.
class NoiseEffect : public EffectBase {
public:
    /// Catalog tags: FastLED origin, the noise kernel.
    const char* tags() const override { return "⚡️🌫️"; }
    /// Volumetric under drift, where morph shows one field in every slice.
    Dim dimensions() const override { return Dim::D3; }

    static constexpr const char* kMotionOptions[] = {"drift", "morph"};

    /// Whether the field moves across the fixture or changes in place.
    uint8_t motion = 0;
    /// The field's spatial frequency, where lower is broader.
    uint8_t scale = 4;
    /// How fast it moves.
    uint8_t bpm = 60;

    /// Publish the motion, the field's scale and its speed.
    void defineControls() override {
        controls_.addSelect("motion", motion, kMotionOptions, 2);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("bpm", bpm, 1, 255);
    }

    /// Sample the field per light and read the palette at its value.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width(), h = height(), d = depth();
        const uint8_t cpl = channelsPerLight();
        const nrOfLightsType count = nrOfLights();
        const nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;
        if (cpl == 0 || w == 0 || h == 0) return;

        // Accumulated incrementally, and kept wide until the read so a fast frame cannot stall it.
        phase_.advanceScaled(elapsed(), static_cast<uint64_t>(bpm) * w * 64);
        const uint32_t t = phase_.phase(1);
        const uint8_t sc = scale ? scale : 1;

        for (nrOfLightsType i = 0; i < count; i++) {
            const nrOfLightsType rem = i % wh;
            const lengthType x = static_cast<lengthType>(rem % w);
            const lengthType y = static_cast<lengthType>(rem / w);
            const lengthType z = static_cast<lengthType>(i / wh);

            uint8_t n;
            if (motion == 1) {
                // Time is the third axis here, so the field changes without traveling.
                n = inoise8(static_cast<uint32_t>(x) * 256u / sc,
                            static_cast<uint32_t>(y) * 256u / sc, t);
            } else {
                // The coordinates scroll, each axis at its own rate so the field flows.
                const uint32_t nx = (static_cast<uint32_t>(x) * 256u + t) / sc;
                const uint32_t ny = (static_cast<uint32_t>(y) * 256u + t / 3u) / sc;
                n = d > 1 ? inoise8(nx, ny, (static_cast<uint32_t>(z) * 256u + t / 5u) / sc)
                          : inoise8(nx, ny);
            }

            const RGB c = colorFromPalette(*Palettes::active(), n);
            const size_t offset = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[offset + 0] = c.r;
            if (cpl >= 2) buf[offset + 1] = c.g;
            if (cpl >= 3) buf[offset + 2] = c.b;
        }
    }

private:
    BeatPhase phase_;   ///< the motion clock
    // The field itself is the shared inoise8, which this effect only scales coordinates into.
};

} // namespace mm
