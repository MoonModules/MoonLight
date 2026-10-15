/// @module RingLayout
/// @also CarLightsLayout, Rings241Layout, SingleColumnLayout, TubesLayout, PanelLayout, PanelsLayout, GridLayout, GridBlacksLayout

#include "doctest.h"
#include "light/layouts/CarLightsLayout.h"
#include "light/layouts/GridBlacksLayout.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"
#include "light/layouts/Rings241Layout.h"
#include "light/layouts/TubesLayout.h"

#include <tuple>
#include <vector>

// A layout built from another layout's geometry places exactly what that layout places: these pin the reuse, so a change to a part's geometry reaches every layout made of it.

namespace {

struct Light { mm::lengthType x, y, z; bool operator==(const Light&) const = default; };
void collect(void* ctx, mm::nrOfLightsType, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<Light>*>(ctx)->push_back({x, y, z});
}
template <typename L>
std::vector<Light> lightsOf(const L& layout) {
    std::vector<Light> v;
    layout.placeLights(mm::CoordSink{collect, nullptr, &v});
    return v;
}
std::vector<Light> ring(mm::lengthType cx, mm::lengthType cy, uint8_t n, uint16_t angleFirst, uint16_t rotation, bool clockwise, uint8_t scale) {
    mm::RingLayout r;
    r.centerX = cx;
    r.centerY = cy;
    r.nrOfLEDs = n;
    r.angleFirst = angleFirst;
    r.rotation = rotation;
    r.clockwise = clockwise;
    r.scale = scale;
    return lightsOf(r);
}

} // namespace

TEST_CASE("a Ring layout without a center places itself by its own size, and one with a center is shifted there") {
    mm::RingLayout r;
    r.nrOfLEDs = 24;
    r.rotation = 90;   // a partial arc, so the count is the filter's
    const auto own = lightsOf(r);
    CHECK(own.size() == r.lightCount());
    const mm::lengthType c = static_cast<mm::lengthType>(1.1f * mm::RingLayout::radius(24));
    CHECK(own == ring(c, c, 24, 0, 90, true, 1));
    const auto moved = ring(c + 5, c + 2, 24, 0, 90, true, 1);
    REQUIRE(moved.size() == own.size());
    for (size_t i = 0; i < own.size(); i++)
        CHECK(moved[i] == Light{static_cast<mm::lengthType>(own[i].x + 5), static_cast<mm::lengthType>(own[i].y + 2), 0});
}

TEST_CASE("car lights are Ring layouts' rings and two strips, each with a quarter arc and an upright") {
    for (uint8_t scale : {1, 2, 3}) {
        mm::CarLightsLayout car;
        car.scale = scale;
        std::vector<Light> want;
        const auto add = [&](const std::vector<Light>& part) { want.insert(want.end(), part.begin(), part.end()); };
        const auto at = [](int x, int y) { return Light{static_cast<mm::lengthType>(x), static_cast<mm::lengthType>(y), 0}; };
        // The four lamps: rings from 90 degrees about each lamp's center, the outer two with a ring of 32 more.
        for (auto [cx, cy, rings] : {std::tuple{20, 8, 5}, std::tuple{9, 6, 6}, std::tuple{34, 8, 5}, std::tuple{45, 6, 6}})
            for (uint8_t n : {1, 8, 12, 16, 24, 32}) {
                if (rings-- == 0) break;
                add(ring(static_cast<mm::lengthType>(cx), static_cast<mm::lengthType>(cy), n, 90, 360, true, scale));
            }
        // The left strip walks back, its arc turns clockwise, and its upright stands at x 0.
        for (int x = 25 * scale; x >= 9 * scale; x--) want.push_back(at(x, 15 * scale));
        add(ring(9, 6, 52, 180, 90, true, scale));
        for (int y = 5; y >= 1; y--) want.push_back(at(0, y * scale));
        // The right strip walks out, its arc turns counter-clockwise, and its upright stands at the far edge.
        for (int x = 28 * scale; x <= 44 * scale; x++) want.push_back(at(x, 15 * scale));
        add(ring(45, 6, 52, 180, 90, false, scale));
        for (int y = 5; y >= 1; y--) want.push_back(at(53 * scale, y * scale));
        const auto v = lightsOf(car);
        CHECK(v.size() == car.lightCount());
        CHECK(v == want);
    }
}

TEST_CASE("the 241 disc is nine Ring layout rings about one center") {
    mm::Rings241Layout disc;
    const auto v = lightsOf(disc);
    REQUIRE(v.size() == 241);
    const auto outer = ring(static_cast<mm::lengthType>(static_cast<uint8_t>(1.1f * mm::RingLayout::radius(60))),
                            static_cast<mm::lengthType>(static_cast<uint8_t>(1.1f * mm::RingLayout::radius(60))), 60, 0, 360, true, disc.scale);
    CHECK(std::vector<Light>(v.end() - 60, v.end()) == outer);
}

TEST_CASE("a tube is a Single Column layout's column at the tube's x") {
    mm::TubesLayout tubes;
    tubes.nrOfTubes = 2;
    tubes.ledsPerTube = 5;
    tubes.tubeDistance = 3;
    tubes.reversed = true;
    mm::SingleColumnLayout column;
    column.height = 5;
    column.xposition = 3;
    column.reversed_order = true;
    const auto v = lightsOf(tubes);
    REQUIRE(v.size() == 10);
    CHECK(std::vector<Light>(v.begin() + 5, v.end()) == lightsOf(column));
}

TEST_CASE("a Panels tile is a Panel layout shifted to its place in the grid") {
    mm::PanelsLayout wall;
    wall.horizontalPanels = 2;
    wall.verticalPanels = 1;
    wall.panelWidth = 4;
    wall.panelHeight = 3;
    mm::PanelLayout panel;
    panel.panelWidth = 4;
    panel.panelHeight = 3;
    const auto v = lightsOf(wall);
    const auto one = lightsOf(panel);
    REQUIRE(v.size() == 2 * one.size());
    for (size_t i = 0; i < one.size(); i++) {
        CHECK(v[i] == one[i]);
        CHECK(v[one.size() + i] == Light{static_cast<mm::lengthType>(one[i].x + 4), one[i].y, one[i].z});
    }
}

TEST_CASE("a GridBlacks without dark columns places what a Grid places") {
    mm::GridBlacksLayout blacks;
    blacks.width = 5;
    blacks.height = 3;
    blacks.serpentine = true;
    mm::GridLayout grid;
    grid.width = 5;
    grid.height = 3;
    grid.serpentine = true;
    CHECK(lightsOf(blacks) == lightsOf(grid));
}

TEST_CASE("a later part continues the numbering and stops at the light cap, the safe-mode boot's 1024") {
    mm::GridLayout first;  first.width = 1023; first.height = 1;
    mm::GridLayout second; second.width = 2;   second.height = 1;
    std::vector<std::tuple<mm::nrOfLightsType, mm::lengthType>> placed;
    mm::CoordSink sink{[](void* c, mm::nrOfLightsType i, mm::lengthType x, mm::lengthType, mm::lengthType) {
        static_cast<std::vector<std::tuple<mm::nrOfLightsType, mm::lengthType>>*>(c)->emplace_back(i, x);
    }, nullptr, &placed};
    mm::LightEmitter e{&sink, 1024};
    e.place(first);
    e.place(second, 0, 1);
    REQUIRE(placed.size() == 1024);
    CHECK(placed.back() == std::make_tuple(mm::nrOfLightsType(1023), mm::lengthType(0)));   // the second part's first light
    for (const auto& [i, x] : placed) CHECK(i < 1024);
}

