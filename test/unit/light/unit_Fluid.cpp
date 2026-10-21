/// @module Fluid
/// @also draw, FluidEffect

/// The stable-fluid solver.
/// What matters is not that it computes something, but the three properties that separate a fluid from a field of arrows: it stays divergence-free (so what it carries neither piles up nor drains away), a push actually moves the medium downstream, and a field at rest STAYS at rest rather than drifting on its own rounding.

#include "doctest.h"
#include "light/powerfunctions/fluid.h"
#include "core/module/MoonModule.h"
#include "golden_frame.h"                 // the effect harness: Layouts, Grid, Layer
#include "light/effects/FluidEffect.h"

#include <cmath>
#include <cstdlib>

using namespace mm;

namespace {
/// A bare owner: ScratchBuffer needs a module to register with, and the solver needs nothing else.
struct Owner : MoonModule {
    const char* name() const { return "fluidTest"; }
};

/// The worst absolute divergence over the interior.
/// Zero is a perfect flow; the solver relaxes toward it rather than reaching it exactly, which is what the tolerance below is about.
int64_t worstDivergence(const Fluid& f) {
    const int32_t* vx = f.velocityX();
    const int32_t* vy = f.velocityY();
    const lengthType w = f.width(), h = f.height();
    int64_t worst = 0;
    for (lengthType y = 1; y < h - 1; y++)
        for (lengthType x = 1; x < w - 1; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const int64_t d = static_cast<int64_t>(vx[i + 1]) - vx[i - 1]
                            + vy[i + w] - vy[i - w];
            const int64_t a = d < 0 ? -d : d;
            if (a > worst) worst = a;
        }
    return worst;
}
}  // namespace

TEST_CASE("each step drives the flow further toward divergence-free") {
    // The property `project` exists for, stated as the method actually promises it.
    // A single pass cannot cancel a point impulse: the discrete pressure gradient is spread over neighboring cells while the push is concentrated in one, so a float reference with the same scaling also leaves about 60% of it after one pass.
    // What must hold is that the solver CONVERGES, and that is what this pins: the divergence falls with every step and ends far below where it began.
    Owner owner;
    Fluid fluid(owner);
    REQUIRE(fluid.resize(24, 24));

    for (int i = 0; i < 8; i++) fluid.addVelocity(12, 12, 3 * Fluid::kOne, Fluid::kOne);
    const int64_t pushed = worstDivergence(fluid);
    REQUIRE(pushed > 0);

    int64_t previous = pushed;
    for (int f = 0; f < 8; f++) {
        fluid.step(0, Fluid::kOne / 60, 5);
        const int64_t now = worstDivergence(fluid);
        // Never materially worse. Once it has settled the last bits wobble by a thousand or so, which is integer rounding in the relaxation rather than divergence, so the bound is generous in absolute terms and still catches a solver that is actually growing: a diverging one doubles rather than drifting.
        CHECK(now <= previous + previous / 4 + 2000);
        previous = now;
    }
    // And it settles to where only the PRESSURE SOLVE can take it. This bound is what makes the test about `project` rather than about advection: measured over these 8 steps, the worst divergence falls from 1572864 to about 13000 with the solve and stalls near 253000 without it, so a bound of a hundredth passes only when the projection is actually running.
    CHECK(previous < pushed / 100);
}

TEST_CASE("more solver iterations leave less divergence, so the cost knob buys correctness") {
    // `iterations` is the honest cost control: it is a relaxation, so more passes converge further. If this did not hold, the knob would be paying for nothing.
    Owner owner;
    Fluid coarse(owner), fine(owner);
    REQUIRE(coarse.resize(24, 24));
    REQUIRE(fine.resize(24, 24));
    for (int i = 0; i < 8; i++) {
        coarse.addVelocity(12, 12, 3 * Fluid::kOne, Fluid::kOne);
        fine.addVelocity(12, 12, 3 * Fluid::kOne, Fluid::kOne);
    }
    coarse.step(0, Fluid::kOne / 60, 1);
    fine.step(0, Fluid::kOne / 60, 20);
    CHECK(worstDivergence(fine) <= worstDivergence(coarse));
}

TEST_CASE("a jet carries the medium downstream, so a push is felt where it points") {
    // A fluid must TRANSPORT. Push right at one place and the velocity a few cells to the right must pick it up: without advection the push would stay where it was made.
    Owner owner;
    Fluid fluid(owner);
    REQUIRE(fluid.resize(32, 16));
    const lengthType jetX = 6, jetY = 8;
    for (int f = 0; f < 30; f++) {
        for (int i = 0; i < 4; i++) fluid.addVelocity(jetX, jetY, 4 * Fluid::kOne, 0);
        fluid.step(0, Fluid::kOne / 60, 5);
    }
    const int32_t* vx = fluid.velocityX();
    const size_t downstream = static_cast<size_t>(jetY) * 32 + (jetX + 5);
    CHECK(vx[downstream] > 0);        // the medium is moving right, well past the jet itself
}

TEST_CASE("a fluid at rest stays at rest, so an idle fixture does not drift") {
    // Nothing pushed, so nothing may move. A solver that leaks energy from its own rounding would show a fixture creeping while the user is doing nothing at all.
    Owner owner;
    Fluid fluid(owner);
    REQUIRE(fluid.resize(16, 16));
    for (int f = 0; f < 60; f++) fluid.step(Fluid::kOne / 100, Fluid::kOne / 60, 5);
    const int32_t* vx = fluid.velocityX();
    const int32_t* vy = fluid.velocityY();
    for (size_t i = 0; i < 16 * 16; i++) {
        CHECK(vx[i] == 0);
        CHECK(vy[i] == 0);
    }
}

TEST_CASE("a long stall leaves a plausible field rather than infinities") {
    // The reason this solver and not an explicit one. It is unconditionally stable, so a frame that took a whole second resumes with a field a viewer would accept instead of a broken fixture.
    Owner owner;
    Fluid fluid(owner);
    REQUIRE(fluid.resize(16, 16));
    for (int i = 0; i < 16; i++) fluid.addVelocity(8, 8, 6 * Fluid::kOne, 6 * Fluid::kOne);
    fluid.step(0, Fluid::kOne, 5);            // dt of a FULL SECOND

    const int32_t* vx = fluid.velocityX();
    for (size_t i = 0; i < 16 * 16; i++) {
        CHECK(std::abs(vx[i]) < 100 * Fluid::kOne);   // bounded, not exploded
    }
}

TEST_CASE("a grid too small to have an interior is refused rather than half-built") {
    Owner owner;
    Fluid fluid(owner);
    CHECK_FALSE(fluid.resize(2, 2));          // no interior cell at all
    CHECK_FALSE(fluid.valid());
    CHECK(fluid.resize(8, 8));
    CHECK(fluid.valid());
    fluid.release();
    CHECK_FALSE(fluid.valid());
}

// The release path.
// MoonModule::release() frees every ScratchBuffer a module registered, and it runs whenever the module or an ancestor is disabled.
// The re-enable only REQUESTS a prepare for the next scheduler loop, and Layer::tick runs every child whose enabled flag is set, so one frame can tick between the release and the prepare that rebuilds the grids.
// That frame crashed the desktop with a null dye plane (2026-09-05).
// The solver and the effect both read readiness from their buffers now.

TEST_CASE("a fluid whose grids were released reports itself not ready, and resizing to the same shape rebuilds them") {
    Owner o; Fluid f(o);
    REQUIRE(f.resize(16, 16));
    REQUIRE(f.valid());
    o.release();                              // what a disabled ancestor does to every buffer
    CHECK_FALSE(f.valid());
    CHECK(f.velocityX() == nullptr);
    f.step(0, Fluid::kOne / 50, 5);           // must be a no-op rather than a write through null
    f.addVelocity(8, 8, Fluid::kOne, 0);
    // prepare() runs again with the SAME grid: the shape alone must not pass as "already built".
    REQUIRE(f.resize(16, 16));
    CHECK(f.valid());
    CHECK(f.velocityX() != nullptr);
}

TEST_CASE("Fluid ticks dark rather than crashing on the frame between a release and its prepare, then renders again") {
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; FluidEffect effect;
    grid.width = 32; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();
    for (uint16_t i = 0; i < 20; i++) { platform::setTestNowMs(1000 + i * 20u); layer.tick(); }

    effect.release();                         // the disable, with the enabled flag still set
    platform::setTestNowMs(1500);
    layer.tick();                             // the frame before the requested prepare: no crash

    layer.applyState();                       // the prepare the scheduler services next loop
    bool lit = false;
    for (uint16_t i = 0; i < 40 && !lit; i++) {
        platform::setTestNowMs(1600 + i * 20u);
        layer.tick();
        const auto& buf = layer.buffer();
        for (size_t b = 0; b < buf.bytes(); b++) if (buf.data()[b]) { lit = true; break; }
    }
    CHECK(lit);
}


TEST_CASE("Fluid reshaped to the same light count starts from black rather than the old layout's dye") {
    // 8x16 to 16x8: the sample count is identical, so resize() keeps the buffer and its contents, which are laid out for the OLD geometry.
    // Both planes must be cleared, because the ping-pong swaps the spare one in on the very next frame.
    // Trails and Nebula carry the same guard.
    //
    // The check reads the planes through prepare() alone, with no tick in between: a rendered frame pours fresh dye on top and would hide a stale plane behind it.
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; FluidEffect effect;
    grid.width = 8; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();
    for (uint16_t i = 0; i < 40; i++) { platform::setTestNowMs(1000 + i * 20u); layer.tick(); }

    uint64_t before = 0;
    for (size_t k = 0; k < effect.dyeSamples(); k++) before += effect.dyeAt(k);
    REQUIRE(before > 0);                           // there is dye that could carry over

    grid.width = 16; grid.height = 8;              // the same count, transposed
    layer.applyState();
    uint64_t after = 0;
    for (size_t k = 0; k < effect.dyeSamples(); k++) after += effect.dyeAt(k);
    CHECK(after == 0);                             // both planes cleared for the new geometry
}
