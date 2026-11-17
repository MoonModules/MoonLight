/// @module MoonManEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/MoonManEffect.h"

namespace {

// MoonMan's yellow is fixed and belongs to no other character in the cast, so counting it counts MoonMen on the wall. Testing the drawn frame rather than the pool keeps this a statement about what a user sees.
int yellowPixels(mm::Layer& layer) {
    const auto& buf = layer.buffer();
    int n = 0;
    for (size_t i = 0; i + 2 < buf.count() * 3; i += 3)
        if (buf.data()[i] == 255 && buf.data()[i + 1] == 214 && buf.data()[i + 2] == 0) n++;
    return n;
}

}  // namespace

// Changing how many of each KIND are on the wall must change what is drawn. Slots are positional (the first `moonmen` slots are MoonMan, the rest invaders) and a slot's role is decided when it launches. Trading one kind for another keeps the TOTAL the same, so no slot respawns, and unless a re-role pass runs, the cast keeps the shapes it already had: the controls say three MoonMen while the wall still shows one. Found in review.
TEST_CASE("Trading invaders for MoonMen changes what is drawn on the wall") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 64; grid.height = 64; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::MoonManEffect fx;
    fx.moonmen = 1;
    fx.invaders = 4;
    fx.spriteSize = 2;          // fixed, so the pixel counts do not ride on the auto-scale rule
    layer.addChild(&fx);
    layer.applyState();
    layer.tick();

    const int oneMan = yellowPixels(layer);
    CHECK(oneMan > 0);          // the single MoonMan is on the wall

    // Same five characters, a different split: nothing needs to spawn or die. This is the UI path - a control change, NOT a re-prepare, which would relaunch every slot and hide the very staleness under test.
    fx.moonmen = 3;
    fx.invaders = 2;
    layer.tick();

    CHECK(yellowPixels(layer) > oneMan);   // more MoonMen means more yellow
}
