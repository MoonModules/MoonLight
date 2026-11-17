/// @module Layer
/// @also MetaballsEffect, SpiralEffect, LavaLampEffect, SpiralEffect

#include "doctest.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/NoiseEffect.h"
#include "platform/platform.h"

#include <vector>

// Regression: per-tick phase accumulators computed `dt * bpm * 256 / 60000`, which truncates to 0 on host where dt ≈ 0..1 ms, the animations froze.
// Fix accumulates the raw (dt*bpm) numerator and only divides at the read site.
// These tests pin animation across a short time gap for every affected effect.

namespace {

// Run the effect through a sequence of short ticks totalling `total_ms`, take snapshots along the way, and return true if at least two snapshots differ.
// The robustness matters because some effects' visible state can land on a near-identical frame at two sampled instants by accident, a single before/after pair can miss the motion. Multiple samples across a longer interval can't.
//
// Time advances through the platform test-clock seam, no wall-clock sleeps, so the test is deterministic and instant regardless of CI load.
template <typename Effect>
bool animates_over_ms(int total_ms) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    // 32×32 is large enough to show the effect's spatial motion and small enough to keep the test fast.
    grid.width = 32; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    Effect e;
    layer.addChild(&e);
    layouts.applyState();
    layer.applyState();

    uint32_t virtualNow = 1000;       // arbitrary non-zero start
    mm::platform::setTestNowMs(virtualNow);
    layer.tick();
    std::vector<uint8_t> baseline(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    const int step_ms = 20;
    bool animated = false;
    for (int elapsed = 0; elapsed < total_ms; elapsed += step_ms) {
        virtualNow += step_ms;
        mm::platform::setTestNowMs(virtualNow);
        layer.tick();
        std::vector<uint8_t> now(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
        if (now != baseline) { animated = true; break; }
    }
    mm::platform::setTestNowMs(0);  // restore real clock for the next case
    return animated;
}

} // namespace

// Metaballs visibly changes over 100ms even when per-tick dt is sub-millisecond (no phase-accumulator truncation).
TEST_CASE("MetaballsEffect animates over a 100ms gap (host dt ≈ 0..1ms)") {
    CHECK(animates_over_ms<mm::MetaballsEffect>(100));
}

// SpiralEffect advances at host speed (the spiral rotates across 100ms).
TEST_CASE("SpiralEffect animates over a 100ms gap") {
    CHECK(animates_over_ms<mm::SpiralEffect>(100));
}

// LavaLamp animates across 100ms (blobs move).
TEST_CASE("LavaLampEffect animates over a 100ms gap") {
    CHECK(animates_over_ms<mm::LavaLampEffect>(100));
}

// Replace path: swap one effect for another mid-flight (same shape as HttpServerModule::handleReplaceModule) and confirm the new effect animates. Replacing one effect with another mid-tick (HttpServerModule's swap path) leaves the new effect animating, not frozen.
TEST_CASE("Replacing an effect at runtime: new effect still animates") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    auto* noise = new mm::NoiseEffect();
    layer.addChild(noise);

    layouts.applyState();
    layer.applyState();
    layer.tick();

    auto* fresh = new mm::MetaballsEffect();
    mm::MoonModule* old = layer.replaceChildAt(0, fresh);
    fresh->defineControls();
    fresh->setup();
    fresh->applyState();
    if (old) { old->release(); delete old; }
    layer.applyState();

    mm::platform::setTestNowMs(1000);
    layer.tick();
    std::vector<uint8_t> first(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    mm::platform::setTestNowMs(1100);
    layer.tick();
    std::vector<uint8_t> second(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    mm::platform::setTestNowMs(0);
    CHECK_FALSE_MESSAGE(first == second, "Replaced Metaballs frozen across 100ms gap");
}
