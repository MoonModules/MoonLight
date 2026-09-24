/// @module RectangleLayout
///
/// Pins the hollow-rectangle perimeter walk: the corner-counted-once light count, the reference clockwise-from-top-left order, the degenerate line cases, and the eight wiring permutations.
///
/// @moreinfo
///
/// The wiring controls must reorder INDICES only, since the set of emitted coordinates is a property of the box and must be byte-identical however the strip is wired.

#include "doctest.h"
#include "light/layouts/RectangleLayout.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using mm::RectangleLayout;

namespace {

// Collect (index → position) by walking the layout the way the Layer's LUT build does.
std::vector<std::pair<int, int>> walk(const RectangleLayout& r) {
    std::vector<std::pair<int, int>> out;
    mm::CoordSink sink{
        [](void* ctx, mm::nrOfLightsType, mm::lengthType x, mm::lengthType y, mm::lengthType) {
            static_cast<std::vector<std::pair<int, int>>*>(ctx)->push_back({x, y});
        },
        nullptr, &out};
    r.placeLights(sink);
    return out;
}

}  // namespace

// A rectangle is FLAT, with every light at z = 0. Nothing in the x/y checks below would notice a stray depth, but a non-zero z inflates the bounding box, so the Layer allocates `depth` times over.
TEST_CASE("RectangleLayout: every light is emitted flat at z = 0") {
    RectangleLayout r;
    r.width = 7; r.height = 5;
    int maxZ = -1;
    mm::CoordSink sink{
        [](void* ctx, mm::nrOfLightsType, mm::lengthType, mm::lengthType, mm::lengthType z) {
            int& m = *static_cast<int*>(ctx);
            if (z > m) m = static_cast<int>(z);
        },
        nullptr, &maxZ};
    r.placeLights(sink);
    CHECK(maxZ == 0);
}

// The perimeter of a w×h box counts each corner once: 2·(w+h) − 4. A strip bent around a frame has exactly one LED in each corner, even though that corner belongs to two edges.
TEST_CASE("RectangleLayout: light count is the perimeter with corners counted once") {
    RectangleLayout r;
    r.width = 4; r.height = 3;
    CHECK(r.lightCount() == 10);      // 2*(4+3) - 4
    r.width = 32; r.height = 18;      // the 16:9 default
    CHECK(r.lightCount() == 96);
    r.width = 2; r.height = 2;
    CHECK(r.lightCount() == 4);       // the smallest real rectangle
}

// lightCount() must agree with what placeLights() actually emits, or the Layer allocates a buffer of one size and the LUT build walks another.
TEST_CASE("RectangleLayout: emitted light count matches lightCount()") {
    RectangleLayout r;
    r.width = 7; r.height = 5;
    CHECK(walk(r).size() == r.lightCount());
}

// The reference walk: clockwise from the top-left corner, along the top edge first.
TEST_CASE("RectangleLayout: default walk runs clockwise from the top-left corner") {
    RectangleLayout r;
    r.width = 4; r.height = 3;        // 10 lights
    const auto p = walk(r);
    REQUIRE(p.size() == 10);
    // top edge, left to right
    CHECK(p[0] == std::pair{0, 0});
    CHECK(p[1] == std::pair{1, 0});
    CHECK(p[2] == std::pair{2, 0});
    CHECK(p[3] == std::pair{3, 0});
    // right edge, top to bottom (the top-right corner was already emitted)
    CHECK(p[4] == std::pair{3, 1});
    CHECK(p[5] == std::pair{3, 2});
    // bottom edge, right to left
    CHECK(p[6] == std::pair{2, 2});
    CHECK(p[7] == std::pair{1, 2});
    CHECK(p[8] == std::pair{0, 2});
    // left edge, bottom to top: one cell, both its corners already placed
    CHECK(p[9] == std::pair{0, 1});
}

// Every light lands on its own cell: the walk goes round the frame exactly once. A duplicate would mean two LEDs mapped to one logical position (one of them dark), a gap would mean an unlit LED.
TEST_CASE("RectangleLayout: every light occupies a distinct perimeter cell") {
    RectangleLayout r;
    r.width = 9; r.height = 6;
    const auto p = walk(r);
    const std::set<std::pair<int, int>> unique(p.begin(), p.end());
    CHECK(unique.size() == p.size());
    // and none of them is an interior cell: this is a HOLLOW rectangle
    for (const auto& [x, y] : p)
        CHECK((x == 0 || x == r.width - 1 || y == 0 || y == r.height - 1));
}

// startCorner and clockwise change the WIRING rather than the shape, so only the index order differs. Every position is compared rather than the set, since a set has no order.
TEST_CASE("RectangleLayout: each of the eight wirings emits the reference walk in its own order") {
    const int w = 6, h = 4;
    RectangleLayout ref;
    ref.width = w; ref.height = h;   // top-left, clockwise, shared corners: the reference
    const auto base = walk(ref);
    const size_t n = base.size();
    REQUIRE(n == static_cast<size_t>(2 * w + 2 * h - 4));

    // kStartCornerOptions order: top-left, top-right, bottom-right, bottom-left.
    const std::pair<int, int> corners[4] = {{0, 0}, {w - 1, 0}, {w - 1, h - 1}, {0, h - 1}};

    for (uint8_t corner = 0; corner < RectangleLayout::kStartCornerCount; corner++) {
        size_t s = 0;                                   // where that corner sits in the reference
        while (s < n && base[s] != corners[corner]) s++;
        REQUIRE(s < n);
        for (bool cw : {true, false}) {
            RectangleLayout r;
            r.width = w; r.height = h;
            r.startCorner = corner;
            r.clockwise = cw;
            const auto p = walk(r);
            REQUIRE(p.size() == n);
            for (size_t i = 0; i < n; i++) {
                const size_t j = cw ? (s + i) % n : (s + n - i) % n;
                CHECK(p[i] == base[j]);
            }
            const std::set<std::pair<int, int>> got(p.begin(), p.end());
            CHECK(got.size() == n);   // and still each cell exactly once
        }
    }
}

// Light 0 lands on the corner the user named: the control's whole purpose. (x, y) origin is top-left, so "bottom" is y = height − 1.
TEST_CASE("RectangleLayout: light 0 sits on the chosen start corner") {
    const int w = 6, h = 4;
    const std::pair<int, int> corners[4] = {
        {0, 0}, {w - 1, 0}, {w - 1, h - 1}, {0, h - 1}};   // TL, TR, BR, BL: kStartCornerOptions order
    for (uint8_t c = 0; c < 4; c++) {
        RectangleLayout r;
        r.width = w; r.height = h;
        r.startCorner = c;
        CHECK(walk(r)[0] == corners[c]);
    }
}

// Counter-clockwise reverses the direction of travel while keeping the same first light: from the top-left corner it heads DOWN the left edge instead of right along the top.
TEST_CASE("RectangleLayout: counter-clockwise reverses travel from the same corner") {
    RectangleLayout r;
    r.width = 4; r.height = 3;
    r.clockwise = false;
    const auto p = walk(r);
    REQUIRE(p.size() == 10);
    CHECK(p[0] == std::pair{0, 0});   // same start corner
    CHECK(p[1] == std::pair{0, 1});   // ...but down the left edge (clockwise gave (1,0))
    CHECK(p[2] == std::pair{0, 2});
    CHECK(p[3] == std::pair{1, 2});   // then right along the bottom
}

// A box one light thick has no interior to go around, so it degenerates to a plain line. The rectangle formula would walk those cells twice and light phantom positions, so it is not used.
TEST_CASE("RectangleLayout: a one-light-thick box degenerates to a line, not a doubled-back frame") {
    RectangleLayout row;
    row.width = 5; row.height = 1;
    CHECK(row.lightCount() == 5);          // not 2*5 + 2*1 - 4 == 8
    const auto rp = walk(row);
    REQUIRE(rp.size() == 5);
    CHECK(rp[0] == std::pair{0, 0});
    CHECK(rp[4] == std::pair{4, 0});

    RectangleLayout col;
    col.width = 1; col.height = 5;
    CHECK(col.lightCount() == 5);
    const auto cp = walk(col);
    REQUIRE(cp.size() == 5);
    CHECK(cp[0] == std::pair{0, 0});
    CHECK(cp[4] == std::pair{0, 4});
}

// A zero side is not a shape. It emits nothing rather than dividing by zero in the modular walk.
TEST_CASE("RectangleLayout: a zero-sided box emits no lights") {
    RectangleLayout r;
    r.width = 0; r.height = 10;
    CHECK(r.lightCount() == 0);
    CHECK(walk(r).empty());
}

// --- Four separate strips (sharedCorners off) ---------------------------------------------------
// One strip bent around a frame has ONE light in each corner. Four strips have their own end there, so the count is the plain sum of the edges and two lights share each corner coordinate.

TEST_CASE("RectangleLayout: unshared corners count every edge in full") {
    RectangleLayout r;
    r.width = 20; r.height = 10;
    r.sharedCorners = false;
    CHECK(r.lightCount() == 60);   // 20 + 20 + 10 + 10, no corners deducted
    CHECK(walk(r).size() == 60);

    r.sharedCorners = true;
    CHECK(r.lightCount() == 56);   // the same box, four corners folded away
}

// The extra lights must land ON the corners rather than past them, since an edge running its full length is one step from walking outside the box.
TEST_CASE("RectangleLayout: unshared corners double the corner cells and stay in the box") {
    RectangleLayout r;
    r.width = 4; r.height = 3;
    r.sharedCorners = false;
    const auto pts = walk(r);
    REQUIRE(pts.size() == 14);

    int corners = 0;
    for (const auto& p : pts) {
        CHECK(p.first >= 0);
        CHECK(p.first < 4);
        CHECK(p.second >= 0);
        CHECK(p.second < 3);
        const bool onCorner = (p.first == 0 || p.first == 3) && (p.second == 0 || p.second == 2);
        if (onCorner) corners++;
    }
    CHECK(corners == 8);   // four corners, two lights each
}

// With four separate strips, light 0 still has to land on the named corner rather than one cell past it.
TEST_CASE("RectangleLayout: unshared corners still honor the chosen start corner") {
    const int w = 4, h = 3;
    const std::pair<int, int> corners[4] = {
        {0, 0}, {w - 1, 0}, {w - 1, h - 1}, {0, h - 1}};
    for (uint8_t c = 0; c < 4; c++) {
        RectangleLayout r;
        r.width = w; r.height = h;
        r.sharedCorners = false;
        r.startCorner = c;
        CHECK(walk(r)[0] == corners[c]);
    }
}

// --- offset --------------------------------------------------------------------------------------
// A strip rarely starts exactly at a corner. offset slides where index 0 sits WITHOUT moving any light: the same coordinates come out, rotated in the wiring order.

TEST_CASE("RectangleLayout: offset rotates the wiring and emits the same coordinates") {
    RectangleLayout plain, shifted;
    plain.width = shifted.width = 7;
    plain.height = shifted.height = 5;
    shifted.offset = 3;

    const auto a = walk(plain);
    const auto b = walk(shifted);
    REQUIRE(a.size() == b.size());
    CHECK(a != b);                                   // the order moved
    CHECK(b[0] == a[3]);                             // by exactly three steps
    CHECK(std::set<std::pair<int, int>>(a.begin(), a.end()) ==
          std::set<std::pair<int, int>>(b.begin(), b.end()));   // the shape did not
}

// A full lap is a no-op, and anything beyond it wraps: the walk is modular, so an offset larger than the perimeter must not run off the end of it.
TEST_CASE("RectangleLayout: an offset of a full lap or more wraps") {
    RectangleLayout plain, lap;
    plain.width = lap.width = 7;
    plain.height = lap.height = 5;
    lap.offset = static_cast<uint16_t>(plain.lightCount());
    CHECK(walk(lap) == walk(plain));

    lap.offset = static_cast<uint16_t>(plain.lightCount() + 2);
    RectangleLayout two;
    two.width = 7; two.height = 5; two.offset = 2;
    CHECK(walk(lap) == walk(two));
}
