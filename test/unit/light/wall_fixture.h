/// The wall a driver test drives, as production wires it: a Layout gives the Layer its physical size and the driver reads that, having no geometry of its own.
#pragma once
#include "doctest.h"
#include "correction_presets.h"
#include "light/drivers/Correction.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"

namespace mm::test {

/// A wall of `width` x `height`.
struct Wall {
    mm::Layouts layouts;    ///< the layouts the layer reads its size from
    mm::GridLayout grid;    ///< the one layout, a grid of the wall's size
    mm::Layer layer;        ///< the layer the driver is given
    /// Build the grid and apply it, so the layer carries the wall's size.
    Wall(uint16_t width, uint16_t height) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

/// Wire `driver` onto `wall` over a `count`-light source with a plain RGB correction, ready to tick.
template <class Driver>
void wire(Driver& driver, mm::Buffer& source, Wall& wall, mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    mm::Correction correction;
    rebuildFromPreset(correction, 255, PresetOrder::RGB);
    driver.setLayer(&wall.layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
}

}  // namespace mm::test
