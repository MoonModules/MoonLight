/// @module RandomEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/RandomEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"   // setTestNowMs: the spawn is counted in elapsed time

// A single frame on a fresh black buffer lights exactly ONE light (one setRGB per frame).
TEST_CASE("RandomEffect lights exactly one light per frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RandomEffect effect;
    effect.fade = 255;  // fade every remaining light fully black, isolating this frame's single write
    layer.addChild(&effect);

    layer.applyState();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 64);

    layer.tick();

    // Count lights with any non-black channel. With fade=255 the whole buffer is cleared each frame, so precisely the one randomly chosen light survives, the direct equivalent of MoonLight's single index-based setRGB.
    const uint8_t cpl = buf.channelsPerLight();
    int litLights = 0;
    for (size_t i = 0; i < buf.count(); i++) {
        const size_t off = i * cpl;
        if (buf.data()[off] != 0 || buf.data()[off + 1] != 0 || buf.data()[off + 2] != 0) litLights++;
    }
    CHECK(litLights == 1);
}

// Over many frames with light fade the sparkle field fills, more than one light ends up lit.
TEST_CASE("RandomEffect scatters color across many lights over many frames") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RandomEffect effect;
    effect.fade = 1;  // barely fade, so lit lights linger and the field fills over frames
    layer.addChild(&effect);

    layer.applyState();

    // 200 frames of virtual time, since the spawn follows elapsed time rather than the frame count.
    for (uint32_t f = 0; f < 200; f++) {
        mm::platform::setTestNowMs(1000 + f * 17);
        layer.tick();
    }

    auto& buf = layer.buffer();
    const uint8_t cpl = buf.channelsPerLight();
    int litLights = 0;
    for (size_t i = 0; i < buf.count(); i++) {
        const size_t off = i * cpl;
        if (buf.data()[off] != 0 || buf.data()[off + 1] != 0 || buf.data()[off + 2] != 0) litLights++;
    }
    CHECK(litLights > 1);
}

// The effect runs at degenerate grid sizes without crashing (Effects-must-run-at-every-grid-size).
TEST_CASE("RandomEffect survives degenerate grids") {
    for (mm::lengthType n : {0, 1}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = n;
        grid.height = n;
        grid.depth = n;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::RandomEffect effect;
        layer.addChild(&effect);

        layer.applyState();
        layer.tick();  // must not crash on 0×0×0 or 1×1×1

        CHECK(true);  // reaching here means no crash / hang
    }
}
