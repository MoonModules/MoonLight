#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: MoonLight original (metaballs)
/// Metaballs effect: smooth merging blobs via a scalar field.
/// @card MetaballsEffect.gif
///
/// Each ball contributes a field that falls off with distance, and the sum picks the color.
/// Where two balls approach, their fields add and the blobs appear to merge.
/// LavaLampEffect runs the same field slowly through a palette instead.
class MetaballsEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, David Jupijn and Rising Step.
    const char* tags() const override { return "💫🦅"; }
    /// Iterates y and x, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the balls travel their orbits.
    uint8_t bpm = 30;
    /// How far each ball's field reaches.
    uint8_t radius = 28;
    /// How many balls share the field, each on its own path.
    uint8_t count = 4;
    /// Walks the whole field around the palette.
    uint8_t hue_shift = 0;

    /// The orbit table's size, and so the ball count's ceiling.
    static constexpr uint8_t MAX_BALLS = 8;

    /// Publish the orbit speed, the ball size and count, and the palette shift.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("radius", radius, 4, 255);
        controls_.addControl("count", count, 1, MAX_BALLS);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    /// This effect's own orbits, at class scope since a static local is flagged in a nonblocking function.
    static constexpr draw::BlobPath BLOB_PATHS[MAX_BALLS] = {
        {1,   0,  64}, {2,  30,  94}, {3,  60, 124}, {1, 120, 184},
        {2, 160,  16}, {3, 200, 210}, {1,  90, 150}, {2, 220,  40},
    };

    /// Place each ball on its orbit, then color every pixel by the summed field.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        uint32_t now = elapsed();
        // BeatPhase keeps its numerator wide until the read, so a fast frame cannot round to zero.
        phase_.advanceTo(now, bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        const uint8_t n = count < MAX_BALLS ? count : MAX_BALLS;
        int16_t bx[MAX_BALLS];
        int16_t by[MAX_BALLS];
        draw::blobCenters(BLOB_PATHS, n, t, w, h, bx, by);

        // The field's strength, which falls off with the squared distance.
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * w * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint32_t field = draw::blobField(x, y, bx, by, n, r2);
                uint8_t bright = field > 255 ? 255 : static_cast<uint8_t>(field);
                uint8_t hue = static_cast<uint8_t>((field >> 1) + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue, bright);

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    BeatPhase phase_;   ///< the orbit clock
};

} // namespace mm
