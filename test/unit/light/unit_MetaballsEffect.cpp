/// @module MetaballsEffect

#include "doctest.h"
#include "light/effects/MetaballsEffect.h"
#include "golden_frame.h"   // LayerFixture and ScopedTestClock: the layer off the stack, one known moment of the orbit

using mm::golden::cornersDiffer;
using mm::golden::litLights;

// One tick on a 16×16 grid leaves at least one light lit (proves the effect rendered).
TEST_CASE("MetaballsEffect writes non-zero RGB data to buffer") {
    mm::golden::ScopedTestClock clock(1000);   // the suite's fixed moment, the same frame on every run
    mm::golden::LayerFixture fx(16, 16);
    fx.add(fx.own<mm::MetaballsEffect>());
    fx.layer.tick();
    CHECK(litLights(fx.frame()) > 0);
}

// Pixels at opposite corners of a 32×32 grid differ in color (the effect is not flat-filling the buffer).
TEST_CASE("MetaballsEffect produces spatial variation") {
    mm::golden::ScopedTestClock clock(1000);   // the corners match at a few moments of the orbit, so the test picks one where they differ
    mm::golden::LayerFixture fx(32, 32);
    fx.add(fx.own<mm::MetaballsEffect>());
    fx.layer.tick();
    CHECK(cornersDiffer(fx.frame()));
}
