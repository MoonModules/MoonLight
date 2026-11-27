/// @module Layer
/// @also RainbowEffect, NoiseEffect, PlasmaEffect, SpiralEffect, MetaballsEffect, RingsEffect, RipplesEffect, LavaLampEffect, FireEffect, ParticlesEffect, GameOfLifeEffect, GEQ3DEffect, PaintBrushEffect

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/modifiers/ModifierBase.h"

// Effects work at every grid size: a modifier can shrink the grid to 0,0,0, or every layout can be off, and tick() is then a clean no-op, free of division by zero and of writes out of bounds.

namespace {

template <typename Effect>
void run_with_empty_layout() {
    mm::Layouts layouts;  // no children → totalLightCount() == 0
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    Effect e;
    layer.addChild(&e);
    layouts.applyState();
    layer.applyState();  // logical/physical dims all zero, no buffer
    // The real assertion is "doesn't crash", if tick() reaches a divide-by-zero or an OOB write the process dies before we get here.
    layer.tick();
    CHECK(layer.width() == 0);
    CHECK(layer.height() == 0);
    CHECK(layer.depth() == 0);
}

/// A modifier that only counts its ticks, enough to tell "still running" from "frozen".
class CountingModifier : public mm::ModifierBase {
public:
    void tick() MM_NONBLOCKING override { ticks++; }
    uint32_t ticks = 0;
};

/// A modifier that declares per-frame work, so the Layer's live remap pass is reached, a gate CountingModifier, whose hasModifyLive() is false, leaves closed.
class LiveCountingModifier : public mm::ModifierBase {
public:
    void tick() MM_NONBLOCKING override { ticks++; }
    bool hasModifyLive() const MM_NONBLOCKING override { return true; }
    void modifyLive(mm::Coord3D& pos, const mm::Coord3D& /*logical*/) const override { remaps++; (void)pos; }
    uint32_t ticks = 0;
    mutable uint32_t remaps = 0;
};

} // namespace

// Each per-effect case runs the same probe: build a Layer over an empty Layouts (no children → 0 lights), then prepare() + tick(). The assertion is "no crash, no div-by-zero, no OOB write" plus dims == 0.

// Rainbow on 0,0,0 grid: no crash.
TEST_CASE("RainbowEffect on 0,0,0 grid")     { run_with_empty_layout<mm::RainbowEffect>(); }
// Noise on 0,0,0 grid: no crash.
TEST_CASE("NoiseEffect on 0,0,0 grid")       { run_with_empty_layout<mm::NoiseEffect>(); }
// Plasma on 0,0,0 grid: no crash.
TEST_CASE("PlasmaEffect on 0,0,0 grid")      { run_with_empty_layout<mm::PlasmaEffect>(); }
// Spiral on 0,0,0 grid: no crash.
TEST_CASE("SpiralEffect on 0,0,0 grid")      { run_with_empty_layout<mm::SpiralEffect>(); }
// Metaballs on 0,0,0 grid: no crash.
TEST_CASE("MetaballsEffect on 0,0,0 grid")   { run_with_empty_layout<mm::MetaballsEffect>(); }
// Rings on 0,0,0 grid: no crash.
TEST_CASE("RingsEffect on 0,0,0 grid")       { run_with_empty_layout<mm::RingsEffect>(); }
// Ripples on 0,0,0 grid: no crash.
TEST_CASE("RipplesEffect on 0,0,0 grid")     { run_with_empty_layout<mm::RipplesEffect>(); }
// LavaLamp on 0,0,0 grid: no crash.
TEST_CASE("LavaLampEffect on 0,0,0 grid")    { run_with_empty_layout<mm::LavaLampEffect>(); }
// Fire on 0,0,0 grid: no heat buffer allocated, no crash.
TEST_CASE("FireEffect on 0,0,0 grid")        { run_with_empty_layout<mm::FireEffect>(); }
// Particles on 0,0,0 grid: no trail buffer allocated, no crash.
TEST_CASE("ParticlesEffect on 0,0,0 grid")   { run_with_empty_layout<mm::ParticlesEffect>(); }
// GameOfLife on 0,0,0 grid: no heap alloc for 0 cells, no crash.
TEST_CASE("GameOfLifeEffect on 0,0,0 grid")  { run_with_empty_layout<mm::GameOfLifeEffect>(); }
// GEQ3D / PaintBrush on 0,0,0 grid: audio effects, no crash with no buffer.
TEST_CASE("GEQ3DEffect on 0,0,0 grid")       { run_with_empty_layout<mm::GEQ3DEffect>(); }
TEST_CASE("PaintBrushEffect on 0,0,0 grid")  { run_with_empty_layout<mm::PaintBrushEffect>(); }

// A modifier's per-frame state keeps moving while the grid is empty, so a beat-driven modifier comes back in phase when the layout returns: the empty interval passes through the chain.
TEST_CASE("modifiers keep ticking while the grid is empty") {
    mm::Layouts layouts;              // no children → 0 lights, so the effect pass is skipped
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    CountingModifier mod;
    layer.addChild(&mod);
    layouts.applyState();
    layer.applyState();

    REQUIRE(layer.width() == 0);      // the precondition the test is about
    for (int i = 0; i < 5; i++) layer.tick();
    CHECK(mod.ticks == 5);            // every frame reached the modifier
}

// A zero channel count is refused at the setter, the one entry point, so effects and draw primitives assume `cpl >= 1` rather than meeting a zero-byte buffer and a per-light stride of 0.
TEST_CASE("a layer refuses a zero channel count") {
    mm::Layouts layouts;
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    const uint8_t before = layer.channelsPerLight();
    layer.setChannelsPerLight(0);
    CHECK(layer.channelsPerLight() == before);   // unchanged, not zeroed
    layer.setChannelsPerLight(4);
    CHECK(layer.channelsPerLight() == 4);        // a valid value still applies
}

// The live pass walks the modifier mapping into the buffer, so an empty layout has nothing for it to remap. It is gated on hasGrid alongside the effect pass; the modifier ticks still run.
TEST_CASE("a live modifier is skipped on an empty grid without crashing") {
    mm::Layouts layouts;                 // no children -> 0 lights
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    LiveCountingModifier mod;            // declares live work, so the REMAP gate is reached
    layer.addChild(&mod);
    layouts.applyState();
    layer.applyState();

    REQUIRE(layer.width() == 0);
    for (int i = 0; i < 5; i++) layer.tick();
    CHECK(mod.ticks == 5);               // modifiers still advance...
    CHECK(mod.remaps == 0);              // ...but with no lights there is nothing to remap
}
