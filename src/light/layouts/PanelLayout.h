#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// A 2D matrix panel with full wiring control: the axis order, the direction per axis, and serpentine.
/// @card PanelLayout.gif
///
/// The general matrix layout, of which @ref GridLayout is the simple case.
///
/// @attribution Boustrophedon
///
/// @technical
///
/// The outer walk never snakes; the inner one reverses on an odd outer value when snake is on, as a grid's serpentine does.
class PanelLayout : public LayoutBase {
public:
    /// The panel's width in lights.
    lengthType panelWidth  = 16;
    /// The panel's height in lights.
    lengthType panelHeight = 16;

    /// Rows first or columns first.
    uint8_t wiringOrder = 0;

    /// Whether x counts up rather than down.
    bool incX = true;
    /// Whether y counts up.
    bool incY = true;

    /// Serpentine wiring: alternate lines run in reverse.
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

    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // Multiply in uint32_t to detect overflow before casting, per GridLayout.
        uint32_t n = static_cast<uint32_t>(panelWidth) * static_cast<uint32_t>(panelHeight);
        return clampedCount(n);
    }

    /// The wiring orders a panel offers, rows first or columns first; Panels offers the same at both of its levels.
    static constexpr const char* kWiringOptions[] = {"XY", "YX"};
    /// How many wiring orders there are.
    static constexpr uint8_t kWiringCount = 2;

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink, lightCount()};
        forEachCell(panelWidth, panelHeight, wiringOrder, incX, incY, snake, [&](lengthType x, lengthType y) { return e.pixel(x, y, 0); });
    }

private:
    /// Visit every cell in wiring order until `visit` returns false. `wiringOrder` picks rows or columns first, the increments the direction per axis, and the outer walk never snakes while the inner one reverses on an odd outer value.
    template <typename Visit>
    static void forEachCell(lengthType width, lengthType height, uint8_t wiringOrder, bool incX, bool incY, bool snake, Visit&& visit) {
        // axes[0] is the outer axis and axes[1] the inner: "XY" walks y outer and x inner, "YX" the other way round.
        constexpr uint8_t kAxisOrders[2][2] = {{1, 0}, {0, 1}};
        const uint8_t wo = wiringOrder < kWiringCount ? wiringOrder : 0;
        const uint8_t outerAxis = kAxisOrders[wo][0];
        const uint8_t innerAxis = kAxisOrders[wo][1];
        const lengthType extent[2] = {width, height};
        const bool inc[2] = {incX, incY};
        const lengthType outerN = extent[outerAxis];
        const lengthType innerN = extent[innerAxis];
        for (lengthType oi = 0; oi < outerN; oi++) {
            const lengthType outerVal = inc[outerAxis] ? oi : static_cast<lengthType>(outerN - 1 - oi);
            // The inner walk reverses when snaking on an odd outer value: reverse flips the increment.
            const bool innerForward = inc[innerAxis] != (snake && (outerVal & 1));
            for (lengthType ii = 0; ii < innerN; ii++) {
                lengthType coord[2] = {0, 0};
                coord[outerAxis] = outerVal;
                coord[innerAxis] = innerForward ? ii : static_cast<lengthType>(innerN - 1 - ii);
                if (!visit(coord[0], coord[1])) return;
            }
        }
    }

};

} // namespace mm
