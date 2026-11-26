/// @module SineEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/SineEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform_clock.h"   // setTestNowMs

// Render the effect on a w×h×d grid for one tick; returns the layer (buffer populated).
static void buildLayer(mm::Layouts& layouts, mm::GridLayout& grid, mm::Layer& layer,
                       mm::SineEffect& sine, mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    grid.width = w; grid.height = h; grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&sine);
    layer.applyState();
    layer.tick();
}

TEST_CASE("SineEffect writes non-zero RGB data") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    buildLayer(layouts, grid, layer, sine, 8, 8, 1);
    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    bool nonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) if (buf.data()[i]) { nonZero = true; break; }
    CHECK(nonZero);
}

TEST_CASE("SineEffect amplitude 0 yields a black buffer") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    sine.amplitude = 0;
    buildLayer(layouts, grid, layer, sine, 8, 8, 1);
    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.bytes(); i++) CHECK(buf.data()[i] == 0);
}

TEST_CASE("SineEffect varies across the x axis (R channel follows x)") {
    // The clock stands still, since the scroll phase moves both columns: at some phases two columns an eighth of a turn apart sit either side of the peak and read alike.
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1);
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    sine.frequency = 8;   // the angle steps 8 per column on the 256-step circle
    buildLayer(layouts, grid, layer, sine, 8, 1, 1);
    auto& buf = layer.buffer();
    // R of column 0 against column 4, 32 steps apart, which differ while the phase sits at the start.
    CHECK(buf.data()[0] != buf.data()[4 * 3]);
}

TEST_CASE("SineEffect survives a 0x0x0 grid") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    buildLayer(layouts, grid, layer, sine, 0, 0, 0);   // must not crash
    CHECK(true);
}
