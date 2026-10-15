#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// A bicycle wheel: spokes radiating from a center hub, each a straight row of lights.
/// @card WheelLayout.gif
///
/// @attribution Polar coordinates
///
/// @moreinfo
///
/// Angles are a byte, 256 to the turn, read through the sin8 and cos8 table, so the placement is integer-only.
class WheelLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    /// How many spokes radiate from the hub.
    uint16_t spokes = 8;
    /// How many lights each spoke carries.
    uint16_t ledsPerSpoke = 10;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("spokes", spokes, 2, 64);
        controls_.addControl("ledsPerSpoke", ledsPerSpoke, 1, 256);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        return static_cast<nrOfLightsType>(spokes) * static_cast<nrOfLightsType>(ledsPerSpoke);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        const int32_t maxR = ledsPerSpoke;             // outermost radius (center shift)
        nrOfLightsType idx = 0;
        for (uint16_t s = 0; s < spokes; s++) {
            // Spoke angle in uint8 turn units; the signed direction components.
            const uint8_t a = static_cast<uint8_t>((static_cast<uint32_t>(s) * 256u) / spokes);
            const int32_t cx = static_cast<int32_t>(cos8(a)) - 128;   // [-128,127]
            const int32_t sy = static_cast<int32_t>(sin8(a)) - 128;
            for (uint16_t r = 0; r < ledsPerSpoke; r++) {
                const int32_t radius = r + 1;          // first LED sits one step from center
                // offset = radius * component / 128, then shift so coords are ≥ 0.
                const int32_t x = maxR + ((radius * cx) >> 7);
                const int32_t y = maxR + ((radius * sy) >> 7);
                sink.pixel(idx,
                   static_cast<lengthType>(x),
                   static_cast<lengthType>(y),
                   0);
                idx++;
            }
        }
    }
};

} // namespace mm
