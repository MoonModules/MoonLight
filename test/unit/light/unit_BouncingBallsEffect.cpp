/// @module BouncingBallsEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/BouncingBallsEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"  // setTestNowMs: a frozen clock for a deterministic frame

#include <vector>

// Restore the real clock after any test that froze it, so a frozen value can't leak into order-dependent neighbors.
namespace { struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } }; }

namespace {
/// The frame a fresh BouncingBallsEffect draws on a `w` by `h` grid at `ms`.
std::vector<uint8_t> frameAt(int w, int h, uint32_t ms) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = w;
    grid.height = h;
    grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::BouncingBallsEffect balls;
    layer.addChild(&balls);
    mm::platform::setTestNowMs(ms);
    layer.applyState();
    layer.tick();
    std::vector<uint8_t> bytes(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    layer.removeChild(&balls);
    return bytes;
}
}  // namespace

// A ball is a function of its column and the time alone, so two effects started apart draw the same frame at the same moment: what keeps the boards of a wall in one picture.
TEST_CASE("BouncingBallsEffect draws the same frame at the same moment, whenever it started") {
    ClockGuard guard;
    const auto a = frameAt(8, 16, 123457);
    const auto b = frameAt(8, 16, 123457);
    CHECK(a == b);
    bool lit = false;
    for (uint8_t v : a) lit = lit || v;
    CHECK(lit);
}

// numBalls=0 draws nothing: the effect clamps below its minimum and the buffer stays black.
TEST_CASE("BouncingBallsEffect with zero balls leaves the buffer black") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    balls.numBalls = 0;  // nBalls <= 0 → tick() returns before drawing
    layer.addChild(&balls);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// The effect runs at a degenerate grid size without crashing (the "every grid size" hard rule).
TEST_CASE("BouncingBallsEffect survives a 0x0x0 grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    layer.addChild(&balls);

    // No allocation, no draw, no crash on a zero grid.
    layer.applyState();
    layer.tick();

    CHECK(layer.buffer().count() == 0);
}

// A ball reaches its apex mid-flight: once time advances, at least one ball has risen off the bottom row, so the lit column occupies a row above the floor.
TEST_CASE("BouncingBallsEffect balls rise above the floor as time advances") {
    ClockGuard guard;

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 1;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    balls.numBalls = 8;
    layer.addChild(&balls);

    layer.applyState();

    // A ball with the maximum kick climbs for about 450 ms before falling back; a spread of instants keeps the assertion off one exact frame.
    const int w = 1, h = 16;
    bool roseAboveFloor = false;
    for (uint32_t t = 50; t <= 400 && !roseAboveFloor; t += 50) {
        mm::platform::setTestNowMs(1 + t);
        layer.tick();
        for (int y = 0; y < h - 1; y++) {  // any row strictly above the floor
            size_t idx = static_cast<size_t>(y * w + 0) * 3;
            if (layer.buffer().data()[idx] | layer.buffer().data()[idx + 1] | layer.buffer().data()[idx + 2]) {
                roseAboveFloor = true;
                break;
            }
        }
    }
    CHECK(roseAboveFloor);
}
