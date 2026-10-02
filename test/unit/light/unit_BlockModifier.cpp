/// @module BlockModifier

#include "doctest.h"
#include "light/modifiers/BlockModifier.h"

// Each light folds to its Chebyshev distance from the box center, written into y, so a 1D effect along y draws square rings.

// The logical box a BlockModifier produces for a given physical box.
static mm::Coord3D blockSize(mm::BlockModifier& b, mm::Coord3D box) {
    b.modifyLogicalSize(box);
    return box;
}

// Fold a physical coord through the modifier. Stashes the box first (modifyLogicalSize saves the physical box the const fold reads for its center), then folds (x,y,z).
static mm::Coord3D fold(mm::BlockModifier& b, mm::lengthType x, mm::lengthType y,
                        mm::lengthType z, mm::Coord3D box) {
    b.modifyLogicalSize(box);   // stashes the physical box
    mm::Coord3D p{x, y, z};
    b.modifyLogical(p);         // never rejects: returns true, we assert the coord
    return p;
}

// The center light of the box folds to distance 0 (the innermost ring, y=0); a corner folds to the largest distance, and z is always cleared to 0.
TEST_CASE("BlockModifier folds to Chebyshev distance from the box center") {
    mm::BlockModifier b;
    // On a 4x4 box the floor-biased center is (1,1). The center light maps to y=0.
    CHECK(fold(b, 1, 1, 0, {4, 4, 1}) == mm::Coord3D{0, 0, 0});
    // Corner (0,0): dx=dy=1 -> distance 1.
    CHECK(fold(b, 0, 0, 0, {4, 4, 1}) == mm::Coord3D{0, 1, 0});
    // Far corner (3,3): dx=dy=2 -> distance 2 (the outermost square ring).
    CHECK(fold(b, 3, 3, 0, {4, 4, 1}) == mm::Coord3D{0, 2, 0});
}

// The distance is max(|dx|, |dy|), not the sum or the Euclidean length, which is what makes the rings square.
TEST_CASE("BlockModifier uses max(|dx|,|dy|) so rings are axis-aligned squares") {
    mm::BlockModifier b;
    // 5x5 box, center (2,2). Light (4,2): dx=2, dy=0 -> distance 2.
    CHECK(fold(b, 4, 2, 0, {5, 5, 1}) == mm::Coord3D{0, 2, 0});
    // Light (4,3): dx=2, dy=1 -> max is 2, same ring as (4,2), a square edge, not a circle (Euclidean would differ, sum-of-deltas would give 3).
    CHECK(fold(b, 4, 3, 0, {5, 5, 1}) == mm::Coord3D{0, 2, 0});
    // z is not part of the distance, a light off the plane still folds by x/y only.
    CHECK(fold(b, 4, 2, 3, {5, 5, 4}) == mm::Coord3D{0, 2, 0});
}

// modifyLogicalSize collapses the box to one column, its height the farthest light's block distance + 1 (every ring a light lands on, no empty one), depth 1.
TEST_CASE("BlockModifier logical box is {1, maxDistance + 1, 1}") {
    mm::BlockModifier b;
    // 4x4: center (1,1), farthest light (3,3) at distance 2 -> height 3.
    CHECK(blockSize(b, {4, 4, 1}) == mm::Coord3D{1, 3, 1});
    // 3x3: center (1,1), farthest light (2,2) at distance 1 -> height 2.
    CHECK(blockSize(b, {3, 3, 1}) == mm::Coord3D{1, 2, 1});
    // A wide box takes its larger axis: 8x2 -> center (3,0), farthest light (7,1) at max(4,1)=4 -> 5.
    CHECK(blockSize(b, {8, 2, 1}) == mm::Coord3D{1, 5, 1});
}

// Degenerate grids never crash and stay well-formed: 0x0x0 and 1x1x1 both fold and size without dividing by zero or producing a zero-height box (the Effects hard rule).
TEST_CASE("BlockModifier survives degenerate grids") {
    mm::BlockModifier b;
    // 1x1x1: single light is the center -> distance 0; logical box {1,1,1}.
    CHECK(fold(b, 0, 0, 0, {1, 1, 1}) == mm::Coord3D{0, 0, 0});
    CHECK(blockSize(b, {1, 1, 1}) == mm::Coord3D{1, 1, 1});
    // 0x0x0: no crash; the fold and size are still finite (each axis grows by one).
    CHECK_NOTHROW(fold(b, 0, 0, 0, {0, 0, 0}));
    CHECK(blockSize(b, {0, 0, 0}).x >= 1);
    CHECK(blockSize(b, {0, 0, 0}).y >= 1);
    CHECK(blockSize(b, {0, 0, 0}).z >= 1);
}
