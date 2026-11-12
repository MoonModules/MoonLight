/// @module light_types

#include "doctest.h"
#include "light/util/light_types.h"

#include <type_traits>

// Coord3D is the type a modifier's fold mutates: per-axis operators let it read as geometry (`pos = pos % size`), and % and / guard a zero axis, so a fold over a 1-wide or empty axis stays defined.

TEST_CASE("Coord3D arithmetic is per-axis") {
    mm::Coord3D a{10, 20, 30};
    mm::Coord3D b{1, 2, 3};
    CHECK((a + b) == mm::Coord3D{11, 22, 33});
    CHECK((a - b) == mm::Coord3D{9, 18, 27});
    CHECK((a * b) == mm::Coord3D{10, 40, 90});
}

TEST_CASE("Coord3D modulo and divide fold per axis") {
    mm::Coord3D pos{7, 5, 9};
    mm::Coord3D size{4, 4, 4};
    // The textbook Multiply fold: position within its tile, and which tile.
    CHECK((pos % size) == mm::Coord3D{3, 1, 1});
    CHECK((pos / size) == mm::Coord3D{1, 1, 2});
}

TEST_CASE("Coord3D % and / guard a zero or degenerate axis") {
    mm::Coord3D pos{7, 5, 3};
    // A 0-extent or 1-extent axis must not divide-by-zero or wrap; the coordinate passes through (% ) or stays put-ish (/), so a fold over a flat axis is safe.
    mm::Coord3D zero{0, 1, 0};
    CHECK((pos % zero) == mm::Coord3D{7, 0, 3});   // x: %0 → unchanged, y: %1 → 0, z: %0 → unchanged
    CHECK((pos / zero) == mm::Coord3D{7, 5, 3});   // /0 → unchanged on x and z, /1 → unchanged on y
}

TEST_CASE("Coord3D equality") {
    CHECK(mm::Coord3D{1, 2, 3} == mm::Coord3D{1, 2, 3});
    CHECK(mm::Coord3D{1, 2, 3} != mm::Coord3D{1, 2, 4});
    CHECK(mm::Coord3D{} == mm::Coord3D{0, 0, 0});   // default-constructed is the origin
}

// A layout can place a light outside the box: a script's out-of-range coordinate arrives negative, and a composed layout can be shifted below 0. Inside means every axis from 0 up to below the size.
TEST_CASE("Coord3D is inside a box only when every axis is from 0 up to below the box's size") {
    const mm::Coord3D box{4, 2, 1};
    CHECK(mm::Coord3D{0, 0, 0}.isInside(box));
    CHECK(mm::Coord3D{3, 1, 0}.isInside(box));
    CHECK_FALSE(mm::Coord3D{-1, 1, 0}.isInside(box));
    CHECK_FALSE(mm::Coord3D{0, -1, 0}.isInside(box));
    CHECK_FALSE(mm::Coord3D{0, 0, -1}.isInside(box));
    CHECK_FALSE(mm::Coord3D{4, 0, 0}.isInside(box));
    CHECK_FALSE(mm::Coord3D{0, 2, 0}.isInside(box));
    CHECK_FALSE(mm::Coord3D{0, 0, 1}.isInside(box));
}

// The index is row-major and 64 bits on every board: the largest box, 32767 a side, stays exact under 2^45, past a 32-bit size_t.
TEST_CASE("Coord3D's index in a box is row-major and 64 bits on every board") {
    CHECK(std::is_same_v<decltype(mm::Coord3D{}.indexIn({})), uint64_t>);
    CHECK(mm::Coord3D{1, 1, 0}.indexIn({4, 2, 1}) == 5u);
    CHECK(mm::Coord3D{0, 0, 1}.indexIn({4, 2, 2}) == 8u);
    CHECK(mm::Coord3D{1023, 1023, 2047}.indexIn({1024, 1024, 2048}) == uint64_t{1024} * 1024 * 2048 - 1);
    CHECK(mm::Coord3D{32766, 32766, 32766}.indexIn({32767, 32767, 32767}) == uint64_t{32767} * 32767 * 32767 - 1);
}

// Whether a position is index i of a box: inside it, and its 64-bit index is i, so no size overflows or aliases.
TEST_CASE("Coord3D knows whether it is a given row-major index of a box") {
    CHECK(mm::Coord3D{1, 1, 0}.isAtIndex(5, {4, 2, 1}));
    CHECK_FALSE(mm::Coord3D{1, 1, 0}.isAtIndex(4, {4, 2, 1}));
    CHECK(mm::Coord3D{1023, 1023, 2047}.isAtIndex(uint64_t{1024} * 1024 * 2048 - 1, {1024, 1024, 2048}));
    CHECK(mm::Coord3D{112, 16, 1}.isAtIndex(70000, {256, 257, 2}));
    CHECK_FALSE(mm::Coord3D{0, 0, 0}.isAtIndex(65536, {1, 1, 1}));   // an index past the box is no position in it, however it would narrow
    CHECK_FALSE(mm::Coord3D{-1, 1, 0}.isAtIndex(3, {4, 2, 1}));      // outside the box is no index of it
}
