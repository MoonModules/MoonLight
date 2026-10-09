/// @module Layouts

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"

#include <string>
#include <vector>

// Layouts skips disabled children in totalLightCount and placeLights, and later enabled children shift down to close the gap, as Layer, Effects and Drivers gate theirs.

namespace {

struct Sample {
    mm::nrOfLightsType idx;
    mm::lengthType x, y, z;
};

void collect(void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<Sample>*>(ctx)->push_back({idx, x, y, z});
}

} // namespace

// Disabled layouts contribute nothing; enabled siblings shift down to close the gap (no index holes).
TEST_CASE("Layouts skips disabled children and shifts indices") {
    mm::Layouts layouts;
    mm::GridLayout a;
    a.width = 4; a.height = 1; a.depth = 1;   // 4 lights, indices 0..3 when active
    mm::GridLayout b;
    b.width = 2; b.height = 1; b.depth = 1;   // 2 lights, indices 4..5 when both active
    layouts.addChild(&a);
    layouts.addChild(&b);

    // Both enabled: 6 lights total, A occupies 0..3, B occupies 4..5.
    CHECK(layouts.totalLightCount() == 6);
    std::vector<Sample> samples;
    layouts.placeLights(mm::CoordSink{collect, nullptr, &samples});
    REQUIRE(samples.size() == 6);
    CHECK(samples[0].idx == 0);
    CHECK(samples[3].idx == 3);
    CHECK(samples[4].idx == 4);
    CHECK(samples[5].idx == 5);

    // Disable A: total drops to B's 2 lights and B shifts to indices 0..1.
    a.setEnabled(false);
    CHECK(layouts.totalLightCount() == 2);
    samples.clear();
    layouts.placeLights(mm::CoordSink{collect, nullptr, &samples});
    REQUIRE(samples.size() == 2);
    CHECK(samples[0].idx == 0);
    CHECK(samples[1].idx == 1);
    // Coordinates are B's (a 2-wide grid starting at x=0), not A's.
    CHECK(samples[0].x == 0);
    CHECK(samples[1].x == 1);

    // Disable B too: zero lights.
    b.setEnabled(false);
    CHECK(layouts.totalLightCount() == 0);
    samples.clear();
    layouts.placeLights(mm::CoordSink{collect, nullptr, &samples});
    CHECK(samples.empty());

    // Re-enable both: original layout restored.
    a.setEnabled(true);
    b.setEnabled(true);
    CHECK(layouts.totalLightCount() == 6);
    samples.clear();
    layouts.placeLights(mm::CoordSink{collect, nullptr, &samples});
    REQUIRE(samples.size() == 6);
    CHECK(samples[0].idx == 0);
    CHECK(samples[5].idx == 5);
}

// Disabling the Layouts container itself zeroes totalLightCount and yields no coordinates.
TEST_CASE("Disabling the Layouts container reports zero lights and an empty iteration") {
    // The Scheduler can't gate Layouts (no tick() to skip) so totalLightCount / placeLights apply the gate themselves. Same universal-enable intent as every other container: disabled means no contribution.
    mm::Layouts layouts;
    mm::GridLayout g;
    g.width = 3; g.height = 1; g.depth = 1;
    layouts.addChild(&g);

    CHECK(layouts.totalLightCount() == 3);

    layouts.setEnabled(false);
    CHECK(layouts.totalLightCount() == 0);
    std::vector<Sample> samples;
    layouts.placeLights(mm::CoordSink{collect, nullptr, &samples});
    CHECK(samples.empty());

    layouts.setEnabled(true);
    CHECK(layouts.totalLightCount() == 3);
}

// A layout too large for the device's memory crashes it at every boot, so safe mode places only the first 1,024 lights and says so.
TEST_CASE("safe mode places the first 1024 lights of a larger layout") {
    struct Record { ~Record() { mm::platform::setTestBootRecord({}); } } guard;
    mm::platform::setTestBootRecord({0, 2});
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 64; grid.height = 64; grid.depth = 1;
    layouts.addChild(&grid);
    CHECK(layouts.totalLightCount() == mm::Layouts::kSafeModeLights);
    std::vector<Sample> samples;
    layouts.placeLights(mm::CoordSink{collect, nullptr, &samples});
    CHECK(samples.size() == mm::Layouts::kSafeModeLights);
    layouts.prepare();
    REQUIRE(layouts.status() != nullptr);
    CHECK(std::string(layouts.status()).find("safe mode") != std::string::npos);
    layouts.removeChild(&grid);
}
