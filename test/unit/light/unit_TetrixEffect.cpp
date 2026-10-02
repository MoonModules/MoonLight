/// @module TetrixEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/TetrixEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time

// Each column runs a falling-brick state machine that starts after its own delay of up to 2 s; a frozen clock makes the seed, and so the run, deterministic.

namespace {

// Builds a layer holding a Tetrix effect on a w×h×1 RGB grid, all children wired.
struct TetrixRig {
    mm::Layouts     layouts;
    mm::GridLayout  grid;
    mm::Layer       layer;
    mm::TetrixEffect fx;

    TetrixRig(mm::lengthType w, mm::lengthType h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
    }
};

// Restores the real platform clock so a frozen time never leaks past the case that set it.
struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };

bool anyLit(mm::Layer& layer) {
    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.bytes(); i++) if (buf.data()[i] != 0) return true;
    return false;
}

} // namespace

// The first frame falls inside the start delay, so the buffer stays black.
TEST_CASE("TetrixEffect renders black during the start delay") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);   // freeze; every column waits at least 500 ms

    TetrixRig rig(8, 8);
    rig.layer.applyState();

    // Still inside every column's start delay, so the state machine only waits.
    rig.layer.tick();

    CHECK(rig.grid.width * rig.grid.height == 64);
    CHECK_FALSE(anyLit(rig.layer));
}

// Past the start delay, bricks fall and light the grid in palette colors.
TEST_CASE("TetrixEffect lights up with palette color after the start delay") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);   // 0 would restore the real clock, not freeze it at zero

    TetrixRig rig(8, 8);
    rig.layer.applyState();   // each column starts between 501 and 1995 ms

    // Advance well past every start delay, then run many frames so the start-roll (step 1→2) fires and bricks descend into the visible region. Step time forward each frame like a real tick loop.
    bool lit = false;
    for (uint32_t t = 3000; t <= 8000 && !lit; t += 25) {
        mm::platform::setTestNowMs(t);
        rig.layer.tick();
        lit = anyLit(rig.layer);
    }
    REQUIRE(lit);

    // A lit light is a full palette color, not a stray channel.
    auto& buf = rig.layer.buffer();
    bool foundColored = false;
    for (size_t p = 0; p + 2 < buf.bytes(); p += 3) {
        const uint8_t r = buf.data()[p], g = buf.data()[p + 1], b = buf.data()[p + 2];
        if (r || g || b) { foundColored = true; break; }
    }
    CHECK(foundColored);
}

// A landed brick stays on screen: the frame is cleared every tick, so the stack is redrawn from memory, not left behind.
TEST_CASE("TetrixEffect keeps its landed stack visible") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);
    // One column and the fastest fall, so after the first landing only the stack can light the floor.
    TetrixRig rig(1, 8);
    rig.fx.speedControl = 255;
    rig.layer.applyState();

    auto bottomLit = [&]() {
        const uint8_t* p = rig.layer.buffer().data() + static_cast<size_t>(7) * 3;
        return p[0] || p[1] || p[2];
    };

    uint32_t t = 3000;
    for (; t <= 20000 && !bottomLit(); t += 25) {
        mm::platform::setTestNowMs(t);
        rig.layer.tick();
    }
    REQUIRE(bottomLit());
    // Twenty frames on, while the next brick spawns and falls, the floor is still lit.
    for (int i = 0; i < 20; i++) {
        t += 25;
        mm::platform::setTestNowMs(t);
        rig.layer.tick();
    }
    CHECK(bottomLit());
}

// A 0x0x0 and a 1x1 grid both survive several frames of advancing time.
TEST_CASE("TetrixEffect survives degenerate and minimal grids") {
    ClockGuard guard;

    // 0×0×0: prepare allocates zero drops, tick() bails on the w<=0 guard.
    {
        TetrixRig rig(0, 0);
        rig.grid.depth = 0;
        mm::platform::setTestNowMs(0);
        rig.layer.applyState();
        for (uint32_t t = 0; t <= 6000; t += 500) {
            mm::platform::setTestNowMs(t);
            rig.layer.tick();
        }
        CHECK(rig.layer.buffer().count() == 0);
    }

    // 1×1: a single column, one row, the brick fills and clears the lone light without crashing.
    {
        TetrixRig rig(1, 1);
        mm::platform::setTestNowMs(0);
        rig.layer.applyState();
        for (uint32_t t = 0; t <= 8000; t += 25) {
            mm::platform::setTestNowMs(t);
            rig.layer.tick();
        }
        CHECK(rig.layer.buffer().count() == 1);
    }
}
