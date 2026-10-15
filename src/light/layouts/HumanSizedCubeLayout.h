#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// A walk-in cube of five LED curtains: front, back, top, left and right, open at the bottom.
/// @card HumanSizedCubeLayout.gif
///
/// Each face is a curtain the size of the box's side, for a room-scale cube a person stands inside.
///
/// @attribution troyhacks
///
/// @technical
///
/// The front sits at z 0 and the back at depth + 1; the top, left and right curtains hang one cell outside them, bridging the two.
class HumanSizedCubeLayout : public LayoutBase {
public:
    /// The cube's extent on x.
    uint8_t width  = 10;
    /// The cube's extent on y.
    uint8_t height = 10;
    /// Its extent on z.
    uint8_t depth  = 10;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("width",  width,  1, 20);
        controls_.addControl("height", height, 1, 20);
        controls_.addControl("depth",  depth,  1, 20);
    }

    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // The five face areas summed, widened first so an oversized cube clamps.
        const uint32_t w = width, h = height, d = depth;
        uint32_t n = 2u * w * h + w * d + 2u * d * h;
        return clampedCount(n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // One loop per face, in the source's own order, so an index lands on the same cell.

        // front: z = 0: for x { for y }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), static_cast<lengthType>(y + 1), 0);

        // back: z = depth+1: for x { for y }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), static_cast<lengthType>(y + 1),
                   static_cast<lengthType>(depth + 1));

        // above: y = 0: for x { for z }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType z = 0; z < depth && idx < limit; z++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), 0, static_cast<lengthType>(z + 1));

        // The bottom face is deliberately absent: the cube is open there.

        // left: x = 0: for z { for y }
        for (lengthType z = 0; z < depth && idx < limit; z++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   0, static_cast<lengthType>(y + 1), static_cast<lengthType>(z + 1));

        // right: x = width+1: for z { for y }
        for (lengthType z = 0; z < depth && idx < limit; z++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(width + 1), static_cast<lengthType>(y + 1),
                   static_cast<lengthType>(z + 1));
    }
};

} // namespace mm
