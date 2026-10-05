/// @module Correction

#include "doctest.h"
#include "light/effects/MovingHeadEffect.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/util/FixtureChannels.h"
#include "platform/platform.h"   // setTestNowMs: the sweep needs the clock to move
#include "light/drivers/Correction.h"
#include "correction_presets.h"

#include <cstdint>
#include <set>
#include <vector>

// Pins the per-driver output correction: brightness LUT, channel reorder, and RGBW white derivation. The Drivers container owns a Correction, rebuilds it on a brightness/fixture change, and hands it to each physical driver, which calls apply() per light. These tests pin the transform so a regression in the LUT fill, the preset→role-offset mapping, or the white math fails here. A light is a span of outChannels bytes with a named offset per color role (offRed/offGreen/offBlue, and offWhite for the RGBW family; kAbsent = no white), so the checks read the observable apply() output plus outChannels, not an internal permutation table.

using mm::Correction;
// At brightness=255, the LUT maps every input value to itself (no scaling).
TEST_CASE("Correction brightness LUT: full brightness is identity") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    for (int v = 0; v < 256; v++) CHECK(c.briLut[v] == v);
}

// At brightness=128, every entry is roughly halved using scale8 (255→128, 128→64, 2→1).
TEST_CASE("Correction brightness LUT: half brightness halves each value (scale8)") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 128, mm::test::PresetOrder::RGB);
    CHECK(c.briLut[0] == 0);
    CHECK(c.briLut[255] == 128);   // (255*128)/255 = 128
    CHECK(c.briLut[128] == 64);    // (128*128)/255 = 64
    CHECK(c.briLut[2] == 1);       // (2*128)/255 = 1
}

// RGB preset at full brightness passes the source RGB through unchanged (3 output channels, no white).
TEST_CASE("Correction RGB preset: apply is identity at full brightness") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    CHECK(c.outChannels == 3);
    CHECK(c.offWhite == Correction::kAbsent);   // no white channel for the RGB family
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[3] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
}

// GRB preset swaps R and G in the output (G first, then R, then B), for WS2812-like drivers.
TEST_CASE("Correction GRB preset: channels reordered, 3 output channels") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::GRB);
    CHECK(c.outChannels == 3);
    CHECK(c.offWhite == Correction::kAbsent);   // no white channel for the RGB family
    const uint8_t src[3] = {10, 20, 30};  // R=10 G=20 B=30
    uint8_t out[3] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 20);  // G
    CHECK(out[1] == 10);  // R
    CHECK(out[2] == 30);  // B
}

// BGR preset reverses the channel order entirely (B, G, R).
TEST_CASE("Correction BGR preset: full reverse") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::BGR);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[3] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 30);  // B
    CHECK(out[1] == 20);  // G
    CHECK(out[2] == 10);  // R
}

// RGBW preset adds a fourth white channel derived as min(R, G, B) per pixel.
TEST_CASE("Correction RGBW preset: 4 channels, white = min(r,g,b)") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    CHECK(c.outChannels == 4);
    CHECK(c.offWhite == 3);   // white derived into the 4th channel
    const uint8_t src[3] = {10, 20, 30};  // min = 10
    uint8_t out[4] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 10);  // R
    CHECK(out[1] == 20);  // G
    CHECK(out[2] == 30);  // B
    CHECK(out[3] == 10);  // W = min(10,20,30)
}

// GRBW preset combines the GRB reorder with the W derivation (G, R, B, W=min).
TEST_CASE("Correction GRBW preset: reordered RGB + white") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::GRBW);
    CHECK(c.outChannels == 4);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 20);  // G
    CHECK(out[1] == 10);  // R
    CHECK(out[2] == 30);  // B
    CHECK(out[3] == 10);  // W = min
}

// Brightness scaling runs before white derivation so W = min of the *scaled* RGB values.
TEST_CASE("Correction: brightness applied BEFORE white derivation") {
    // White must be min of the *scaled* channels, not the raw ones.
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 128, mm::test::PresetOrder::RGBW);  // half brightness
    const uint8_t src[3] = {100, 200, 60};  // scaled: 50, 100, 30 → min = 30
    uint8_t out[4] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 50);   // (100*128)/255
    CHECK(out[1] == 100);  // (200*128)/255
    CHECK(out[2] == 30);   // (60*128)/255
    CHECK(out[3] == 30);   // min(50,100,30) — proves white uses scaled values
}

// rebuild() can switch the output channel count between RGB (3) and RGBW (4) on the fly.
TEST_CASE("Correction: rebuild switches output channel count RGB<->RGBW") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    CHECK(c.outChannels == 3);
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    CHECK(c.outChannels == 4);
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::GRB);
    CHECK(c.outChannels == 3);
    CHECK(c.offWhite == Correction::kAbsent);   // white dropped back to absent
}

using mm::WhiteMode;

// whiteMode = Min is the default and derives W = min(scaled R,G,B) leaving RGB intact, this is the byte-identical behavior the earlier RGBW tests already pin. whiteMode = None forces the white channel to 0 each frame (for effects that drive W themselves), written, not skipped, so a reused buffer can't keep a stale value (see the assertion below).
TEST_CASE("Correction whiteMode None: white channel forced to 0, RGB intact") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    c.whiteMode = WhiteMode::None;
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {0, 0, 0, 77};   // pre-fill W with a stale value from a prior frame
    c.apply(src, out, 3);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
    // None synthesizes no white, but MUST still write the channel to 0 each frame: the driver's corrected_ buffer is reused frame-to-frame, so leaving W unwritten would keep the stale 77 and stick the white LED on after a switch to None.
    CHECK(out[3] == 0);
}

// whiteMode = Accurate pulls the common white component OUT of RGB (so the white LED carries it) rather than adding it on top, R,G,B each drop by min(R,G,B).
TEST_CASE("Correction whiteMode Accurate: white subtracted from RGB") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    c.whiteMode = WhiteMode::Accurate;
    const uint8_t src[3] = {10, 20, 30};  // min = 10
    uint8_t out[4] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 0);    // R: 10 - 10
    CHECK(out[1] == 10);   // G: 20 - 10
    CHECK(out[2] == 20);   // B: 30 - 10
    CHECK(out[3] == 10);   // W: min(10,20,30)
}

using mm::ChannelRole;

// A Custom wiring is described by a channel-role array; rebuild() derives the color offsets from it. Here: white first, then B, G, R, a 4-channel arbitrary order that no curated preset names, proving the role array reaches any wiring.
TEST_CASE("Correction roles array: arbitrary Custom wiring derives correct offsets") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    const ChannelRole roles[4] = {ChannelRole::White, ChannelRole::Blue,
                                  ChannelRole::Green, ChannelRole::Red};
    c.rebuild(128, roles, 4);   // half brightness
    // Offsets derived from the array: each role's channel index.
    CHECK(c.offWhite == 0);
    CHECK(c.offBlue == 1);
    CHECK(c.offGreen == 2);
    CHECK(c.offRed == 3);
    CHECK(c.outChannels == 4);
    CHECK(c.briLut[255] == 128);   // LUT refreshed (brightness applied)
    const uint8_t src[3] = {200, 100, 60};  // scaled: 100, 50, 30 → min = 30
    uint8_t out[4] = {};
    c.apply(src, out, 3);
    CHECK(out[3] == 100);  // R at channel 3
    CHECK(out[2] == 50);   // G at channel 2
    CHECK(out[1] == 30);   // B at channel 1
    CHECK(out[0] == 30);   // W = min at channel 0
}

// A color role absent from the array stays kAbsent and apply() doesn't write it, a wiring can carry any SUBSET of color roles (e.g. a 2-channel R,B light with no green channel).
TEST_CASE("Correction roles array: absent color role is not emitted") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    const ChannelRole roles[2] = {ChannelRole::Red, ChannelRole::Blue};   // no green channel
    c.rebuild(255, roles, 2);
    CHECK(c.offRed == 0);
    CHECK(c.offBlue == 1);
    CHECK(c.offGreen == Correction::kAbsent);   // green has nowhere to go
    CHECK(c.outChannels == 2);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[2] = {0, 0};
    c.apply(src, out, 3);
    CHECK(out[0] == 10);   // R
    CHECK(out[1] == 30);   // B — green (20) simply not written
}

// A non-color role (Pan) occupies a channel but apply()'s RGB path ignores it, the channel is left for the fixture role writer, and outChannels still counts it.
TEST_CASE("Correction roles array: non-color role reserves a channel apply() skips") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    const ChannelRole roles[4] = {ChannelRole::Pan, ChannelRole::Red,
                                  ChannelRole::Green, ChannelRole::Blue};
    c.rebuild(255, roles, 4);
    CHECK(c.outChannels == 4);
    CHECK(c.offRed == 1);
    CHECK(c.offGreen == 2);
    CHECK(c.offBlue == 3);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {77, 0, 0, 0};   // channel 0 (Pan) pre-set; apply() must leave it
    c.apply(src, out, 3);
    CHECK(out[0] == 77);   // Pan channel untouched by the RGB path
    CHECK(out[1] == 10);   // R
    CHECK(out[2] == 20);   // G
    CHECK(out[3] == 30);   // B
}

// WarmWhite / Yellow / UV are synthesized from RGB off the SAME whiteMode as White, so a fixture carrying them lights up (best-effort approximations, not a color model yet): WW ≈ min(RGB), Yellow ≈ min(R,G), UV ≈ the blue-excess max(0, B-max(R,G)). This is the "all channels burn so you can eyeball a fixture" behavior the finding asked for; a real per-emitter model comes later.
TEST_CASE("Correction: WarmWhite/Yellow/UV synthesised from RGB via whiteMode") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    const ChannelRole roles[6] = {ChannelRole::Red, ChannelRole::Green, ChannelRole::Blue,
                                  ChannelRole::WarmWhite, ChannelRole::Yellow, ChannelRole::UV};
    c.rebuild(255, roles, 6);
    CHECK(c.offWarmWhite == 3);
    CHECK(c.offYellow == 4);
    CHECK(c.offUV == 5);
    const uint8_t src[3] = {40, 100, 200};   // R=40 G=100 B=200
    uint8_t out[6] = {};
    c.apply(src, out, 3);
    CHECK(out[0] == 40);   // R
    CHECK(out[1] == 100);  // G
    CHECK(out[2] == 200);  // B
    CHECK(out[3] == 40);   // WW = min(40,100,200)
    CHECK(out[4] == 40);   // Yellow = min(R,G) = min(40,100)
    CHECK(out[5] == 100);  // UV = B - max(R,G) = 200 - 100 (fires on the blue excess)
}

// Under Accurate, White pulls its component OUT of RGB, but the additive stand-ins (WW/Yellow/UV) must approximate from the RGB the effect produced, BEFORE that subtraction, or they collapse. This pins the compute-stand-ins-before-White ordering (a regression would compute them post-subtraction).
TEST_CASE("Correction Accurate: Yellow/UV use pre-subtraction RGB, not post-White") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    // Fixture carries White AND Yellow AND UV, so all three synthesis paths run in one apply().
    const ChannelRole roles[6] = {ChannelRole::Red, ChannelRole::Green, ChannelRole::Blue,
                                  ChannelRole::White, ChannelRole::Yellow, ChannelRole::UV};
    c.rebuild(255, roles, 6);
    c.whiteMode = WhiteMode::Accurate;
    const uint8_t src[3] = {40, 100, 200};   // R=40 G=100 B=200, so w = min = 40
    uint8_t out[6] = {};
    c.apply(src, out, 3);
    // White = min(R,G,B) = 40, subtracted from RGB → R=0, G=60, B=160.
    CHECK(out[3] == 40);            // White
    CHECK(out[0] == 0);            // R after subtraction
    CHECK(out[1] == 60);           // G after subtraction
    CHECK(out[2] == 160);          // B after subtraction
    // Yellow is computed from the PRE-subtraction RGB. Yellow = min(R,G) = min(40,100) = 40. This is the case that catches the bug: computed AFTER the subtraction it would be min(0,60) = 0, the stand-in would collapse to near-zero exactly when White is active. (UV = B-max(R,G) happens to be subtraction-invariant since w cancels, so it stays 100 either way, Yellow is the witness.)
    CHECK(out[4] == 40);           // Yellow, from pre-subtraction RGB (post would be 0 — the regression)
    CHECK(out[5] == 100);          // UV = 200 - max(40,100) = 100
}

// UV stays dark on a warm color (no blue excess), and every synthesized emitter is forced to 0 under whiteMode=None so none holds a stale value, the same reuse-safety the White channel has.
TEST_CASE("Correction: UV dark on warm colors; whiteMode None zeroes WW/Y/UV") {
    Correction c;
    c.curve = Correction::Curve::Linear;   // these pin roles and white math, not the curve
    const ChannelRole roles[6] = {ChannelRole::Red, ChannelRole::Green, ChannelRole::Blue,
                                  ChannelRole::WarmWhite, ChannelRole::Yellow, ChannelRole::UV};
    c.rebuild(255, roles, 6);
    {   // warm color: R,G high, B low → UV = max(0, B-max(R,G)) = 0
        const uint8_t src[3] = {200, 180, 20};
        uint8_t out[6] = {};
        c.apply(src, out, 3);
        CHECK(out[5] == 0);            // UV dark: no blue excess
        CHECK(out[4] == 180);          // Yellow = min(200,180)
    }
    {   // whiteMode None: every synthesized emitter forced to 0, even with stale pre-fill
        c.whiteMode = WhiteMode::None;
        const uint8_t src[3] = {40, 100, 200};
        uint8_t out[6] = {0, 0, 0, 55, 66, 77};   // stale WW/Y/UV
        c.apply(src, out, 3);
        CHECK(out[3] == 0);            // WW zeroed
        CHECK(out[4] == 0);            // Yellow zeroed
        CHECK(out[5] == 0);            // UV zeroed
    }
}

// A fixture with a master dimmer channel must actually be LIT. The dimmer is a real output, not a motion role: a moving head whose preset maps Pan/Tilt/Dimmer/RGBW stayed completely dark on the bench with a perfectly correct color map, because nothing ever wrote its dimmer and a linear dimmer at 0 emits nothing. The pre-existing "IRGB" preset had the same defect.
TEST_CASE("A preset's master dimmer channel is driven, so the fixture actually lights") {
    using R = mm::ChannelRole;
    const R roles[] = {R::Pan, R::None, R::Tilt, R::None, R::None, R::Dimmer,
                       R::None, R::Red, R::Green, R::Blue, R::White};
    mm::Correction c;
    c.curve = mm::Correction::Curve::Linear;   // the dimmer channel is the subject, not the curve
    c.rebuild(255, roles, 11);

    const uint8_t src[3] = {200, 100, 50};
    uint8_t out[11] = {};
    c.apply(src, out, 3);

    CHECK(out[5] == 255);      // CH6 dimmer: open, or the fixture is dark whatever the colors say
    CHECK(out[7] == 200);      // CH8 red still lands on its own channel
    CHECK(out[8] == 100);      // CH9 green
    CHECK(out[9] == 50);       // CH10 blue
}

// Motion roles are somebody else's to write: apply() must not touch them, or a future pan/tilt writer would fight the color path every frame.
TEST_CASE("Pan and tilt channels are left alone by the color path") {
    using R = mm::ChannelRole;
    const R roles[] = {R::Pan, R::Tilt, R::Dimmer, R::Red, R::Green, R::Blue};
    mm::Correction c;
    c.rebuild(255, roles, 6);

    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[6] = {77, 88, 0, 0, 0, 0};   // pan/tilt pre-set by their own writer
    c.apply(src, out, 3);

    CHECK(out[0] == 77);       // pan untouched
    CHECK(out[1] == 88);       // tilt untouched
    CHECK(out[2] == 255);      // dimmer open
}

// Motion channels ride the same buffer as color: a wide light carries pan/tilt at the offsets the preset gives them, and apply() hands those bytes to the fixture untouched.
TEST_CASE("Motion channels pass through to the fixture, unscaled by brightness") {
    using R = mm::ChannelRole;
    const R roles[] = {R::Pan, R::None, R::Tilt, R::None, R::None, R::Dimmer,
                       R::None, R::Red, R::Green, R::Blue, R::White};
    mm::Correction c;
    c.rebuild(64, roles, 11);          // a QUARTER brightness, to catch any scaling of motion

    // Color lives at the START of a light, whatever the fixture's own channel order; motion sits at the offset the preset gives it, which is where the effect wrote it.
    uint8_t rgb[11] = {};
    rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;
    // Motion goes in the LAYER's slots (packed after RGBW), not at the fixture's channel numbers: the fixture's pan is CH1, which in a layer light is the red byte.
    rgb[mm::FixtureChannels::kMotionBase + 0] = 200;   // pan
    rgb[mm::FixtureChannels::kMotionBase + 1] = 30;    // tilt

    uint8_t out[11] = {};
    c.apply(rgb, out, 11);

    CHECK(out[c.offPan] == 200);       // aim survives dimming: brightness must not steer the head
    CHECK(out[c.offTilt] == 30);
    CHECK(out[c.offRed] < 255);        // color IS dimmed, unlike aim
    CHECK(out[c.offRed] > 0);
}

// A fixture with no motion channels must not pay for motion support: the flag is what keeps the hot path a single branch instead of a five-slot scan for every light of every frame.
TEST_CASE("A plain RGB fixture reports no motion channels") {
    using R = mm::ChannelRole;
    const R rgb[] = {R::Red, R::Green, R::Blue};
    mm::Correction c;
    c.rebuild(255, rgb, 3);
    CHECK_FALSE(c.hasMotion);

    const R head[] = {R::Pan, R::Tilt, R::Red, R::Green, R::Blue};
    c.rebuild(255, head, 5);
    CHECK(c.hasMotion);
}

// The layer must be wide enough to hold the motion slots BEFORE it allocates, or an effect's setPan() falls outside the light and the fixture never moves. Modules prepare in registration order with Effects ahead of Drivers, so this only works because Drivers publishes the fixture layout in setup(), which runs for every module before any module's prepare(). Bench-observed before the fix: 12 bytes for 4 lights on a cold boot, and a motionless head until a rebuild.
TEST_CASE("A motion preset widens the light enough to carry pan and tilt") {
    using R = mm::ChannelRole;
    const R roles[] = {R::Pan, R::None, R::Tilt, R::None, R::None, R::Dimmer,
                       R::None, R::Red, R::Green, R::Blue, R::White};
    mm::Correction c;
    c.rebuild(255, roles, 11);
    REQUIRE(c.hasMotion);

    // Two motion roles, so the layer needs kMotionBase + 2 channels per light.
    mm::FixtureChannels fc;
    const bool present[5] = {c.offPan != mm::Correction::kAbsent,
                             c.offTilt != mm::Correction::kAbsent, false, false, false};
    uint8_t* const dst[5] = {&fc.pan, &fc.tilt, &fc.zoom, &fc.rotate, &fc.gobo};
    mm::FixtureChannels::forEachMotionSlot(present,
        [&](uint8_t role, uint8_t slot) { *dst[role] = slot; });

    CHECK(fc.pan == mm::FixtureChannels::kMotionBase);
    CHECK(fc.tilt == mm::FixtureChannels::kMotionBase + 1);
    CHECK(fc.movable());
}

// The formations are the point of the moving-head effect: the same sweep, different relationships between the heads. A formation that produced identical aim for every head would be `unison` wearing another name, so each one is pinned by what makes it distinguishable on a real rig.
TEST_CASE("Each moving-head formation aims the rig differently") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    // A 1D layout is width 1 by height N: a rig of 4 heads is 1x4, not 4x1. Extrude duplicates the x=0 column, so the other way round would copy head 0 over every head.
    grid.width = 1; grid.height = 4; grid.depth = 1;   // a 4-head chain
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(6);                      // RGBW + pan + tilt
    mm::FixtureChannels fc;
    fc.pan = mm::FixtureChannels::kMotionBase;
    fc.tilt = mm::FixtureChannels::kMotionBase + 1;
    layer.setFixtureChannels(fc);

    mm::MovingHeadEffect fx;
    layer.addChild(&fx);

    auto aimOf = [&](uint8_t formation) {
        fx.formation = formation;
        layer.applyState();
        // BeatPhase's first advance only establishes the time base, and it accumulates from elapsed() rather than frame count, so the clock has to MOVE or every head sits at sin(0) = center and the formations are indistinguishable.
        mm::platform::setTestNowMs(1);
        layer.tick();
        for (int f = 1; f <= 12; f++) { mm::platform::setTestNowMs(1 + f * 400u); layer.tick(); }
        std::vector<uint8_t> pans;
        const auto& b = layer.buffer();
        for (mm::nrOfLightsType i = 0; i < b.count(); i++)
            pans.push_back(b.data()[i * b.channelsPerLight() + fc.pan]);
        return pans;
    };

    // Unison is the reference: every head on the same aim.
    const auto unison = aimOf(mm::MovingHeadEffect::kUnison);
    REQUIRE(unison.size() == 4);
    for (size_t i = 1; i < unison.size(); i++) CHECK(unison[i] == unison[0]);

    // Chase delays each head along the sweep, so neighbors differ.
    const auto chase = aimOf(mm::MovingHeadEffect::kChase);
    bool chaseVaries = false;
    for (size_t i = 1; i < chase.size(); i++) if (chase[i] != chase[0]) chaseVaries = true;
    CHECK(chaseVaries);

    // Cross opposes alternate heads: 0 and 1 sit on opposite sides of center.
    const auto cross = aimOf(mm::MovingHeadEffect::kCross);
    CHECK(cross[0] != cross[1]);

    // Mirror splits the rig in half, so the first and last head oppose.
    const auto mirror = aimOf(mm::MovingHeadEffect::kMirror);
    CHECK(mirror[0] != mirror[3]);

    mm::platform::setTestNowMs(0);   // back to the real clock for every later test
}

// --- The perceptual curve -------------------------------------------------------------------

// CIE 1931 lightness (CIE 15 / ISO 11664-4) is the default because it models the thing actually being corrected: the eye's response to luminance. The endpoints are what a user notices first, and the shape between them is what makes a fade look even.
TEST_CASE("The CIE curve keeps the endpoints and bends the middle down") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    c.curve = Correction::Curve::Cie;
    c.rebuildBrightness(255);

    CHECK(c.briLut[0] == 0);       // black stays black
    CHECK(c.briLut[255] == 255);   // and full stays full: the curve redistributes, it does not dim
    // A mid control position is well under half output, which is the whole point: half the PERCEIVED brightness is a small fraction of the luminance.
    CHECK(c.briLut[128] < 80);
    CHECK(c.briLut[128] > 30);
    // Monotonic, or a gradient would band or reverse.
    for (int v = 1; v < 256; v++) CHECK(c.briLut[v] >= c.briLut[v - 1]);
}

// The guard that stops a fade-out snapping to black. Every curve here crushes the low end at 8 bits, so without it the dimmest usable values are simply missing.
TEST_CASE("A non-zero value never curves down to black") {
    for (auto curve : {Correction::Curve::Cie, Correction::Curve::Gamma22, Correction::Curve::Gamma28}) {
        Correction c;
        mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
        c.curve = curve;
        c.rebuildBrightness(255);
        for (int v = 1; v < 256; v++) CHECK(c.briLut[v] >= 1);
    }
}

// Linear is not a fallback: a downstream device that corrects its own output needs the value on the wire to mean duty cycle, or the picture is corrected twice.
TEST_CASE("Linear is an exact identity at full brightness, for a device that corrects itself") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    c.curve = Correction::Curve::Linear;
    c.rebuildBrightness(255);
    for (int v = 0; v < 256; v++) CHECK(c.briLut[v] == v);
}

// Brightness is a LINEAR pre-scale and the curve is applied last. Turning the slider down must not change the shape of the curve, only how far up it reaches.
TEST_CASE("Brightness scales before the curve, so black stays black and the top tracks the slider") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    c.curve = Correction::Curve::Cie;

    c.rebuildBrightness(255);
    const uint8_t fullTop = c.briLut[255];
    c.rebuildBrightness(128);
    const uint8_t halfTop = c.briLut[255];

    CHECK(c.briLut[0] == 0);          // zero is zero at any brightness
    CHECK(halfTop < fullTop);         // the slider still reaches the output
    // Half the SLIDER is well under half the light, because the curve applies after the scale.
    CHECK(halfTop < fullTop / 2);
}

// The white emitters are derived in LINEAR light and curved on the way out, so min() and the Accurate subtraction mean what they say. A curve applied before the subtraction would remove an amount that does not correspond to the light the white LED adds back.
TEST_CASE("RGBW white is derived in linear light, then curved once") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    c.curve = Correction::Curve::Cie;
    c.rebuildBrightness(255);
    c.whiteMode = mm::WhiteMode::Accurate;

    const uint8_t src[3] = {200, 150, 100};
    uint8_t out[4] = {};
    c.apply(src, out, 3);

    // white = min(200,150,100) = 100 in LINEAR light, then curved.
    Correction ref;
    mm::test::rebuildFromPreset(ref, 255, mm::test::PresetOrder::RGB);
    ref.curve = Correction::Curve::Cie;
    ref.rebuildBrightness(255);
    CHECK(out[3] == ref.briLut[100]);
    // and RGB carry the remainder (200-100, 150-100, 0), each curved.
    CHECK(out[0] == ref.briLut[100]);
    CHECK(out[1] == ref.briLut[50]);
    CHECK(out[2] == ref.briLut[0]);
}

namespace {
using R = mm::ChannelRole;
// A 16-bit strip's wiring: each color as its high byte, then its low byte.
constexpr R kRGB16[6] = {R::Red, R::RedFine, R::Green, R::GreenFine, R::Blue, R::BlueFine};
constexpr R kRGBW16[8] = {R::Red, R::RedFine, R::Green, R::GreenFine, R::Blue, R::BlueFine, R::White, R::WhiteFine};
}  // namespace

// The curve runs to 16 bits and goes out high byte first, so full scale and black stay exact.
TEST_CASE("A 16-bit strip writes each color as its high byte, then its low byte") {
    Correction c;
    std::vector<uint16_t> table(256);
    c.lut16 = table.data();
    c.curve = Correction::Curve::Linear;
    c.rebuild(255, kRGB16, 6);
    CHECK(c.hasFine);
    CHECK(c.outChannels == 6);
    const uint8_t src[3] = {255, 0, 128};
    uint8_t out[6] = {};
    c.apply(src, out, 3);
    CHECK((out[0] == 0xFF && out[1] == 0xFF));   // 255 is 65535
    CHECK((out[2] == 0 && out[3] == 0));
    CHECK(out[4] * 256 + out[5] == 128 * 257);   // linear: 8 bits widened by replication
}

// The reason for 16 bits: at low brightness the curve merges most inputs in 8 bits, and 16 bits keeps each one distinct.
TEST_CASE("At low brightness a 16-bit channel keeps every level the 8-bit table merges") {
    Correction c;
    std::vector<uint16_t> table(256);
    c.lut16 = table.data();
    c.rebuild(16, kRGB16, 6);   // the default CIE curve
    std::set<int> narrow, wide;
    for (int v = 0; v < 256; v++) {
        const uint8_t src[3] = {static_cast<uint8_t>(v), 0, 0};
        uint8_t out[6] = {};
        c.apply(src, out, 3);
        narrow.insert(c.briLut[v]);
        wide.insert(out[0] * 256 + out[1]);
    }
    CHECK(narrow.size() <= 4);
    CHECK(wide.size() == 256);
}

// White is derived in linear light as on an 8-bit strip, then written to 16 bits like the colors.
TEST_CASE("A 16-bit RGBW strip derives its white and writes it to 16 bits") {
    Correction c;
    std::vector<uint16_t> table(256);
    c.lut16 = table.data();
    c.curve = Correction::Curve::Linear;
    c.whiteMode = mm::WhiteMode::Accurate;
    c.rebuild(255, kRGBW16, 8);
    const uint8_t src[3] = {200, 100, 50};
    uint8_t out[8] = {};
    c.apply(src, out, 3);
    CHECK(out[6] * 256 + out[7] == 50 * 257);    // white = min(r, g, b)
    CHECK(out[0] * 256 + out[1] == 150 * 257);   // red with the white pulled out
}

// An 8-bit profile never enters the 16-bit path, and a 16-bit one whose table is missing degrades to 8 bits rather than writing stale bytes.
TEST_CASE("An 8-bit profile has no fine roles, and a missing table degrades to 8 bits") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    CHECK_FALSE(c.hasFine);
    Correction wide;
    wide.curve = Correction::Curve::Linear;
    wide.rebuild(255, kRGB16, 6);   // no table given
    const uint8_t src[3] = {9, 9, 9};
    uint8_t out[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    wide.apply(src, out, 3);
    CHECK((out[0] == 9 && out[1] == 0));
}

// A moving head with 16-bit aim gets its 8-bit pan and tilt widened by repeating the byte, so the full sweep stays full.
TEST_CASE("Pan fine and tilt fine repeat the aim's byte, so a full aim reaches the full range") {
    const R roles[] = {R::Pan, R::PanFine, R::Tilt, R::TiltFine, R::Red, R::Green, R::Blue};
    Correction c;
    c.rebuild(64, roles, 7);   // brightness must not steer the head, coarse or fine
    uint8_t src[12] = {};
    src[mm::FixtureChannels::kMotionBase + 0] = 255;   // pan
    src[mm::FixtureChannels::kMotionBase + 1] = 30;    // tilt
    uint8_t out[7] = {};
    c.apply(src, out, 12);
    CHECK((out[0] == 255 && out[1] == 255));   // 65535, the end of the sweep
    CHECK((out[2] == 30 && out[3] == 30));     // 30 * 257
    CHECK_FALSE(c.hasFine);                    // aim needs no color table
}

// A 16-bit master dimmer is driven wide open on both bytes, as its 8-bit coarse byte already is.
TEST_CASE("Dimmer fine is driven wide open with its coarse byte") {
    const R roles[] = {R::Dimmer, R::DimmerFine, R::Red, R::Green, R::Blue};
    Correction c;
    c.rebuild(64, roles, 5);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[5] = {};
    c.apply(src, out, 3);
    CHECK((out[0] == 255 && out[1] == 255));
}
