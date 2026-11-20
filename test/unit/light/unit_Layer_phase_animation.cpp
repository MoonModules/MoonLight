/// @module Layer
/// @also MetaballsEffect, SpiralEffect, LavaLampEffect, TrailsEffect

#include "doctest.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/TrailsEffect.h"
#include "core/util/ClockSync.h"   // FleetTime: a fleet clock stepping under a running layer
#include "golden_frame.h"          // LayerFixture, ScopedTestClock
#include "platform/platform.h"

#include <vector>

// Every phase-driven effect moves across a run of short ticks: a phase keeps its numerator wide and divides where it is read, so a frame of a millisecond still moves it.

namespace {

/// Whether `Effect` changes over `total_ms` of short ticks on the test clock, sampled several times so one near-identical pair of frames cannot pass for a freeze.
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

// An effect swapped for another mid-run, as HttpServerModule's replace does, leaves the new one animating.
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

// --- The frame length an effect integrates by, against a fleet-clock step --------------------------

namespace {
/// Step this process's fleet clock `aheadMs` ahead of its own, as a device's first sync does.
void stepFleetClock(int64_t aheadMs) {
    mm::ClockDiscipline d;
    d.steer(static_cast<int64_t>(mm::platform::uptimeUs()), aheadMs * 1000);
    mm::FleetTime::publish(d);
}

/// Lit lights in a layer's buffer.
int litLights(mm::Layer& layer) {
    int lit = 0;
    const uint8_t* b = layer.buffer().data();
    for (size_t i = 0; i < layer.buffer().count(); i++) lit += (b[i * 3] | b[i * 3 + 1] | b[i * 3 + 2]) != 0;
    return lit;
}
}  // namespace

TEST_CASE("a layer's frame length follows the local clock while a fleet-clock step moves its elapsed time") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(8, 8);
    fx.layer.applyState();
    fx.tickAt(1000);
    CHECK(fx.layer.frameMs() == 0);   // the first frame has none before it
    stepFleetClock(86400000);         // a day ahead, the reference's uptime
    fx.tickAt(1020);
    CHECK(fx.layer.elapsed() > 86400000u);   // the fleet time jumped
    CHECK(fx.layer.frameMs() == 20);         // the frame did not
    mm::FleetTime::reset();
}

TEST_CASE("Trails keeps its trail when the fleet clock steps under it, as a device's first sync does") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(16, 16);
    mm::TrailsEffect trails;
    fx.add(trails);
    for (uint32_t t = 1000; t <= 3000; t += 20) fx.tickAt(t);
    const int before = litLights(fx.layer);
    REQUIRE(before > 0);
    stepFleetClock(86400000);
    fx.tickAt(3020);
    CHECK(litLights(fx.layer) * 2 > before);   // one frame's fade, not a wiped or flooded plane
    fx.layer.removeChild(&trails);
    mm::FleetTime::reset();
}
