/// @module BreakoutEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/BreakoutEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time

namespace {

// A layer holding one Breakout effect on a w x h RGB grid.
struct BreakoutRig {
    mm::Layouts       layouts;
    mm::GridLayout    grid;
    mm::Layer         layer;
    mm::BreakoutEffect fx;

    BreakoutRig(mm::lengthType w, mm::lengthType h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
    }
};

// Restores the real clock, so a frozen time never leaks past the case that set it.
struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };

}  // namespace

// The first frame deals a full wall: every brick of every requested row stands.
TEST_CASE("Breakout deals a full wall on the first frame") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);
    BreakoutRig rig(32, 32);
    rig.layer.applyState();
    rig.layer.tick();
    CHECK(rig.fx.bricksLeftForTest() == 5 * 8);
}

// Over a minute of play the paddle keeps the ball in, the ball stays on the court, and bricks fall.
TEST_CASE("Breakout knocks bricks down and keeps the ball on the court") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);
    BreakoutRig rig(32, 32);
    rig.layer.applyState();
    rig.layer.tick();
    const uint16_t full = rig.fx.bricksLeftForTest();
    bool fell = false;
    for (uint32_t t = 1000; t <= 61000; t += 20) {
        mm::platform::setTestNowMs(t);
        rig.layer.tick();
        CHECK(rig.fx.ballXForTest() >= 0);
        CHECK(rig.fx.ballXForTest() <= 4096);
        CHECK(rig.fx.ballYForTest() >= 0);
        CHECK(rig.fx.ballYForTest() <= 4096);
        if (rig.fx.bricksLeftForTest() < full) fell = true;
    }
    CHECK(fell);
}

// `rows` applies live: changing it deals a fresh wall of the new height on the next frame.
TEST_CASE("Breakout deals a new wall when rows changes") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);
    BreakoutRig rig(32, 32);
    rig.layer.applyState();
    rig.layer.tick();
    rig.fx.rows = 3;
    rig.layer.tick();
    CHECK(rig.fx.bricksLeftForTest() == 3 * 8);
}

// A grid too short for the requested wall gets the rows that fit, so a strip still plays.
TEST_CASE("Breakout fits the wall to a short grid") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);
    BreakoutRig rig(16, 8);
    rig.fx.rows = 8;
    rig.layer.applyState();
    rig.layer.tick();
    CHECK(rig.fx.bricksLeftForTest() < 8 * 8);
}

// `descend` lowers the wall by elapsed time and enters a fresh row at the top, so the wall grows past the rows it was dealt.
TEST_CASE("Breakout's descending wall gains rows over time") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);
    BreakoutRig rig(32, 64);
    rig.fx.rows = 3;
    rig.fx.descend = true;
    rig.layer.applyState();
    rig.layer.tick();
    const uint16_t dealt = rig.fx.bricksLeftForTest();
    // Twelve seconds at ten rows a minute is two new rows; the ball takes a few bricks meanwhile.
    for (uint32_t t = 1000; t <= 13000; t += 20) {
        mm::platform::setTestNowMs(t);
        rig.layer.tick();
    }
    CHECK(rig.fx.bricksLeftForTest() > dealt);
}

// Writing the paddle control takes the paddle: it holds where the control says, the ends clamped so the whole paddle stays on the court.
TEST_CASE("Breakout hands a written paddle to the player") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);
    BreakoutRig rig(32, 32);
    rig.layer.applyState();
    rig.fx.player = 255;
    rig.fx.onControlChanged("player");
    const int32_t half = 4096 * rig.fx.paddle / 200;
    for (uint32_t t = 1000; t < 5000; t += 20) {
        mm::platform::setTestNowMs(t);
        rig.layer.tick();
        CHECK(rig.fx.paddleForTest() <= 4096 - half / 2);   // at the right end, halved or not
        CHECK(rig.fx.paddleForTest() >= 4096 - half);
    }
    CHECK(rig.fx.player == 255);   // the control is the player's, so the game leaves it alone
}

