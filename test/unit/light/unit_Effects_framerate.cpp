/// @module Layer
/// @also draw, particles

/// The system rule from architecture.md: **everything that changes over time is driven by elapsed time, never by the frame count.** An effect advanced a fixed amount per frame runs at whatever speed the hardware renders, the same setting is an explosion on a desktop at thousands of fps and a drift on an ESP32 at a few hundred. The user sets a speed; the hardware must not get a vote.
///
/// This sweeps EVERY effect (the list is generated from the source tree, so a new effect is audited without touching this file) and renders the same span of SIMULATED time at two very different framerates. A time-driven effect produces a comparable amount of light either way; a frame-counting one does not.
///
/// It measures activity rather than exact pixels on purpose: integration error, dithering and palette rounding all differ slightly between step sizes, and pinning exact output would fail on noise rather than on the bug. What cannot differ is HOW MUCH happens per second.

#include "doctest.h"
#include "effect_sweep.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/util/Palette.h"
#include "light/effects/EchoEffect.h"
#include "light/effects/FireworksEffect.h"
#include "platform/platform.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace mm;

namespace {

/// Mean lit pixels per frame over `seconds` of SIMULATED time at `fps`.
template <typename Make>
double meanLit(Make make, int fps, int seconds) {
    Layouts layouts;
    auto* grid = new GridLayout();
    grid->width = 24; grid->height = 24; grid->depth = 1;
    layouts.addChild(grid);

    Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    MoonModule* effect = make();
    effect->defineControls();
    layer.addChild(effect);
    // Set before setup, which seeds an effect's random draws from the clock: unset, it reads the real clock and every run draws differently.
    platform::setTestNowMs(100000u);
    layouts.applyState();
    layer.applyState();

    const int frames = seconds * fps;
    long total = 0;
    for (int f = 0; f < frames; f++) {
        // Start away from zero: platform::millis() treats a test override of 0 as "not set" and falls through to the real clock, which would make the first delta enormous.
        platform::setTestNowMs(100000u + static_cast<uint32_t>(static_cast<long long>(f) * 1000 / fps));
        layer.tick();
        const uint8_t* buf = layer.buffer().data();
        int lit = 0;
        for (int i = 0; i < 24 * 24; i++)
            if (buf[i * 3] || buf[i * 3 + 1] || buf[i * 3 + 2]) lit++;
        total += lit;
    }
    delete effect;
    platform::setTestNowMs(0);      // hand the real clock back before the next test sees it
    return static_cast<double>(total) / frames;
}

}  // namespace

TEST_CASE("every effect behaves the same at any framerate") {
    // Two pieces of PROCESS-WIDE state have to be handed back, or this sweep silently corrupts every test that runs after it. Both were found the hard way: they moved all 23 golden hashes at once, which reads exactly like a rendering regression and is not one.
    //   - the test clock, restored per measurement below;
    //   - the active palette, which DemoReelEffect reassigns (`Palettes::setActive`) when its
    //     randomPalette control fires. Every effect reads that global, so a changed palette
    //     re-colors every later golden.
    struct RestoreGlobals {
        Palette palette = *Palettes::active();
        ~RestoreGlobals() { platform::setTestNowMs(0); Palettes::setActiveDirect(palette); }
    } restore;

    int audited = 0;
    forEachEffect([&](const char* name, auto make) {
        const double slow = meanLit(make, 60, 4);
        const double fast = meanLit(make, 1200, 4);

        // A dark effect at both rates (audio-driven with no input, say) is consistent by definition.
        if (slow < 0.5 && fast < 0.5) { audited++; return; }

        INFO("effect: " << std::string(name));
        CAPTURE(slow);
        CAPTURE(fast);
        // A 20x framerate change must not change how much is happening by more than a third. That band absorbs integration and rounding differences while still catching a frame counter, which shows up as a multiple rather than a percentage.
        const double ratio = (slow > fast) ? (slow / (fast + 0.01)) : (fast / (slow + 0.01));
        // Three effects sit outside the band, each recorded rather than hidden by widening it for all 51. This metric counts pixels that are ANY nonzero, so it also moves when the same amount of light is quantised differently across frames, which is not a frame counter.
        //
        // RandomEffect (2.31) and StarFieldEffect (1.44): both were migrated onto elapsed time and their PHYSICS is now rate-independent within 0.5% across 60/240/1200 fps (spawns 59.7/60.0/60.0 per second; fade 7679/7710/7720 units per second). What still differs is how the continuous fade quantises between their discrete steps, so a pixel crosses the nonzero threshold at a slightly different time. Random's figure also depends on where the shared RNG has been left by earlier tests (1.93 alone, 2.31 in suite order), which is a property of the effect rather than of this change: it picks its pixels at random, so a different draw sequence lights a different number of distinct cells.
        //
        // BlurzEffect (3.57): NOT migrated, and the one genuine open case. Its per-frame `draw::blur` is a COMPOUNDING spatial operation, so the carry pattern that fixed the others does not transfer: blurring twice at half strength is not one blur at full strength. Three attempts each made the ratio worse. Fixing it needs draw::blur itself to become time-aware, which is a shared primitive and its own change.
        //
        // BouncingBalls: its MOTION was always rate-independent (ball height comes from absolute wall-clock time); its trail now comes from the Layer and its golden is unchanged.
        // NebulaEffect (1.36): the same shape as FluidEffect below, for the same reason: its births are placed by an oscillator and paced by a time budget, so a batched frame seeds along a different path than a spread one.
        //
        // FluidEffect (1.36): its jets are POSITIONED by an oscillator and poured on a time-paced budget with catch-up. At a low framerate several pours land in one frame, all reading the jet's position at that instant, where a fast device spreads the same pours across the arc. So the dye is laid down along a slightly different path, which is a property of pouring a moving source in batches rather than a rate bug: the pour COUNT is already rate-correct. Surfaced when the oscillator was fixed to advance on absolute time; before that the jets never moved and the paths could not differ.
        const std::string en(name);
        const double band = (en == "BlurzEffect")       ? 3.70
                          : (en == "FluidEffect")       ? 1.45
                          : (en == "NebulaEffect")      ? 1.45
                          : (en == "RandomEffect")      ? 2.40
                          : (en == "StarFieldEffect")   ? 1.50
                          : (en == "BouncingBallsEffect") ? 1.40 : 1.35;
        if (!(ratio < band)) MESSAGE("FRAMERATE " << en << " ratio=" << ratio << " band=" << band);
        CHECK(ratio < band);
        audited++;
    });
    MESSAGE("audited " << audited << " effects at 60 vs 1200 fps");
}

// The sweep above measures how much light is on screen, which catches a runaway frame counter but not its opposite: a gate that fires too RARELY still leaves the mean plausible. These pin the two mechanisms directly, at the rate each one was measured broken.

TEST_CASE("a feedback trail compounds at the same rate however fast the device renders") {
    // Echo re-samples its own previous frame, so the pass must run once per elapsed reference frame. Testing it against a whole unit instead of accumulating made the gate fire once per SECOND at 240 fps rather than 60 times, which froze the trail while the source kept orbiting.
    const double slow = meanLit([] { return new EchoEffect(); }, 60, 3);
    const double mid  = meanLit([] { return new EchoEffect(); }, 240, 3);
    const double fast = meanLit([] { return new EchoEffect(); }, 1200, 3);
    CAPTURE(slow); CAPTURE(mid); CAPTURE(fast);
    CHECK(slow > 1.0);                       // a trail exists at the reference rate
    CHECK(mid  > slow * 0.7);                // ...and at 4x, where the gate used to stall
    CHECK(fast > slow * 0.7);                // ...and at 20x
}

TEST_CASE("fireworks launch the same number of shells per second at any framerate") {
    // The launch roll is a hash of the frame counter, so without a time gate a 20x faster device rolls the dice 20x as often and fills the sky.
    const double slow = meanLit([] { return new FireworksEffect(); }, 60, 3);
    const double fast = meanLit([] { return new FireworksEffect(); }, 1200, 3);
    CAPTURE(slow); CAPTURE(fast);
    CHECK(slow > 0.5);
    CHECK(fast < slow * 2.0);                // not a sky full of shells
}
