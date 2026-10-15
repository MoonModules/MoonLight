#pragma once

#include "light/layouts/LayoutBase.h"
#include "light/layouts/RingLayout.h"   // the rings each lamp is made of

namespace mm {

/// A pair of car headlights: concentric rings of lights placed like a car's front lamps.
/// @card CarLightsLayout.gif
///
/// Nested rings of 1, 8, 12, 16 and 24 lights form each lamp, with a ring of 32 more on the outer two. Side strips join them, in a fixed arrangement drawn with @ref RingLayout geometry.
///
/// @attribution Eric Marciniak, Polar coordinates
///
/// @moreinfo
///
/// Every ring, the lamps' and the strips' quarter arcs alike, is a @ref RingLayout object with its center set. A lamp is drawn by the same layout a user adds as a Ring.
class CarLightsLayout : public LayoutBase {
public:
    /// The overall size: the spacing out from each lamp's center.
    uint8_t scale = 2;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("scale", scale, 1, 10);
    }

    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // The same walk as the emit, so a partial arc's count cannot disagree.
        LightEmitter e;
        walk(e);
        return static_cast<nrOfLightsType>(e.idx);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink};
        walk(e);
    }

private:
    /// The one home for the geometry: every lamp's rings, then both strips with their arcs, in wiring order.
    void walk(LightEmitter& e) const {
        constexpr lengthType kLeft = 9;   // the margin every part is placed from
        RingLayout ring;   // one ring at a time: its public settings set, then placed as the next part
        ring.scale = scale;
        // Each lamp is concentric rings about its own center, starting at 90 degrees: the inner lamps five rings, the outer six.
        struct Lamp { lengthType cx, cy; uint8_t rings; };
        constexpr uint8_t kRingSizes[] = {1, 8, 12, 16, 24, 32};
        constexpr Lamp kLamps[] = {{kLeft + 11, 8, 5}, {kLeft, 6, 6}, {kLeft + 25, 8, 5}, {kLeft + 36, 6, 6}};
        ring.angleFirst = 90;
        for (const Lamp& lamp : kLamps) {
            ring.centerX = lamp.cx;
            ring.centerY = lamp.cy;
            for (uint8_t r = 0; r < lamp.rings; r++) {
                ring.nrOfLEDs = kRingSizes[r];
                e.place(ring);
            }
        }

        // Each strip is a straight run, then a quarter of a 52-light ring from 180 degrees, then an upright.
        ring.nrOfLEDs = 52;
        ring.angleFirst = 180;
        ring.rotation = 90;
        for (int x = (kLeft + 16) * scale; x >= kLeft * scale; x--)
            e.pixel(static_cast<lengthType>(x), static_cast<lengthType>(15 * scale), 0);
        ring.centerX = kLeft;
        ring.centerY = 6;
        e.place(ring);
        for (int y = 5; y >= 1; y--)
            e.pixel(0, static_cast<lengthType>(y * scale), 0);

        // The right strip walks outward, and its arc turns counter-clockwise.
        for (int x = (kLeft + 19) * scale; x <= (kLeft + 35) * scale; x++)
            e.pixel(static_cast<lengthType>(x), static_cast<lengthType>(15 * scale), 0);
        ring.centerX = kLeft + 36;
        ring.clockwise = false;
        e.place(ring);
        for (int y = 5; y >= 1; y--)
            e.pixel(static_cast<lengthType>((kLeft + 44) * scale), static_cast<lengthType>(y * scale), 0);
    }
};

} // namespace mm
