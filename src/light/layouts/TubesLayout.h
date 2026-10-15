#pragma once

#include "light/layouts/LayoutBase.h"
#include "light/layouts/SingleColumnLayout.h"   // the column each tube is

namespace mm {

/// Parallel vertical tubes, each a column of lights, standing side by side.
/// @card TubesLayout.gif
///
/// @technical
///
/// Each tube is a @ref SingleColumnLayout object, placed tube by tube.
class TubesLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    /// How many tubes stand side by side.
    lengthType nrOfTubes = 4;
    /// How many lights each tube carries.
    lengthType ledsPerTube = 54;
    /// The spacing between tubes, in light-units.
    lengthType tubeDistance = 10;
    /// Wire each tube from its top.
    bool reversed = false;

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
        return clampedCount(n);
    }

    /// Emit every light's coordinate, in wiring order: each tube a single column, the whole of one before the next.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink, lightCount()};
        SingleColumnLayout column;
        column.height = static_cast<uint16_t>(ledsPerTube);
        column.reversed_order = reversed;
        for (lengthType tube = 0; tube < nrOfTubes && e.idx < e.limit; tube++) {
            column.xposition = static_cast<uint16_t>(tube * tubeDistance);
            e.place(column);
        }
    }
};

} // namespace mm
