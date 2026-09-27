#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Effect moving a lit sphere through a 3D layout.
/// @card SphereMoveEffect.gif
///
/// A hollow sphere whose shell sweeps the volume on a Lissajous path, breathing as it goes.
/// Every voxel inside the one-unit shell lights from the palette, jittered so it shimmers.
///
/// Prior art: MoonLight's SphereMove, whose oscillator and shell test this reproduces.
///
/// @moreinfo
///
/// ## Float per voxel, deliberately
///
/// The distance test is a square root per voxel, kept for exact fidelity with the source.
/// The time base is float throughout, so the sweep and the breathing integrate continuously.
/// Dividing in integers first would quantize both into visible steps.
class SphereMoveEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, and 3D-native.
    const char* tags() const override { return "💫"; }
    /// A sphere needs the whole volume.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the sphere sweeps, where higher shrinks the divisor.
    uint8_t speed = 50;

    /// Publish the sweep rate.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 99);
    }

    /// Place the sphere on its path, then light every voxel inside its shell.
    void tick() MM_NONBLOCKING override {
        const int w = width();
        const int h = height();
        const int d = depth();

        const draw::Canvas cv = canvas();

        // A fill rather than a fade, since this redraws every pixel and wants the buffer blank now.
        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t ms = elapsed();

        // All float, so time advances continuously: an integer divide first quantizes the sweep.
        const float time_interval = static_cast<float>(ms) / static_cast<float>(100 - speed) / 6.4f;

        const float ox = w / 2.0f * (1.0f + sinf(time_interval));
        const float oy = h / 2.0f * (1.0f + cosf(time_interval));
        const float oz = d / 2.0f * (1.0f + cosf(time_interval));

        const float diameter = 2.0f + sinf(time_interval / 3.0f);

        // The index drifts with time, and each lit voxel adds a jitter of its own.
        const uint8_t indexBase = static_cast<uint8_t>(ms / 50);

        for (int z = 0; z < d; z++) {
            const float dz = z - oz;
            for (int y = 0; y < h; y++) {
                const float dy = y - oy;
                for (int x = 0; x < w; x++) {
                    const float dx = x - ox;
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist > diameter && dist < diameter + 1.0f) {
                        const uint8_t index = static_cast<uint8_t>(indexBase + rng_.below(64));
                        draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)},
                                    colorFromPalette(*Palettes::active(), index));
                    }
                }
            }
        }
    }

private:
    Random8 rng_;   ///< the per-voxel palette jitter
};

} // namespace mm
