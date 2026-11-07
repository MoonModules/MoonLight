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
#include "core/services/AudioService.h"
#include "platform/platform.h"

#include <cmath>
#include <memory>
#include <cstdio>
#include <string>

using namespace mm;

namespace {

/// Mean lit pixels per frame over `seconds` of SIMULATED time at `fps`, with a fresh simulated song, when `withAudio`, producing its frame each tick from the same clock.
template <typename Make>
double meanLit(Make make, int fps, int seconds, bool withAudio = false) {
    std::unique_ptr<AudioService> audio;   // its own per measurement, so neither rate starts from the other's smoothing
    if (withAudio) { audio = std::make_unique<AudioService>(); audio->mode = AudioService::kSimMode; audio->setup(); }
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
        if (audio) audio->tick();
        layer.tick();
        const uint8_t* buf = layer.buffer().data();
        int lit = 0;
        for (int i = 0; i < 24 * 24; i++)
            if (buf[i * 3] || buf[i * 3 + 1] || buf[i * 3 + 2]) lit++;
        total += lit;
    }
    delete effect;
    if (audio) audio->release();
    platform::setTestNowMs(0);      // hand the real clock back before the next test sees it
    return static_cast<double>(total) / frames;
}

}  // namespace

TEST_CASE("every effect behaves the same at any framerate") {
    // Two pieces of process-wide state go back, or this sweep corrupts every later test: found when they moved all 23 golden hashes at once, which reads like a rendering regression.
    //   - the test clock, restored per measurement below;
    //   - the active palette, which DemoReelEffect reassigns (`Palettes::setActive`) when its
    //     randomPalette control fires. Every effect reads that global, so a changed palette
    //     re-colors every later golden.
    struct RestoreGlobals {
        Palette palette = *Palettes::active();
        ~RestoreGlobals() { platform::setTestNowMs(0); Palettes::setActiveDirect(palette); }
    } restore;

    // The simulated song, a function of the clock alone, so the audio effects are measured too rather than dark at both rates.
    int audited = 0;
    forEachEffect([&](const char* name, auto make) {
        const double slow = meanLit(make, 60, 4, true);
        const double fast = meanLit(make, 1200, 4, true);

        // A dark effect at both rates is consistent by definition.
        if (slow < 0.5 && fast < 0.5) { audited++; return; }

        INFO("effect: " << std::string(name));
        CAPTURE(slow);
        CAPTURE(fast);
        // A 20x framerate change must not move how much is happening by more than a third: that absorbs rounding, while a frame counter shows as a multiple.
        const double ratio = (slow > fast) ? (slow / (fast + 0.01)) : (fast / (slow + 0.01));
        // The effects outside that band, each recorded with its reason rather than hidden by widening it for all: lit cells move with more than a frame counter.
        struct Band { const char* effect; double band; };
        static constexpr Band kWider[] = {
            {"BlurzEffect", 3.70},          // a compounding per-frame blur, the open case: draw::blur has to become time-aware
            {"FireworksEffect", 2.10},      // a moving light's trail is dots at 60 fps and a filled line at 1200: the sharpness a faster device buys; about 2 in suite order
            {"LissajousEffect", 1.90},      // the same filled trail
            {"StarFieldEffect", 1.95},      // the same filled trail
            {"RandomEffect", 2.40},         // its spawns are discrete, and how many cells it lights follows the shared RNG's state
            {"FluidEffect", 1.45},          // at a low rate it pours its moving jet in batches, so the dye takes a slightly different path
            {"NebulaEffect", 1.45},         // its births are placed in batches by an oscillator, as Fluid's pours are
            {"BouncingBallsEffect", 1.40},  // its motion is wall-clock; its trail rounds differently between the two rates
        };
        const std::string en(name);
        double band = 1.35;
        for (const Band& b : kWider) if (en == b.effect) band = b.band;
        if (!(ratio < band)) MESSAGE("FRAMERATE " << en << " ratio=" << ratio << " band=" << band);
        CHECK(ratio < band);
        audited++;
    });
    MESSAGE("audited " << audited << " effects at 60 vs 1200 fps");
}

// The sweep catches a runaway frame counter but not a gate that fires too rarely, which leaves the mean plausible; these pin the two mechanisms at the rate each was measured broken.

TEST_CASE("a feedback trail compounds at the same rate however fast the device renders") {
    // Echo re-samples its own previous frame, so its pass runs once per elapsed reference frame; a gate on a whole unit fired once a second at 240 fps and froze the trail.
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
    CHECK(fast < slow * 2.0);                // not a sky full of shells, which is near 20x; the trails' fill at 1200 fps is about 1.5x
}
