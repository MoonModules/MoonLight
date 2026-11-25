/// @module StarFieldEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/StarFieldEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform_clock.h"   // setTestNowMs

// Restore the real clock on scope exit, even if a REQUIRE aborts the case mid-way, so a frozen millis() can't leak into a later test (same pattern as unit_BouncingBallsEffect.cpp).
namespace { struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } }; }

// A frame past the speed throttle interval lights at least one star (greyscale, so every lit pixel is a pure grey R==G==B), the field advances and re-projects stars onto the panel.
TEST_CASE("StarFieldEffect paints greyscale stars once the throttle elapses") {
    ClockGuard guard;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::StarFieldEffect stars;
    stars.numStars = 255;   // many stars so at least one projects on-panel
    stars.usePalette = false;
    layer.addChild(&stars);
    layer.applyState();

    // speed=20 → throttle 1000/20 = 50 ms; step_ starts at 0, so millis()=100 clears the gate.
    mm::platform::setTestNowMs(100);
    layer.tick();

    auto* data = layer.buffer().data();
    const size_t count = layer.buffer().count();
    REQUIRE(data != nullptr);
    REQUIRE(count == 256);

    bool anyLit = false;
    bool allGrey = true;
    for (size_t i = 0; i < count; i++) {
        const uint8_t r = data[i * 3], g = data[i * 3 + 1], b = data[i * 3 + 2];
        if (r || g || b) {
            anyLit = true;
            if (!(r == g && g == b)) allGrey = false;  // greyscale: R==G==B for every lit star
        }
    }
    CHECK(anyLit);
    CHECK(allGrey);

}

// speed=0 pauses the field: the buffer stays fully black no matter how much virtual time passes.
TEST_CASE("StarFieldEffect at speed 0 leaves the buffer black") {
    ClockGuard guard;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::StarFieldEffect stars;
    stars.speed = 0;        // paused
    stars.numStars = 255;
    layer.addChild(&stars);
    layer.applyState();

    mm::platform::setTestNowMs(1000);
    layer.tick();

    auto& buf = layer.buffer();
    bool allBlack = true;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { allBlack = false; break; }
    }
    CHECK(allBlack);

}

// The palette variant lights on-panel stars in color (not forced grey), usePalette drives hue.
TEST_CASE("StarFieldEffect with usePalette lights stars from the palette") {
    ClockGuard guard;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::StarFieldEffect stars;
    stars.numStars = 255;
    stars.usePalette = true;
    layer.addChild(&stars);
    layer.applyState();

    // Rainbow palette (0), generated at full saturation/value so entries are colorful, not grey, Palettes::active() is a process-wide static any prior test can mutate, so pin it here.
    mm::Palettes::setActive(0);

    mm::platform::setTestNowMs(100);
    layer.tick();

    auto* data = layer.buffer().data();
    const size_t count = layer.buffer().count();
    bool anyLit = false;
    bool anyColored = false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t r = data[i * 3], g = data[i * 3 + 1], b = data[i * 3 + 2];
        if (r || g || b) {
            anyLit = true;
            if (!(r == g && g == b)) { anyColored = true; break; }  // a non-grey pixel = palette color
        }
    }
    CHECK(anyLit);
    CHECK(anyColored);

}

// Hard rule: the effect runs at a degenerate 0×0×0 grid without crashing (it allocates nothing and the loop bails on the zero dimensions).
TEST_CASE("StarFieldEffect survives a 0x0x0 grid") {
    ClockGuard guard;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::StarFieldEffect stars;
    layer.addChild(&stars);

    layer.applyState();
    mm::platform::setTestNowMs(100);
    layer.tick();   // must not crash

    CHECK(layer.buffer().count() == 0);

}
