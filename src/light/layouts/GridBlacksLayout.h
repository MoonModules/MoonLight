#pragma once

#include "light/layouts/LayoutBase.h"
#include "light/layouts/GridLayout.h"   // the walk the dark columns sit in

namespace mm {

/// A grid with dark columns held black in every row, for a sealed panel that must stay dark down a strip, or a slat wall.
/// @card GridBlacksLayout.gif
///
/// A dark column is still a wire position the driver clocks, so data flows through the unlit lights to the lit columns beyond. The lit columns keep their true positions, so an effect maps straight across the gap. Without dark columns it is a @ref GridLayout.
///
/// @attribution Boustrophedon
///
/// @moreinfo
///
/// A @ref GridLayout object places the lights, and the gap is decided on the true column, so a dark column stays dark whichever way a serpentine strip enters the row.
///
/// A non-empty run tells the Layer to build the folded mapping, which drops the gap slots, rather than the dense identity map, which would light them.
class GridBlacksLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// The grid's extent on x.
    lengthType width = 16;
    /// Its extent on y.
    lengthType height = 16;
    /// Its extent on z.
    lengthType depth = 1;
    /// Every other row runs in reverse, matching a snaked strip.
    bool serpentine = false;
    /// The first dark column.
    lengthType blackStart = 0;
    /// How many dark columns; none renders exactly like a Grid.
    lengthType blackCount = 0;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("width",  width,  1, 512);
        controls_.addControl("height", height, 1, 512);
        controls_.addControl("depth",  depth,  1, 512);
        controls_.addControl("serpentine", serpentine);
        controls_.addControl("blackCount", blackCount, 0, 512);
        controls_.addControl("blackStart", blackStart, 0, 512);
        controls_.setHidden(controls_.count() - 1, blackCount == 0);   // blackStart matters only with a run
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // A gap is a physical wire slot the driver clocks, so it counts toward the box.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        return clampedCount(n);
    }

    /// Whether dark columns exist, which is what makes the Layer fold the mapping.
    bool hasBlackPixels() const override { return blackCount != 0; }

    /// Emit every light's coordinate, in wiring order: a GridLayout object places the lights, and a dark column's are held black.
    void placeLights(const CoordSink& sink) const override {
        GridLayout grid;
        grid.width = width;
        grid.height = height;
        grid.depth = depth;
        grid.serpentine = serpentine;
        // Decided here at the emit site, on the true column, so it is never re-derived from an index.
        struct Holes { const CoordSink* sink; lengthType start, end; bool any; };
        Holes holes{&sink, blackStart, static_cast<lengthType>(blackStart + blackCount), blackCount != 0};
        grid.placeLights(CoordSink{[](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType z) {
            auto* h = static_cast<Holes*>(c);
            if (h->any && x >= h->start && x < h->end) h->sink->blackPixel(i, x, y, z);
            else h->sink->pixel(i, x, y, z);
        }, nullptr, &holes});
    }
};

} // namespace mm
