/// @module PlasmaEffect
/// @also NoiseEffect

#include "doctest.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/NoiseEffect.h"
#include "golden_frame.h"   // LayerFixture and ScopedTestClock: the layer off the stack, one known moment of the plasma

using mm::golden::cornersDiffer;
using mm::golden::litLights;

// One tick on an 8×8 grid leaves at least one light lit.
TEST_CASE("PlasmaEffect writes non-zero RGB data to buffer") {
    mm::golden::ScopedTestClock clock(500);   // this file's fixed moment, the same frame on every run
    mm::golden::LayerFixture fx(8, 8);
    fx.add(fx.own<mm::PlasmaEffect>());
    fx.layer.tick();
    CHECK(litLights(fx.frame()) > 0);
}

// Opposite corners of a 16×16 grid differ in color (the plasma is not flat-filling).
TEST_CASE("PlasmaEffect produces spatial variation") {
    mm::golden::ScopedTestClock clock(500);   // 500 where the suite uses 1000: the corners match at 1024-1031 ms of each 2,000 ms cycle, 24 ms past 1000
    mm::golden::LayerFixture fx(16, 16);
    fx.add(fx.own<mm::PlasmaEffect>());
    fx.layer.tick();
    CHECK(cornersDiffer(fx.frame()));
}

// Plasma and Noise produce visibly different frames on 8×8 grids at one moment (sanity check that they're distinct algorithms).
TEST_CASE("PlasmaEffect produces different output than NoiseEffect") {
    mm::golden::ScopedTestClock clock(500);   // both frames at one moment
    mm::golden::LayerFixture plasmaFx(8, 8);
    plasmaFx.add(plasmaFx.own<mm::PlasmaEffect>());
    plasmaFx.layer.tick();

    mm::golden::LayerFixture noiseFx(8, 8);
    noiseFx.add(noiseFx.own<mm::NoiseEffect>());
    noiseFx.layer.tick();

    const auto plasma = plasmaFx.frame();
    const auto noise = noiseFx.frame();
    REQUIRE(plasma.size() == noise.size());
    CHECK(plasma != noise);
}
