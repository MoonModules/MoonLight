#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Lights on the surface of a hollow sphere: a shell one light thick, nothing inside.
/// @card SphereLayout.gif
///
/// @technical
///
/// A lattice point is on the shell when its distance from the center rounds to the radius, the half-open band [radius − 0.5, radius + 0.5). Distances compare squared, so no light costs a square root.
class SphereLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// The shell's radius, in lights.
    lengthType radius = 4;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("radius", radius, 1, 64);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // Recomputed only on a radius change, and cheap beside rendering.
        nrOfLightsType n = 0;
        forEachShellPoint([](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, nullptr, &n);
        return n;
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        forEachShellPoint(sink.cb, sink.ctx, nullptr);
    }

private:
    /// Walk the bounding lattice once, so the count and the emit share one predicate.
    void forEachShellPoint(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        const int32_t r = radius;
        // A half-open band, compared in squared integer space by scaling the whole inequality.
        const int32_t lo = (2 * r - 1) * (2 * r - 1);   // 4*(r-0.5)^2
        const int32_t hi = (2 * r + 1) * (2 * r + 1);   // 4*(r+0.5)^2
        nrOfLightsType idx = 0;
        for (int32_t z = 0; z <= 2 * r; z++) {
            const int32_t dz = z - r;
            for (int32_t y = 0; y <= 2 * r; y++) {
                const int32_t dy = y - r;
                for (int32_t x = 0; x <= 2 * r; x++) {
                    const int32_t dx = x - r;
                    const int32_t d4 = 4 * (dx * dx + dy * dy + dz * dz);
                    if (d4 < lo || d4 >= hi) continue;
                    cb(ctx, idx,
                       static_cast<lengthType>(x),
                       static_cast<lengthType>(y),
                       static_cast<lengthType>(z));
                    idx++;
                }
            }
        }
        if (count) *count = idx;
    }
};

} // namespace mm
