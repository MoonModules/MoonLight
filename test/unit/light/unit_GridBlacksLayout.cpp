/// @module GridBlacksLayout
/// @also Layouts, GridLayout

#include "doctest.h"
#include "light/layouts/GridBlacksLayout.h"

#include <vector>

// GridBlacks is a Grid with mid-strand dark columns: gaps, wire slots the driver clocks that map to no logical light. Gaps arrive through sink.addGap() at their true position, and the physical index advances across them.

namespace {

struct GapEntry { mm::nrOfLightsType idx; mm::lengthType x, y, z; bool black; };
void collectLit(void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<GapEntry>*>(ctx)->push_back({idx, x, y, z, false});
}
void collectBlack(void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<GapEntry>*>(ctx)->push_back({idx, x, y, z, true});
}
std::vector<GapEntry> walk(const mm::GridBlacksLayout& g) {
    std::vector<GapEntry> v;
    g.placeLights(mm::CoordSink{collectLit, collectBlack, &v});
    return v;
}

} // namespace

// A dark column run [blackStart, blackStart+blackCount) is black in every row; the index advances across gaps and lit columns past the run keep their true x, so the picture is holed with every column in place.
TEST_CASE("GridBlacks black columns emit gaps at their true position") {
    mm::GridBlacksLayout grid;
    grid.width = 5;
    grid.height = 2;
    grid.depth = 1;
    grid.blackStart = 2;   // columns 2,3 dark
    grid.blackCount = 2;

    // Gaps are physical slots: lightCount (the wire/driver count) is the full box.
    CHECK(grid.lightCount() == 10);
    CHECK(grid.hasGaps());

    auto v = walk(grid);
    REQUIRE(v.size() == 10);                 // every cell emitted, lit and gap alike
    // Row 0: x=0,1 lit; x=2,3 gap; x=4 lit, indices contiguous 0..4.
    CHECK(v[0].x == 0); CHECK_FALSE(v[0].black); CHECK(v[0].idx == 0);
    CHECK(v[1].x == 1); CHECK_FALSE(v[1].black);
    CHECK(v[2].x == 2); CHECK(v[2].black);       CHECK(v[2].idx == 2);   // gap keeps its index
    CHECK(v[3].x == 3); CHECK(v[3].black);
    CHECK(v[4].x == 4); CHECK_FALSE(v[4].black); CHECK(v[4].idx == 4);   // lit past the gap, true x
    // Row 1 mirrors row 0, offset by width.
    CHECK(v[7].x == 2); CHECK(v[7].black);
    CHECK(v[9].x == 4); CHECK_FALSE(v[9].black); CHECK(v[9].idx == 9);
}

// The gap test is on the TRUE x, not the wire order, so a serpentine strip keeps the same physical columns dark whichever way it snakes into a row.
TEST_CASE("GridBlacks black columns hold on the true column under serpentine") {
    mm::GridBlacksLayout grid;
    grid.width = 4;
    grid.height = 2;
    grid.serpentine = true;
    grid.blackStart = 0;   // column 0 dark
    grid.blackCount = 1;

    auto v = walk(grid);
    REQUIRE(v.size() == 8);
    // Row 0 (L→R): x=0 first, is the gap.
    CHECK(v[0].x == 0); CHECK(v[0].black);
    CHECK(v[1].x == 1); CHECK_FALSE(v[1].black);
    // Row 1 (R→L under serpentine): x walks 3,2,1,0, the gap is the LAST emission of the row, but still column 0. Index advances linearly; the dark column is unchanged.
    CHECK(v[4].x == 3); CHECK_FALSE(v[4].black);
    CHECK(v[7].x == 0); CHECK(v[7].black);       // true column 0, emitted last in the reversed row
}

// No black run → no gaps, and the walk is byte-identical to a plain grid (a GridBlacks with blackCount 0 renders exactly like a Grid). Every slot reaches the light handler; hasGaps is false.
TEST_CASE("GridBlacks with no black run emits zero gaps") {
    mm::GridBlacksLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.blackCount = 0;   // the default

    CHECK_FALSE(grid.hasGaps());
    auto v = walk(grid);
    REQUIRE(v.size() == 16);
    for (const auto& e : v) CHECK_FALSE(e.black);   // every emission is a light
}

// Robustness: a black run wider than the grid darkens every column (whole grid dark, no crash), and a run starting past the right edge darkens nothing.
TEST_CASE("GridBlacks black run tolerates out-of-range bounds") {
    {   // all columns dark
        mm::GridBlacksLayout grid;
        grid.width = 3; grid.height = 2;
        grid.blackStart = 0; grid.blackCount = 100;   // > width
        auto v = walk(grid);
        REQUIRE(v.size() == 6);
        for (const auto& e : v) CHECK(e.black);        // every cell is a gap, none crash
    }
    {   // run starts past the last column → nothing dark
        mm::GridBlacksLayout grid;
        grid.width = 3; grid.height = 2;
        grid.blackStart = 10; grid.blackCount = 5;     // beyond width
        auto v = walk(grid);
        REQUIRE(v.size() == 6);
        for (const auto& e : v) CHECK_FALSE(e.black);
    }
}
