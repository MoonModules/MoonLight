/// @module SwirlEffect

#include "doctest.h"
#include "audio_effect_test.h"   // seat(), the audio service the mod-slot tests play through
#include "golden_frame.h"
#include "light/effects/SwirlEffect.h"
#include "light/powerfunctions/modmatrix.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "platform/platform.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using mm::SwirlEffect;
using Src = mm::modmatrix::Source;

// Render `frames` ticks of `e` 20 ms apart on a `w` by `h` grid, and return the last frame's bytes.
std::vector<uint8_t> render(SwirlEffect& e, uint16_t w = 32, uint16_t h = 32, uint16_t frames = 60) {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(w, h);
    fx.add(e);
    for (uint16_t f = 0; f <= frames; f++) fx.tickAt(1000 + f * 20);
    return fx.frame();
}

using mm::golden::litLights;

// How far a particle sits from the center of the engine space.
float fromCenter(const mm::swirl::Particle& p) {
    const float dx = p.x - mm::swirl::kCenter, dy = p.y - mm::swirl::kCenter;
    return std::sqrt(dx * dx + dy * dy);
}

// Load preset `p` the way the card does: the select moves, and its change runs.
void load(SwirlEffect& e, uint8_t p) {
    e.preset = p;
    e.onControlChanged("preset");
}

// The control named `name`, which every test here expects to exist.
const mm::ControlDescriptor& control(SwirlEffect& e, const char* name) {
    e.rebuildControls();
    const auto& list = e.controls();
    for (uint8_t i = 0; i < list.count(); i++)
        if (std::strcmp(list[i].name, name) == 0) return list[i];
    FAIL("no control " << name);
    return list[0];
}

// The effect on a grid with the audio service seated, so the mod-slot tests can play frames through it.
struct ModRig {
    mm::golden::ScopedTestClock clock{1000};   // the fixed clock, first so the prepare reads test time
    mm::golden::LayerFixture fx;
    SwirlEffect& effect;
    mm::AudioService& audio;
    explicit ModRig(SwirlEffect& e, uint16_t w = 32, uint16_t h = 32) : fx(w, h), effect(e), audio(mm::seat()) { fx.add(effect); }
    // Leave silence behind and vacate the mic seat, so the next test starts from a quiet frame and an empty seat.
    ~ModRig() {
        audio.frameForTest() = mm::AudioFrame{};
        audio.release();
    }
    // Play frame `f` at `ms`, then tick once.
    void playAt(const mm::AudioFrame& f, uint32_t ms) {
        audio.frameForTest() = f;
        fx.tickAt(ms);
    }
};

// Arm slot `s` with source `src` on knob `p` at `depth`.
void route(SwirlEffect& e, uint8_t s, Src src, SwirlEffect::Param p, uint8_t depth) {
    e.modSource[s] = static_cast<uint8_t>(src);
    e.modDest[s] = static_cast<uint8_t>(p + 1);
    e.modDepth[s] = depth;
}

}  // namespace

// ---- the presets -------------------------------------------------------------------------------

TEST_CASE("SwirlEffect starts as the Halvorsen preset: a cloud on the attractor") {
    SwirlEffect e;
    CHECK(e.preset == SwirlEffect::kHalvorsen);
    CHECK(e.emitter == static_cast<uint8_t>(mm::swirl::Emitter::kCloud));
    CHECK(e.field == static_cast<uint8_t>(mm::swirl::Field::kAttractor));
    CHECK(e.param[SwirlEffect::kShape] == 189);
}

TEST_CASE("SwirlEffect draws every preset") {
    for (uint8_t p = 0; p < SwirlEffect::kPresetCount; p++) {
        CAPTURE(p);
        SwirlEffect e;
        load(e, p);
        CHECK(litLights(render(e)) > 0);
        SwirlEffect wide;
        load(wide, p);
        CHECK(litLights(render(wide, 64, 16)) > 0);
    }
}

// The prepare settles the swarm onto the attractor: spread well past the seed cloud, inside the attractor's reach.
TEST_CASE("SwirlEffect Halvorsen settles onto the attractor before the first frame") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    fx.add(e);
    CHECK(e.liveForTest() == 120);
    float spread = 0;
    for (uint16_t i = 0; i < e.liveForTest(); i++) spread = std::max(spread, fromCenter(*e.particleForTest(i)));
    CHECK(spread > 3.0f);
    CHECK(spread < 15.5f);   // the attractor's diagonal reach at the default zoom, measured: inside the grid
}

// The jet leaves the origin and the frame turns it, about a circle in fifteen seconds.
TEST_CASE("SwirlEffect NozzleJet sprays from the origin and turns") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kNozzleJet);
    fx.add(e);
    REQUIRE(e.liveForTest() > 0);
    float farthest = 0;
    for (uint16_t i = 0; i < e.liveForTest(); i++) farthest = std::max(farthest, fromCenter(*e.particleForTest(i)));
    CHECK(farthest > 4.0f);
    const float before = e.motionForTest().thetaForTest();
    for (uint16_t f = 1; f <= 30; f++) fx.tickAt(1000 + f * 20);
    CHECK(e.motionForTest().thetaForTest() > before);
}

// Rings are blown whole, one every period: held still, five rings stand after the prepare's hundred and fifty steps.
TEST_CASE("SwirlEffect VortexRings blows whole rings") {
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kVortexRings);
    e.param[SwirlEffect::kExpand] = 128;   // no expansion, so no ring reaches the border
    fx.add(e);
    CHECK(e.liveForTest() == 5 * e.param[SwirlEffect::kRingSize]);
}

// The rings grow out from the origin.
TEST_CASE("SwirlEffect VortexRings grow out from the origin") {
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kVortexRings);
    fx.add(e);
    REQUIRE(e.liveForTest() > 0);
    CHECK(e.liveForTest() <= e.param[SwirlEffect::kCount]);
    float farthest = 0;
    for (uint16_t i = 0; i < e.liveForTest(); i++) farthest = std::max(farthest, fromCenter(*e.particleForTest(i)));
    CHECK(farthest > 5.0f);
}

// The scatter is born everywhere at once, so the flow is full on the first frame, less the few the step carried past the border.
TEST_CASE("SwirlEffect Advection fills the flow at once") {
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kAdvection);
    fx.add(e);
    CHECK(e.liveForTest() > 150);
    CHECK(e.liveForTest() <= 160);
}

// Born on the border, pulled in and around the origin: the swarm draws inside the border, around a dark eye. On 32x32 the mean radius is 16.6 at the first frame and 12.2 to 14.0 from the 300th, the eye about 6.8.
TEST_CASE("SwirlEffect Whirlpool draws the border in around a dark eye") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kWhirlpool);
    CHECK(e.emitter == static_cast<uint8_t>(mm::swirl::Emitter::kEdge));
    CHECK(e.field == static_cast<uint8_t>(mm::swirl::Field::kOrbit));
    fx.add(e);
    // The live particles' mean and least distance from the origin.
    auto radii = [&e](float& mean, float& nearest) {
        REQUIRE(e.liveForTest() > 0);
        float sum = 0;
        nearest = 2 * mm::swirl::kCenter;
        for (uint16_t i = 0; i < e.liveForTest(); i++) {
            const float r = fromCenter(*e.particleForTest(i));
            sum += r;
            nearest = std::min(nearest, r);
        }
        mean = sum / e.liveForTest();
    };
    float firstMean = 0, firstNearest = 0;
    fx.tickAt(1000);
    radii(firstMean, firstNearest);
    float mean = 0, nearest = 0, eye = firstNearest;
    for (uint16_t f = 1; f <= 480; f++) {
        fx.tickAt(1000 + f * 20);
        radii(mean, nearest);
        eye = std::min(eye, nearest);
    }
    CHECK(mean < firstMean);
    CHECK(mean < mm::swirl::kCenter);
    CHECK(eye < firstNearest);
    CHECK(eye < mm::swirl::kCenter / 2);
    CHECK(eye > mm::swirl::kCenter / 4);
}

// A preset writes its knobs; a rebuild, which a restore runs, leaves the knobs as they were set.
TEST_CASE("SwirlEffect loads a preset on its change, never on a rebuild") {
    SwirlEffect e;
    load(e, SwirlEffect::kNozzleJet);
    CHECK(e.emitter == static_cast<uint8_t>(mm::swirl::Emitter::kNozzle));
    CHECK(e.param[SwirlEffect::kTurbulence] == 64);
    e.param[SwirlEffect::kSpread] = 200;
    e.rebuildControls();
    CHECK(e.param[SwirlEffect::kSpread] == 200);
}

// A stored preset outside the list loads the first.
TEST_CASE("SwirlEffect loads Halvorsen for an unknown preset") {
    SwirlEffect e;
    load(e, SwirlEffect::kAdvection);
    load(e, 99);
    CHECK(e.preset == SwirlEffect::kHalvorsen);
    CHECK(e.field == static_cast<uint8_t>(mm::swirl::Field::kAttractor));
}

// ---- the controls ------------------------------------------------------------------------------

// A part's knobs show only while it is chosen; the shared ones always.
TEST_CASE("SwirlEffect shows each part's knobs only while it is chosen") {
    SwirlEffect e;
    CHECK(control(e, "spread").hidden);
    CHECK_FALSE(control(e, "shape").hidden);
    CHECK_FALSE(control(e, "scale").hidden);
    CHECK_FALSE(control(e, "wander").hidden);
    load(e, SwirlEffect::kNozzleJet);
    CHECK_FALSE(control(e, "spread").hidden);
    CHECK(control(e, "shape").hidden);
    CHECK_FALSE(control(e, "drag").hidden);
    CHECK(control(e, "pull").hidden);
    e.field = static_cast<uint8_t>(mm::swirl::Field::kOrbit);
    CHECK_FALSE(control(e, "pull").hidden);
    CHECK_FALSE(control(e, "swirl").hidden);
}

// A slot's destination and depth show once it has a source, and an LFO's rate once a slot listens to it.
TEST_CASE("SwirlEffect shows a slot's routing once it has a source") {
    SwirlEffect e;
    CHECK(control(e, "mod1Dest").hidden);
    CHECK(control(e, "mod1Depth").hidden);
    CHECK(control(e, "lfo1Rate").hidden);
    e.modSource[0] = static_cast<uint8_t>(Src::kLfo1);
    CHECK_FALSE(control(e, "mod1Dest").hidden);
    CHECK_FALSE(control(e, "mod1Depth").hidden);
    CHECK_FALSE(control(e, "lfo1Rate").hidden);
    CHECK(control(e, "lfo2Rate").hidden);
}

// Every hidden control stays bound, so a restore finds its value: the count of controls is the same for every choice.
TEST_CASE("SwirlEffect declares the same controls whatever is chosen") {
    SwirlEffect a, b;
    load(b, SwirlEffect::kVortexRings);
    b.modSource[2] = static_cast<uint8_t>(Src::kBass);
    a.rebuildControls();
    b.rebuildControls();
    CHECK(a.controls().count() == b.controls().count());
}

// ---- memory by the choice ------------------------------------------------------------------------

// The pool holds what count can reach, rounded up to 32; the depth is the attractor's alone; the trail is the grid's.
TEST_CASE("SwirlEffect holds the pool, depth and trail the choice needs") {
    mm::golden::LayerFixture fx(16, 16);
    SwirlEffect e;
    fx.add(e);
    CHECK(e.capacityForTest() == 128);
    CHECK(e.depthHeldForTest());
    CHECK(e.dynamicBytes() == 128 * sizeof(mm::swirl::Particle) + 128 * sizeof(float) + 16 * 16 * 3);
}

TEST_CASE("SwirlEffect holds no depth for a 2D field") {
    mm::golden::LayerFixture fx(16, 16);
    SwirlEffect e;
    load(e, SwirlEffect::kAdvection);
    fx.add(e);
    CHECK_FALSE(e.depthHeldForTest());
    CHECK(e.capacityForTest() == 160);
    CHECK(e.dynamicBytes() == 160 * sizeof(mm::swirl::Particle) + 16 * 16 * 3);
}

// At trail 0 no plane is held, and the particles still draw.
TEST_CASE("SwirlEffect at trail 0 holds no trail and still draws") {
    SwirlEffect e;
    load(e, SwirlEffect::kAdvection);
    e.param[SwirlEffect::kTrail] = 0;
    const std::vector<uint8_t> frame = render(e);
    CHECK_FALSE(e.trailHeldForTest());
    CHECK(e.dynamicBytes() == 160 * sizeof(mm::swirl::Particle));
    CHECK(litLights(frame) > 0);
}

// A slot on the trail can lift it from 0, so its trail is held.
TEST_CASE("SwirlEffect holds the trail a slot can lift from 0") {
    mm::golden::LayerFixture fx(16, 16);
    SwirlEffect e;
    e.param[SwirlEffect::kTrail] = 0;
    route(e, 0, Src::kBass, SwirlEffect::kTrail, 200);
    fx.add(e);
    CHECK(e.trailHeldForTest());
}

// The pool reaches the count plus the depth of a slot on it, so a modulated count allocates in the prepare alone.
TEST_CASE("SwirlEffect sizes the pool for a modulated count and allocates nothing on the tick") {
    SwirlEffect e;
    load(e, SwirlEffect::kAdvection);
    e.param[SwirlEffect::kCount] = 40;
    route(e, 0, Src::kLevel, SwirlEffect::kCount, 100);
    ModRig rig(e);
    CHECK(e.capacityForTest() == 160);   // 40 + 100 rounds up to 160
    const size_t held = e.dynamicBytes();
    mm::AudioFrame loud;
    loud.level = 255;
    for (uint16_t t = 0; t < 10; t++) rig.playAt(loud, 1000 + t * 20);
    CHECK(e.nowForTest(SwirlEffect::kCount) == 140);
    CHECK(e.liveForTest() > 130);   // filled to 140 each step, less the few the step carried past the border
    CHECK(e.liveForTest() <= 140);
    CHECK(e.dynamicBytes() == held);
    rig.playAt(mm::AudioFrame{}, 1400);
    CHECK(e.liveForTest() <= 40);   // the count falls back, the extra let go at once
    CHECK(e.liveForTest() > 35);
}

// A control prepares again exactly when the buffers it needs differ from those held, or when it loads a preset.
TEST_CASE("SwirlEffect prepares again only when the buffers must change") {
    mm::golden::LayerFixture fx(16, 16);
    SwirlEffect e;
    fx.add(e);   // Halvorsen: pool 128, depth, trail
    CHECK_FALSE(e.affectsPrepare("speed"));
    CHECK(e.affectsPrepare("preset"));
    e.param[SwirlEffect::kCount] = 100;    // still within 128
    CHECK_FALSE(e.affectsPrepare("count"));
    e.param[SwirlEffect::kCount] = 130;    // past it
    CHECK(e.affectsPrepare("count"));
    e.param[SwirlEffect::kCount] = 120;
    e.field = static_cast<uint8_t>(mm::swirl::Field::kFlow);   // a 2D field: the attractor's depth goes
    CHECK(e.affectsPrepare("field"));
    e.field = static_cast<uint8_t>(mm::swirl::Field::kAttractor);
    e.emitter = static_cast<uint8_t>(mm::swirl::Emitter::kRing);   // an emitter needs no buffer of its own
    CHECK_FALSE(e.affectsPrepare("emitter"));
    e.param[SwirlEffect::kTrail] = 100;
    CHECK_FALSE(e.affectsPrepare("trail"));
    e.param[SwirlEffect::kTrail] = 0;
    CHECK(e.affectsPrepare("trail"));
}

// ---- modulation --------------------------------------------------------------------------------

// A slot on the bass needs the analysis and one on the chord the notes, while the level and the LFOs run regardless. The demand follows the slot's change with no prepare, so routing a sound keeps the picture running.
TEST_CASE("SwirlEffect needs the audio analysis only while a slot reads it") {
    mm::golden::LayerFixture fx(16, 16);
    SwirlEffect e;
    fx.add(e);
    const auto demands = [&e] {
        uint8_t bits = 0;
        for (const mm::ModuleResource* r = e.resources(); r; r = r->nextResource()) bits |= r->demandsOf(mm::AudioDemand::kProvider);
        return bits;
    };
    const auto change = [&e](const char* name) {   // as the scheduler reacts: the change runs, then the prepare question
        e.onControlChanged(name);
        return e.affectsPrepare(name);
    };
    const uint8_t bands = mm::AudioDemand::bitOf(mm::AudioFeature::Bands), notes = mm::AudioDemand::bitOf(mm::AudioFeature::Notes);
    CHECK(demands() == 0);
    route(e, 0, Src::kLevel, SwirlEffect::kSpeed, 64);
    CHECK_FALSE(change("mod1Source"));
    CHECK(demands() == 0);
    route(e, 0, Src::kBass, SwirlEffect::kSpeed, 64);
    CHECK_FALSE(change("mod1Source"));
    CHECK(demands() == bands);
    e.modDest[0] = 0;   // a source with no knob left to move
    CHECK_FALSE(change("mod1Dest"));
    CHECK(demands() == 0);
    route(e, 1, Src::kChord, SwirlEffect::kSpin, 64);
    CHECK_FALSE(change("mod2Source"));
    CHECK(demands() == (bands | notes));
    route(e, 1, Src::kLfo1, SwirlEffect::kSpin, 64);
    CHECK_FALSE(change("mod2Source"));
    CHECK(demands() == 0);
    route(e, 2, Src::kChord, SwirlEffect::kSize, 1);   // a document's slot, stored at boot with no change run, read at the prepare that follows the load
    e.prepare();
    CHECK(demands() == (bands | notes));
}

TEST_CASE("SwirlEffect slot lifts its destination by its depth at a full source") {
    SwirlEffect e;
    e.param[SwirlEffect::kSpeed] = 60;
    route(e, 0, Src::kLevel, SwirlEffect::kSpeed, 100);
    ModRig rig(e);
    mm::AudioFrame loud;
    loud.level = 255;
    rig.playAt(loud, 1000);
    CHECK(e.nowForTest(SwirlEffect::kSpeed) == 160);
    rig.playAt(mm::AudioFrame{}, 1020);
    CHECK(e.nowForTest(SwirlEffect::kSpeed) == 60);
}

TEST_CASE("SwirlEffect stacks two slots on one destination within its range") {
    SwirlEffect e;
    e.param[SwirlEffect::kShape] = 189;
    route(e, 0, Src::kBass, SwirlEffect::kShape, 40);
    route(e, 1, Src::kKick, SwirlEffect::kShape, 40);
    ModRig rig(e);
    mm::AudioFrame f;
    f.bass = 255;
    f.onsetLow = 255;
    rig.playAt(f, 1000);
    CHECK(e.nowForTest(SwirlEffect::kShape) == 255);   // 189 + 40 + 40 clamps at the shape's top
}

// A destination the chosen parts hide is outside the motion, so routing to it leaves the panel as it was.
TEST_CASE("SwirlEffect slot on a hidden knob changes nothing") {
    SwirlEffect plain, routed;
    route(routed, 0, Src::kLevel, SwirlEffect::kSpread, 255);   // spread is the nozzle's; Halvorsen runs a cloud
    std::vector<uint8_t> a, b;
    {
        ModRig rig(plain);
        mm::AudioFrame loud;
        loud.level = 255;
        for (uint16_t t = 0; t < 20; t++) rig.playAt(loud, 1000 + t * 20);
        a = rig.fx.frame();
    }
    {
        ModRig rig(routed);
        mm::AudioFrame loud;
        loud.level = 255;
        for (uint16_t t = 0; t < 20; t++) rig.playAt(loud, 1000 + t * 20);
        b = rig.fx.frame();
    }
    REQUIRE(litLights(a) > 0);
    CHECK(a == b);
}

// An LFO moves a knob with no audio at all.
TEST_CASE("SwirlEffect LFO moves a knob with no audio") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    e.param[SwirlEffect::kWander] = 0;
    e.lfoRate[0] = 32;   // a cycle a second
    route(e, 0, Src::kLfo1, SwirlEffect::kWander, 255);
    fx.add(e);
    uint8_t lo = 255, hi = 0;
    for (uint16_t f = 1; f <= 50; f++) {
        fx.tickAt(1000 + f * 20);
        lo = std::min(lo, e.nowForTest(SwirlEffect::kWander));
        hi = std::max(hi, e.nowForTest(SwirlEffect::kWander));
    }
    CHECK(lo < 20);
    CHECK(hi > 235);
}

// ---- movement ----------------------------------------------------------------------------------

// Wander sends the origin off the center; at 0 it rests there.
TEST_CASE("SwirlEffect wander moves the origin over the space") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect still, roaming;
    roaming.param[SwirlEffect::kWander] = 255;
    fx.add(still);
    mm::golden::LayerFixture fx2(32, 32);
    fx2.add(roaming);
    CHECK(still.motionForTest().originXForTest() == doctest::Approx(mm::swirl::kCenter));
    CHECK(std::fabs(roaming.motionForTest().originXForTest() - mm::swirl::kCenter) > 3.0f);   // six hundred settle steps along its path
}

// At spin 128 the frame holds; either side of it, it turns one way or the other.
TEST_CASE("SwirlEffect spin turns the frame both ways") {
    for (uint8_t spin : {uint8_t{128}, uint8_t{200}, uint8_t{60}}) {
        mm::golden::LayerFixture fx(16, 16);
        SwirlEffect e;
        load(e, SwirlEffect::kAdvection);
        e.param[SwirlEffect::kSpin] = spin;
        fx.add(e);   // one settle step: the turn of one step
        const float theta = e.motionForTest().thetaForTest();
        if (spin == 128) CHECK(theta == doctest::Approx(0.0f));
        if (spin == 200) CHECK(theta == doctest::Approx(72 * 0.0005f));
        if (spin == 60) CHECK(theta == doctest::Approx(4 * mm::boids::kQuarterTurn - 68 * 0.0005f));
    }
}

// The trail fades once a step however the steps fall into ticks: thirteen steps in five ticks leave the frame thirteen in three leave.
TEST_CASE("SwirlEffect draws the same trail at any frame rate") {
    const auto run = [](uint32_t every, uint32_t until) {
        mm::golden::ScopedTestClock clock(1000);
        mm::golden::LayerFixture fx(32, 32);
        SwirlEffect e;   // Halvorsen, its trail held
        fx.add(e);
        for (uint32_t t = 1000; t <= until; t += every) fx.tickAt(t);   // the first tick owes one step, then 60 a second
        return fx.frame();
    };
    CHECK(run(50, 1200) == run(100, 1200));
}

// At speed 0 the picture holds exactly: no steps, no fade, the heads frozen.
TEST_CASE("SwirlEffect at speed 0 holds the picture") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    fx.add(e);
    for (uint32_t f = 0; f <= 60; f++) fx.tickAt(1000 + f * 20);
    const std::vector<uint8_t> first = fx.frame();
    REQUIRE(litLights(first) > 0);
    e.param[SwirlEffect::kSpeed] = 0;
    for (uint32_t f = 61; f <= 120; f++) fx.tickAt(1000 + f * 20);
    CHECK(fx.frame() == first);
}

// A lower count lets the rest go at once, a higher one fills on the next steps.
TEST_CASE("SwirlEffect follows its count both ways") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kAdvection);
    fx.add(e);
    e.param[SwirlEffect::kCount] = 20;
    fx.tickAt(1020);
    CHECK(e.liveForTest() <= 20);
    CHECK(e.liveForTest() > 15);
    e.param[SwirlEffect::kCount] = 150;   // within the pool of 160
    fx.tickAt(1040);
    fx.tickAt(1060);
    CHECK(e.liveForTest() > 140);
    CHECK(e.liveForTest() <= 150);
}

// ---- any input ---------------------------------------------------------------------------------

// The effect survives a 1x1 and a 0x0 grid in every preset.
TEST_CASE("SwirlEffect survives tiny grids") {
    for (uint8_t p = 0; p < SwirlEffect::kPresetCount; p++) {
        for (int side : {1, 0}) {
            mm::golden::ScopedTestClock clock(1000);
            mm::golden::LayerFixture fx(side, side);
            SwirlEffect e;
            load(e, p);
            fx.add(e);
            fx.tickAt(1000);
            fx.tickAt(1020);
        }
    }
}

// Every knob of `e` at the bottom of its range, or the top, as its control declares it.
void knobsAt(SwirlEffect& e, bool top) {
    e.rebuildControls();
    const auto& list = e.controls();
    for (uint8_t i = 0; i < SwirlEffect::kParamCount; i++)
        for (uint8_t c = 0; c < list.count(); c++)
            if (list[c].ptr == &e.param[i]) e.param[i] = static_cast<uint8_t>(top ? list[c].max : list[c].min);
}

// Run `e` 20 ticks on 16x16 and check every live particle is inside the pool and finite.
void runsSound(SwirlEffect& e) {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(16, 16);
    fx.add(e);
    for (uint16_t f = 1; f <= 20; f++) fx.tickAt(1000 + f * 20);
    CHECK(e.liveForTest() <= e.capacityForTest());
    for (uint16_t i = 0; i < e.liveForTest(); i++) {
        const mm::swirl::Particle* p = e.particleForTest(i);
        CHECK(std::isfinite(p->x));
        CHECK(std::isfinite(p->y));
    }
}

// Every emitter with every field, each on a border in turn, every knob at its bottom and at its top while the pace runs: every particle stays in the pool and finite.
TEST_CASE("SwirlEffect runs every pairing at both ends of every knob") {
    for (uint8_t em = 0; em < static_cast<uint8_t>(mm::swirl::Emitter::kCount); em++)
        for (uint8_t fi = 0; fi < static_cast<uint8_t>(mm::swirl::Field::kCount); fi++)
            for (bool top : {false, true}) {
                CAPTURE(em);
                CAPTURE(fi);
                CAPTURE(top);
                SwirlEffect e;
                e.emitter = em;
                e.field = fi;
                e.edge = static_cast<uint8_t>((em + fi + top) % static_cast<uint8_t>(mm::swirl::Edge::kCount));   // every border, spread over the pairings
                e.color = static_cast<uint8_t>((em + fi) % static_cast<uint8_t>(mm::swirl::Color::kCount));
                knobsAt(e, top);
                e.param[SwirlEffect::kSpeed] = mm::swirl::kReferenceHz;   // the pace held, so the ticks step the motion; its own ends run below
                runsSound(e);
            }
}

// The pace's own ends, on the busiest pairing: 0 holds, 255 runs four steps a tick.
TEST_CASE("SwirlEffect runs at both ends of its pace") {
    for (uint8_t speed : {uint8_t{0}, uint8_t{255}}) {
        CAPTURE(speed);
        SwirlEffect e;
        load(e, SwirlEffect::kNozzleJet);
        e.param[SwirlEffect::kCount] = 255;
        e.param[SwirlEffect::kTurbulence] = 255;
        e.param[SwirlEffect::kSpeed] = speed;
        runsSound(e);
    }
}

// Every pairing at its default knobs draws, with the border ending what crosses it; the one blank pairing is the edge emitter under an outward vortex, as the card says.
TEST_CASE("SwirlEffect draws every pairing at its default knobs") {
    for (uint8_t em = 0; em < static_cast<uint8_t>(mm::swirl::Emitter::kCount); em++)
        for (uint8_t fi = 0; fi < static_cast<uint8_t>(mm::swirl::Field::kCount); fi++) {
            if (em == static_cast<uint8_t>(mm::swirl::Emitter::kEdge) && fi == static_cast<uint8_t>(mm::swirl::Field::kVortex)) continue;
            CAPTURE(em);
            CAPTURE(fi);
            SwirlEffect e;
            e.emitter = em;
            e.field = fi;
            e.edge = static_cast<uint8_t>(mm::swirl::Edge::kDie);
            CHECK(litLights(render(e)) > 0);
        }
}

// Stored values past every list, from an older build or a hand-edited file, run as a valid motion.
TEST_CASE("SwirlEffect runs with every select past its list") {
    SwirlEffect e;
    e.emitter = 99;
    e.field = 99;
    e.edge = 99;
    e.color = 99;
    e.modSource[0] = 200;
    e.modDest[0] = 200;
    e.modDepth[0] = 255;
    CHECK(litLights(render(e)) > 0);
}

// ---- the review's fixes ------------------------------------------------------------------------

// A count below the ring's size blows a smaller ring rather than none.
TEST_CASE("SwirlEffect blows a smaller ring when the count is below the ring's size") {
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kVortexRings);
    e.param[SwirlEffect::kCount] = 20;
    e.param[SwirlEffect::kExpand] = 128;   // held still, so the rings stay to be counted
    fx.add(e);
    CHECK(e.liveForTest() == 20);
}

// An inward vortex swallows its particles at the origin: with no life to end them, none piles up there.
TEST_CASE("SwirlEffect inward vortex swallows its particles") {
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kVortexRings);
    e.param[SwirlEffect::kExpand] = 0;
    e.param[SwirlEffect::kSwirl] = 128;
    e.param[SwirlEffect::kLife] = 0;
    fx.add(e);   // five rings blown over the settle, each swallowed within a few steps
    CHECK(e.liveForTest() < e.param[SwirlEffect::kRingSize]);
}

// Each knob's control carries the name a mod slot lists it by, so the two lists stay one.
TEST_CASE("SwirlEffect names each knob as its mod slots list it") {
    SwirlEffect e;
    e.rebuildControls();
    const mm::ControlDescriptor& dest = control(e, "mod1Dest");
    const auto* options = reinterpret_cast<const char* const*>(dest.aux);
    const auto& list = e.controls();
    for (uint8_t i = 0; i < SwirlEffect::kParamCount; i++) {
        CAPTURE(i);
        bool found = false;
        for (uint8_t c = 0; c < list.count(); c++) {
            if (list[c].ptr != &e.param[i]) continue;
            CHECK(std::strcmp(list[c].name, options[i + 1]) == 0);
            found = true;
        }
        CHECK(found);
    }
}

// The prepare settles with turbulence at 0, the dearest term, and the first tick brings it back.
TEST_CASE("SwirlEffect settles without turbulence and churns from the first tick") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    SwirlEffect e;
    load(e, SwirlEffect::kNozzleJet);
    fx.add(e);
    CHECK(e.motionForTest().turbulence == 0);
    fx.tickAt(1020);
    CHECK(e.motionForTest().turbulence == 64 * mm::swirl::Motion::kTurbStrength / 255);
}

// A memory refusal frees every buffer and warns; the tick then draws the layer black over the last frame, and the next prepare that fits clears the warning.
TEST_CASE("SwirlEffect frees every buffer and draws black when the trail is refused") {
    struct AllocGuard { ~AllocGuard() { mm::platform::setTestAllocRefusedFrom(0); } } alloc;
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(64, 64);   // a 12,288-byte trail beside a 3,072-byte pool
    SwirlEffect e;
    e.param[SwirlEffect::kTrail] = 0;
    fx.add(e);   // the layer holds its own buffer first, and the effect no trail, so the refusal below meets the trail alone
    for (uint32_t t = 1000; t <= 1100; t += 20) fx.tickAt(t);
    REQUIRE(litLights(fx.frame()) > 0);
    e.param[SwirlEffect::kTrail] = 230;
    mm::platform::setTestAllocRefusedFrom(8192);
    e.prepare();
    CHECK(e.capacityForTest() == 0);
    CHECK_FALSE(e.depthHeldForTest());
    CHECK_FALSE(e.trailHeldForTest());
    CHECK(e.dynamicBytes() == 0);
    REQUIRE(e.status() != nullptr);
    CHECK(std::strcmp(e.status(), mm::swirl::kNoMemory) == 0);
    fx.tickAt(1120);
    CHECK(litLights(fx.frame()) == 0);
    mm::platform::setTestAllocRefusedFrom(0);
    e.prepare();
    CHECK(e.capacityForTest() == 128);
    CHECK(e.status() == nullptr);
}

TEST_CASE("SwirlEffect frees every buffer when the pool is refused") {
    struct AllocGuard { ~AllocGuard() { mm::platform::setTestAllocRefusedFrom(0); } } alloc;
    mm::golden::LayerFixture fx(16, 16);   // a 768-byte trail beside a 6,120-byte pool
    SwirlEffect e;
    e.param[SwirlEffect::kCount] = 255;
    mm::platform::setTestAllocRefusedFrom(4096);
    fx.add(e);
    CHECK(e.capacityForTest() == 0);
    CHECK(e.dynamicBytes() == 0);
}

// The review's sequence: a slot moved to the trail while memory refuses it, then moved back, leaves every buffer sized for one pool, so the count lifted to the pool's top writes inside it.
TEST_CASE("SwirlEffect keeps the depth the pool's size after a refused prepare") {
    struct AllocGuard { ~AllocGuard() { mm::platform::setTestAllocRefusedFrom(0); } } alloc;
    SwirlEffect e;
    e.param[SwirlEffect::kTrail] = 0;
    e.param[SwirlEffect::kCount] = 55;
    route(e, 0, Src::kLevel, SwirlEffect::kCount, 200);
    ModRig rig(e, 64, 64);
    REQUIRE(e.capacityForTest() == 255);
    e.modDest[0] = static_cast<uint8_t>(SwirlEffect::kTrail + 1);
    REQUIRE(e.affectsPrepare("mod1Dest"));
    mm::platform::setTestAllocRefusedFrom(8192);
    e.prepare();
    mm::platform::setTestAllocRefusedFrom(0);
    e.modDest[0] = static_cast<uint8_t>(SwirlEffect::kCount + 1);
    REQUIRE(e.affectsPrepare("mod1Dest"));
    e.prepare();
    CHECK(e.dynamicBytes() == 255 * sizeof(mm::swirl::Particle) + 255 * sizeof(float));
    mm::AudioFrame loud;
    loud.level = 255;
    for (uint16_t t = 0; t < 5; t++) rig.playAt(loud, 1000 + t * 20);
    CHECK(e.liveForTest() > 250);   // filled to 255 each step, less any the step ended
    CHECK(e.liveForTest() <= 255);
}

// A state document that names a preset and tuned knobs, applied to a running effect, ends at the knobs it names: the preset leads, the knobs apply after it.
TEST_CASE("SwirlEffect keeps a document's tuned knobs over the preset it names") {
    mm::Scheduler s;
    auto* e = new SwirlEffect();
    e->setName("Swirl");
    s.addModule(e);
    e->rebuildControls();   // its controls declared, as the device's setup does
    const mm::StateDocumentResult r = mm::applyStateAt(s, *e, R"("preset": 1, "count": 200, "spread": 99)");   // the module's members, inside the braces applyStateAt opens
    CAPTURE(std::string(r.error));
    CAPTURE(std::string(r.where));
    CHECK(r.ok);
    CHECK(e->emitter == static_cast<uint8_t>(mm::swirl::Emitter::kNozzle));
    CHECK(e->param[SwirlEffect::kCount] == 200);
    CHECK(e->param[SwirlEffect::kSpread] == 99);
    s.release();
}

// The prepare's settle runs at most Halvorsen's default work, 600 steps at 120 particles: the attractor at 255 settles 282 steps. The frame's turn counts the steps.
TEST_CASE("SwirlEffect caps the settle's work by the count") {
    for (uint8_t count : {uint8_t{120}, uint8_t{255}}) {
        CAPTURE(count);
        mm::golden::LayerFixture fx(16, 16);
        SwirlEffect e;
        e.param[SwirlEffect::kCount] = count;
        e.param[SwirlEffect::kSpin] = 129;   // one notch: 0.0005 a step, small enough to stay inside one turn
        fx.add(e);
        const float steps = count == 120 ? 600.0f : 282.0f;
        CHECK(e.motionForTest().thetaForTest() == doctest::Approx(steps * 0.0005f).epsilon(0.001));
    }
}
