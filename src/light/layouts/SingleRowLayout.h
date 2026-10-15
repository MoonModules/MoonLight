#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// A horizontal line of lights at a fixed y: the 1D row, a bare strip laid straight.
/// @card SingleRowLayout.gif
///
/// @attribution Limpkin
class SingleRowLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D1; }

    /// The column the row starts at.
    uint8_t startX = 0;
    /// How many lights the row holds.
    uint16_t width = 30;
    /// The row's y.
    uint16_t yPosition = 0;
    /// Wire it right to left rather than left to right.
    bool reversedOrder = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("starting X", startX, 0, 255);
        controls_.addControl("width", width, 1, 1000);
        controls_.addControl("Y position", yPosition, 0, 255);
        controls_.addControl("reversed order", reversedOrder);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // width is the whole count and already fits, so no clamp is needed.
        return static_cast<nrOfLightsType>(width);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // Only the x walk direction depends on reversedOrder; the row and z are fixed.
        const lengthType y = static_cast<lengthType>(yPosition);
        nrOfLightsType idx = 0;
        if (reversedOrder) {
            for (int32_t x = static_cast<int32_t>(startX) + width - 1;
                 x >= static_cast<int32_t>(startX); x--) {
                sink.pixel(idx++, static_cast<lengthType>(x), y, 0);
            }
        } else {
            for (int32_t x = static_cast<int32_t>(startX);
                 x < static_cast<int32_t>(startX) + width; x++) {
                sink.pixel(idx++, static_cast<lengthType>(x), y, 0);
            }
        }
    }
};

} // namespace mm
