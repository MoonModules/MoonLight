#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

/// A gradient-noise field indexed straight into the palette, the plainest way to turn noise into light.
/// @card NoiseEffect.gif
///
/// It is the effect every other noise effect is a variation on. The `motion` control decides what moves, which is the whole character of the effect.
///
/// @attribution Mark Kriegsman
///
/// @moreinfo
///
/// Drift scrolls the sample coordinates and spends the third axis on depth, so volumetric slices differ. Morph spends that axis on time, so every slice of a volume shows the same field.
class NoiseEffect : public EffectBase {
public:
    /// Catalog tags: FastLED origin, the noise kernel.
    const char* tags() const override { return "⚡️🌫️"; }
    /// Volumetric under drift, where morph shows one field in every slice.
    Dim dimensions() const override { return Dim::D3; }

    /// The motion choices, in option order.
    enum Motion : uint8_t { kMotionDrift, kMotionMorph, kMotionCount };
    static constexpr const char* kMotionOptions[] = {"drift", "morph"};

    /// Whether the field drifts across the fixture or morphs in place.
    /// @guide `drift` slides the field across the fixture like weather, each axis at its own rate. `morph` holds the field still and changes it in place, which on a panel is the plasma wash.
    uint8_t motion = kMotionDrift;
    /// The field's spatial frequency: low is broad blobs, high is fine detail.
    uint8_t scale = 4;
    /// How fast the field moves.
    uint8_t bpm = 60;

    /// Publish the motion, the field's scale and its speed.
    void defineControls() override {
        controls_.addSelect("motion", motion, kMotionOptions, kMotionCount);
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
            if (motion == kMotionMorph) {
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
    BeatPhase phase_;   ///< the motion clock, since the field itself is the shared inoise8
};

} // namespace mm
