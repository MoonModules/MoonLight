#pragma once

#include "light/layouts/LayoutBase.h"
#include "light/layouts/RingLayout.h"   // the rings the disc is made of

namespace mm {

/// The classic 241-light disc: nine concentric rings of 1, 8, 12, 16, 24, 32, 40, 48 and 60 lights sharing a center.
/// @card Rings241Layout.gif
///
/// @attribution Polar coordinates
///
/// @guide
///
/// ## outside in
///
/// A disc is soldered from its center or from its 60-light rim, and this follows the end the data enters. Each ring keeps the direction it is wired in.
///
/// @moreinfo
///
/// Every ring is a @ref RingLayout object about the disc's shared center.
class Rings241Layout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    /// The size: scales the ring radii and the shared center.
    uint8_t scale = 2;
    /// Light 0 on the outer ring, wired inward rather than outward.
    bool outside_in = false;
    /// The angle light 0 of each ring sits at, in degrees.
    uint16_t angleFirst = 0;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("scale", scale, 1, 10);
        controls_.addControl("outside in", outside_in);
        controls_.addControl("angleFirst", angleFirst, 0, 359);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // Fixed by construction: the nine ring sizes sum to 241 and no light is culled.
        nrOfLightsType total = 0;
        for (uint8_t n : kRingSizes) total += n;
        return total;  // 1+8+12+16+24+32+40+48+60 = 241
    }

    /// Emit every light's coordinate, in wiring order: a ring per size about the disc's shared center.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink};
        RingLayout ring;
        // The shared center, the outer ring's own, truncated to an integer as the source does.
        ring.centerX = ring.centerY = static_cast<lengthType>(1.1f * RingLayout::radius(60));
        ring.scale = scale;
        ring.angleFirst = angleFirst;
        // Rings run smallest first, or largest first when wired from the outside in.
        constexpr uint8_t kRings = sizeof(kRingSizes) / sizeof(kRingSizes[0]);
        for (uint8_t r = 0; r < kRings; r++) {
            ring.nrOfLEDs = kRingSizes[outside_in ? kRings - 1 - r : r];
            e.place(ring);
        }
    }

private:
    /// The nine ring sizes of the 241-LED disc, inner to outer.
    static constexpr uint8_t kRingSizes[9] = {1, 8, 12, 16, 24, 32, 40, 48, 60};
};

} // namespace mm
