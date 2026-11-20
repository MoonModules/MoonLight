/// @module Layouts
/// @also Layer, LinesEffect, TruchetEffect, RaymarchEffect, PlasmaEffect, SphereMoveEffect, LissajousEffect, RipplesEffect, BouncingBallsEffect

/// A device placed in a wall of several, its position and the wall's size on its Layouts: a region effect's tiles rendered apart, each only its own lights, equal the whole wall rendered at once, and a mirrored effect draws as it always has.

#include "doctest.h"
#include "light/effects/BouncingBallsEffect.h"
#include "golden_frame.h"
#include "light/effects/LinesEffect.h"
#include "light/effects/LissajousEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/RaymarchEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/SphereMoveEffect.h"
#include "light/effects/TruchetEffect.h"
#include "core/util/ModuleFactory.h"
#include "module_types.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

using namespace mm;
using mm::golden::LayerFixture;
using mm::golden::ScopedTestClock;

namespace {

constexpr lengthType kTile = 16;   ///< one bench board's panel
constexpr int kTiles = 3;          ///< the 3x3 wall
constexpr lengthType kWall = kTile * kTiles;

/// A device's size, its place in the wall and the wall's size, 0 for a device alone; the depth axis defaults to a flat panel in a flat wall.
struct Spot {
    lengthType w, h, x, y, wallW, wallH;
    lengthType d = 1, z = 0, wallD = 1;
};

/// Render `frames` ticks 20 ms apart on a device at `at`, and return its bytes.
template <typename EffectT, typename Setup>
std::vector<uint8_t> render(Setup setup, Spot at, uint16_t frames) {
    auto effect = std::make_unique<EffectT>();
    setup(*effect);
    LayerFixture fx(at.w, at.h, at.d);
    Devices devices;   // the wall the group sync would derive, set as it would set it
    devices.setWall(WallPlace{0, 0, 0, static_cast<uint16_t>(at.wallW), static_cast<uint16_t>(at.wallH), static_cast<uint16_t>(at.wallD)});
    fx.layouts.useDevices(&devices);
    fx.layouts.superSync = at.wallW > 0;
    fx.layouts.positionX = at.x;
    fx.layouts.positionY = at.y;
    fx.layouts.positionZ = at.z;
    fx.add(*effect);
    fx.layouts.prepare();   // its own size, which the place carries
    fx.layouts.tick1s();    // the place out and the wall in, as each second does
    for (uint16_t f = 0; f < frames; f++) fx.tickAt(1000 + static_cast<uint32_t>(f) * 20);
    std::vector<uint8_t> bytes = fx.frame();
    fx.layer.removeChild(effect.get());
    return bytes;
}

/// Whether every tile of the wall, rendered alone at its place, equals its part of the whole wall rendered at once.
template <typename EffectT, typename Setup>
void checkSplitEqualsWhole(Setup setup, uint16_t frames) {
    ScopedTestClock clock(1000);
    const std::vector<uint8_t> wall = render<EffectT>(setup, Spot{kWall, kWall, 0, 0, 0, 0}, frames);
    for (int ty = 0; ty < kTiles; ty++)
        for (int tx = 0; tx < kTiles; tx++) {
            const lengthType ox = static_cast<lengthType>(tx * kTile), oy = static_cast<lengthType>(ty * kTile);
            const std::vector<uint8_t> tile = render<EffectT>(setup, Spot{kTile, kTile, ox, oy, kWall, kWall}, frames);
            REQUIRE(tile.size() == static_cast<size_t>(kTile) * kTile * 3);
            for (lengthType y = 0; y < kTile; y++) {
                const size_t mine = static_cast<size_t>(y) * kTile * 3;
                const size_t there = (static_cast<size_t>(oy + y) * kWall + ox) * 3;
                CAPTURE(tx); CAPTURE(ty); CAPTURE(y);
                CHECK(std::memcmp(tile.data() + mine, wall.data() + there, static_cast<size_t>(kTile) * 3) == 0);
            }
        }
}

/// Whether two cubes stacked in depth, each rendered alone at its place, equal one cube twice as deep rendered at once.
template <typename EffectT, typename Setup>
void checkStackEqualsWhole(Setup setup, uint16_t frames) {
    ScopedTestClock clock(1000);
    Spot deep{kTile, kTile, 0, 0, 0, 0};
    deep.d = 2 * kTile;
    const std::vector<uint8_t> whole = render<EffectT>(setup, deep, frames);
    const size_t layer = static_cast<size_t>(kTile) * kTile * kTile * 3;   // one cube's bytes, z the outermost axis
    for (lengthType z = 0; z < 2 * kTile; z += kTile) {
        Spot cube{kTile, kTile, 0, 0, kTile, kTile};
        cube.d = kTile; cube.z = z; cube.wallD = 2 * kTile;
        const std::vector<uint8_t> part = render<EffectT>(setup, cube, frames);
        REQUIRE(part.size() == layer);
        CAPTURE(z);
        CHECK(std::memcmp(part.data(), whole.data() + static_cast<size_t>(z / kTile) * layer, layer) == 0);
    }
}

bool anyLit(const std::vector<uint8_t>& bytes) {
    return std::ranges::any_of(bytes, [](uint8_t b) { return b != 0; });
}

}  // namespace

TEST_CASE("Lines on each of nine boards draws its part of one red and one green line sweeping the whole wall") {
    checkSplitEqualsWhole<LinesEffect>([](LinesEffect& e) { e.axis = LinesEffect::kAxisAll; }, 120);
}

TEST_CASE("Lines leaves a board dark while the wall's lines are elsewhere, rather than drawing lines of its own") {
    ScopedTestClock clock(1000);
    // One second into speed 30's two-second sweep both lines sit at the wall's index 24, crossing the middle board and missing the corners.
    const auto corner = render<LinesEffect>([](LinesEffect& e) { e.axis = LinesEffect::kAxisAll; },
                                            Spot{kTile, kTile, 2 * kTile, 2 * kTile, kWall, kWall}, 1);
    CHECK_FALSE(anyLit(corner));
    const auto middle = render<LinesEffect>([](LinesEffect& e) { e.axis = LinesEffect::kAxisAll; },
                                            Spot{kTile, kTile, kTile, kTile, kWall, kWall}, 1);
    CHECK(anyLit(middle));
}

TEST_CASE("Every effect carries the SuperSync tag exactly when it draws its part of the wall's one picture") {
    registerModuleTypes();
    int effects = 0;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++) {
        // The registry's own effects, each with its doc page: a test elsewhere registers stubs of the role.
        if (ModuleFactory::typeRole(i) != ModuleRole::Effect || !ModuleFactory::typeDocPath(i)[0]) continue;
        MoonModule* m = ModuleFactory::create(ModuleFactory::typeName(i));
        REQUIRE(m != nullptr);
        const bool region = static_cast<EffectBase*>(m)->footprint() == EffectBase::Footprint::Region;
        const bool tagged = std::strstr(ModuleFactory::typeTags(i), "🧩") != nullptr;
        CHECK_MESSAGE(region == tagged, std::string(ModuleFactory::typeName(i)));
        effects++;
        delete m;
    }
    CHECK(effects > 0);
}

TEST_CASE("A device with SuperSync on publishes its place, its own size from its layouts") {
    LayerFixture fx(kTile, kTile);
    Devices devices;
    fx.layouts.useDevices(&devices);
    fx.layouts.superSync = true;
    fx.layouts.positionX = 32;
    fx.layouts.positionY = 16;
    fx.layouts.positionZ = 8;
    fx.layouts.prepare();
    fx.layouts.tick1s();
    CHECK(devices.place() == WallPlace{32, 16, 8, kTile, kTile, 1});
    fx.layouts.superSync = false;
    fx.layouts.tick1s();
    CHECK_FALSE(devices.place().placed());   // off: no part of a wall, whatever its position
}

TEST_CASE("A device alone in no wall draws Lines over its own lights, as it always has") {
    ScopedTestClock clock(1000);
    const auto alone = render<LinesEffect>([](LinesEffect& e) { e.axis = LinesEffect::kAxisAll; }, Spot{kTile, kTile, 0, 0, 0, 0}, 60);
    const auto ownWall = render<LinesEffect>([](LinesEffect& e) { e.axis = LinesEffect::kAxisAll; }, Spot{kTile, kTile, 0, 0, kTile, kTile}, 60);
    CHECK(alone == ownWall);
}

TEST_CASE("Truchet on each of nine boards shades its part of one tiling, its arcs meeting across every seam") {
    checkSplitEqualsWhole<TruchetEffect>([](TruchetEffect&) {}, 120);
}

TEST_CASE("Plasma on each of nine boards draws its part of one field") {
    checkSplitEqualsWhole<PlasmaEffect>([](PlasmaEffect&) {}, 120);
}

TEST_CASE("Two cubes stacked in depth draw their parts of one deeper picture, Lines' blue plane passing from one into the other") {
    checkStackEqualsWhole<LinesEffect>([](LinesEffect& e) { e.axis = LinesEffect::kAxisAll; }, 120);
}

TEST_CASE("Two cubes stacked in depth draw their parts of one plasma volume") {
    checkStackEqualsWhole<PlasmaEffect>([](PlasmaEffect&) {}, 120);
}

TEST_CASE("SphereMove on each of nine boards draws its part of one shell, its shimmer alike across every seam") {
    checkSplitEqualsWhole<SphereMoveEffect>([](SphereMoveEffect&) {}, 120);
}

TEST_CASE("Two cubes stacked in depth draw their parts of one moving sphere") {
    checkStackEqualsWhole<SphereMoveEffect>([](SphereMoveEffect&) {}, 120);
}

TEST_CASE("Lissajous on each of nine boards traces its part of one figure, its trail fading alike") {
    checkSplitEqualsWhole<LissajousEffect>([](LissajousEffect&) {}, 120);
}

TEST_CASE("Ripples on each of nine boards draws its part of one surface around the wall's center") {
    checkSplitEqualsWhole<RipplesEffect>([](RipplesEffect&) {}, 120);
}

TEST_CASE("Two cubes stacked in depth draw their parts of one rippling surface") {
    checkStackEqualsWhole<RipplesEffect>([](RipplesEffect&) {}, 120);
}

TEST_CASE("Bouncing Balls on each of nine boards draws its part of one forest, a ball crossing from one board to the next") {
    checkSplitEqualsWhole<BouncingBallsEffect>([](BouncingBallsEffect&) {}, 120);
}

#if MM_HEAVY_COMPUTE
TEST_CASE("Raymarch on each of nine boards marches its share of one scene") {
    checkSplitEqualsWhole<RaymarchEffect>([](RaymarchEffect&) {}, 30);
}
#endif

TEST_CASE("A mirrored effect's surface carries no place in the wall, a region effect's does") {
    LayerFixture fx(kTile, kTile);
    Devices devices;
    devices.setWall(WallPlace{0, 0, 0, kWall, kWall, 1});
    fx.layouts.useDevices(&devices);
    fx.layouts.superSync = true;
    fx.layouts.positionX = kTile;
    fx.layouts.positionY = 2 * kTile;
    RainbowEffect mirrored;   // declares no footprint
    LinesEffect region;
    fx.add(mirrored);
    fx.add(region);
    fx.layouts.prepare();
    fx.layouts.tick1s();
    const draw::Canvas own = mirrored.canvas();
    CHECK(own.origin == Coord3D{0, 0, 0});
    CHECK(own.extent() == own.dims);
    const draw::Canvas part = region.canvas();
    CHECK(part.origin == Coord3D{kTile, 2 * kTile, 0});
    CHECK(part.extent() == Coord3D{kWall, kWall, 1});
    fx.layer.removeChild(&mirrored);
    fx.layer.removeChild(&region);
}
