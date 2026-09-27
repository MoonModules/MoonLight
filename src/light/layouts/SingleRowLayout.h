#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of one horizontal LED row (1D).
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A single horizontal row of lights: `width` lights along the x-axis starting at `start_x`, all sharing row `yposition` (y) at z=0.
/// The 1D strip primitive, a bare LED strip laid out straight.
/// `reversedOrder` flips the wiring direction, so driver index 0 lands at the high-x end instead of the low-x end.
/// The emitted coordinates are identical either way and only the index-to-position order changes, the same distinction `GridLayout` draws for serpentine wiring.
///
/// Prior art: MoonLight's SingleRowLayout (L_MoonLight.h), the same start, width, y-position and reversed fields.
/// We drop MoonLight's per-strip pin plumbing (the ledPinDIO select, its "LED NN" pin menu, and nextPin()): a MoonLight layout emits coordinates only, the driver owns pin assignment.
class SingleRowLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D1; }

    /// First x of the row. uint8_t (0..255), MoonLight's exact type/range.
    uint8_t startX = 0;
    /// Number of lights in the row. uint16_t, 1..1000, MoonLight's exact range.
    uint16_t width = 30;
    /// Row index (y). uint16_t storage, 0..255 range, MoonLight's exact bounds.
    uint16_t yPosition = 0;
    /// Wire the row from the high-x end back to start_x (index 0 at x=start_x+width-1).
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
