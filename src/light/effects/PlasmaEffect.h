#pragma once

#include "core/util/math16.h"            // BeatPhase: the phase of fleet time at a rate
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
    const char* tags() const override { return "💫🦅🧩"; }
    /// Volumetric: a fifth sine varies the field along the depth axis.
    Dim dimensions() const override { return Dim::D3; }
    /// The field is a function of a light's place and the time, its scale independent of the size, so each device of a wall draws its own part.
    Footprint footprint() const MM_NONBLOCKING override { return Footprint::Region; }

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
        // The phase advances with time alone, not with the grid, so speed holds on any fixture.
        phase_.advanceTo(elapsed(), bpm);
        const uint32_t phase = phase_.phase(256);

        const uint8_t step_x = static_cast<uint8_t>(256 / scale_x);
        const uint8_t step_y = static_cast<uint8_t>(256 / scale_y);
        // Depth reuses the y scale, which keeps the control surface simple.
        const uint8_t step_z = step_y;
        const uint8_t t1 = static_cast<uint8_t>(phase);
        const uint8_t t2 = static_cast<uint8_t>(phase * 2);
        const uint8_t t3 = static_cast<uint8_t>(phase * 3);

        // A flat layer keeps the four-sine path, and a volume adds a fifth driven by depth.
        const bool is3d = depth() > 1;
        const Palette& pal = *Palettes::active();
        draw::eachLight(canvas(), [&](Coord3D p) {
            const uint8_t wx = static_cast<uint8_t>(p.x), wy = static_cast<uint8_t>(p.y), wz = static_cast<uint8_t>(p.z);
            const uint8_t s1 = sin8(static_cast<uint8_t>(wx * step_x + t1));
            const uint8_t s2 = sin8(static_cast<uint8_t>(wy * step_y + t2));
            const uint8_t s3 = sin8(static_cast<uint8_t>(wx * step_x + wy * step_x - t3));
            const uint8_t s4 = sin8(static_cast<uint8_t>(wx * step_y + 128 - wy * step_y + t1));
            // The divide stays literal, since the compiler already lowers it to a multiply.
            const uint8_t hue = is3d
                ? static_cast<uint8_t>(static_cast<uint16_t>(s1 + s2 + s3 + s4 + sin8(static_cast<uint8_t>(wz * step_z + t1))) / 5 + hue_shift)
                : static_cast<uint8_t>(((s1 + s2 + s3 + s4) >> 2) + hue_shift);
            return colorFromPalette(pal, hue);
        });
    }

private:
    BeatPhase phase_;   ///< the roll clock
};

} // namespace mm
