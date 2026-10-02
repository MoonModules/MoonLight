/// @module SpaceInvadersEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/SpaceInvadersEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time

// Writing `player` and pressing `fire` take the cannon: it moves where the control says and fires one shot, a second press ignored while that shot flies.
TEST_CASE("Space Invaders hands the cannon to the player, one shot at a time") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 64; grid.height = 64; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::SpaceInvadersEffect fx;
    layer.addChild(&fx);
    layer.applyState();
    layer.tick();

    fx.player = 255;
    fx.onControlChanged("player");
    mm::platform::setTestNowMs(1020);
    layer.tick();
    CHECK(fx.cannonForTest() == 64 - 13);   // the right end: the grid less the cannon's width
    CHECK(fx.cannonShotsForTest() == 0);

    fx.onControlChanged("fire");
    mm::platform::setTestNowMs(1040);
    layer.tick();
    CHECK(fx.cannonShotsForTest() == 1);

    fx.onControlChanged("fire");
    mm::platform::setTestNowMs(1060);
    layer.tick();
    CHECK(fx.cannonShotsForTest() == 1);   // still the one shot: the arcade allows one in the air
}
