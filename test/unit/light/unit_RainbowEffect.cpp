/// @module RainbowEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/RainbowEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform_clock.h"   // millis

// A single frame on a 4×4 grid leaves the buffer non-zero (rainbow always paints somewhere).
TEST_CASE("RainbowEffect writes non-zero RGB data to buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.applyState();

    // Simulate a frame at elapsed=0 (effect uses platform::millis())
    layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 16);

    // At least some lights should be non-zero (rainbow produces color everywhere)
    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

// Pixel (0,0) carries a lit palette color, confirms the effect writes a real RGB there.
TEST_CASE("RainbowEffect pixel 0,0 produces valid RGB") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.applyState();
    // Pin a known-colorful palette (Rainbow=0, generated at full saturation/value so no entry is black), Palettes::active() is a process-wide static any prior test can mutate, so this makes the non-black assertion order-independent.
    mm::Palettes::setActive(0);
    layer.tick();

    auto* data = layer.buffer().data();
    // Pixel (0,0): the effect maps the diagonal hue through the active palette, so the exact color depends on elapsed() (the phase) and the palette. The stable, time-independent contract is that the effect writes a LIT color there (not black), asserting a channel == 255 was false, since palette colors are not all fully saturated (the old hsvToRgb(h,255,255) assumption is stale).
    uint8_t r = data[0], g = data[1], b = data[2];
    CHECK((r > 0 || g > 0 || b > 0));
}

// Distant pixels carry different hues (the rainbow gradient is spatial, not uniform).
TEST_CASE("RainbowEffect different positions produce different hues") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.applyState();
    layer.tick();

    auto* data = layer.buffer().data();
    // Pixel (0,0) and pixel (8,8) should have different colors
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t idx88 = (8 * 16 + 8) * 3;
    uint8_t r1 = data[idx88], g1 = data[idx88 + 1], b1 = data[idx88 + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}
