/// @module PraxisEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/PraxisEffect.h"
#include "light/layouts/GridLayout.h"

// Praxis overwrites EVERY pixel each frame (a full-grid palette field, no black background), with a non-black palette active, no light is left at (0,0,0).
TEST_CASE("PraxisEffect fills every pixel from the palette") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PraxisEffect praxis;
    layer.addChild(&praxis);

    layer.applyState();
    // Rainbow palette (0) is generated at full saturation/value, so every wheel index maps to a lit color, makes "every pixel lit" order-independent of prior tests.
    mm::Palettes::setActive(0);
    layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 64);

    // No pixel is black: Praxis writes every (x,y) rather than painting sparsely.
    auto* data = buf.data();
    bool everyPixelLit = true;
    for (size_t p = 0; p < buf.count(); p++) {
        uint8_t r = data[p * 3], g = data[p * 3 + 1], b = data[p * 3 + 2];
        if (r == 0 && g == 0 && b == 0) { everyPixelLit = false; break; }
    }
    CHECK(everyPixelLit);
}

// The hue is a function of (x, y): pixels far apart in the grid carry different colors, so the field is spatial, not a uniform fill.
TEST_CASE("PraxisEffect varies color across the grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PraxisEffect praxis;
    layer.addChild(&praxis);

    layer.applyState();
    mm::Palettes::setActive(0);
    layer.tick();

    auto* data = layer.buffer().data();
    // Two lights can share a color at a moment, since the mutators follow the clock, so the grid as a whole holds more than one.
    bool varies = false;
    for (size_t i = 1; i < 16u * 16u && !varies; i++)
        varies = data[i * 3] != data[0] || data[i * 3 + 1] != data[1] || data[i * 3 + 2] != data[2];
    CHECK(varies);
}

// Hard rule: the effect runs at a degenerate grid without crashing. width/height <= 0 is guarded, and a 1×1 grid exercises the render loop at its smallest.
TEST_CASE("PraxisEffect survives degenerate grid sizes") {
    for (int dim : {0, 1}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = dim;
        grid.height = dim;
        grid.depth = dim;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::PraxisEffect praxis;
        layer.addChild(&praxis);

        layer.applyState();
        layer.tick();  // must not crash at 0×0×0 or 1×1×1
        CHECK(true);
    }
}
