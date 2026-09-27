#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of a walk-in cube built from five LED-curtain faces.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A human-sized LED cube built from five hanging "curtain" faces of a width×height×depth box: front (z=0), back (z=depth+1), above (y=0), left (x=0) and right (x=width+1).
/// Each face is a flat matrix of lights.
/// The offsets inset the four side and top curtains by one cell, so they hang just outside the front and back planes.
/// The front sits at zero and the back one past the depth, with the wrap-around curtains bridging them.
/// The bottom face is left open (a person stands inside), so only five of the six faces are emitted.
///
/// Prior art: MoonLight's HumanSizedCubeLayout (Node "Human Sized Cube", tags 🚥).
/// The geometry is reproduced exactly: the five face loops, their nested axis order, and every offset match the source's own emit sequence.
/// The emitted coordinates and their wiring order are therefore identical.
/// MoonLight's per-curtain pin calls are dropped, since a MoonLight layout emits coordinates only and the driver owns pins.
/// Its sixth face is dropped too, being disabled in the source itself.
class HumanSizedCubeLayout : public LayoutBase {
public:
    /// Verbatim MoonLight defaults and ranges: a 10×10×10 cube, each edge 1..20.
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

    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // The five face areas summed, widened first so an oversized cube clamps.
        const uint32_t w = width, h = height, d = depth;
        uint32_t n = 2u * w * h + w * d + 2u * d * h;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // One loop per face, in the source's own order, so an index lands on the same cell.

        // front: z = 0  — for x { for y }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), static_cast<lengthType>(y + 1), 0);

        // back: z = depth+1  — for x { for y }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), static_cast<lengthType>(y + 1),
                   static_cast<lengthType>(depth + 1));

        // above: y = 0  — for x { for z }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType z = 0; z < depth && idx < limit; z++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), 0, static_cast<lengthType>(z + 1));

        // The bottom face is deliberately absent: the cube is open there.

        // left: x = 0  — for z { for y }
        for (lengthType z = 0; z < depth && idx < limit; z++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   0, static_cast<lengthType>(y + 1), static_cast<lengthType>(z + 1));

        // right: x = width+1  — for z { for y }
        for (lengthType z = 0; z < depth && idx < limit; z++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                sink.pixel(static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(width + 1), static_cast<lengthType>(y + 1),
                   static_cast<lengthType>(z + 1));
    }
};

} // namespace mm
