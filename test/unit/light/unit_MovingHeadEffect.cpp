/// @module MovingHeadEffect

#include "doctest.h"
#include "light/util/FixtureChannels.h"
#include "light/effects/MovingHeadEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "audio_effect_test.h"   // seat and EffectOnGrid: the audio service and the grid

#include <set>
#include <utility>
#include <vector>

namespace {

// A rig of moving heads: a grid of `channels`-wide lights carrying RGBW plus pan/tilt, which is what a fixture preset produces. Returns the layer so a case can tick it and read the buffer.
constexpr uint8_t kChannels = mm::FixtureChannels::kMotionBase + 2;   // RGBW + pan + tilt

// A fixture that can be aimed. Without this the Layer knows of no motion channels and setPan / setTilt are deliberate no-ops (the same reason they cost nothing on a plain LED strip), so a test that skipped it would read zeros and prove nothing.
mm::FixtureChannels motionRig() {
    mm::FixtureChannels fc;
    fc.pan  = mm::FixtureChannels::kMotionBase + 0;
    fc.tilt = mm::FixtureChannels::kMotionBase + 1;
    return fc;
}

// Every head's (pan, tilt) after a few frames, indexed the way the light buffer is.
std::vector<std::pair<uint8_t, uint8_t>> aimsAfterTicks(mm::Layer& layer, int frames) {
    for (int i = 0; i < frames; i++) layer.tick();
    auto& buf = layer.buffer();
    std::vector<std::pair<uint8_t, uint8_t>> out;
    for (size_t i = 0; i < buf.count(); i++) {
        const uint8_t* light = buf.data() + i * kChannels;
        out.emplace_back(light[mm::FixtureChannels::kMotionBase + 0],
                         light[mm::FixtureChannels::kMotionBase + 1]);
    }
    return out;
}

}  // namespace

// The bug this pins: a 2D truss where every fixture in a row moved identically. The effect used to declare Dim::D1, so the Layer EXTRUDED its single column across x and every row was a copy of head 0. A rig is not a gradient: each head has its own place in the formation, on a 1 x N chain, a 2D truss and a 3D array alike. Found on the bench 2026-08-29.
TEST_CASE("every fixture in a 2D rig gets its own aim, not a copy of its row's first head") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(kChannels);
    layer.setFixtureChannels(motionRig());

    mm::MovingHeadEffect heads;
    heads.formation = mm::MovingHeadEffect::kChase;   // a traveling wave: every head differs
    layer.addChild(&heads);
    layer.applyState();

    const auto aims = aimsAfterTicks(layer, 4);
    REQUIRE(aims.size() == 16);

    // Within one row the heads must differ. Under the old D1 extrude every entry of a row was identical, which is exactly what "all fixtures on one row move the same" reported.
    for (int row = 0; row < 4; row++) {
        std::set<std::pair<uint8_t, uint8_t>> inRow;
        for (int col = 0; col < 4; col++) inRow.insert(aims[row * 4 + col]);
        CHECK(inRow.size() > 1);
    }

    // And a column varies too, so the rig reads in both axes rather than only down one.
    std::set<std::pair<uint8_t, uint8_t>> inColumn;
    for (int row = 0; row < 4; row++) inColumn.insert(aims[row * 4]);
    CHECK(inColumn.size() > 1);
}

// The same promise one dimension up: a 3D array must not repeat slice by slice.
TEST_CASE("a 3D rig varies through depth instead of repeating each slice") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 2;
    grid.height = 2;
    grid.depth = 2;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(kChannels);
    layer.setFixtureChannels(motionRig());

    mm::MovingHeadEffect heads;
    heads.formation = mm::MovingHeadEffect::kChase;
    layer.addChild(&heads);
    layer.applyState();

    const auto aims = aimsAfterTicks(layer, 4);
    REQUIRE(aims.size() == 8);
    // Slice 0 is lights 0-3, slice 1 is lights 4-7: the two must not be identical.
    bool sliceDiffers = false;
    for (int i = 0; i < 4; i++) if (aims[i] != aims[i + 4]) sliceDiffers = true;
    CHECK(sliceDiffers);
}

// A 1 x N chain is the shape a fixture chain actually takes (heads at consecutive DMX addresses), and the position-driven formations must still behave there: mirror splits the chain in half.
TEST_CASE("a 1 x N fixture chain still varies head by head") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 1;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(kChannels);
    layer.setFixtureChannels(motionRig());

    mm::MovingHeadEffect heads;
    // Chase, not mirror. Mirror's halves differ only by sweep DIRECTION, which multiplies a sine of the elapsed time, and `elapsed()` is 0 across a host tick loop (no wall clock passes): every head then sits exactly at center and the formation is unobservable here. Chase separates its heads by static phase offset, so it is the formation a host test can see. What matters for the reported bug is the same either way: the aim varies ACROSS the rig.
    heads.formation = mm::MovingHeadEffect::kChase;
    heads.panCenter = 128;
    layer.addChild(&heads);
    layer.applyState();

    const auto aims = aimsAfterTicks(layer, 4);
    REQUIRE(aims.size() == 4);
    // Down the chain the heads must differ: this is the 1 x N shape a fixture chain actually takes, and the position-driven formations must keep working on it.
    std::set<uint8_t> pans;
    for (const auto& a : aims) pans.insert(a.first);
    CHECK(pans.size() > 1);
}


// --- the beam wheels ---------------------------------------------------------------------------
//
// A head's gobo and rotate channels were reachable by the PRESET model (FixtureChannels carries all five motion roles, Drivers assigns their slots and Correction reads them back) but by no EFFECT: EffectBase exposed setPan/setTilt/setZoom and nothing else, so the two wheels were plumbed end to end with nobody able to write them. Ported from MoonLight's MovingHeads effects, which drive both.

namespace {

// A head with the beam wheels as well as aim: RGBW + pan + tilt + rotate + gobo, in the packing order FixtureChannels::forEachMotionSlot defines (pan, tilt, zoom, rotate, gobo), with zoom absent, which is what makes this a good test: the slots must close up around the missing role.
constexpr uint8_t kBeamChannels = mm::FixtureChannels::kMotionBase + 4;

mm::FixtureChannels beamRig() {
    mm::FixtureChannels fc;
    fc.pan    = mm::FixtureChannels::kMotionBase + 0;
    fc.tilt   = mm::FixtureChannels::kMotionBase + 1;
    fc.rotate = mm::FixtureChannels::kMotionBase + 2;
    fc.gobo   = mm::FixtureChannels::kMotionBase + 3;
    return fc;
}

}  // namespace

TEST_CASE("a head's gobo and rotate carry the values the effect asks for") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 3;
    grid.height = 1;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(kBeamChannels);
    layer.setFixtureChannels(beamRig());

    mm::MovingHeadEffect heads;
    heads.gobo = 96;
    heads.rotate = 200;
    layer.addChild(&heads);
    layer.applyState();

    for (int i = 0; i < 3; i++) layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 3);
    for (size_t i = 0; i < buf.count(); i++) {
        const uint8_t* light = buf.data() + i * kBeamChannels;
        CHECK(light[mm::FixtureChannels::kMotionBase + 3] == 96);    // gobo
        CHECK(light[mm::FixtureChannels::kMotionBase + 2] == 200);   // rotate
    }
}

// The orthogonality the whole role model rests on: the same effect on a fixture WITHOUT the wheels writes no beam bytes rather than scribbling on whatever channel happens to sit there. A rig of plain pan/tilt heads has its own meaning for the bytes past tilt, and a stray gobo write would land on one of them.
TEST_CASE("a head with no gobo channel is not written past its own channels") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 2;
    grid.height = 1;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(kBeamChannels);   // room for four motion bytes
    layer.setFixtureChannels(motionRig());      // but the fixture declares only pan + tilt

    mm::MovingHeadEffect heads;
    heads.gobo = 96;
    heads.rotate = 200;
    layer.addChild(&heads);
    layer.applyState();

    for (int i = 0; i < 3; i++) layer.tick();

    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.count(); i++) {
        const uint8_t* light = buf.data() + i * kBeamChannels;
        CHECK(light[mm::FixtureChannels::kMotionBase + 2] == 0);
        CHECK(light[mm::FixtureChannels::kMotionBase + 3] == 0);
    }
}

// The frame number is tracked with goboOnBeat off too: turning it on with a hit still in the last frame rolls nothing, and the next frame's hit rolls once (review, 2026-11-06).
TEST_CASE("turning goboOnBeat on does not replay the hit of a frame already past") {
    mm::golden::ScopedTestClock clock(1000);
    mm::AudioService& audio = mm::seat();
    mm::AudioFrame& f = audio.frameForTest();
    f.gate = 40; f.gateOpen = true; f.onset = 200;
    f.seq++;

    mm::EffectOnGrid<mm::MovingHeadEffect> heads(3, 1);   // the roll is counted whatever channels the heads have
    heads.layer.tick();
    heads.e.goboOnBeat = true;
    mm::platform::setTestNowMs(1020);
    heads.layer.tick();
    CHECK(heads.e.goboRollsForTest() == 0);
    f.seq++;   // the next frame, a hit too
    mm::platform::setTestNowMs(1040);
    heads.layer.tick();
    CHECK(heads.e.goboRollsForTest() == 1);
    audio.release();
}
