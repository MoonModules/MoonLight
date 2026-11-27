/// @module CubeLayout

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/CubeLayout.h"
#include "layout_lights.h"   // Light, lightsOf

#include <cstdlib>
#include <vector>

namespace {

using mm::test::Light;

// The cube's lights in strip order.
std::vector<Light> strip(const mm::CubeLayout& cube) { return mm::test::lightsOf(cube); }

// Whether each light sits next to the one before it, as one continuous strip does.
bool continuous(const std::vector<Light>& lights) {
    for (size_t i = 1; i < lights.size(); i++) {
        const int d = std::abs(lights[i].x - lights[i - 1].x) + std::abs(lights[i].y - lights[i - 1].y) + std::abs(lights[i].z - lights[i - 1].z);
        if (d != 1) return false;
    }
    return true;
}

// A cube snaking on x and on y, x fastest, then y, then z.
void snakeCube(mm::CubeLayout& cube, mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    cube.width = w;
    cube.height = h;
    cube.depth = d;
    cube.wiringOrder = 0;   // XYZ: x fastest, then y, then z
    cube.snakeX = true;
    cube.snakeY = true;
    cube.snakeZ = false;
}

}  // namespace

TEST_CASE("CubeLayout: a cube of odd planes, each turned 180 degrees, is one continuous strip, the next plane starting where the last ended") {
    mm::CubeLayout cube;
    snakeCube(cube, 7, 7, 2);
    const auto lights = strip(cube);
    REQUIRE(lights.size() == 98);
    CHECK(continuous(lights));
    // Plane 0 ends at (6, 6, 0), and plane 1 starts right behind it, its first row running back toward x = 0.
    CHECK(lights[48].x == 6);
    CHECK(lights[48].y == 6);
    CHECK(lights[49].x == 6);
    CHECK(lights[49].y == 6);
    CHECK(lights[49].z == 1);
    CHECK(lights[50].x == 5);
    CHECK(lights[97].x == 0);   // and ends at the corner the strip began over
    CHECK(lights[97].y == 0);
}

TEST_CASE("CubeLayout: a cube whose middle axis runs straight starts every plane at the same corner") {
    mm::CubeLayout cube;
    snakeCube(cube, 7, 7, 2);
    cube.snakeY = false;   // each plane wired alike, a return wire from its end to the next plane's start
    const auto lights = strip(cube);
    REQUIRE(lights.size() == 98);
    CHECK(lights[49].x == 0);
    CHECK(lights[49].y == 0);
    CHECK(lights[49].z == 1);
    CHECK(lights[50].x == 1);   // its first row runs the way plane 0's did
    CHECK(lights[56].x == 6);   // and its second row snakes back
    CHECK(lights[56].y == 1);
}

TEST_CASE("CubeLayout: an even cube snakes as before, one continuous strip") {
    mm::CubeLayout cube;
    snakeCube(cube, 4, 4, 3);
    const auto lights = strip(cube);
    REQUIRE(lights.size() == 48);
    CHECK(continuous(lights));
    CHECK(lights[16].x == 0);   // plane 1 starts at x = 0, where an even plane ends
    CHECK(lights[16].y == 3);
}
