/// @module PongEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/PongEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time

// Short paddles miss, so a few minutes of play score points.
TEST_CASE("Pong scores misses") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 32; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::PongEffect fx;
    fx.paddle = 10;
    fx.rallyBpm = 200;
    layer.addChild(&fx);
    layer.applyState();

    bool scored = false;
    for (uint32_t t = 1000; t <= 301000 && !scored; t += 20) {
        mm::platform::setTestNowMs(t);
        layer.tick();
        if (fx.scoreForTest(0) + fx.scoreForTest(1) > 0) scored = true;
    }
    CHECK(scored);
}

// The ball turns in front of a paddle, never inside it: on a small grid the paddle's own column is the end column.
TEST_CASE("Pong's ball stays out of the paddle columns") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::PongEffect fx;
    fx.paddle = 60;   // long paddles, so most balls are hit rather than scored
    fx.rallyBpm = 200;
    layer.addChild(&fx);
    layer.applyState();

    int outside = 0, onLeft = 0, onRight = 0;
    for (uint32_t t = 1000; t <= 61000; t += 20) {
        mm::platform::setTestNowMs(t);
        layer.tick();
        const int32_t column = fx.ballColumnForTest();
        if (column < 1 || column > 6) outside++;
        if (column == 1) onLeft++;
        if (column == 6) onRight++;
    }
    CHECK(outside == 0);
    // Both ends alike: the column in front of each paddle is shown about as long.
    CHECK(onRight * 10 >= onLeft * 7);
    CHECK(onLeft * 10 >= onRight * 7);
}

// The eleventh point ends the game: both scores start over, as the original played to 11.
TEST_CASE("Pong starts a new game at 11") {
    mm::PongEffect fx;
    for (int i = 0; i < 10; i++) fx.pointForTest(0);
    fx.pointForTest(1);
    REQUIRE(fx.scoreForTest(0) == 10);
    REQUIRE(fx.scoreForTest(1) == 1);
    fx.pointForTest(0);
    CHECK(fx.scoreForTest(0) == 0);
    CHECK(fx.scoreForTest(1) == 0);
}

// Writing a paddle control takes that paddle until ten quiet seconds pass; then the game plays it again and the control follows.
TEST_CASE("Pong hands a written paddle to the player, and takes it back when the input goes quiet") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 32; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::PongEffect fx;
    layer.addChild(&fx);
    layer.applyState();

    fx.player1 = 255;   // a fader at the top: the paddle at the top of the court
    fx.onControlChanged("player1");
    uint32_t t = 1000;
    for (; t < 10000; t += 20) {
        mm::platform::setTestNowMs(t);
        layer.tick();
        CHECK(fx.paddleForTest(0) == 0);
    }
    // Past the hold the game plays the paddle again, and the control reads back where it went.
    bool moved = false;
    for (; t < 30000; t += 20) {
        mm::platform::setTestNowMs(t);
        layer.tick();
        if (fx.paddleForTest(0) != 0) moved = true;
    }
    CHECK(moved);
    CHECK(fx.player1 == static_cast<uint8_t>(255 - fx.paddleForTest(0) * 255 / 4096));
}

