/// @module Drivers

/// Pins Drivers::firstOutputRgb, the domain-neutral seam the WLED-compatibility shim uses to tint the app's device card with the live first-LED color. It reads pixel 0 of whichever buffer Drivers is driving (the single-layer fast path here: the layer's own buffer).

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"

#include <cstdint>

namespace {

// A 4×4 grid, one layer, pinned directly to a Drivers (the test-rig path). Returns the layer so the caller can write pixel 0.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::Drivers drivers;

    Rig() {
        grid.width = 4; grid.height = 4; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layouts.applyState();
        layer.applyState();
        drivers.setLayer(&layer);   // pin: Drivers reads this layer's buffer
    }
};

}  // namespace

TEST_CASE("Drivers::firstOutputRgb reads pixel 0 of the driven buffer") {
    Rig r;
    uint8_t* buf = r.layer.buffer().data();
    REQUIRE(buf != nullptr);
    buf[0] = 10; buf[1] = 200; buf[2] = 60;   // pixel 0 = R,G,B (logical channel order)

    uint8_t rgb[3] = {0, 0, 0};
    REQUIRE(r.drivers.firstOutputRgb(rgb));
    CHECK(rgb[0] == 10);
    CHECK(rgb[1] == 200);
    CHECK(rgb[2] == 60);
}

TEST_CASE("Drivers::firstOutputRgb reports black pixel 0 as-is (caller substitutes the default)") {
    Rig r;
    uint8_t* buf = r.layer.buffer().data();
    REQUIRE(buf != nullptr);
    buf[0] = 0; buf[1] = 0; buf[2] = 0;       // black — firstOutputRgb returns true with 0,0,0

    uint8_t rgb[3] = {1, 2, 3};
    REQUIRE(r.drivers.firstOutputRgb(rgb));      // true: there IS an output, and it is black
    CHECK(rgb[0] == 0);
    CHECK(rgb[1] == 0);
    CHECK(rgb[2] == 0);
    // (The WLED shim is what maps an all-black read to MoonLight purple, pinned in the HTTP shim's own logic, not here; this seam reports the raw pixel.)
}

TEST_CASE("Drivers::firstOutputRgb returns false when there is no driven buffer") {
    mm::Drivers drivers;            // no layer pinned, no buffer
    uint8_t rgb[3] = {9, 9, 9};
    CHECK_FALSE(drivers.firstOutputRgb(rgb));
}

TEST_CASE("MoonModule::firstOutputRgb defaults to false (no output module)") {
    // A plain module that doesn't drive output never claims a first LED, the seam's safe default, so the shim falls back to purple for a device with no Drivers.
    mm::MoonModule m;
    uint8_t rgb[3] = {};
    CHECK_FALSE(m.firstOutputRgb(rgb));
}
