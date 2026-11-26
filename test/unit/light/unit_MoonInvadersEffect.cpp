/// @module MoonInvadersEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/MoonInvadersEffect.h"
#include "light/layouts/GridLayout.h"
#include "audio_effect_test.h"   // seat and EffectOnGrid: the audio service and the grid
#include "platform/platform_clock.h"   // setTestNowMs

// Writing `player` and pressing `fire` take the cannon: it moves where the control says and fires one shot, a second press ignored while that shot flies.
TEST_CASE("MoonInvaders hands the cannon to the player, one shot at a time") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 64; grid.height = 64; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::MoonInvadersEffect fx;
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
TEST_CASE("MoonInvaders turned audio-reactive does not march on a kick already past") {
    mm::golden::ScopedTestClock clock(1000);   // the clock stands still, so only a kick can step the march
    mm::AudioService& audio = mm::seat();
    mm::AudioFrame& f = audio.frameForTest();
    f.gate = 40; f.gateOpen = true; f.onset = f.onsetLow = 200;
    f.seq++;

    mm::EffectOnGrid<mm::MoonInvadersEffect> fx(64, 64);
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

// An effect whose audio is a switch needs the analysis only while the switch is on, so a MoonInvaders marching on its own clock leaves the audio resting.
TEST_CASE("MoonInvaders needs the audio analysis only while audioReactive is on") {
    mm::EffectOnGrid<mm::MoonInvadersEffect> fx(32, 32);
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

namespace {
// How many pupils of the frame have the white of their eye on the side `dx` names, the pupil's own pixels apart.
int pupilsWithWhiteAt(const mm::Layer& layer, int w, int h, int dx) {
    const auto px = [&](int x, int y) { const uint8_t* d = layer.buffer().data() + (static_cast<size_t>(y) * w + x) * 3; return mm::RGB{d[0], d[1], d[2]}; };
    const auto is = [](mm::RGB c, mm::RGB want) { return c.r == want.r && c.g == want.g && c.b == want.b; };
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 1; x + 1 < w; x++)
            if (is(px(x, y), {20, 20, 120}) && is(px(x + dx, y), {255, 255, 255})) n++;
    return n;
}
}  // namespace

TEST_CASE("MoonInvaders' ghosts look the way the formation marches, and turn their eyes when it turns at a wall") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    uint32_t now = 1000;
    mm::platform::setTestNowMs(now);
    mm::EffectOnGrid<mm::MoonInvadersEffect> game(64, 64);
    mm::Layer& layer = game.layer;
    layer.tick();
    REQUIRE(pupilsWithWhiteAt(layer, 64, 64, -1) > 0);   // marching right: the white behind each pupil, on its left
    CHECK(pupilsWithWhiteAt(layer, 64, 64, +1) == 0);
    bool turned = false;
    for (int frame = 0; frame < 4000 && !turned; frame++) {   // a wall is a few steps away
        now += 50;
        mm::platform::setTestNowMs(now);
        layer.tick();
        turned = pupilsWithWhiteAt(layer, 64, 64, +1) > 0 && pupilsWithWhiteAt(layer, 64, 64, -1) == 0;
    }
    CHECK(turned);   // marching left: mirrored
}
