#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout tiling multiple panels into one grid.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A tiled grid of full matrix panels: an M×N arrangement of panels, each panel a serpentine matrix in its own right.
/// Two levels of wiring, an OUTER walk over the panel grid (which panel, in what order) and an INNER walk over each panel's lights (PanelLayout's matrix).
/// Both the panel-to-panel order and the light-to-light order inside a panel are independently configurable.
/// Each panel at grid cell (px, py) is offset by px·panelWidth in X and py·panelHeight in Y, so the panels tile edge-to-edge into one large matrix.
/// Every light is at an integer (x, y, 0); this is a 2D layout.
///
/// ## Prior art
///
/// Prior art: MoonLight PanelsLayout (Node "Panels", tags 🚥), which nests a `panel` Wiring inside a `panels` Wiring and walks both with `iterate()`.
/// We reproduce the geometry, the two-level nesting, the axis-order table shared by both levels, the snake-on-odd-outer serpentine.
/// The per-panel offset (coordsP[axis]·panel.size[axis]), and the control set, but drop MoonLight's pin/wiring plumbing (the Wiring pin count, panelsPerPin, nextPin()): a MoonLight layout emits coordinates only.
/// The driver owns pins. tags 💫 marks the MoonLight lineage.
/// The single-panel case is the sibling PanelLayout (Node "Panel"); this module tiles that panel across a grid.
///
/// The MoonLight `Wiring`/`iterate` implementation is not in the ported source (only the Panels usage site is).
/// The iteration is reconstructed from that usage plus the control labels/defaults, identically to PanelLayout's already-shipped reconstruction.
/// The reconstructed logic is marked // RECONSTRUCTED and, on the defaults, cross-checks against a plain serpentine tiling.
class PanelsLayout : public LayoutBase {
public:
    /// Outer level, the panel grid (MoonLight `panels`, default 2×2 panels).
    lengthType horizontalPanels = 2;   // panels along X (MoonLight panels.size[0])
    lengthType verticalPanels   = 2;   // panels along Y (MoonLight panels.size[1])
    /// Which axis the panel grid walks first.
    uint8_t    wiringOrderP     = 0;
    /// Whether the panel grid's x counts up.
    bool       incXP            = true;
    /// Whether its y counts up.
    bool       incYP            = true;
    bool       snakeP           = false; // "snakeP": snake the panel grid (default off)

    /// Inner level, each panel (MoonLight `panel`, default 16×16, snake on Y).
    lengthType panelWidth  = 16;   // panel extent along X (MoonLight panel.size[0])
    lengthType panelHeight = 16;   // panel extent along Y (MoonLight panel.size[1])
    /// Which axis a panel walks first.
    uint8_t    wiringOrder = 0;
    /// Whether a panel's x counts up.
    bool       incX        = true;
    /// Whether its y counts up.
    bool       incY        = true;
    bool       snake       = true; // "snake": snake each panel's inner loop (default on)

    /// The controls a user sets on the card.
    void defineControls() override {
        // Panel grid (outer). MoonLight ranges 1..32; clamped to lengthType (int16_t).
        controls_.addControl("horizontalPanels", horizontalPanels, 1, 32);
        controls_.addControl("verticalPanels",   verticalPanels,   1, 32);
        controls_.addSelect("wiringOrderP", wiringOrderP, kWiringOptions, kWiringCount);
        controls_.addControl("X++P",  incXP);
        controls_.addControl("Y++P",  incYP);
        controls_.addControl("snakeP", snakeP);

        // Per-panel (inner). MoonLight ranges 1..65536; clamped to int16_t max (512-safe).
        controls_.addControl("panelWidth",  panelWidth,  1, 512);
        controls_.addControl("panelHeight", panelHeight, 1, 512);
        controls_.addSelect("wiringOrder", wiringOrder, kWiringOptions, kWiringCount);
        controls_.addControl("X++",   incX);
        controls_.addControl("Y++",   incY);
        controls_.addControl("snake", snake);
    }

    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// Widened before the multiply, so an oversized wall clamps rather than wrapping.
        uint32_t panelCount  = static_cast<uint32_t>(horizontalPanels) * static_cast<uint32_t>(verticalPanels);
        uint32_t perPanel    = static_cast<uint32_t>(panelWidth) * static_cast<uint32_t>(panelHeight);
        uint64_t n = static_cast<uint64_t>(panelCount) * perPanel;
        constexpr uint64_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // One table drives both walks: axes[0] is the outer axis and axes[1] the inner.
        const uint8_t axisOrders[2][2] = {
            {1, 0},  // "XY": axis 1 (Y) outer loop, axis 0 (X) inner loop
            {0, 1},  // "YX": axis 0 (X) outer loop, axis 1 (Y) inner loop
        };

        const uint32_t limit = lightCount();
        Emit e{sink.cb, sink.ctx, panelWidth, panelHeight, limit, 0};

        // The outer walk is the panel grid, emitting one whole panel per cell.
        const uint8_t woP = wiringOrderP < kWiringCount ? wiringOrderP : 0;
        const uint8_t outerAxisP = axisOrders[woP][0];
        const uint8_t innerAxisP = axisOrders[woP][1];
        const lengthType extentP[2] = {horizontalPanels, verticalPanels};  // extentP[0]=X, [1]=Y
        const bool incP[2] = {incXP, incYP};

        // Per-panel axis order, resolved once (constant across all panels).
        const uint8_t wo = wiringOrder < kWiringCount ? wiringOrder : 0;
        const uint8_t outerAxis = axisOrders[wo][0];
        const uint8_t innerAxis = axisOrders[wo][1];
        const lengthType extent[2] = {panelWidth, panelHeight};  // extent[0]=X, [1]=Y
        const bool inc[2] = {incX, incY};

        // The same reconstructed reading as one panel, applied at both levels here.
        const lengthType outerNP = extentP[outerAxisP];
        const lengthType innerNP = extentP[innerAxisP];
        const bool outerAscP = incP[outerAxisP];
        const bool innerAscP = incP[innerAxisP];

        for (lengthType oip = 0; oip < outerNP && e.idx < limit; oip++) {
            const lengthType outerValP = outerAscP
                ? oip
                : static_cast<lengthType>(outerNP - 1 - oip);

            const bool innerReverseP = snakeP && (outerValP & 1);
            const bool innerForwardP = innerAscP != innerReverseP;  // XOR: reverse flips inc

            for (lengthType iip = 0; iip < innerNP && e.idx < limit; iip++) {
                const lengthType innerValP = innerForwardP
                    ? iip
                    : static_cast<lengthType>(innerNP - 1 - iip);

                /// Scatter the two loop values back to a panel cell via the axis order.
                lengthType coordsP[2] = {0, 0};  // coordsP[0]=px, coordsP[1]=py
                coordsP[outerAxisP] = outerValP;
                coordsP[innerAxisP] = innerValP;

                // ---- Inner walk: this panel's matrix, offset into the tiled grid. ----
                emitPanel(e, coordsP[0], coordsP[1],
                          outerAxis, innerAxis, extent, inc, limit);
            }
        }
    }

private:
    // Threaded through both walks, so the physical index stays continuous across panels.
    struct Emit {
        CoordCallback cb;
        void* ctx;
        lengthType panelWidth;
        lengthType panelHeight;
        uint32_t limit;
        uint32_t idx;
    };

    // One panel's lights, the tile offset added to each emitted coordinate.
    void emitPanel(Emit& e, lengthType px, lengthType py,
                   uint8_t outerAxis, uint8_t innerAxis,
                   const lengthType extent[2], const bool inc[2],
                   uint32_t limit) const {
        const lengthType offsetX = static_cast<lengthType>(px * e.panelWidth);
        const lengthType offsetY = static_cast<lengthType>(py * e.panelHeight);

        const lengthType outerN = extent[outerAxis];
        const lengthType innerN = extent[innerAxis];
        const bool outerAsc = inc[outerAxis];
        const bool innerAsc = inc[innerAxis];

        for (lengthType oi = 0; oi < outerN && e.idx < limit; oi++) {
            const lengthType outerVal = outerAsc
                ? oi
                : static_cast<lengthType>(outerN - 1 - oi);

            const bool innerReverse = snake && (outerVal & 1);
            const bool innerForward = innerAsc != innerReverse;  // XOR: reverse flips inc

            for (lengthType ii = 0; ii < innerN && e.idx < limit; ii++) {
                const lengthType innerVal = innerForward
                    ? ii
                    : static_cast<lengthType>(innerN - 1 - ii);

                /// Scatter the loop values onto their axes, then add the tile offset.
                lengthType coord[2] = {0, 0};  // coord[0]=x, coord[1]=y (panel-local)
                coord[outerAxis] = outerVal;
                coord[innerAxis] = innerVal;

                e.cb(e.ctx, static_cast<nrOfLightsType>(e.idx++),
                     static_cast<lengthType>(offsetX + coord[0]),
                     static_cast<lengthType>(offsetY + coord[1]),
                     0);
            }
        }
    }

    static constexpr const char* kWiringOptions[] = {"XY", "YX"};
    static constexpr uint8_t kWiringCount = 2;
};

} // namespace mm
