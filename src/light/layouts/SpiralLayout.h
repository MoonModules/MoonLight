#pragma once

#include "light/layouts/LayoutBase.h"
#include <numbers>

namespace mm {

/// Layout winding LEDs up a conical spiral.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A tapering conical spiral: lights wound up a cone whose radius falls linearly to zero at the top.
/// The rise from base to tip is the height control.
/// The winding rate is fixed at the base's light density, one light per unit of its circumference.
/// The angle therefore simply accumulates while the radius shrinks, so the spiral packs tightly near the base and opens out toward the tip.
/// The whole shape is offset so its base sits at (bottomRadius, 0, bottomRadius), keeping every coordinate non-negative.
///
/// Prior art: MoonLight SpiralLayout (MoonModules/projectMM, src light nodes).
/// The geometry is reproduced exactly, its float trig running on the cold build path rather than the render loop.
/// The source's own trig calls and its truncation of each coordinate are kept as they are.
/// MoonLight's per-strip pin plumbing (nextPin) is dropped: a MoonLight layout emits coordinates only; the driver owns wiring.
class SpiralLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// Total lights along the spiral.
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
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        const uint32_t n = ledCount > 0 ? static_cast<uint32_t>(ledCount) : 0;
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
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
