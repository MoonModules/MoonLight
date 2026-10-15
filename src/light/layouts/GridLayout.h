#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// The default grid edge, small so a fresh device shows something manageable.
constexpr lengthType defaultGridSize = 16;

/// A dense grid of lights in 1D, 2D or 3D: every position holds a light, wired row by row.
/// @card GridLayout.gif
///
/// The wiring runs x fastest, then y, then z, and a strip that snakes back and forth runs every other row in reverse.
///
/// @attribution Boustrophedon
class GridLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// The grid's extent on x.
    lengthType width = defaultGridSize;
    /// Its extent on y.
    lengthType height = defaultGridSize;
    /// Its extent on z, 1 for a flat panel.
    lengthType depth = 1;
    /// Every other row runs in reverse, for a snaked strip.
    bool serpentine = false;

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
        // Multiply in uint32_t to detect overflow before casting.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        return clampedCount(n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink, lightCount()};
        forEachCell(width, height, depth, serpentine, [&](lengthType x, lengthType y, lengthType z) { return e.pixel(x, y, z); });
    }

private:
    /// Visit every cell in wiring order, x fastest, a serpentine strip reversing every other row, until `visit` returns false.
    template <typename Visit>
    static void forEachCell(lengthType width, lengthType height, lengthType depth, bool serpentine, Visit&& visit) {
        for (lengthType z = 0; z < depth; z++) {
            for (lengthType y = 0; y < height; y++) {
                // Serpentine changes only the index-to-position order, which is what makes the map non-identity.
                const bool reverse = serpentine && (y & 1);
                for (lengthType i = 0; i < width; i++) {
                    const lengthType x = reverse ? static_cast<lengthType>(width - 1 - i) : i;
                    if (!visit(x, y, z)) return;
                }
            }
        }
    }
};

} // namespace mm
