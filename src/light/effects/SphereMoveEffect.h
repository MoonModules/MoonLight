#pragma once

#include "core/util/math16.h"            // hashInt: the jitter by place and tick
#include "light/effects/EffectBase.h"

namespace mm {

/// A hollow spherical shell that bounces through the 3D volume, its surface colored from the palette, leaving no trail.
/// @card SphereMoveEffect.gif
///
/// The shell sweeps the volume on a Lissajous path, breathing as it goes. Every voxel inside the one-unit shell lights from the palette, jittered so it shimmers.
///
/// @technical
///
/// The distance test is a square root per voxel, and the time base is float throughout, so the sweep and the breathing integrate continuously. Dividing in integers first quantizes both into visible steps.
///
/// The jitter is a hash of the voxel's place and a 20 ms tick of fleet time, so every device of a wall shimmers its part of one shell alike.
class SphereMoveEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, and 3D-native.
    const char* tags() const override { return "💫🧩"; }
    /// A sphere needs the whole volume.
    Dim dimensions() const override { return Dim::D3; }
    /// The shell is a function of a voxel's place and the time, so each device of a wall draws its part of one sphere.
    Footprint footprint() const MM_NONBLOCKING override { return Footprint::Region; }

    /// How fast the sphere moves through the volume.
    uint8_t speed = 50;

    /// Publish the sweep rate.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 99);
    }

    /// Place the sphere on its path through the whole picture, then light every voxel inside its shell.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const Coord3D whole = cv.extent();
        const uint32_t ms = elapsed();

        // All float, so time advances continuously: an integer divide first quantizes the sweep.
        const float time_interval = static_cast<float>(ms) / static_cast<float>(100 - speed) / 6.4f;

        const float ox = whole.x / 2.0f * (1.0f + sinf(time_interval));
        const float oy = whole.y / 2.0f * (1.0f + cosf(time_interval));
        const float oz = whole.z / 2.0f * (1.0f + cosf(time_interval));

        const float diameter = 2.0f + sinf(time_interval / 3.0f);

        // The index drifts with time, and each lit voxel adds a jitter of its own.
        const uint8_t indexBase = static_cast<uint8_t>(ms / 50);
        const uint32_t tick = ms / 20;
        const Palette& pal = *Palettes::active();

        draw::eachLight(cv, [&](Coord3D p) {
            const float dx = p.x - ox, dy = p.y - oy, dz = p.z - oz;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist <= diameter || dist >= diameter + 1.0f) return RGB{0, 0, 0};
            const uint8_t jitter = static_cast<uint8_t>(hashInt(static_cast<uint16_t>(p.x), static_cast<uint16_t>(p.y), static_cast<uint16_t>(p.z), tick) & 63);
            return colorFromPalette(pal, static_cast<uint8_t>(indexBase + jitter));
        });
    }
};

} // namespace mm
