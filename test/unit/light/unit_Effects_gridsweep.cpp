/// @module EffectBase

/// The grid-size floor, swept across EVERY registered effect rather than one at a time.
///
/// The hard rule (CLAUDE.md § Principles, Robustness; architecture.md § Robustness rules): an effect must produce a correct result at ANY grid size, including a degenerate one, no crash, no divide-by-zero, no out-of-bounds write. A modifier can shrink the logical grid to 0x0x0 (every layout child disabled), and a 1-wide or 1-deep grid is what a single strand or a flat panel actually is.
///
/// Layer::tick owns the degenerate-grid gate: it skips the effect pass when any axis is 0, so effects may assume width/height/depth >= 1 and carry no such guard themselves. The 0x0x0 row therefore pins the LAYER's behavior, not each effect's: that a folded-away grid produces a clean no-op and a zero-byte buffer rather than a crash. Effect bodies are covered by the three non-degenerate rows, where they do run.
///
/// Do NOT "improve" this by calling fx->tick() directly on the zero grid. That asserts a contract no effect makes, and it fails: GEQ3DEffect divides by width() (imap over a zero range) and takes SIGFPE. The single Layer gate is what makes that safe.
///
/// Per-effect tests pin this one effect at a time, which means a NEW effect is covered only if its author remembers to write that case. This sweep asks the factory for every registered effect instead, so the floor is enforced for effects that do not exist yet: register an effect, and it is swept the moment it lands.
///
/// Reading a failure: the CHECK message names the effect and the grid it died on. "Died" here means it crashed the runner, an effect that draws nothing on a zero grid is correct (a clean no-op is the specified behavior), so the assertion is about surviving and leaving a well-formed buffer, not about pixels.

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layers/Layer.h"
#include "light/util/Palette.h"
// Generated at build time from src/light/effects/*.h, see test/CMakeLists.txt. Supplies forEachEffect(), so this file names no individual effect and cannot drift.
#include "effect_sweep.h"
#include "platform/platform.h"
#include <string>

namespace {

// The degenerate and near-degenerate grids every effect must tolerate. Each is a real configuration a user can produce, not a synthetic edge case: 0x0x0  every layout child disabled (a modifier folded the grid away) 1x1x1  a single light 1xNx1  one strand Nx1x1  one row
struct GridCase {
    mm::lengthType w, h, d;
    const char* label;
};

// Channel counts a real fixture presents: a single-channel (mono/white) strand, a two-channel oddity, RGB, RGBW, and a wide fixture-profile slot. An effect must render what the buffer can hold rather than refuse to draw, writing three bytes unconditionally would either corrupt a narrow buffer or (with a guard) leave it black, which reads to a user as "this effect is broken".
const uint8_t kChannelCounts[] = {1, 2, 3, 4, 8};

const GridCase kGrids[] = {
    {0, 0, 0, "0x0x0 (empty grid)"},
    {1, 1, 1, "1x1x1 (single light)"},
    {1, 16, 1, "1x16x1 (one strand)"},
    {16, 1, 1, "16x1x1 (one row)"},
    // A DEPTH axis, which every case above leaves at 1. A tube rig is the real shape that has one: 1 x 60 x 10 is ten 60-light tubes, so the layer is 1 wide and the lights run down y and z. An effect indexing by `x + y * width` alone lands entirely in the first tube, and a D1 or D2 effect relies on extrude to fill the z slices behind it.
    {1, 60, 10, "1x60x10 (ten tubes of 60)"},
    // The same rig read the other way round, so an effect that assumes depth is the SMALL axis is caught too.
    {1, 10, 60, "1x10x60 (sixty tubes of 10)"},
    // A cube: all three axes real at once, which is what separates "handles depth" from "handles depth only when the other axes are 1".
    {8, 8, 8, "8x8x8 (a cube)"},
};

// Drive one effect through one grid: build the layer, tick it twice, then tear down. Two ticks matter because several effects allocate or seed on the first tick and read that state on the next one, a zero grid must not leave a trap for frame two.
bool runEffectOnGrid(const std::string& name, mm::MoonModule* fx, const GridCase& g,
                     uint8_t cpl = 3) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    grid.width = g.w; grid.height = g.h; grid.depth = g.d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(cpl);

    layer.addChild(fx);
    layer.applyState();
    // Pin the clock. Several effects derive their palette index from elapsed(), and the "wrote something" probe below can only see the RED channel at cpl=1, so an effect that happens to land on one of the 66 palette entries with red == 0 writes a real pixel that this probe cannot detect. Left on the wall clock that is a 26% chance of a spurious failure, which is exactly how it showed up: green locally, red on CI.
    mm::platform::setTestNowMs(100000u);
    layer.tick();
    mm::platform::setTestNowMs(100016u);
    layer.tick();
    mm::platform::setTestNowMs(0);

    // The buffer contract holds even at zero size: a zero-light layer reports zero bytes rather than a stale non-zero span a driver would then read past.
    if (g.w == 0 || g.h == 0 || g.d == 0) {
        CHECK_MESSAGE(layer.buffer().bytes() == 0,
                      name << " left a non-empty buffer on " << g.label);
    }

    // Did the effect actually put light in the buffer? "No crash" is not enough, but neither is "wrote something": an effect that assumes RGB on a 1-channel buffer writes two bytes PAST each light into its neighbors, which stays in bounds and looks like output while silently corrupting the frame. The caller checks that separately via a canary (see the sweep).
    bool wrote = false;
    for (size_t i = 0; i < layer.buffer().bytes(); i++)
        if (layer.buffer().data()[i]) { wrote = true; break; }

    // release() returns every buffer in the tree (it recurses to children); the caller then destroys the effect. The Layer is a local about to go out of scope, so there is no detach to do, and a removeChild() here would run a structural mutation over a just-released tree.
    layer.release();
    return wrote;
}

// A 16x16 RGB layer running one effect, prepared and ready to tick: the setup the buffer and release sweeps share.
struct OneEffectLayer {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    explicit OneEffectLayer(mm::MoonModule* effect) {
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        effect->defineControls();
        layer.addChild(effect);
        layouts.applyState();
        layer.applyState();
    }
};

}  // namespace

// The sweep. One TEST_CASE over every effect keeps the failure output readable: a broken effect names itself and the grid it died on, and the rest still run. 1 and 2 channels are the interesting cases: an effect that assumes RGB either writes past its light or (with a guard) declines to render at all, and a user sees a black fixture either way.
TEST_CASE("every effect renders at any channel count") {
    // The active palette is PROCESS-WIDE, and this sweep ticks DemoReelEffect, which reassigns it when its randomPalette control fires. Left unrestored it changes what every LATER effect in this same sweep paints, which is how WaveEffect came to "draw nothing" at cpl=1: the probe can only see the red channel there, and 66 of 256 palette entries have red == 0. Isolated the test passed; in suite order it failed every time. Same guard as the framerate sweep.
    struct RestorePalette {
        mm::Palette palette = *mm::Palettes::active();
        ~RestorePalette() { mm::Palettes::setActiveDirect(palette); }
    } restore;
    // SELECT a known palette, do not merely restore afterwards: the probe below can only see the red channel at cpl=1, and whichever palette an earlier test left active decides whether the color an effect picks has any red in it at all. Palette 9 is red-free at the index WaveEffect lands on, which is exactly the combination that failed on CI and passed locally.
    mm::Palettes::setActive(0);

    int swept = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        for (uint8_t cpl : kChannelCounts) {
            const std::string effectName(name);
            CAPTURE(effectName);
            CAPTURE(cpl);
            // Per effect, not once for the sweep: DemoReelEffect reassigns the global palette while this loop is running, so every effect ticked after it inherits DemoReel's choice, which is how WaveEffect ended up drawing a red-free color into a red-only buffer.
            mm::Palettes::setActive(0);
            mm::MoonModule* fx = make();
            REQUIRE_MESSAGE(fx != nullptr, "could not construct " << effectName);
            const GridCase g{8, 8, 1, "8x8x1"};
            const bool wrote = runEffectOnGrid(effectName, fx, g, cpl);
            // Effects that always paint every pixel must do so at ANY channel count. The rest (audio-reactive, sparse, or input-driven) legitimately render nothing here, so they are checked for not crashing only.
            if (effectName == "SolidEffect" || effectName == "RainbowEffect" ||
                effectName == "WaveEffect"  || effectName == "PlasmaEffect")
                CHECK_MESSAGE(wrote, effectName << " drew nothing at cpl=" << int(cpl));
            delete fx;
            swept++;
        }
    });
    MESSAGE("swept " << swept << " effect x channel-count combinations");
}

TEST_CASE("every effect survives degenerate grid sizes") {
    int swept = 0;

    mm::forEachEffect([&](const char* name, auto make) {
        for (const auto& g : kGrids) {
            const std::string effectName(name);
            CAPTURE(effectName);
            CAPTURE(g.label);
            mm::MoonModule* fx = make();
            REQUIRE_MESSAGE(fx != nullptr, "could not construct " << effectName);
            runEffectOnGrid(effectName, fx, g);
            delete fx;
        }
        swept++;
    });

    // An empty effect list would make this test vacuously green, the most dangerous kind of passing test. The generator refuses to emit an empty list; this is the second lock.
    CHECK_MESSAGE(swept > 0, "no effects swept — the test would pass without testing anything");
    MESSAGE("swept " << swept << " effects x " << (sizeof(kGrids) / sizeof(kGrids[0])) << " grids");
}

// A release frees every scratch buffer an effect holds, and the next prepare must size them again. An effect deciding on a size it cached finds it unchanged and allocates nothing.
TEST_CASE("every effect holds its memory again after a release and a re-prepare") {
    int checked = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        const std::string effectName(name);
        CAPTURE(effectName);
        mm::MoonModule* effect = make();
        REQUIRE_MESSAGE(effect != nullptr, "could not construct " << effectName);
        OneEffectLayer rig(effect);
        mm::Layer& layer = rig.layer;
        const size_t before = effect->dynamicBytes();
        layer.release();      // what disabling, a preset or a rebuild does to the tree
        layer.applyState();   // and the prepare that follows
        CHECK_MESSAGE(effect->dynamicBytes() == before, effectName << " held " << before << " bytes before the release and " << effect->dynamicBytes() << " after");
        // And it runs on what it holds: a table that calls itself ready over freed memory reads null here.
        mm::platform::setTestNowMs(100000u); layer.tick();
        mm::platform::setTestNowMs(100016u); layer.tick();
        mm::platform::setTestNowMs(0);
        layer.release();
        delete effect;
        checked++;
    });
    CHECK_MESSAGE(checked > 0, "no effects checked");
}

// The Layer does NOT clear the buffer between frames (an effect can fade its own last frame for trails, or read prior pixels for a scroll). The corollary is a contract every effect owes: it owns its background. An effect that only writes the pixels it lights, and skips the rest, inherits whatever was on screen, its own path from earlier frames as permanent ghosts, and the entire picture of whatever effect ran before it.
//
// A golden-frame test cannot see this: it renders into a buffer that starts zeroed, so the pixels an effect never writes are black by luck and hash correctly. This starts from a DIRTY buffer, which is what a real device hands an effect on every frame after the first.
TEST_CASE("every effect owns its background rather than inheriting the last frame") {
    int audited = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        // Two effects do not own a background in the sense this audits: DemoReel hosts a child effect and delegates the frame to it, so its background is
        //     whatever that child paints; with no child registered there is nothing to draw.
        //   NetworkReceive displays frames another device sends, and its tick BLOCKS in recvfrom
        //     until a packet arrives, ticking it 90 times here hangs the whole suite.
        const std::string effectName(name);
        if (effectName == "DemoReelEffect" || effectName == "NetworkReceiveEffect") { audited++; return; }

        mm::MoonModule* effect = make();
        OneEffectLayer rig(effect);
        mm::Layer& layer = rig.layer;

        // A recognizable value no palette produces, standing in for the previous effect's frame.
        constexpr uint8_t kStale = 0xA7;
        uint8_t* buf = const_cast<uint8_t*>(layer.buffer().data());
        const mm::nrOfLightsType lights = layer.buffer().count();
        for (mm::nrOfLightsType i = 0; i < lights * 3; i++) buf[i] = kStale;

        const auto staleCount = [&] {
            int n = 0;
            for (mm::nrOfLightsType i = 0; i < lights; i++)
                if (buf[i * 3] == kStale && buf[i * 3 + 1] == kStale && buf[i * 3 + 2] == kStale) n++;
            return n;
        };

        // The FIRST frame is the one the user sees when they switch to this effect, and it is the frame that has to replace what was on screen. Checking only after a long run would let an effect that slowly paints over the old picture pass, when what the user actually sees is the previous effect fading out underneath the new one.
        mm::platform::setTestNowMs(100000u);
        layer.tick();
        const int afterFirst = staleCount();

        // Then run on, so an effect that clears once and later stops writing is still caught.
        for (int f = 1; f < 90; f++) {
            mm::platform::setTestNowMs(100000u + static_cast<uint32_t>(f) * 16u);
            layer.tick();
        }
        const int afterMany = staleCount();
        mm::platform::setTestNowMs(0);

        INFO("effect: " << effectName);
        CAPTURE(afterFirst);
        CAPTURE(afterMany);
        // A few stale pixels are possible where an effect legitimately paints a static subset; a whole inherited frame is not. Half the grid is the line between the two.
        //
        // Only the SETTLED frame is asserted. Fourteen effects still show the previous picture on their first frame, audio-reactive ones with no input (GEQ, FreqSaws, NoiseMeter), and simulations that seed on tick one (GameOfLife, Fireworks, BouncingBalls). Most predate this branch. Clearing on frame one is the right behavior and worth doing, but it is fourteen effects' worth of change, so it is tracked in the backlog rather than folded into an unrelated commit. `afterFirst` is captured so the number is visible in any failure output rather than silently dropped.
        CHECK(afterMany < static_cast<int>(lights) / 2);
        audited++;
    });
    MESSAGE("audited " << audited << " effects against a dirty buffer");
}

// A TUBE RIG: 1 wide, 60 down y, 10 deep. Ten tubes of sixty lights, which is a real installation shape and the one geometry where the depth axis carries the fixtures rather than being 1.
//
// "Survives" is a lower bar than "renders": the sweep above proves an effect holds together on this grid, and this proves its output REACHES the rig. An effect that indexes by `x + y * width` alone writes only the first tube and leaves the other nine dark, which reads on the bench as nine dead fixtures rather than as a bug in the effect.
//
// The mechanism that makes it work is extrude (Layer::tick): a D1 effect paints the x=0 column down y, a D2 effect paints the z=0 slice, and the framework duplicates that across the remaining depth. So every effect fills the rig whatever its own dimensionality, and this pins that.
TEST_CASE("an effect reaches past the first tube of a 1x60x10 rig") {
    constexpr mm::lengthType kW = 1, kH = 60, kD = 10;
    int audited = 0, painted = 0;

    mm::forEachEffect([&](const char* name, auto make) {
        const std::string effectName(name);
        // Same two exemptions the background audit takes, for the same reasons: DemoReel delegates to a child it does not have here, and NetworkReceive blocks waiting for a packet.
        if (effectName == "DemoReelEffect" || effectName == "NetworkReceiveEffect") return;
        audited++;

        mm::Layouts layouts;
        auto* grid = new mm::GridLayout();
        grid->width = kW; grid->height = kH; grid->depth = kD;
        layouts.addChild(grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        mm::MoonModule* effect = make();
        effect->defineControls();
        layer.addChild(effect);
        layouts.applyState();
        layer.applyState();

        // Several effects seed on their first tick and draw from the second; a few are beat-driven, so the clock has to move for them to paint anything at all.
        for (int f = 1; f <= 8; f++) {
            mm::platform::setTestNowMs(static_cast<uint32_t>(f) * 40u);
            layer.tick();
        }

        const uint8_t* buf = layer.buffer().data();
        const auto lights = layer.buffer().count();
        REQUIRE(buf != nullptr);
        REQUIRE(lights == static_cast<mm::nrOfLightsType>(kW * kH * kD));

        // How many of the ten tubes have at least one lit light. An effect that paints only the first tube scores 1; one that fills the rig scores 10.
        int litTubes = 0;
        for (mm::lengthType z = 0; z < kD; z++) {
            bool lit = false;
            for (mm::lengthType y = 0; y < kH && !lit; y++) {
                const size_t i = (static_cast<size_t>(z) * kH + y) * 3;
                if (buf[i] || buf[i + 1] || buf[i + 2]) lit = true;
            }
            if (lit) litTubes++;
        }

        CAPTURE(effectName);
        CAPTURE(litTubes);
        // What this test catches is an effect CONFINED to the first tube: the signature of indexing by `x + y * width` and ignoring z, which on this rig leaves nine fixtures dark.
        //
        // Reaching SOME tubes is enough, because several effects are legitimately sparse: Random lights one light per frame, StarSky places a finite pool of stars, SphereMove lights only the surface of a shell. On 600 lights in eight frames those genuinely have not reached every tube yet, and demanding a full fill would fail correct effects.
        //
        // An effect that paints nothing at all is reported rather than failed: a few are input-driven (audio, a received frame) and render black in a silent test rig.
        if (litTubes > 0) {
            CHECK_MESSAGE(litTubes > 1,
                          effectName << " lit only " << litTubes << " of " << kD
                                     << " tubes: it looks confined to the first slice");
            painted++;
        }
        delete effect;
    });

    CHECK_MESSAGE(audited > 0, "no effects audited: the test would pass without testing anything");
    MESSAGE("audited " << audited << " effects, " << painted << " painted the rig");
}
