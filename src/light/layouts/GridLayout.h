#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// The default grid edge, small so a fresh device shows something manageable.
constexpr lengthType defaultGridSize = 16;

/// Layout of a dense row-major 3D grid.
/// @card GridLayout.png
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class GridLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// The grid's extent on x.
    lengthType width = defaultGridSize;
    /// Its extent on y.
    lengthType height = defaultGridSize;
    /// Its extent on z, 1 for a flat panel.
    lengthType depth = 1;
    bool serpentine = false;   // odd rows wired in reverse (boustrophedon) — the standard matrix
                               // strip layout where the strip snakes back and forth row to row.

    /// The controls a user sets on the card.
    void defineControls() override {
        // Typed dimensions rather than swept magnitudes, bounded where a desktop grid needs.
        controls_.addControl("width",  width,  1, 3840);
        controls_.setNumberField(controls_.count() - 1);
        controls_.addControl("height", height, 1, 2160);
        controls_.setNumberField(controls_.count() - 1);
        controls_.addControl("depth",  depth,  1, 512);
        controls_.setNumberField(controls_.count() - 1);
        controls_.addControl("serpentine", serpentine);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// Multiply in uint32_t to detect overflow before casting.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // A wide index, and the clamped count as the bound, so no emit leaves the buffer.
        const uint32_t limit = lightCount();
        uint32_t idx = 0;
        for (lengthType z = 0; z < depth && idx < limit; z++) {
            for (lengthType y = 0; y < height && idx < limit; y++) {
                // Serpentine changes only the index-to-position order, which is what makes the map non-identity.
                const bool reverse = serpentine && (y & 1);
                for (lengthType i = 0; i < width && idx < limit; i++) {
                    const lengthType x = reverse ? static_cast<lengthType>(width - 1 - i) : i;
                    sink.pixel(static_cast<nrOfLightsType>(idx++), x, y, z);
                }
            }
        }
    }
};

} // namespace mm
