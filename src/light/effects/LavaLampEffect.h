#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: MoonLight original (metaball lava lamp)
/// Lava-lamp effect: slow rising and merging palette blobs.
/// @card LavaLampEffect.gif
///
/// Three slow blobs whose summed field indexes the palette, so the lamp takes its colors.
/// MetaballsEffect runs the same field fast and in HSV instead.
class LavaLampEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, David Jupijn and Rising Step.
    const char* tags() const override { return "💫🦅"; }
    /// Iterates y and x, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How many blobs share the lamp.
    static constexpr uint8_t NUM_BLOBS = 3;

    /// How fast they drift.
    uint8_t bpm = 8;
    /// How large each blob's field reaches.
    uint8_t radius = 36;
    /// How strongly the summed field reads, which is what merges the blobs.
    uint8_t intensity = 200;

    /// Publish the drift, the blob size and the field's strength.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("radius", radius, 8, 255);
        controls_.addControl("intensity", intensity, 1, 255);
    }

    /// The orbits that make this a lava lamp, at class scope since a static local is flagged.
    static constexpr draw::BlobPath BLOB_PATHS[NUM_BLOBS] = {
        {1,   0,  64},
        {2,  80, 200},
        {1, 160, 100},
    };

    /// Place each blob on its orbit, then index the palette by the summed field.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // BeatPhase keeps its numerator wide until the read, so a fast frame cannot round to zero.
        phase_.advanceTo(elapsed(), bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        int16_t bx[NUM_BLOBS] = {};
        int16_t by[NUM_BLOBS] = {};
        draw::blobCenters(BLOB_PATHS, NUM_BLOBS, t, w, h, bx, by);
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint32_t field = draw::blobField(x, y, bx, by, NUM_BLOBS, r2);
                uint32_t scaled = (field * intensity) >> 8;
                uint8_t idx = scaled > 255 ? 255 : static_cast<uint8_t>(scaled);
                // The field value is the palette index, so a palette dark at its low end keeps the gaps dark.
                const RGB c = colorFromPalette(*Palettes::active(), idx);
                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    BeatPhase phase_;   ///< the drift clock
};

} // namespace mm
