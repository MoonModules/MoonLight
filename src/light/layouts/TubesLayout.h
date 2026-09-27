#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of parallel LED tubes.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// Parallel vertical tubes: nrOfTubes single columns, each ledsPerTube lights tall, spaced tubeDistance apart along x (each tube's z stays 0).
/// Tube t sits at x = t * tubeDistance; within a tube y runs 0..ledsPerTube-1, or reversed when `reversed` is set (the strip enters the column from the top).
/// Wiring order is tube-major: all of tube 0's lights, then tube 1's, and so on, matching MoonLight's outer tube loop over an inner single-column emit.
///
/// Prior art: MoonLight TubesLayout (github.com/MoonModules/projectMM), which composes SingleColumnLayout per tube. MoonLight emits coordinates only; the driver owns pin assignment, so MoonLight's per-column nextPin() plumbing is dropped.
class TubesLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    // The defaults come from MoonLight unchanged.
    /// How many tubes stand side by side.
    lengthType nrOfTubes = 4;
    /// How many lights each tube carries.
    lengthType ledsPerTube = 54;
    /// The spacing between tubes, in light-units.
    lengthType tubeDistance = 10;
    bool reversed = false;   // when set, each tube is wired from its top (y descending)

    /// The controls a user sets on the card.
    void defineControls() override {
        // Explicit ranges hold the geometry sane while keeping the bounding box bounded.
        controls_.addControl("nrOfTubes",    nrOfTubes,    1, 64);
        controls_.addControl("ledsPerTube",  ledsPerTube,  1, 255);
        controls_.addControl("tubeDistance", tubeDistance, 0, 255);
        controls_.addControl("reversed", reversed);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // A persisted value can be negative, and a negative dimension emits nothing, so report zero.
        if (nrOfTubes <= 0 || ledsPerTube <= 0) return 0;
        uint32_t n = static_cast<uint32_t>(nrOfTubes) * static_cast<uint32_t>(ledsPerTube);
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // A wide index, and the clamped count as the bound, so no emit leaves the buffer.
        const uint32_t limit = lightCount();
        uint32_t idx = 0;
        for (lengthType tube = 0; tube < nrOfTubes && idx < limit; tube++) {
            const lengthType x = static_cast<lengthType>(tube * tubeDistance);
            // Each tube is a column, the reversed flag changing only the index order.
            for (lengthType i = 0; i < ledsPerTube && idx < limit; i++) {
                const lengthType y = reversed
                    ? static_cast<lengthType>(ledsPerTube - 1 - i)
                    : i;
                sink.pixel(static_cast<nrOfLightsType>(idx++), x, y, 0);
            }
        }
    }
};

} // namespace mm
