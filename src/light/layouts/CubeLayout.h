#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of a 3D cube volume (width×height×depth).
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A solid cube of lights: every integer lattice point in a width×height×depth box gets one LED.
/// Unlike SphereLayout (a shell), this is a filled volume, so lightCount() is simply the product of the three edges.
///
/// The interesting geometry is the WIRING ORDER, the sequence in which the LEDs are addressed.
/// A physical cube is built from a strip that snakes through the volume.
/// Which axis the strip runs along fastest (plus whether it reverses direction on alternate passes, boustrophedon / "snake") determines the index→position mapping.
/// This layout reproduces the predecessor MoonLight's Cube: a 6-way axis-order select, a per-axis increasing-direction flag, and a per-axis snake toggle.
/// The emitted COORDINATE is always the true (x,y,z); only the ORDER of emission (the driver index) changes with these controls, the same principle as GridLayout's serpentine, generalised to three axes.
///
/// Prior art: CubeLayout in the predecessor MoonLight, github.com/ewowi/MoonLight, which keeps its own name now that it is ours too.
/// It drives a `Wiring` helper with per-plane pins (nextPin() per plane).
/// Ours emit coordinates only, the driver owns pins, so that plumbing is dropped and only the geometry is kept.
///
/// ## The wiring walk is reconstructed
///
/// MoonLight expresses this through an iterate helper that is not in the sources we have, so the walk here is the standard boustrophedon its usage and defaults imply.
/// The per-axis increment picks the base scan direction, ascending by default, and the per-axis snake reverses that direction on odd passes of the enclosing loop.
/// That is the snaking a grid applies on one axis, generalised to three.
///
/// The snake toggle keys on the enclosing loop's counter, the physical pass number, which is the physically correct boustrophedon.
/// For the all-ascending default that counter equals the emitted coordinate, so this reproduces MoonLight's default exactly.
class CubeLayout : public LayoutBase {
public:
    /// Cube edges. Defaults 10×10×10, range 1..128, MoonLight's exact defaults.
    lengthType width = 10;
    /// The cube's extent on y.
    lengthType height = 10;
    /// Its extent on z.
    lengthType depth = 10;

    /// Which axis the strip runs along fastest, an index into the axis-order table.
    uint8_t wiringOrder = 3;

    /// Per-axis scan direction: set counts up from zero, clear counts down.
    bool incX = true;
    /// Whether y counts up.
    bool incY = true;
    /// Whether z counts up.
    bool incZ = true;

    /// Per-axis snake: reverse that axis on alternate passes of the enclosing loop.
    bool snakeX = false;
    /// Whether y does.
    bool snakeY = true;
    /// Whether z does.
    bool snakeZ = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("width",  width,  1, 128);
        controls_.addControl("height", height, 1, 128);
        controls_.addControl("depth",  depth,  1, 128);
        controls_.addSelect("wiringOrder", wiringOrder, kWiringOrderOptions, kWiringOrderCount);
        controls_.addControl("X++", incX);
        controls_.addControl("Y++", incY);
        controls_.addControl("Z++", incZ);
        controls_.addControl("snakeX", snakeX);
        controls_.addControl("snakeY", snakeY);
        controls_.addControl("snakeZ", snakeZ);
    }

    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// Widened before the multiply, so an oversized cube clamps rather than wrapping.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // axes[0] is the outermost loop's axis and axes[2] the innermost, so guard the select.
        const uint8_t* axes = kAxisOrders[wiringOrder < kWiringOrderCount ? wiringOrder : 0];

        const lengthType size[3]  = { width, height, depth };
        const bool        inc[3]  = { incX, incY, incZ };
        const bool        snake[3]= { snakeX, snakeY, snakeZ };

        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // Three nested serpentine passes, each pass's direction resolved by axisValue().
        const uint8_t a0 = axes[0], a1 = axes[1], a2 = axes[2];
        const lengthType n0 = size[a0], n1 = size[a1], n2 = size[a2];

        for (lengthType i = 0; i < n0 && idx < limit; i++) {
            const lengthType v0 = axisValue(i, n0, inc[a0], snake[a0], 0);
            for (lengthType j = 0; j < n1 && idx < limit; j++) {
                const lengthType v1 = axisValue(j, n1, inc[a1], snake[a1], i);
                for (lengthType k = 0; k < n2 && idx < limit; k++) {
                    const lengthType v2 = axisValue(k, n2, inc[a2], snake[a2], j);

                    /// Scatter the three loop values onto their real axes, then emit.
                    lengthType coords[3] = {0, 0, 0};
                    coords[a0] = v0;
                    coords[a1] = v1;
                    coords[a2] = v2;
                    sink.pixel(static_cast<nrOfLightsType>(idx++),
                       coords[0], coords[1], coords[2]);
                }
            }
        }
    }

private:
    // Each row is an outer, middle and inner axis index; the wiring select maps onto these.
    static constexpr uint8_t kAxisOrders[6][3] = {
        {2, 1, 0},  // XYZ label — Z outer, Y middle, X inner (X fastest)
        {2, 0, 1},  // YXZ       — Z, X, Y
        {1, 2, 0},  // XZY       — Y, Z, X
        {1, 0, 2},  // YZX       — Y, X, Z   (default, wiringOrder = 3)
        {0, 2, 1},  // ZXY       — X, Z, Y
        {0, 1, 2},  // ZYX       — X, Y, Z
    };
    static constexpr const char* kWiringOrderOptions[6] = {
        "XYZ", "YXZ", "XZY", "YZX", "ZXY", "ZYX"
    };
    static constexpr uint8_t kWiringOrderCount = 6;

    // One pass's value: the base direction, flipped when the enclosing counter is odd.
    static lengthType axisValue(lengthType step, lengthType count,
                                bool inc, bool snake, lengthType prev) {
        bool ascending = inc;
        if (snake && (prev & 1)) ascending = !ascending;
        return ascending ? step : static_cast<lengthType>(count - 1 - step);
    }
};

} // namespace mm
