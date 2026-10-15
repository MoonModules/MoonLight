#pragma once

#include "light/layouts/LayoutBase.h"


namespace mm {

/// A vertical line of lights at a fixed x, the 1D column.
/// @card SingleColumnLayout.gif
///
/// @attribution Limpkin
class SingleColumnLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D1; }
    /// The row the column starts at.
    uint8_t  start_y = 0;
    /// How many lights the column holds.
    uint16_t height = 30;
    /// The column's x.
    uint16_t xposition = 0;
    /// Wire it top to bottom rather than bottom to top.
    bool     reversed_order = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("starting Y", start_y, 0, 255);
        controls_.addControl("height", height, 1, 1000);
        controls_.addControl("X position", xposition, 0, 255);
        controls_.addControl("reversed order", reversed_order);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // One light per y step. Clamp to the index type max, as GridLayout does.
        const uint32_t n = static_cast<uint32_t>(height);
        return clampedCount(n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink, lightCount()};
        // Only the y walk direction depends on the reversed flag.
        for (int32_t i = 0; i < height; i++) {
            const int32_t y = reversed_order ? start_y + height - 1 - i : start_y + i;
            if (!e.pixel(static_cast<lengthType>(xposition), static_cast<lengthType>(y), 0)) return;
        }
    }
};

} // namespace mm
