/// @module RotateModifier

#include "doctest.h"
#include "light/modifiers/RotateModifier.h"
#include "platform/platform_clock.h"   // setTestNowMs

#include <cstring>

// RotateModifier is the one dynamic modifier: a per-frame backward map, the identity at angle 0, which these pin with the in-box invariants and the beat sync.

// Apply the live remap to (x,y) in a w×h box; returns the source coord it samples.
static mm::Coord3D live(mm::RotateModifier& m, mm::lengthType x, mm::lengthType y,
                        mm::lengthType w, mm::lengthType h) {
    mm::Coord3D p{x, y, 0};
    m.modifyLive(p, {w, h, 1});
    return p;
}

TEST_CASE("RotateModifier advertises a live (per-frame) modifier") {
    mm::RotateModifier m;
    CHECK(m.hasModifyLive());
    CHECK(m.dimensions() == mm::Dim::D2);
}

// At the initial angle (0) the rotation matrix is the identity, every cell samples itself.
TEST_CASE("RotateModifier at angle 0 is identity") {
    mm::RotateModifier m;
    const mm::lengthType w = 8, h = 8;
    for (mm::lengthType y = 0; y < h; y++)
        for (mm::lengthType x = 0; x < w; x++)
            CHECK(live(m, x, y, w, h) == mm::Coord3D{x, y, 0});
}

// z passes through (2D rotation), a 3D coord's z is untouched.
TEST_CASE("RotateModifier leaves z untouched") {
    mm::RotateModifier m;
    mm::Coord3D p{3, 4, 2};
    m.modifyLive(p, {8, 8, 4});
    CHECK(p.z == 2);
}

// An empty box doesn't divide-by-zero or wrap: the remap is a no-op-ish transform that the Layer's live pass then treats as out-of-box (dark), never a crash.
TEST_CASE("RotateModifier tolerates an empty box") {
    mm::RotateModifier m;
    mm::Coord3D p{0, 0, 0};
    m.modifyLive(p, {0, 0, 0});   // must not crash
    CHECK(true);
}

// On the beat the turn sits on its step: 8 beats a turn lands on 0, 45, 90 ... 315 degrees, 8192 angle units a step.
TEST_CASE("RotateModifier on beat lands each beat on its step, and the ninth back at 0") {
    for (uint16_t c = 0; c < 9; c++) CHECK(mm::RotateModifier::beatAngle(c, 0, 8, 128) == static_cast<uint16_t>((c % 8) * 8192u));
    CHECK(mm::RotateModifier::beatAngle(3, 0, 4, 0) == 49152);   // 4 beats a turn: 90 degrees a beat
}

// Ease 0 turns at one speed through the beat; a full ease starts slow, passes the middle at the same place and ends slow.
TEST_CASE("RotateModifier eases each step into the beat") {
    const auto at = [](uint8_t phase, uint8_t ease) { return mm::RotateModifier::beatAngle(0, phase, 8, ease); };
    CHECK(at(128, 0) == 4096);                  // half a step, even speed
    CHECK(at(64, 255) < at(64, 0));             // slower out of the beat
    CHECK(at(192, 255) > at(192, 0));           // and slower into the next
    CHECK(at(128, 255) > 4000);
    CHECK(at(128, 255) < 4200);                 // the middle where the even turn has it
    uint16_t last = 0;
    for (int ph = 0; ph < 256; ph += 8) { const uint16_t a = at(static_cast<uint8_t>(ph), 255); CHECK(a >= last); last = a; }   // never turns back
}

// The affine map at angle 0 is the identity, every light sampling itself.
TEST_CASE("RotateModifier's live affine map is the identity at angle 0") {
    mm::RotateModifier m;
    mm::LiveAffine t;
    REQUIRE(m.liveAffine(t, {16, 9, 1}));
    for (mm::lengthType y = 0; y < 9; y++)
        for (mm::lengthType x = 0; x < 16; x++) {
            mm::lengthType sx = x, sy = y;
            t.apply(sx, sy);
            CHECK(sx == x);
            CHECK(sy == y);
        }
}

// On beat with no beat running the image holds still, as a sweep goes dark when the music stops.
TEST_CASE("RotateModifier on beat holds still while no beat runs") {
    mm::RotateModifier m;
    m.sync = mm::RotateModifier::kBeat;
    m.speed = 255;
    for (uint32_t t = 1000; t < 3000; t += 20) { mm::platform::setTestNowMs(t); m.tick(); }
    CHECK(m.angleForTest() == 0);
    mm::platform::setTestNowMs(0);
}

// The card shows the speed on free and the beats and the ease on beat.
TEST_CASE("RotateModifier shows each sync's own controls") {
    mm::RotateModifier m;
    const auto hidden = [&](const char* name) {
        m.rebuildControls();
        for (uint8_t i = 0; i < m.controls().count(); i++)
            if (std::strcmp(m.controls()[i].name, name) == 0) return static_cast<bool>(m.controls()[i].hidden);
        return true;
    };
    CHECK_FALSE(hidden("speed"));
    CHECK(hidden("beats"));
    m.sync = mm::RotateModifier::kBeat;
    CHECK(hidden("speed"));
    CHECK_FALSE(hidden("beats"));
    CHECK_FALSE(hidden("ease"));
}
