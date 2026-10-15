#pragma once

#include "light/layouts/LayoutBase.h"
#include "light/layouts/PanelLayout.h"   // the matrix walk, run over the panel grid and inside each panel

namespace mm {

/// An M × N grid of matrix panels, tiled into one large display for a video wall.
/// @card PanelsLayout.gif
///
/// The wiring is two walks, each set on its own: an outer one over the panel grid and an inner one over each panel's lights, as @ref PanelLayout wires one.
///
/// @attribution Boustrophedon
///
/// @moreinfo
///
/// Both walks are @ref PanelLayout objects: one over the grid of panels, and at each cell one offset by its tile. The panels tile edge to edge into one matrix.
class PanelsLayout : public LayoutBase {
public:
    /// How many panels across.
    lengthType horizontalPanels = 2;
    /// How many panels down.
    lengthType verticalPanels   = 2;
    /// Which axis the panel grid walks first.
    uint8_t    wiringOrderP     = 0;
    /// Whether the panel grid's x counts up.
    bool       incXP            = true;
    /// Whether its y counts up.
    bool       incYP            = true;
    /// Serpentine across the panel grid: alternate panel rows run in reverse.
    bool       snakeP           = false;

    /// Each panel's width in lights.
    lengthType panelWidth  = 16;
    /// Each panel's height in lights.
    lengthType panelHeight = 16;
    /// Which axis a panel walks first.
    uint8_t    wiringOrder = 0;
    /// Whether a panel's x counts up.
    bool       incX        = true;
    /// Whether its y counts up.
    bool       incY        = true;
    /// Serpentine inside each panel: alternate lines run in reverse.
    bool       snake       = true;

    /// The controls a user sets on the card.
    void defineControls() override {
        // The panel grid, the outer walk.
        controls_.addControl("horizontalPanels", horizontalPanels, 1, 32);
        controls_.addControl("verticalPanels",   verticalPanels,   1, 32);
        controls_.addSelect("wiringOrderP", wiringOrderP, PanelLayout::kWiringOptions, PanelLayout::kWiringCount);
        controls_.addControl("X++P",  incXP);
        controls_.addControl("Y++P",  incYP);
        controls_.addControl("snakeP", snakeP);

        // Each panel, the inner walk, its size clamped to what the coordinate type holds.
        controls_.addControl("panelWidth",  panelWidth,  1, 512);
        controls_.addControl("panelHeight", panelHeight, 1, 512);
        controls_.addSelect("wiringOrder", wiringOrder, PanelLayout::kWiringOptions, PanelLayout::kWiringCount);
        controls_.addControl("X++",   incX);
        controls_.addControl("Y++",   incY);
        controls_.addControl("snake", snake);
    }

    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // Widened before the multiply, so an oversized wall clamps rather than wrapping.
        uint32_t panelCount  = static_cast<uint32_t>(horizontalPanels) * static_cast<uint32_t>(verticalPanels);
        uint32_t perPanel    = static_cast<uint32_t>(panelWidth) * static_cast<uint32_t>(panelHeight);
        uint64_t n = static_cast<uint64_t>(panelCount) * perPanel;
        return clampedCount(n);
    }

    /// Emit every light's coordinate, in wiring order: a PanelLayout object walks the grid of panels, and at each cell a second one, the panel, is placed offset to its tile.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink, lightCount()};
        PanelLayout grid;
        grid.panelWidth = horizontalPanels;
        grid.panelHeight = verticalPanels;
        grid.wiringOrder = wiringOrderP;
        grid.incX = incXP;
        grid.incY = incYP;
        grid.snake = snakeP;
        PanelLayout panel;
        panel.panelWidth = panelWidth;
        panel.panelHeight = panelHeight;
        panel.wiringOrder = wiringOrder;
        panel.incX = incX;
        panel.incY = incY;
        panel.snake = snake;
        // Each cell of the grid walk places one whole panel at its tile.
        struct Tiler { LightEmitter* e; const PanelLayout* panel; lengthType w, h; };
        Tiler tiler{&e, &panel, panelWidth, panelHeight};
        grid.placeLights(CoordSink{[](void* c, nrOfLightsType, lengthType px, lengthType py, lengthType) {
            auto* t = static_cast<Tiler*>(c);
            t->e->place(*t->panel, static_cast<lengthType>(px * t->w), static_cast<lengthType>(py * t->h));
        }, nullptr, &tiler});
    }

};

} // namespace mm
