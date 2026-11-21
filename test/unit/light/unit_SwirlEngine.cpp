/// @module swirl

#include "doctest.h"
#include "core/module/MoonModule.h"
#include "light/powerfunctions/swirl.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

// A minimal concrete MoonModule to own the engine in these tests.
struct Owner : mm::MoonModule {};

// A motion that drives every particle at one fixed velocity and leaves the pool's population as it is.
struct DriftMotion {
    float vx = 0, vy = 0;
    void emit(mm::swirl::Engine&) {}
    bool drive(mm::swirl::Particle& p, float*) {
        p.vx = vx;
        p.vy = vy;
        return true;
    }
};

// A motion that emits one particle a step at the center, at rest, living `life` steps, its hue counting births.
struct DripMotion {
    uint16_t life = 0;
    uint8_t emitted = 0;
    void emit(mm::swirl::Engine& e) {
        mm::swirl::Particle* p = e.spawn();
        if (!p) return;
        p->x = mm::swirl::kCenter;
        p->y = mm::swirl::kCenter;
        p->life = life;
        p->hue = emitted++;
    }
    bool drive(mm::swirl::Particle&, float*) { return true; }
};

// A motion that kills every particle whose hue is odd, and holds the rest still.
struct OddDiesMotion {
    void emit(mm::swirl::Engine&) {}
    bool drive(mm::swirl::Particle& p, float*) { return (p.hue & 1) == 0; }
};

// An engine owned by a test module, sized for `n` particles on a `w` by `h` grid of 3 channels, with a trail and a depth as asked.
struct Rig {
    std::unique_ptr<Owner> ownerHeld = std::make_unique<Owner>();   // a module is large, so the test's frame stays small
    Owner& owner = *ownerHeld;
    mm::swirl::Engine engine{owner};
    mm::lengthType w, h;
    explicit Rig(uint16_t n, mm::lengthType w_ = 32, mm::lengthType h_ = 32, bool trail = true, bool depth = false) : w(w_), h(h_) {
        REQUIRE(engine.prepare(owner, n, depth, trail ? static_cast<size_t>(w) * h * 3 : 0));
    }
    // Spawn a particle at (x, y) with hue `hue`.
    mm::swirl::Particle& at(float x, float y, uint8_t hue = 0) {
        mm::swirl::Particle* p = engine.spawn();
        REQUIRE(p != nullptr);
        p->x = x;
        p->y = y;
        p->hue = hue;
        return *p;
    }
    // Step `motion` once on the trail, if one is held.
    template <typename M>
    void step(M& motion, mm::swirl::Edge edge = mm::swirl::Edge::kFree) {
        const auto plane = engine.plane(w, h, 3);
        const mm::swirl::Look look{edge};
        engine.step(motion, plane ? &*plane : nullptr, look);
    }
};

// How many bytes of a buffer carry any color.
size_t lit(const uint8_t* data, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) n += data[i] ? 1 : 0;
    return n;
}

}  // namespace

// The pool, the depth and the trail are sized to exactly what the prepare asks.
TEST_CASE("SwirlEngine holds exactly the pool, depth and trail asked for") {
    Owner owner;
    mm::swirl::Engine e{owner};
    REQUIRE(e.prepare(owner, 64, false, 16 * 16 * 3));
    CHECK(e.pool.count() == 64);
    CHECK_FALSE(e.depthHeld());
    CHECK(e.trail.bytes() == 16 * 16 * 3);
    CHECK(owner.dynamicBytes() == 64 * sizeof(mm::swirl::Particle) + 16 * 16 * 3);
    REQUIRE(e.prepare(owner, 32, true, 0));
    CHECK(e.depth.count() == 32);
    CHECK_FALSE(e.trailHeld());
    CHECK(owner.dynamicBytes() == 32 * sizeof(mm::swirl::Particle) + 32 * sizeof(float));
}

// A particle is 24 bytes, the figure the effect's memory is written for.
TEST_CASE("SwirlEngine particle is 24 bytes") {
    CHECK(sizeof(mm::swirl::Particle) == 24);
}

// Births take the next slot until the pool is full, then the pool refuses rather than overwriting.
TEST_CASE("SwirlEngine spawns into the next slot until the pool is full") {
    Rig rig(3);
    for (uint16_t i = 0; i < 3; i++) {
        mm::swirl::Particle* p = rig.engine.spawn();
        REQUIRE(p == &rig.engine.pool[i]);
    }
    CHECK(rig.engine.live == 3);
    CHECK(rig.engine.spawn() == nullptr);
}

// A death moves the last live particle into the dead one's slot, so the live ones stay packed.
TEST_CASE("SwirlEngine kill moves the last particle into the slot") {
    Rig rig(4);
    for (uint8_t i = 0; i < 4; i++) rig.at(1, 1, i);
    rig.engine.kill(1);
    CHECK(rig.engine.live == 3);
    CHECK(rig.engine.pool[1].hue == 3);
    CHECK(rig.engine.pool[0].hue == 0);
    CHECK(rig.engine.pool[2].hue == 2);
    rig.engine.kill(2);   // the last one: it alone goes
    CHECK(rig.engine.live == 2);
    CHECK(rig.engine.pool[1].hue == 3);
}

// The depth travels with its particle when the last one moves into a freed slot.
TEST_CASE("SwirlEngine kill moves the depth with the particle") {
    Rig rig(3, 32, 32, true, true);
    for (uint8_t i = 0; i < 3; i++) {
        mm::swirl::Particle& p = rig.at(1, 1, i);
        *rig.engine.depthOf(&p) = 10.0f * i;
    }
    rig.engine.kill(0);
    CHECK(rig.engine.pool[0].hue == 2);
    CHECK(rig.engine.depth[0] == doctest::Approx(20.0f));
}

// A step that kills particles still steps every survivor once, the one swapped in included.
TEST_CASE("SwirlEngine step kills in place and steps every survivor") {
    Rig rig(6);
    for (uint8_t i = 0; i < 6; i++) rig.at(1, 1, i);
    OddDiesMotion odd;
    rig.step(odd);
    CHECK(rig.engine.live == 3);
    for (uint16_t i = 0; i < rig.engine.live; i++) {
        CHECK((rig.engine.pool[i].hue & 1) == 0);
        CHECK(rig.engine.pool[i].age == 1);
    }
}

// A lower count lets the particles past it go at once.
TEST_CASE("SwirlEngine trim drops the live count to the cap") {
    Rig rig(8);
    for (uint8_t i = 0; i < 8; i++) rig.at(1, 1, i);
    rig.engine.trim(5);
    CHECK(rig.engine.live == 5);
    rig.engine.trim(9);
    CHECK(rig.engine.live == 5);
}

// Semi-implicit Euler with a fixed velocity is exact.
TEST_CASE("SwirlEngine integrates a fixed velocity exactly") {
    Rig rig(1);
    rig.at(10, 10);
    DriftMotion drift{0.5f, -0.25f};
    for (int s = 0; s < 4; s++) rig.step(drift);
    CHECK(rig.engine.pool[0].x == doctest::Approx(12.0f));
    CHECK(rig.engine.pool[0].y == doctest::Approx(9.0f));
}

// A move is capped at kMaxMove a step, so a wild field cannot draw across the panel in one step.
TEST_CASE("SwirlEngine caps a move at the most a step takes") {
    Rig rig(1);
    rig.at(16, 16);
    DriftMotion fast{30.0f, 0};
    rig.step(fast);
    CHECK(rig.engine.pool[0].x == doctest::Approx(16.0f + mm::swirl::kMaxMove));
}

// A particle with a life dies when its age reaches it, fading on the way.
TEST_CASE("SwirlEngine ages a particle to death and fades it") {
    Rig rig(1);
    mm::swirl::Particle& p = rig.at(16, 16);
    p.life = 4;
    DriftMotion still;
    CHECK(p.brightness() == doctest::Approx(1.0f));
    rig.step(still);
    rig.step(still);
    CHECK(rig.engine.pool[0].brightness() == doctest::Approx(0.5f));
    rig.step(still);
    rig.step(still);
    CHECK(rig.engine.live == 0);
}

// An immortal particle stays alive and full however long it lives.
TEST_CASE("SwirlEngine keeps an immortal particle alive") {
    Rig rig(1);
    rig.at(16, 16);
    DriftMotion still;
    for (int s = 0; s < 500; s++) rig.step(still);
    CHECK(rig.engine.live == 1);
    CHECK(rig.engine.pool[0].brightness() == doctest::Approx(1.0f));
}

// A non-finite velocity or depth kills the particle, the rescue rule's floor.
TEST_CASE("SwirlEngine rescues a non-finite particle by its death") {
    Rig rig(2, 32, 32, true, true);
    rig.at(16, 16, 0);
    mm::swirl::Particle& b = rig.at(16, 16, 2);
    *rig.engine.depthOf(&b) = std::numeric_limits<float>::infinity();
    DriftMotion still;
    rig.step(still);
    REQUIRE(rig.engine.live == 1);   // the infinite depth died, the other stood
    CHECK(rig.engine.pool[0].hue == 0);
    DriftMotion nan{std::numeric_limits<float>::quiet_NaN(), 0};
    rig.step(nan);
    CHECK(rig.engine.live == 0);
}

// Die ends a particle at the border.
TEST_CASE("SwirlEngine edge die ends a particle at the border") {
    Rig rig(1);
    rig.at(31.8f, 16);
    DriftMotion right{0.5f, 0};
    rig.step(right, mm::swirl::Edge::kDie);
    CHECK(rig.engine.live == 0);
}

// Wrap brings a particle back on the far side, the flock's torus.
TEST_CASE("SwirlEngine edge wrap brings a particle back on the far side") {
    Rig rig(1);
    rig.at(31.8f, 0.2f);
    DriftMotion out{0.5f, -0.5f};
    rig.step(out, mm::swirl::Edge::kWrap);
    REQUIRE(rig.engine.live == 1);
    CHECK(rig.engine.pool[0].x == doctest::Approx(0.3f));
    CHECK(rig.engine.pool[0].y == doctest::Approx(31.7f));
}

// Bounce reflects the position and turns the velocity back.
TEST_CASE("SwirlEngine edge bounce reflects off the border") {
    Rig rig(1);
    mm::swirl::Particle& p = rig.at(0.2f, 16);
    p.vx = -0.5f;
    struct Keep { void emit(mm::swirl::Engine&) {} bool drive(mm::swirl::Particle&, float*) { return true; } } keep;
    rig.step(keep, mm::swirl::Edge::kBounce);
    REQUIRE(rig.engine.live == 1);
    CHECK(rig.engine.pool[0].x == doctest::Approx(0.3f));
    CHECK(rig.engine.pool[0].vx == doctest::Approx(0.5f));
}

// Free lets a particle fly off the panel, and ends it one space beyond.
TEST_CASE("SwirlEngine edge free ends a particle one space beyond the border") {
    Rig rig(1);
    rig.at(31, 16);
    DriftMotion right{2.0f, 0};
    rig.step(right);
    CHECK(rig.engine.live == 1);   // off the panel, still flying
    for (int s = 0; s < 16; s++) rig.step(right);
    CHECK(rig.engine.live == 0);
}

// Two engines from the same start, with the same motion and its random draws, step to the same place.
TEST_CASE("SwirlEngine steps deterministically from the same start") {
    Rig a(64), b(64);
    mm::swirl::Motion ma, mb;
    ma.emitter = mb.emitter = mm::swirl::Emitter::kNozzle;
    ma.field = mb.field = mm::swirl::Field::kBallistic;
    ma.turbulence = mb.turbulence = mm::swirl::Motion::kTurbStrength;
    for (int s = 0; s < 200; s++) {
        a.step(ma, mm::swirl::Edge::kDie);
        b.step(mb, mm::swirl::Edge::kDie);
    }
    REQUIRE(a.engine.live > 0);
    REQUIRE(a.engine.live == b.engine.live);
    for (uint16_t i = 0; i < a.engine.live; i++) {
        CHECK(a.engine.pool[i].x == b.engine.pool[i].x);
        CHECK(a.engine.pool[i].y == b.engine.pool[i].y);
        CHECK(a.engine.pool[i].hue == b.engine.pool[i].hue);
    }
}

// Settle moves particles and leaves the trail black.
TEST_CASE("SwirlEngine settle moves particles without drawing") {
    Rig rig(1);
    rig.at(10, 10);
    DriftMotion drift{0.5f, 0};
    rig.engine.settle(drift, 4, mm::swirl::Edge::kFree);
    CHECK(rig.engine.pool[0].x == doctest::Approx(12.0f));
    CHECK(lit(rig.engine.trail.data(), rig.engine.trail.bytes()) == 0);
}

// A step draws each move as a streak into the trail.
TEST_CASE("SwirlEngine draws the streaks into the trail") {
    Rig rig(1, 128, 128);   // four lights a unit, so a move of 1.5 units is a streak
    rig.at(4, 16);
    DriftMotion drift{1.5f, 0};
    rig.step(drift);
    CHECK(lit(rig.engine.trail.data(), rig.engine.trail.bytes()) > 3);
}

// With a trail, present copies it and draws the heads over it.
TEST_CASE("SwirlEngine present copies the trail and draws the heads") {
    Rig rig(1, 128, 128);
    rig.at(4, 16);
    DriftMotion drift{1.5f, 0};
    rig.step(drift);
    std::vector<uint8_t> layer(128 * 128 * 3, 0);
    const mm::draw::Canvas cv{layer.data(), layer.size(), mm::Coord3D{128, 128, 1}, 3};
    const auto plane = rig.engine.plane(128, 128, 3);
    rig.engine.present(&*plane, cv, mm::swirl::Look{});
    CHECK(lit(layer.data(), layer.size()) >= lit(rig.engine.trail.data(), rig.engine.trail.bytes()));
    std::memset(rig.engine.trail.data(), 0, rig.engine.trail.bytes());   // a black trail: what the layer shows now is the heads alone
    std::fill(layer.begin(), layer.end(), 0);
    rig.engine.present(&*plane, cv, mm::swirl::Look{});
    CHECK(lit(layer.data(), layer.size()) > 0);
}

// With no trail, present clears the layer and draws each particle's last step as a streak.
TEST_CASE("SwirlEngine present without a trail draws the last step fresh") {
    Rig rig(1, 128, 128, false);
    rig.at(4, 16);
    DriftMotion drift{1.5f, 0};
    rig.step(drift);
    std::vector<uint8_t> layer(128 * 128 * 3, 77);
    const mm::draw::Canvas cv{layer.data(), layer.size(), mm::Coord3D{128, 128, 1}, 3};
    rig.engine.present(nullptr, cv, mm::swirl::Look{});
    const size_t streak = lit(layer.data(), layer.size());
    CHECK(streak > 3);              // the move drawn
    CHECK(streak < 128 * 3);        // and the old picture gone
}

// The plane is none while the layer has changed size under the trail.
TEST_CASE("SwirlEngine plane is none while the layer has changed under it") {
    Rig rig(1, 16, 16);
    CHECK(rig.engine.plane(16, 16, 3).has_value());
    CHECK_FALSE(rig.engine.plane(32, 16, 3).has_value());
}

// A prepare at the same size starts clean: no particles, a black trail.
TEST_CASE("SwirlEngine starts clean on a prepare at the same size") {
    Rig rig(4);
    rig.at(4, 16);
    DriftMotion drift{1.5f, 0};
    rig.step(drift);
    REQUIRE(rig.engine.prepare(rig.owner, 4, false, 32 * 32 * 3));
    CHECK(rig.engine.live == 0);
    CHECK(lit(rig.engine.trail.data(), rig.engine.trail.bytes()) == 0);
}

// The color modes pick the entry by birth, age, speed and place.
TEST_CASE("SwirlEngine color modes pick the entry by birth, age, speed and place") {
    mm::swirl::Particle p;
    p.hue = 10;
    p.age = 5;
    p.vx = 0.3f;
    p.vy = 0.4f;
    p.x = 32;
    p.y = 32;
    using C = mm::swirl::Color;
    CHECK(mm::swirl::Engine::hue(p, C::kBirth) == 10);
    CHECK(mm::swirl::Engine::hue(p, C::kAge) == 15);
    CHECK(mm::swirl::Engine::hue(p, C::kSpeed) == 255);   // half a unit a step and more is the palette's end
    CHECK(mm::swirl::Engine::hue(p, C::kPlace) == 255);   // the far corner
    p.vx = p.vy = 0;
    p.x = p.y = 0;
    CHECK(mm::swirl::Engine::hue(p, C::kSpeed) == 0);
    CHECK(mm::swirl::Engine::hue(p, C::kPlace) == 0);
}

// An immortal particle's age wraps rather than stopping, so the age color keeps turning.
TEST_CASE("SwirlEngine wraps an immortal particle's age") {
    mm::swirl::Particle p;
    p.age = UINT16_MAX;
    CHECK(mm::swirl::Engine::advance(p, nullptr, mm::swirl::Edge::kFree));
    CHECK(p.age == 0);
    CHECK(mm::swirl::Engine::hue(p, mm::swirl::Color::kAge) == p.hue);
}

// Bounce reflects off the far border too, on the other axis.
TEST_CASE("SwirlEngine edge bounce reflects off the far border") {
    mm::swirl::Particle p;
    p.x = 16;
    p.y = 32.3f;
    p.vy = 0.5f;
    CHECK(mm::swirl::Engine::border(p, mm::swirl::Edge::kBounce));
    CHECK(p.y == doctest::Approx(31.7f));
    CHECK(p.vy == doctest::Approx(-0.5f));
}

// A particle a whole space past the border reflects off both borders in turn, its velocity turned back twice, so it ends inside heading as it was.
TEST_CASE("SwirlEngine edge bounce reflects twice from a whole space out") {
    mm::swirl::Particle p;
    p.x = -33.5f;
    p.y = 16;
    p.vx = 0.3f;
    CHECK(mm::swirl::Engine::border(p, mm::swirl::Edge::kBounce));
    CHECK(p.x == doctest::Approx(30.5f));
    CHECK(p.vx == doctest::Approx(0.3f));
}

// The last position inside is the largest float below the space's size.
TEST_CASE("SwirlEngine's last inside position is the float just below the size") {
    CHECK(mm::swirl::kLast == std::nextafter(mm::swirl::kSize, 0.0f));
}

// The edge emitter births every particle on one of the four sides, moving inward.
TEST_CASE("SwirlEngine edge emitter births on a side moving inward") {
    Rig rig(64);
    mm::swirl::Motion m;
    m.emitter = mm::swirl::Emitter::kEdge;
    m.count = 64;
    m.emit(rig.engine);
    REQUIRE(rig.engine.live == 64);
    for (uint16_t i = 0; i < rig.engine.live; i++) {
        const mm::swirl::Particle& p = rig.engine.pool[i];
        CAPTURE(i);
        const bool left = p.x == 0 && p.vx > 0, right = p.x == mm::swirl::kLast && p.vx < 0;
        const bool top = p.y == 0 && p.vy > 0, bottom = p.y == mm::swirl::kLast && p.vy < 0;
        CHECK((left || right || top || bottom));
    }
}
