/// @module MappingLUT

#include "doctest.h"
#include "light/layers/MappingLUT.h"
#include "platform/platform.h"

#include <vector>

// Restores the maxAllocBlock cap to 0 (unlimited) on scope exit so a forced-paging case can't leak the cap into later tests.
struct BlockCapGuard {
    ~BlockCapGuard() { mm::platform::setTestMaxAllocBlock(0); }
};

// A fresh LUT carries no mapping (hasLUT==false, logicalCount==0); BlendMap takes the fast identity copy path.
TEST_CASE("MappingLUT default is identity (no LUT)") {
    mm::MappingLUT lut;
    CHECK(!lut.hasLUT());
    CHECK(lut.logicalCount() == 0);
}

// setIdentity(N) declares a 1:1 mapping for N lights without allocating a LUT; forEachDestination still iterates correctly.
TEST_CASE("MappingLUT setIdentity") {
    mm::MappingLUT lut;
    lut.setIdentity(256);
    CHECK(!lut.hasLUT());
    CHECK(lut.logicalCount() == 256);

    // forEachDestination in identity mode returns the logical index
    std::vector<mm::nrOfLightsType> dests;
    lut.forEachDestination(42, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 1);
    CHECK(dests[0] == 42);
}

// Each logical light can map to a different count of physical lights; forEachDestination yields every mapped index in order.
TEST_CASE("MappingLUT 1:N mapping") {
    mm::MappingLUT lut;
    // 4 logical lights, each maps to different number of physical lights
    CHECK(lut.build(4, 10));
    CHECK_FALSE(!lut.hasLUT());

    // Logical 0 → physical {0, 3}
    mm::nrOfLightsType map0[] = {0, 3};
    lut.setMapping(0, map0, 2);

    // Logical 1 → physical {1, 2, 5, 7}
    mm::nrOfLightsType map1[] = {1, 2, 5, 7};
    lut.setMapping(1, map1, 4);

    // Logical 2 → physical {4}
    mm::nrOfLightsType map2[] = {4};
    lut.setMapping(2, map2, 1);

    // Logical 3 → physical {6, 8, 9}
    mm::nrOfLightsType map3[] = {6, 8, 9};
    lut.setMapping(3, map3, 3);

    lut.finalize();

    CHECK(lut.logicalCount() == 4);
    CHECK(lut.destinationCount() == 10);

    // Verify each logical index maps correctly
    std::vector<mm::nrOfLightsType> dests;

    dests.clear();
    lut.forEachDestination(0, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 2);
    CHECK(dests[0] == 0);
    CHECK(dests[1] == 3);

    dests.clear();
    lut.forEachDestination(1, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 4);
    CHECK(dests[0] == 1);
    CHECK(dests[1] == 2);
    CHECK(dests[2] == 5);
    CHECK(dests[3] == 7);

    dests.clear();
    lut.forEachDestination(2, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 1);
    CHECK(dests[0] == 4);

    dests.clear();
    lut.forEachDestination(3, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 3);
    CHECK(dests[0] == 6);
    CHECK(dests[1] == 8);
    CHECK(dests[2] == 9);
}

// When no single contiguous block fits (forced via the test cap) but total heap allows it, build() pages the destinations array. The mapping must read back identically to a single-alloc build, paging is an allocation detail, not a behavior change. isPaged() confirms the fallback actually engaged.
TEST_CASE("MappingLUT paged destinations read back identically") {
    BlockCapGuard guard;
    // Cap the largest block well below a 5000-destination array (5000 × sizeof(nrOfLightsType)) so tier-1 fails and tier-2 pages.
    mm::platform::setTestMaxAllocBlock(4096);

    mm::MappingLUT lut;
    REQUIRE(lut.build(3, 5000));
    CHECK(lut.isPaged());  // the fallback engaged

    // Logical 1's run spans well past one 4096-entry page → exercises the page-boundary walk in forEachDestination. Build runs of known values.
    std::vector<mm::nrOfLightsType> run0, run1, run2;
    for (mm::nrOfLightsType i = 0; i < 100; i++) run0.push_back(i);
    for (mm::nrOfLightsType i = 0; i < 4000; i++) run1.push_back(static_cast<mm::nrOfLightsType>(1000 + i)); // crosses the 4096 page boundary
    for (mm::nrOfLightsType i = 0; i < 50; i++) run2.push_back(static_cast<mm::nrOfLightsType>(9000 + i));

    lut.setMapping(0, run0.data(), static_cast<mm::nrOfLightsType>(run0.size()));
    lut.setMapping(1, run1.data(), static_cast<mm::nrOfLightsType>(run1.size()));
    lut.setMapping(2, run2.data(), static_cast<mm::nrOfLightsType>(run2.size()));
    lut.finalize();

    auto collect = [&](mm::nrOfLightsType li) {
        std::vector<mm::nrOfLightsType> out;
        lut.forEachDestination(li, [&](mm::nrOfLightsType idx) { out.push_back(idx); });
        return out;
    };
    CHECK(collect(0) == run0);
    CHECK(collect(1) == run1);  // the boundary-straddling run round-trips exactly
    CHECK(collect(2) == run2);
    CHECK(lut.destinationCount() == run0.size() + run1.size() + run2.size());
}

// build() returns false on genuine exhaustion, total free heap (minus the reserve) can't hold the destinations, so the caller degrades to 1:1. Forced here via a non-zero freeHeap is desktop-only-unavailable, so this case pins the paged path's success and the boundary; the tier-3 false path is covered by the Layer sparse-mapping degrade test on real heap limits.
TEST_CASE("MappingLUT single-block path when a block fits (not paged)") {
    BlockCapGuard guard;
    mm::platform::setTestMaxAllocBlock(1 << 20);  // 1 MB block — fits any small LUT
    mm::MappingLUT lut;
    REQUIRE(lut.build(1, 10));
    CHECK_FALSE(lut.isPaged());  // single contiguous block taken
    mm::nrOfLightsType m[] = {0, 3};
    lut.setMapping(0, m, 2);
    lut.finalize();
    std::vector<mm::nrOfLightsType> out;
    lut.forEachDestination(0, [&](mm::nrOfLightsType idx) { out.push_back(idx); });
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 0);
    CHECK(out[1] == 3);
}

// free() releases memory and resets counts; build() can be called again to install a fresh mapping.
TEST_CASE("MappingLUT free and rebuild") {
    mm::MappingLUT lut;
    lut.build(4, 8);
    lut.free();
    CHECK(!lut.hasLUT());
    CHECK(lut.logicalCount() == 0);

    // Can rebuild after free
    CHECK(lut.build(2, 4));
    mm::nrOfLightsType map[] = {0, 1};
    lut.setMapping(0, map, 2);
    lut.setMapping(1, map, 2);
    lut.finalize();
    CHECK(lut.logicalCount() == 2);
}

// A region moving every frame rebuilds the table each time, so a rebuild at about the same size keeps its blocks and maps as a fresh table does.
TEST_CASE("MappingLUT rebuild at the same size keeps its blocks") {
    BlockCapGuard guard;
    mm::platform::setTestMaxAllocBlock(1 << 20);
    mm::MappingLUT lut;
    REQUIRE(lut.build(4, 8));
    const size_t bytes = mm::platform::allocatedBytes();
    const uint32_t blocks = mm::platform::allocatedCount();
    lut.setOverwrites(false);

    REQUIRE(lut.build(4, 8));
    CHECK(mm::platform::allocatedBytes() == bytes);
    CHECK(mm::platform::allocatedCount() == blocks);
    CHECK(lut.overwrites());   // a rebuild starts as a fresh table
    mm::nrOfLightsType m[] = {2, 5};
    for (mm::nrOfLightsType i = 0; i < 4; i++) lut.setMapping(i, m, 2);
    lut.finalize();
    std::vector<mm::nrOfLightsType> out;
    lut.forEachDestination(3, [&](mm::nrOfLightsType idx) { out.push_back(idx); });
    REQUIRE(out.size() == 2);
    CHECK(out[1] == 5);

    REQUIRE(lut.build(40, 80));   // grown: allocated afresh
    CHECK(mm::platform::allocatedBytes() > bytes);
}

// A layer's fold meets its lights in wire order, not logical order, so the table is built in place: counted, turned into runs, then each light placed in its cell's run, with no copy of the table. It maps as one filled in logical order does, single block or paged.
TEST_CASE("MappingLUT built in place maps as one filled in logical order") {
    BlockCapGuard guard;
    for (const size_t cap : {size_t{1} << 20, size_t{16}}) {   // a single block, then pages: 20 bytes of destinations above a 16-byte block
        mm::platform::setTestMaxAllocBlock(cap);
        // Wire order (logical, physical): cell 2 first, cell 0 split around it, cell 1 empty.
        const std::vector<std::pair<mm::nrOfLightsType, mm::nrOfLightsType>> wire = {{2, 7}, {0, 3}, {2, 8}, {0, 4}, {3, 9}};
        mm::MappingLUT lut;
        REQUIRE(lut.beginBuild(4));
        for (const auto& [li, dest] : wire) lut.count(li);
        REQUIRE(lut.startRuns());
        for (const auto& [li, dest] : wire) lut.place(li, dest);
        lut.endBuild();

        CHECK(lut.hasLUT());
        CHECK(lut.isPaged() == (cap == 16));
        CHECK(lut.destinationCount() == 5);
        const std::vector<std::vector<mm::nrOfLightsType>> want = {{3, 4}, {}, {7, 8}, {9}};
        for (mm::nrOfLightsType li = 0; li < 4; li++) {
            std::vector<mm::nrOfLightsType> got;
            lut.forEachDestination(li, [&](mm::nrOfLightsType idx) { got.push_back(idx); });
            CHECK(got == want[li]);
        }
    }
}
