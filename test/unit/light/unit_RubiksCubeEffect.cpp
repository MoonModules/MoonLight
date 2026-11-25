/// @module RubiksCubeEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/RubiksCubeEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform_clock.h"   // setTestNowMs

#include <vector>

// Restore the real clock after any test that froze it, so a frozen value can't leak into order-dependent neighbors.
namespace { struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } }; }

// The six sticker colors drawCube() paints from (Red, DarkOrange, Blue, Green, Yellow, White), the only colors a lit voxel may carry.
static bool isRubiksFaceColor(uint8_t r, uint8_t g, uint8_t b) {
    static const mm::RGB kMap[6] = {
        {255, 0, 0}, {255, 140, 0}, {0, 0, 255}, {0, 128, 0}, {255, 255, 0}, {255, 255, 255}};
    for (const mm::RGB& c : kMap)
        if (r == c.r && g == c.g && b == c.b) return true;
    return false;
}

// The first frame scrambles a fresh cube and projects it onto the volume: with millis() past t=0 the init() path fires (doInit_ is set at construction), so the buffer holds a drawn cube, not black.
TEST_CASE("RubiksCubeEffect paints the cube on the first frame") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);  // non-zero millis() so `now > step_` (step_ starts at 0) triggers init

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 8;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RubiksCubeEffect cube;
    layer.addChild(&cube);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 8 * 8 * 8);

    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK(anyLit);
}

// Every lit voxel carries exactly one of the six Rubik's face colors, the projection only ever writes COLOR_MAP entries, never a blended or arbitrary RGB.
TEST_CASE("RubiksCubeEffect only paints the six face colors") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 8;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RubiksCubeEffect cube;
    layer.addChild(&cube);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    for (size_t i = 0; i + 2 < buf.bytes(); i += 3) {
        uint8_t r = buf.data()[i], g = buf.data()[i + 1], b = buf.data()[i + 2];
        if (r == 0 && g == 0 && b == 0) continue;  // interior/background voxel — drawCube leaves it black
        CHECK(isRubiksFaceColor(r, g, b));
    }
}

// turnsPerSecond=0 disables the turn pacing (tick() returns before rotating), but the cube is still drawn on the first frame, init() runs and paints before the turn gate is reached.
TEST_CASE("RubiksCubeEffect with turnsPerSecond=0 still draws but never turns") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 8;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RubiksCubeEffect cube;
    cube.turnsPerSecond = 0;
    layer.addChild(&cube);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK(anyLit);

    // Advancing time far past any turn interval must not change the drawn frame: no turn ever fires, so a full second later the buffer is byte-for-byte identical.
    std::vector<uint8_t> before(buf.data(), buf.data() + buf.bytes());
    mm::platform::setTestNowMs(2000);
    layer.tick();
    std::vector<uint8_t> after(buf.data(), buf.data() + buf.bytes());
    CHECK(before == after);
}

// The effect runs at a degenerate grid size without crashing (the "every grid size" hard rule): tick() bails on a zero extent and the buffer stays empty.
TEST_CASE("RubiksCubeEffect survives a 0x0x0 grid") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RubiksCubeEffect cube;
    layer.addChild(&cube);

    layer.applyState();
    layer.tick();

    CHECK(layer.buffer().count() == 0);
}
