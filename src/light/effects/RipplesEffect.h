#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Distance from the center sets a per-column wave phase, so the lit surface ripples like water.
/// @card RipplesEffect.gif
///
/// The effect writes a height across y, so it wants real depth to read as a surface, and a flat layout collapses it to a single row. Its sibling @ref WaterRippleEffect simulates a real wave equation on a plane.
///
/// @attribution David Jupijn
class RipplesEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, a water ripple.
    const char* tags() const override { return "💫🦅🧩"; }
    /// The surface fills a volume, so all three axes are used.
    Dim dimensions() const override { return Dim::D3; }
    /// A column's height is a function of its place and the time, so each device of a wall draws its part of one surface.
    Footprint footprint() const MM_NONBLOCKING override { return Footprint::Region; }

    /// Wave animation rate.
    uint8_t speed = 50;
    /// Wavefront spacing; low is tight rings, high is wide.
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
        // The surface spans the whole picture: its center and its height are the wall's, and each column converts to this device's own grid.
        const draw::Canvas cv = canvas();
        const Coord3D whole = cv.extent();

        // Every column lights at most one height, so the rest must be cleared.
        std::memset(buf, 0, static_cast<size_t>(nrOfLights()) * cpl);

        // Scaled to the layout's height, so the look holds at any grid size.
        const float rippleInterval = 1.3f * ((255.0f - static_cast<float>(interval)) / 128.0f)
                                     * std::sqrt(static_cast<float>(whole.y));
        if (rippleInterval < 0.01f) return;

        // The animation phase, which a higher speed advances faster.
        const float timeInterval = static_cast<float>(elapsed())
                                  / (100.0f - static_cast<float>(speed)) / 6.4f;

        const float cx = static_cast<float>(whole.x - 1) / 2.0f;
        const float cz = static_cast<float>(whole.z - 1) / 2.0f;
        const nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;
        const Palette& pal = *Palettes::active();
        const uint32_t hueBase = elapsed() / 50u;

        for (lengthType z = 0; z < d; z++) {
            for (lengthType x = 0; x < w; x++) {
                const lengthType wx = static_cast<lengthType>(x + cv.origin.x), wz = static_cast<lengthType>(z + cv.origin.z);
                const float dx = static_cast<float>(wx) - cx;
                const float dz = static_cast<float>(wz) - cz;
                // The distance from the floor's center, scaled to the height.
                const float dist = std::sqrt(dx * dx + dz * dz) / 9.899495f * static_cast<float>(whole.y);
                const float phase = dist / rippleInterval + timeInterval;
                const lengthType wy = static_cast<lengthType>(std::floor(static_cast<float>(whole.y) / 2.0f * (1.0f + std::sin(phase))));
                const lengthType y = cv.local({wx, wy, wz}).y;
                if (y < 0 || y >= h) continue;

                // Already a wheel index, so it reads straight as a palette index.
                const RGB c = colorFromPalette(pal, static_cast<uint8_t>(hueBase + static_cast<uint32_t>(wx) * 3u + static_cast<uint32_t>(wz) * 7u), 255);

                // The buffer's own row-major layout.
                const nrOfLightsType idx = static_cast<nrOfLightsType>(z) * wh + static_cast<nrOfLightsType>(y) * w + x;
                uint8_t* px = buf + static_cast<size_t>(idx) * cpl;
                if (cpl >= 1) px[0] = c.r;
                if (cpl >= 2) px[1] = c.g;
                if (cpl >= 3) px[2] = c.b;
            }
        }
    }
};

} // namespace mm
