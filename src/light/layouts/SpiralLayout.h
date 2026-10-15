#pragma once

#include "light/layouts/LayoutBase.h"
#include <numbers>

namespace mm {

/// A conical spiral: lights winding up a cone from a wide base to a point.
/// @card SpiralLayout.gif
///
/// @attribution Conical spiral
///
/// @moreinfo
///
/// The radius falls linearly to zero at the top while the winding rate stays the base's, one light per unit of its circumference. So the spiral packs tightly near the base and opens toward the tip.
class SpiralLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// How many lights wind up the spiral.
    lengthType ledCount = 640;
    /// The radius at the base, in light-units.
    lengthType bottomRadius = 10;
    /// The vertical rise from base to tip.
    lengthType height = 25;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("ledCount",     ledCount,     1, 2048);
        controls_.addControl("bottomRadius", bottomRadius, 1, 100);
        controls_.addControl("height",       height,       1, 200);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // Clamped anyway, so any value a caller writes directly stays in range.
        const uint32_t n = ledCount > 0 ? static_cast<uint32_t>(ledCount) : 0;
        return clampedCount(n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        const uint32_t limit = lightCount();
        if (limit == 0) return;

        // std::numbers is the portable π (M_PI needs _USE_MATH_DEFINES under MSVC).
        constexpr float pi = std::numbers::pi_v<float>;

        // Base sits at (bottomRadius, 0, bottomRadius) so all coords are >= 0.
        const float middleX = static_cast<float>(bottomRadius);
        const float middleZ = static_cast<float>(bottomRadius);

        // The winding rate: one light per unit of the base circumference.
        const float bottomCircumference = 2.0f * pi * static_cast<float>(bottomRadius);
        const float ledsPerRound = bottomCircumference / 1.0f;

        for (uint32_t i = 0; i < limit; i++) {
            // Pinned to zero for a single light, which would otherwise divide zero by zero.
            const float progress = (ledCount > 1)
                ? static_cast<float>(i) / static_cast<float>(ledCount - 1)
                : 0.0f;

            const float currentRadius = static_cast<float>(bottomRadius) * (1.0f - progress);
            const float currentHeight = static_cast<float>(height) * progress;
            const float radians = static_cast<float>(i) * 2.0f * pi / ledsPerRound;

            const float x = currentRadius * sinf(radians);
            const float y = currentHeight;
            const float z = currentRadius * cosf(radians);

            sink.pixel(static_cast<nrOfLightsType>(i),
               static_cast<lengthType>(x + middleX),
               static_cast<lengthType>(y),
               static_cast<lengthType>(z + middleZ));
        }
    }
};

} // namespace mm
