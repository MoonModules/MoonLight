#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Water-ripple effect: distance from the center drives a wave phase.
/// @card RipplesEffect.gif
///
/// Each column of the floor takes a phase from its distance to the center.
/// One pixel per column then lights at the height that phase gives, so the surface ripples.
///
/// @moreinfo
///
/// ## It is genuinely 3D
///
/// The effect writes a height across y, so it wants real depth to read as a surface.
/// A flat layout collapses it to a single row, which is honest rather than a failure.
/// WaterRippleEffect simulates a real wave equation on a plane instead.
///
/// Float trig runs in the loop, since the integer preference is for per-light color work.
class RipplesEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, a water ripple.
    const char* tags() const override { return "💫🦅"; }
    /// The surface fills a volume, so all three axes are used.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the ripples travel, where 0 freezes them.
    uint8_t speed = 50;
    /// The wavefront spacing, where low gives tight rings.
    uint8_t interval = 128;

    /// Publish the ripple's speed and its spacing.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 99);
        controls_.addControl("interval", interval, 1, 254);
    }

    /// Light one pixel per floor column, at the height its wave phase gives.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const lengthType d = depth();
        const uint8_t cpl = channelsPerLight();

        // Every column lights at most one height, so the rest must be cleared.
        std::memset(buf, 0, static_cast<size_t>(nrOfLights()) * cpl);

        // Scaled to the layout's height, so the look holds at any grid size.
        const float rippleInterval = 1.3f * ((255.0f - static_cast<float>(interval)) / 128.0f)
                                     * std::sqrt(static_cast<float>(h));
        if (rippleInterval < 0.01f) return;

        // The animation, which a speed of 0 freezes.
        const float timeInterval = static_cast<float>(elapsed())
                                  / (100.0f - static_cast<float>(speed)) / 6.4f;

        const float cx = static_cast<float>(w - 1) / 2.0f;
        const float cz = static_cast<float>(d - 1) / 2.0f;
        const nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;

        for (lengthType z = 0; z < d; z++) {
            for (lengthType x = 0; x < w; x++) {
                const float dx = static_cast<float>(x) - cx;
                const float dz = static_cast<float>(z) - cz;
                // The distance from the floor's center, scaled to the height.
                const float dist = std::sqrt(dx * dx + dz * dz) / 9.899495f * static_cast<float>(h);
                const float phase = dist / rippleInterval + timeInterval;
                const lengthType y = static_cast<lengthType>(
                    std::floor(static_cast<float>(h) / 2.0f * (1.0f + std::sin(phase))));
                if (y < 0 || y >= h) continue;

                const uint8_t hue = static_cast<uint8_t>(
                    elapsed() / 50u + static_cast<uint32_t>(x) * 3u + static_cast<uint32_t>(z) * 7u);
                // Already a wheel index, so it reads straight as a palette index.
                const RGB c = colorFromPalette(*Palettes::active(), hue, 255);

                // The buffer's own row-major layout.
                const nrOfLightsType idx = static_cast<nrOfLightsType>(z) * wh
                                         + static_cast<nrOfLightsType>(y) * w + x;
                uint8_t* px = buf + static_cast<size_t>(idx) * cpl;
                if (cpl >= 1) px[0] = c.r;
                if (cpl >= 2) px[1] = c.g;
                if (cpl >= 3) px[2] = c.b;
            }
        }
    }
};

} // namespace mm
