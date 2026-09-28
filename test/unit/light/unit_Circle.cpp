/// @module draw
/// @also Canvas

/// `circle`/`fillCircle` (Bresenham midpoint) and `lineAA` (Wu). These sit beside the SDF forms rather than replacing them: Bresenham is exact on integer coordinates and costs no multiply, the SDF is sub-pixel and anti-aliased. The tests pin the properties a caller relies on, the rim is symmetric and hollow, the disc is solid, and the AA line spreads its light without inventing any.

#include "doctest.h"
#include "light/powerfunctions/draw.h"


using namespace mm;

namespace {
struct Surface {
    Buffer buf;
    draw::Canvas cv;
    Surface(lengthType w, lengthType h, uint8_t cpl = 3) {
        buf.allocate(static_cast<nrOfLightsType>(w) * h, cpl);
        buf.clear();
        cv = draw::Canvas::of(buf, w, h, 1);
    }
    uint8_t at(lengthType x, lengthType y) const {
        return buf.data()[(static_cast<size_t>(y) * cv.dims.x + x) * cv.cpl];
    }
    int litCount() const {
        int n = 0;
        for (nrOfLightsType i = 0; i < buf.count(); i++)
            if (buf.data()[static_cast<size_t>(i) * cv.cpl]) n++;
        return n;
    }
    uint32_t total() const {
        uint32_t s = 0;
        for (nrOfLightsType i = 0; i < buf.count(); i++) s += buf.data()[static_cast<size_t>(i) * cv.cpl];
        return s;
    }
};
const RGB kRed{200, 0, 0};
}  // namespace

TEST_CASE("a circle outline is symmetric about its center") {
    Surface s(11, 11);
    draw::circle(s.cv, 5, 5, 3, kRed);
    // Every lit cell has a lit mirror in all four reflections, the eight-way symmetry.
    for (lengthType y = 0; y < 11; y++)
        for (lengthType x = 0; x < 11; x++)
            if (s.at(x, y)) {
                CHECK(s.at(static_cast<lengthType>(10 - x), y) != 0);
                CHECK(s.at(x, static_cast<lengthType>(10 - y)) != 0);
            }
}

TEST_CASE("a circle outline is hollow and sits at the radius") {
    Surface s(11, 11);
    draw::circle(s.cv, 5, 5, 3, kRed);
    CHECK(s.at(5, 5) == 0);          // the center is not part of the rim
    CHECK(s.at(8, 5) == 200);        // due east at r=3
    CHECK(s.at(2, 5) == 200);        // due west
    CHECK(s.at(5, 8) == 200);        // due south
    CHECK(s.at(5, 2) == 200);        // due north
    CHECK(s.at(6, 5) == 0);          // inside the rim stays dark
}

TEST_CASE("a zero-radius circle is a single pixel") {
    Surface s(5, 5);
    draw::circle(s.cv, 2, 2, 0, kRed);
    CHECK(s.at(2, 2) == 200);
    CHECK(s.litCount() == 1);
}

TEST_CASE("a filled circle is solid from center to rim") {
    Surface s(11, 11);
    draw::fillCircle(s.cv, 5, 5, 3, kRed);
    CHECK(s.at(5, 5) == 200);        // center filled
    CHECK(s.at(6, 5) == 200);        // interior filled
    CHECK(s.at(8, 5) == 200);        // rim reached
    CHECK(s.at(9, 5) == 0);          // and not exceeded
}

// A disc must cover strictly more than its outline, the check that catches a fill that only painted the rim.
TEST_CASE("a filled circle covers more cells than its outline") {
    Surface outline(11, 11), disc(11, 11);
    draw::circle(outline.cv, 5, 5, 3, kRed);
    draw::fillCircle(disc.cv, 5, 5, 3, kRed);
    CHECK(disc.litCount() > outline.litCount());
}

TEST_CASE("a filled circle colors by its row offset from the center") {
    Surface s(11, 11);
    // The callback gets the SIGNED row offset, so a caller can ramp top to bottom.
    draw::fillCircle(s.cv, 5, 5, 3, [](lengthType dy) {
        return RGB{static_cast<uint8_t>(100 + 10 * dy), 0, 0};
    });
    CHECK(s.at(5, 5) == 100);        // dy = 0 at the center row
    CHECK(s.at(5, 8) == 130);        // three rows below
    CHECK(s.at(5, 2) == 70);         // three rows above
}

// Robustness: a circle bigger than the grid draws the part that lands, and writes nothing outside.
TEST_CASE("a circle larger than the grid clips instead of overflowing") {
    Surface s(5, 5);
    draw::circle(s.cv, 2, 2, 50, kRed);
    CHECK(s.litCount() == 0);        // the whole rim is off-grid

    Surface disc(5, 5);
    draw::fillCircle(disc.cv, 2, 2, 50, kRed);
    CHECK(disc.litCount() == 25);    // the disc covers every cell, and none beyond
}

// At a radius near the 16-bit ceiling the box's far edge lands on INT16_MAX, the counter wraps negative, and the walk never ends: this asserts termination.
TEST_CASE("a circle larger than the coordinate type terminates") {
    Surface s(16, 16);
    draw::ring(s.cv, draw::toSub(8), draw::toSub(8), draw::toSub(32758), draw::kSubOne, RGB{255, 0, 0});
    draw::disc(s.cv, draw::toSub(8), draw::toSub(8), draw::toSub(32758), RGB{0, 255, 0});
    CHECK(true);   // reaching this line IS the assertion: the calls above returned
}

// Termination is the assertion, not elapsed time: unclipped, these 200 calls are ~3.2 billion distance evaluations and do not finish, so reaching the end proves the box was clipped.
TEST_CASE("a circle far larger than the grid costs the grid, not the radius") {
    Surface s(16, 16);
    for (int i = 0; i < 200; i++)
        draw::ring(s.cv, draw::toSub(8), draw::toSub(8), draw::toSub(4000), draw::kSubOne, RGB{255, 0, 0});
    CHECK(true);
}

TEST_CASE("a negative radius draws nothing") {
    Surface s(5, 5);
    draw::circle(s.cv, 2, 2, -3, kRed);
    draw::fillCircle(s.cv, 2, 2, -3, kRed);
    CHECK(s.litCount() == 0);
}

// The reason lineAA exists: a diagonal that is not at 45 degrees lands between cells, and Wu splits it rather than snapping. A perfectly diagonal line has nothing to split.
TEST_CASE("an anti-aliased line spreads a shallow diagonal over neighboring cells") {
    Surface s(8, 8);
    draw::lineAA(s.cv, {0, 0, 0}, {7, 3, 0}, RGB{255, 0, 0});
    // Somewhere along the run, two vertically-adjacent cells are both partly lit, the AA signature.
    bool foundPair = false;
    for (lengthType x = 1; x < 7 && !foundPair; x++)
        for (lengthType y = 0; y < 7; y++) {
            const uint8_t a = s.at(x, y), b = s.at(x, static_cast<lengthType>(y + 1));
            if (a > 0 && b > 0 && a < 255 && b < 255) { foundPair = true; break; }
        }
    CHECK(foundPair);
}

TEST_CASE("an anti-aliased line reaches both endpoints") {
    Surface s(8, 8);
    draw::lineAA(s.cv, {0, 0, 0}, {7, 3, 0}, RGB{255, 0, 0});
    CHECK(s.at(0, 0) > 0);
    CHECK(s.at(7, 3) > 0);
}

// Conservation: splitting light between two cells must not create any. Each step contributes one pixel's worth, so the total tracks the line's length rather than its slope.
TEST_CASE("an anti-aliased line splits light without inventing it") {
    Surface s(16, 16);
    draw::lineAA(s.cv, {0, 0, 0}, {15, 5, 0}, RGB{255, 0, 0});
    // 16 steps, each contributing ~255 across its two cells.
    CHECK(s.total() > 15u * 240u);
    CHECK(s.total() < 17u * 260u);
}

TEST_CASE("an anti-aliased line handles steep and reversed directions") {
    Surface steep(8, 8);
    draw::lineAA(steep.cv, {1, 0, 0}, {3, 7, 0}, RGB{255, 0, 0});
    CHECK(steep.at(1, 0) > 0);
    CHECK(steep.at(3, 7) > 0);

    // Drawn backwards, the same segment lights the same cells.
    Surface fwd(8, 8), rev(8, 8);
    draw::lineAA(fwd.cv, {0, 0, 0}, {7, 2, 0}, RGB{255, 0, 0});
    draw::lineAA(rev.cv, {7, 2, 0}, {0, 0, 0}, RGB{255, 0, 0});
    CHECK(fwd.total() == rev.total());
}

TEST_CASE("an anti-aliased line of zero length is a single point") {
    Surface s(4, 4);
    draw::lineAA(s.cv, {2, 2, 0}, {2, 2, 0}, RGB{255, 0, 0});
    CHECK(s.at(2, 2) > 0);
    CHECK(s.litCount() == 1);
}

TEST_CASE("an anti-aliased line running off the grid clips instead of overflowing") {
    Surface s(4, 4);
    draw::lineAA(s.cv, {-20, -8, 0}, {20, 9, 0}, RGB{255, 0, 0});
    CHECK(s.total() > 0);            // the part that crosses the grid drew
}
