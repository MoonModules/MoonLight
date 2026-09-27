#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: MoonLight (Sinus, AI-generated), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Effect of a moving sine wave across the layer.
/// @card SineEffect.gif
///
/// Red, green and blue each follow a sine along one axis, a third of a turn apart.
/// So the box glows through shifting colors that scroll with time.
/// A flat layout holds its depth term constant, which reads as a red and green wash.
class SineEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫"; }
    /// Each axis drives its own color channel.
    Dim dimensions() const override { return Dim::D3; }

    /// How many waves cross the box.
    uint8_t frequency = 1;
    /// Peak brightness.
    uint8_t amplitude = 255;
    /// How fast the field scrolls.
    uint8_t bpm = 30;

    /// Publish the wave's frequency, its brightness and its speed.
    void defineControls() override {
        controls_.addControl("frequency", frequency, 1, 20);
        controls_.addControl("amplitude", amplitude, 0, 255);
        controls_.addControl("bpm", bpm, 1, 255);
    }

    /// Walk the phase, then write each channel from its own axis.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const lengthType d = depth();
        const uint8_t cpl = channelsPerLight();

        const uint32_t now = elapsed();
        // BeatPhase keeps its numerator wide until the read, so a fast frame cannot round to zero.
        phase_.advanceTo(now, bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        for (lengthType z = 0; z < d; z++) {
            const uint8_t bz = chan(static_cast<uint8_t>(z), t, 170);   // B: z axis, +240°
            for (lengthType y = 0; y < h; y++) {
                const uint8_t gy = chan(static_cast<uint8_t>(y), t, 85);  // G: y axis, +120°
                uint8_t* row = buf
                    + (static_cast<size_t>(z) * static_cast<size_t>(h) + static_cast<size_t>(y))
                      * static_cast<size_t>(w) * cpl;
                for (lengthType x = 0; x < w; x++) {
                    const uint8_t rx = chan(static_cast<uint8_t>(x), t, 0);   // R: x axis
                    if (cpl >= 1) row[0] = rx;
                    if (cpl >= 2) row[1] = gy;
                    if (cpl >= 3) row[2] = bz;
                    row += cpl;
                }
            }
        }
    }

private:
    /// One channel at its axis coordinate: a sine of the position and time, scaled by amplitude.
    uint8_t chan(uint8_t coord, uint8_t t, uint8_t phaseOffset) const {
        const uint8_t angle = static_cast<uint8_t>(coord * frequency + t + phaseOffset);
        const uint16_t s = sin8(angle);
        return static_cast<uint8_t>((s * amplitude + 255) >> 8);
    }

    BeatPhase phase_;   ///< the scroll clock
};

} // namespace mm
