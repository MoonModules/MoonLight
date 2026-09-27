#pragma once

#include "light/layouts/LayoutBase.h"


namespace mm {

/// Layout of one vertical LED column (1D).
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A vertical line of lights at a fixed x, running along y.
/// The geometry is reproduced exactly: the source emits one light per y across the height, forward or reversed.
///
/// MoonLight's pin and wiring controls are dropped, since a MoonLight layout emits coordinates only and the driver owns pins.
class SingleColumnLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D1; }
    /// Geometry controls mirror MoonLight's defaults and ranges 1:1.
    uint8_t  start_y = 0;          // "starting Y", 0..255
    /// How many lights the column holds.
    uint16_t height = 30;
    /// Which column it occupies.
    uint16_t xposition = 0;
    /// Whether index 0 is the top.
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
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        const uint32_t n = static_cast<uint32_t>(height);
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // Only the y walk direction depends on the reversed flag; the clamped count bounds it.
        const uint32_t limit = lightCount();
        const lengthType x = static_cast<lengthType>(xposition);
        uint32_t idx = 0;
        if (reversed_order) {
            for (int32_t y = static_cast<int32_t>(start_y) + static_cast<int32_t>(height) - 1;
                 y >= static_cast<int32_t>(start_y) && idx < limit; y--) {
                sink.pixel(static_cast<nrOfLightsType>(idx++), x, static_cast<lengthType>(y), 0);
            }
        } else {
            for (int32_t y = static_cast<int32_t>(start_y);
                 y < static_cast<int32_t>(start_y) + static_cast<int32_t>(height) && idx < limit; y++) {
                sink.pixel(static_cast<nrOfLightsType>(idx++), x, static_cast<lengthType>(y), 0);
            }
        }
    }
};

} // namespace mm
