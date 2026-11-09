/// @module SpaceInvadersEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/SpaceInvadersEffect.h"
#include "light/layouts/GridLayout.h"
#include "audio_effect_test.h"   // seat and EffectOnGrid: the audio service and the grid
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

// The frame number is tracked with audioReactive off too: turning it on with a kick still in the last frame holds the formation, and the next frame's kick steps it (review, 2026-11-06).
TEST_CASE("Space Invaders turned audio-reactive does not march on a kick already past") {
    mm::golden::ScopedTestClock clock(1000);   // the clock stands still, so only a kick can step the march
    mm::AudioService& audio = mm::seat();
    mm::AudioFrame& f = audio.frameForTest();
    f.gate = 40; f.gateOpen = true; f.onset = f.onsetLow = 200;
    f.seq++;

    mm::EffectOnGrid<mm::SpaceInvadersEffect> fx(64, 64);
    fx.layer.tick();
    const int16_t before = fx.e.formationForTest();
    fx.e.audioReactive = true;
    fx.layer.tick();
    CHECK(fx.e.formationForTest() == before);
    f.seq++;   // the next frame, a kick too
    fx.layer.tick();
    CHECK(fx.e.formationForTest() == before + fx.e.stepX);
    audio.release();
}

// An effect whose audio is a switch needs the analysis only while the switch is on, so a Space Invaders marching on its own clock leaves the audio resting.
TEST_CASE("Space Invaders needs the audio analysis only while audioReactive is on") {
    mm::EffectOnGrid<mm::SpaceInvadersEffect> fx(32, 32);
    const auto demanded = [&fx] {
        uint8_t bits = 0;
        for (const mm::ModuleResource* r = fx.e.resources(); r; r = r->nextResource()) bits |= r->demandsOf(mm::AudioDemand::kProvider);
        return (bits & mm::AudioDemand::bitOf(mm::AudioFeature::Bands)) != 0;
    };
    fx.e.audioReactive = true;
    fx.e.prepare();
    CHECK(demanded());
    CHECK(fx.e.affectsPrepare("audioReactive"));   // the switch prepares the effect again, so its demand follows
    fx.e.audioReactive = false;
    fx.e.prepare();
    CHECK_FALSE(demanded());
}
