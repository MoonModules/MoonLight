#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of LEDs around a wheel/disc.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A bicycle-wheel arrangement: `spokes` straight spokes radiate from a center hub, each carrying `ledsPerSpoke` LEDs spaced one unit apart from the center outward.
/// Spoke k points at angle k/spokes of a full turn; LED r on it sits at radius r+1 along that angle. lightCount() = spokes * ledsPerSpoke.
///
/// Integer-only (same discipline as SphereLayout, even though layout iteration is a cold path).
/// Angles are uint8_t (256 = full turn) and the project's sin8/cos8 LUT gives the direction. sin8/cos8 return 0..255 centerd at 128.
/// (val-128) is the signed component in [-128,127].
/// A radius-r offset is (r*(val-128))>>7 (÷128 → back to unit scale).
/// The whole wheel is shifted by +maxRadius so every coordinate is ≥ 0 (the physical address space starts at 0), giving a (2R+1)-wide bounding box.
///
/// Prior art: MoonLight ring/spoke layouts (L_MoonLight.h); MoonLight v2 WheelLayoutModule (those used double cos/sin/round, this is the integer-LUT equivalent).
class WheelLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
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
