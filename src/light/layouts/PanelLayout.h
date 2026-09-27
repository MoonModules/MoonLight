#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of a single 2D LED panel.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A serpentine 2D LED matrix (panel), emitting each light's (x, y, 0) coordinate in physical wiring order.
/// The general matrix layout: GridLayout is the simple row-major/serpentine case; this adds a configurable axis order (walk X-major or Y-major), a per-axis increment direction, and a snake toggle.
///
/// Prior art: MoonLight PanelLayout (Node "Panel", tags 🚥), which drives the panel off a `Wiring{size, count, inc[], snake[]}` helper and an `iterate()` walk.
/// We reproduce the geometry (axis-order table, snake-on-odd-outer serpentine) and the control set, but drop MoonLight's pin/wiring plumbing (the Wiring struct's pin count, nextPin()), a MoonLight layout emits coordinates only.
/// The driver owns pins. tags 💫 marks the MoonLight lineage.
///
/// The MoonLight `Wiring`/`iterate` implementation is not in the ported source (only the Panel usage site is), so the iteration is reconstructed from that usage plus the control labels/defaults.
/// The reconstructed logic is marked // RECONSTRUCTED and cross-checks against GridLayout's serpentine on the defaults.
///
/// ## The wiring walk is reconstructed
///
/// MoonLight's iterate helper is not in the ported source, so the semantics here come from how the panel used it, plus the control labels and defaults.
///
/// The outer loop walks its axis across that axis's extent, and the direction is the per-axis increment flag, which matches the axis-named labels the cube uses too.
/// The outer loop never snakes, its parent index being a constant and so always even.
/// The inner loop reverses when the snake toggle is on and the emitted outer index is odd.
///
/// MoonLight's exact snake-array indexing, whether keyed by physical axis or by loop slot, is not recoverable from what was ported.
/// Both that reading and the default comment about snaking on one axis yield the same geometry, the standard boustrophedon panel, so this is the faithful behavior.
///
/// On the defaults this emits one row ascending, the next descending, identical to a grid's serpentine.
class PanelLayout : public LayoutBase {
public:
    /// Geometry (verbatim MoonLight defaults): a 16×16 panel.
    lengthType panelWidth  = 16;   // extent along X (MoonLight panel.size[0])
    lengthType panelHeight = 16;   // extent along Y (MoonLight panel.size[1])

    /// Which axis is the outer loop: row-major by default, or column-major.
    uint8_t wiringOrder = 0;

    /// Per-axis direction: set walks that coordinate up, clear walks it down.
    bool incX = true;
    /// Whether y counts up.
    bool incY = true;

    /// Snake the inner loop on odd outer steps, which is one exposed toggle in 2D.
    bool snake = true;

    /// The controls a user sets on the card.
    void defineControls() override {
        // Geometry only, its ranges clamped to what the coordinate type holds.
        controls_.addControl("panelWidth",  panelWidth,  1, 512);
        controls_.addControl("panelHeight", panelHeight, 1, 512);
        controls_.addSelect("wiringOrder", wiringOrder, kWiringOptions, kWiringCount);
        controls_.addControl("X++", incX);
        controls_.addControl("Y++", incY);
        controls_.addControl("snake", snake);
    }

    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// Multiply in uint32_t to detect overflow before casting, per GridLayout.
        uint32_t n = static_cast<uint32_t>(panelWidth) * static_cast<uint32_t>(panelHeight);
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // axes[0] is the outer axis and axes[1] the inner, per the selected order.
        const uint8_t axisOrders[2][2] = {
            {1, 0},  // "XY": Y(1) outer loop, X(0) inner loop
            {0, 1},  // "YX": X(0) outer loop, Y(1) inner loop
        };
        const uint8_t wo = wiringOrder < kWiringCount ? wiringOrder : 0;
        const uint8_t outerAxis = axisOrders[wo][0];
        const uint8_t innerAxis = axisOrders[wo][1];

        const lengthType extent[2] = {panelWidth, panelHeight};  // extent[0]=X, extent[1]=Y
        const bool inc[2] = {incX, incY};                        // inc[0]=X++, inc[1]=Y++

        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // The outer loop never snakes and the inner one reverses on an odd outer index.
        const lengthType outerN = extent[outerAxis];
        const lengthType innerN = extent[innerAxis];
        const bool outerAsc = inc[outerAxis];
        const bool innerAsc = inc[innerAxis];

        for (lengthType oi = 0; oi < outerN && idx < limit; oi++) {
            // Outer coordinate value along its axis (direction only; no snake at outermost).
            const lengthType outerVal = outerAsc
                ? oi
                : static_cast<lengthType>(outerN - 1 - oi);

            // Inner loop reverses when snaking and the outer coordinate is odd.
            const bool innerReverse = snake && (outerVal & 1);
            const bool innerForward = innerAsc != innerReverse;  // XOR: reverse flips inc

            for (lengthType ii = 0; ii < innerN && idx < limit; ii++) {
                const lengthType innerVal = innerForward
                    ? ii
                    : static_cast<lengthType>(innerN - 1 - ii);

                /// Scatter the two loop values onto their axes via the chosen order.
                lengthType coord[2] = {0, 0};  // coord[0]=x, coord[1]=y
                coord[outerAxis] = outerVal;
                coord[innerAxis] = innerVal;

                sink.pixel(static_cast<nrOfLightsType>(idx++), coord[0], coord[1], 0);
            }
        }
    }

private:
    static constexpr const char* kWiringOptions[] = {"XY", "YX"};
    static constexpr uint8_t kWiringCount = 2;
};

} // namespace mm
