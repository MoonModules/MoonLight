#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

/// Summed sine waves on orthogonal and diagonal axes make large rolling blobs, in 3D on volumetric layouts.
/// @card PlasmaEffect.gif
///
/// Four sines are summed per pixel, or five in a volume, with their average picking the palette index. A larger scale means a smaller per-pixel step, so the blobs grow bigger and calmer.
///
/// @attribution FastLED, WLED, David Jupijn
class PlasmaEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, David Jupijn and Rising Step.
    const char* tags() const override { return "💫🦅"; }
    /// Volumetric: a fifth sine varies the field along the depth axis.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the field rolls.
    uint8_t bpm = 30;
    /// Blob size along x; larger is bigger and calmer.
    uint8_t scale_x = 48;
    /// Blob size along y, which the depth axis reuses.
    uint8_t scale_y = 48;
    /// Rotates the palette index.
    uint8_t hue_shift = 0;

    /// Publish the roll speed, both scales and the palette shift.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("scale_x", scale_x, 1, 255);
        controls_.addControl("scale_y", scale_y, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    /// Sum the sines per pixel and read the palette at their average.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        lengthType d = depth();
        uint8_t cpl = channelsPerLight();

        // The phase advances with time alone, not with the grid, so speed holds on any fixture.
        phase_.advanceTo(elapsed(), bpm);
        const uint32_t phase = phase_.phase(256);

        uint8_t step_x = static_cast<uint8_t>(256 / scale_x);
        uint8_t step_y = static_cast<uint8_t>(256 / scale_y);
        // Depth reuses the y scale, which keeps the control surface simple.
        uint8_t step_z = step_y;
        uint8_t t1 = static_cast<uint8_t>(phase);
        uint8_t t2 = static_cast<uint8_t>(phase * 2);
        uint8_t t3 = static_cast<uint8_t>(phase * 3);

        // A flat layer keeps the four-sine path, and a volume adds a fifth driven by depth.
        const bool is3d = (d > 1);
        for (lengthType z = 0; z < d; z++) {
            uint8_t s5_z = is3d
                ? sin8(static_cast<uint8_t>(static_cast<uint8_t>(z) * step_z + t1))
                : 0;
            for (lengthType y = 0; y < h; y++) {
                uint8_t s2_y = sin8(static_cast<uint8_t>(static_cast<uint8_t>(y) * step_y + t2));
                uint8_t yx_off = static_cast<uint8_t>(static_cast<uint8_t>(y) * step_x - t3);
                uint8_t yx_neg = static_cast<uint8_t>(128 - static_cast<uint8_t>(y) * step_y + t1);

                uint8_t* row = buf
                    + (static_cast<size_t>(z) * static_cast<size_t>(h) + static_cast<size_t>(y))
                      * static_cast<size_t>(w) * cpl;
                for (lengthType x = 0; x < w; x++) {
                    uint8_t xs = static_cast<uint8_t>(static_cast<uint8_t>(x) * step_x);
                    uint8_t s1 = sin8(static_cast<uint8_t>(xs + t1));
                    uint8_t s3 = sin8(static_cast<uint8_t>(xs + yx_off));
                    uint8_t s4 = sin8(static_cast<uint8_t>(
                        static_cast<uint8_t>(static_cast<uint8_t>(x) * step_y) + yx_neg));
                    // The divide stays literal, since the compiler already lowers it to a multiply.
                    uint8_t hue = is3d
                        ? static_cast<uint8_t>(
                              (static_cast<uint16_t>(s1 + s2_y + s3 + s4 + s5_z) / 5) + hue_shift)
                        : static_cast<uint8_t>(((s1 + s2_y + s3 + s4) >> 2) + hue_shift);
                    RGB c = colorFromPalette(*Palettes::active(), hue);

                    if (cpl >= 1) row[0] = c.r;
                    if (cpl >= 2) row[1] = c.g;
                    if (cpl >= 3) row[2] = c.b;
                    row += cpl;
                }
            }
        }
    }

private:
    BeatPhase phase_;   ///< the roll clock
};

} // namespace mm
