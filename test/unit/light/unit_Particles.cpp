/// @module particles
/// @also draw, math16

/// The particle kernel: one integrator, wall bounce and aging for every particle effect.
///
/// These tests pin the physics an effect relies on (a force accelerates, a bounce loses its fraction, a dead slot is reused), not coordinates on the fixed-point scale.

#include "doctest.h"
#include "light/powerfunctions/particles.h"

#include <cmath>

using namespace mm;
using namespace mm::particles;
using draw::toSub;

namespace {
/// A pool over plain arrays, standing in for an effect's ScratchBuffers, which works because the kernel is a view that owns nothing.
template <uint16_t N>
struct TestPool {
    draw::pos_t x[N]{}, y[N]{}, vx[N]{}, vy[N]{};
    uint16_t ttl[N]{};
    uint8_t hue[N]{}, acc[N]{}, size[N]{};
    Pool pool;
    TestPool() {
        pool.x = x; pool.y = y; pool.vx = vx; pool.vy = vy;
        pool.ttl = ttl; pool.hue = hue; pool.acc = acc; pool.size = size; pool.count = N;
        pool.clear();
    }
};
}  // namespace

TEST_CASE("a fresh pool is valid and entirely dead") {
    TestPool<8> t;
    CHECK(t.pool.valid());
    CHECK(t.pool.liveCount() == 0);
    CHECK(t.pool.findFree() == 0);          // every slot is available
}

TEST_CASE("spawning brings a particle to life and consumes a slot") {
    TestPool<4> t;
    CHECK(t.pool.spawn(toSub(2), toSub(3), 0, 0, 100, 42) == true);
    CHECK(t.pool.liveCount() == 1);
    CHECK(t.pool.findFree() == 1);          // slot 0 is taken, 1 is next
}

// A pool is a fixed budget: an emitter must be told when it is full rather than overwriting a living particle, which would make bursts eat each other.
TEST_CASE("spawning into a full pool fails rather than overwriting") {
    TestPool<2> t;
    CHECK(t.pool.spawn(0, 0, 0, 0, 50, 0) == true);
    CHECK(t.pool.spawn(0, 0, 0, 0, 50, 0) == true);
    CHECK(t.pool.spawn(0, 0, 0, 0, 50, 0) == false);   // full
    CHECK(t.pool.liveCount() == 2);
}

TEST_CASE("a particle spawned with no life still appears for one frame") {
    TestPool<2> t;
    t.pool.spawn(0, 0, 0, 0, /*life=*/0, 0);
    CHECK(t.pool.liveCount() == 1);         // ttl 0 would be born dead; clamped to 1
}

// --- Integration ---------------------------------------------------------------------------

TEST_CASE("step moves a particle by its velocity") {
    TestPool<2> t;
    t.pool.spawn(toSub(5), toSub(5), toSub(1), toSub(2), 200, 0);
    t.pool.step();
    CHECK(t.x[0] == toSub(6));
    CHECK(t.y[0] == toSub(7));
}

TEST_CASE("a particle with no velocity stays where it was put") {
    TestPool<2> t;
    t.pool.spawn(toSub(4), toSub(4), 0, 0, 200, 0);
    for (int i = 0; i < 10; i++) t.pool.step();
    CHECK(t.x[0] == toSub(4));
    CHECK(t.y[0] == toSub(4));
}

// The reason for semi-implicit Euler: under a constant force the particle must ACCELERATE, each frame covering more ground than the last. An integrator that applied the old velocity would move at a constant rate for the first frame and lag permanently.
TEST_CASE("gravity accelerates: each frame covers more ground than the last") {
    TestPool<2> t;
    t.pool.spawn(toSub(0), toSub(0), 0, 0, 255, 0);
    const draw::pos_t g = toSub(1) / 8;
    draw::pos_t prevStep = 0;
    for (int frame = 0; frame < 6; frame++) {
        const draw::pos_t before = t.y[0];
        t.pool.gravity(g);
        t.pool.step();
        const draw::pos_t moved = t.y[0] - before;
        CHECK(moved > prevStep);            // strictly faster every frame
        prevStep = moved;
    }
}

TEST_CASE("a force pushes along both axes") {
    TestPool<2> t;
    t.pool.spawn(toSub(5), toSub(5), 0, 0, 200, 0);
    t.pool.force(toSub(1), -toSub(1));
    t.pool.step();
    CHECK(t.x[0] > toSub(5));               // pushed right
    CHECK(t.y[0] < toSub(5));               // and up
}

TEST_CASE("drag slows a particle without reversing it") {
    TestPool<2> t;
    t.pool.spawn(0, 0, toSub(4), 0, 200, 0);
    const draw::pos_t v0 = t.vx[0];
    t.pool.drag(64);                        // keep 3/4
    CHECK(t.vx[0] < v0);
    CHECK(t.vx[0] > 0);                     // slowed, not reversed
}

TEST_CASE("zero drag leaves velocity untouched") {
    TestPool<2> t;
    t.pool.spawn(0, 0, toSub(3), toSub(3), 200, 0);
    t.pool.drag(0);
    CHECK(t.vx[0] == toSub(3));
    CHECK(t.vy[0] == toSub(3));
}

// Drag must converge toward rest rather than oscillating or sticking at a nonzero floor.
TEST_CASE("repeated drag brings a particle to rest") {
    TestPool<2> t;
    t.pool.spawn(0, 0, toSub(8), 0, 200, 0);
    for (int i = 0; i < 200; i++) t.pool.drag(128);
    CHECK(t.vx[0] == 0);
}

// --- Life ----------------------------------------------------------------------------------

TEST_CASE("aging kills a particle and frees its slot for reuse") {
    TestPool<2> t;
    t.pool.spawn(0, 0, 0, 0, 3, 0);
    t.pool.age(1);  CHECK(t.pool.liveCount() == 1);
    t.pool.age(1);  CHECK(t.pool.liveCount() == 1);
    t.pool.age(1);  CHECK(t.pool.liveCount() == 0);   // ttl hit zero
    CHECK(t.pool.findFree() == 0);                    // and the slot is reusable
    CHECK(t.pool.spawn(0, 0, 0, 0, 10, 0) == true);
}

TEST_CASE("aging never wraps past zero") {
    TestPool<2> t;
    t.pool.spawn(0, 0, 0, 0, 2, 0);
    t.pool.age(200);                        // a rate larger than the remaining life
    CHECK(t.pool.liveCount() == 0);         // clamped to dead, not wrapped to 255
}

TEST_CASE("a rate of zero makes the pool immortal") {
    TestPool<2> t;
    t.pool.spawn(0, 0, 0, 0, 5, 0);
    for (int i = 0; i < 50; i++) t.pool.age(0);
    CHECK(t.pool.liveCount() == 1);
}

// Dead particles must cost nothing but their slot: no movement, no force, no draw.
TEST_CASE("dead particles are skipped by every pass") {
    TestPool<2> t;
    t.pool.spawn(toSub(5), toSub(5), toSub(2), toSub(2), 1, 0);
    t.pool.age(1);                          // now dead
    const draw::pos_t x0 = t.x[0], y0 = t.y[0];
    t.pool.gravity(toSub(1));
    t.pool.force(toSub(1), toSub(1));
    t.pool.step();
    CHECK(t.x[0] == x0);                    // untouched
    CHECK(t.y[0] == y0);
}

// --- Boundaries ------------------------------------------------------------------------------

TEST_CASE("a particle bounces off a wall and reverses direction") {
    TestPool<2> t;
    t.pool.spawn(toSub(1), toSub(5), -toSub(2), 0, 200, 0);   // moving left
    t.pool.step();                                            // now past the left wall
    t.pool.bounce(toSub(16), toSub(16), 256);
    CHECK(t.x[0] >= 0);                     // pushed back inside
    CHECK(t.vx[0] > 0);                     // and heading the other way
}

TEST_CASE("restitution sets how much speed a bounce keeps") {
    TestPool<2> t;
    t.pool.spawn(toSub(1), toSub(5), -toSub(4), 0, 200, 0);
    t.pool.step();
    t.pool.bounce(toSub(16), toSub(16), 128);       // keep half
    CHECK(t.vx[0] > 0);
    CHECK(t.vx[0] < toSub(4));                      // slower than it arrived

    TestPool<2> dead;
    dead.pool.spawn(toSub(1), toSub(5), -toSub(4), 0, 200, 0);
    dead.pool.step();
    dead.pool.bounce(toSub(16), toSub(16), 0);      // keep nothing
    CHECK(dead.vx[0] == 0);                         // stops dead at the wall
}

// A ball dropped under gravity onto a lossy floor must settle, not gain energy, the classic integrator bug is a bounce that grows.
TEST_CASE("a bouncing particle loses energy rather than gaining it") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(1), 0, 0, 255, 0);
    const draw::pos_t g = toSub(1) / 4;
    draw::pos_t peakSpeed = 0;
    for (int frame = 0; frame < 400; frame++) {
        t.pool.gravity(g);
        t.pool.step();
        t.pool.bounce(toSub(16), toSub(16), 180);   // ~70% restitution
        const draw::pos_t s = t.vy[0] < 0 ? -t.vy[0] : t.vy[0];
        if (s > peakSpeed) peakSpeed = s;
    }
    // Terminal speed under this gravity and restitution is bounded; an energy-gaining bounce would run away instead.
    CHECK(peakSpeed < toSub(8));
}

TEST_CASE("particles outside the grid can be killed instead of bounced") {
    TestPool<4> t;
    t.pool.spawn(toSub(50), toSub(5), 0, 0, 200, 0);    // well off the right edge
    t.pool.spawn(toSub(5), toSub(5), 0, 0, 200, 0);     // inside
    t.pool.killOutside(toSub(16), toSub(16));
    CHECK(t.pool.liveCount() == 1);                     // only the stray died
}

// --- Emitters --------------------------------------------------------------------------------

TEST_CASE("an angled burst emits the requested number of particles") {
    TestPool<16> t;
    t.pool.angleEmit(toSub(8), toSub(8), 0, toSub(2), 8192, 6, 200, 0, 1234);
    CHECK(t.pool.liveCount() == 6);
}

TEST_CASE("a burst stops early rather than overflowing the pool") {
    TestPool<4> t;
    t.pool.angleEmit(toSub(8), toSub(8), 0, toSub(2), 8192, 20, 200, 0, 1234);
    CHECK(t.pool.liveCount() == 4);          // filled the pool, then stopped
}

// A burst must actually spread: particles all sharing one velocity would read as a single moving dot rather than an explosion.
TEST_CASE("a burst sends particles in different directions") {
    TestPool<16> t;
    t.pool.angleEmit(toSub(8), toSub(8), 0, toSub(3), 16384, 8, 200, 0, 99);
    int distinct = 0;
    for (uint16_t i = 1; i < 8; i++)
        if (t.vx[i] != t.vx[0] || t.vy[i] != t.vy[0]) distinct++;
    CHECK(distinct >= 5);
}

// The supersync property: the same seed must produce the same burst on any device, which a stream RNG could not guarantee.
TEST_CASE("the same seed produces the same burst") {
    TestPool<8> a, b;
    a.pool.angleEmit(toSub(4), toSub(4), 1000, toSub(2), 8192, 5, 100, 7, 555);
    b.pool.angleEmit(toSub(4), toSub(4), 1000, toSub(2), 8192, 5, 100, 7, 555);
    for (uint16_t i = 0; i < 8; i++) {
        CHECK(a.vx[i] == b.vx[i]);
        CHECK(a.vy[i] == b.vy[i]);
    }
}

// --- Attraction ------------------------------------------------------------------------------

TEST_CASE("an attractor pulls a particle toward it") {
    TestPool<2> t;
    t.pool.spawn(toSub(2), toSub(8), 0, 0, 200, 0);
    t.pool.attract(toSub(12), toSub(8), 4096);
    CHECK(t.vx[0] > 0);                     // pulled to the right, toward the attractor
    CHECK(t.vy[0] == 0);                    // no vertical component: it is level with it
}

// The near-field clamp: a particle sitting on the attractor must not receive an unbounded impulse, which is where an unclamped inverse-square divides by zero.
TEST_CASE("an attractor does not fling a particle sitting on top of it") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 200, 0);
    t.pool.attract(toSub(8), toSub(8), 100000);
    // The magnitude, since the blow-up goes either way; widened first, as std::abs of int32's minimum has no int32 value.
    CHECK(std::llabs(static_cast<int64_t>(t.vx[0])) < toSub(100));   // finite, not a blowup
    CHECK(std::llabs(static_cast<int64_t>(t.vy[0])) < toSub(100));
}

// --- Rendering -------------------------------------------------------------------------------

TEST_CASE("rendering lights the grid where particles are, and nowhere else") {
    Buffer buf;
    buf.allocate(16 * 16, 3);
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 16, 16, 1);

    TestPool<4> t;
    t.pool.spawn(toSub(4), toSub(4), 0, 0, 255, 100);
    t.pool.render(cv, 255, RenderStyle::Hard);

    const size_t at = (4 * 16 + 4) * 3;
    CHECK((buf.data()[at] || buf.data()[at + 1] || buf.data()[at + 2]));
    const size_t elsewhere = (10 * 16 + 10) * 3;
    CHECK(buf.data()[elsewhere] == 0);
}

TEST_CASE("an empty pool renders nothing") {
    Buffer buf;
    buf.allocate(8 * 8, 3);
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 8, 8, 1);
    TestPool<4> t;
    t.pool.render(cv);
    for (size_t i = 0; i < buf.bytes(); i++) CHECK(buf.data()[i] == 0);
}

// Brightness rides ttl, so a particle fades out as it dies without the effect tracking a second quantity.
TEST_CASE("a dying particle renders dimmer than a fresh one") {
    Buffer fresh, dying;
    fresh.allocate(8 * 8, 3); fresh.clear();
    dying.allocate(8 * 8, 3); dying.clear();
    const draw::Canvas cf = draw::Canvas::of(fresh, 8, 8, 1);
    const draw::Canvas cd = draw::Canvas::of(dying, 8, 8, 1);

    TestPool<2> a, b;
    a.pool.spawn(toSub(4), toSub(4), 0, 0, 255, 100);
    b.pool.spawn(toSub(4), toSub(4), 0, 0, 20, 100);
    a.pool.render(cf, 255, RenderStyle::Hard);
    b.pool.render(cd, 255, RenderStyle::Hard);

    auto sum = [](const Buffer& x) {
        uint32_t s = 0;
        for (size_t i = 0; i < x.bytes(); i++) s += x.data()[i];
        return s;
    };
    CHECK(sum(dying) < sum(fresh));
}

// A particle outside the grid must clip rather than write out of bounds, the robustness rule the draw primitives already follow, checked here because the pool is a new caller of them.
TEST_CASE("particles outside the grid draw nothing and do not crash") {
    Buffer buf;
    buf.allocate(8 * 8, 3);
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 8, 8, 1);
    TestPool<4> t;
    t.pool.spawn(toSub(-20), toSub(-20), 0, 0, 255, 0);
    t.pool.spawn(toSub(99), toSub(99), 0, 0, 255, 0);
    t.pool.render(cv);
    for (size_t i = 0; i < buf.bytes(); i++) CHECK(buf.data()[i] == 0);
}

// An invalid pool is a real state: an effect whose ScratchBuffer allocation failed must degrade, never crash.
TEST_CASE("a pool with no storage reports itself invalid") {
    Pool p;
    CHECK(p.valid() == false);
}

// --- The industry-standard details ------------------------------------------------------------

// `-1 >> 1` is `-1`, so a signed shift rounds negatives away from zero; per frame that drifts leftward movers further, so scaling is symmetric.
TEST_CASE("drag slows left and right movers by the same amount") {
    TestPool<4> t;
    t.pool.spawn(0, 0,  toSub(3), 0, 200, 0);   // moving right
    t.pool.spawn(0, 0, -toSub(3), 0, 200, 0);   // moving left, same speed
    for (int i = 0; i < 30; i++) t.pool.drag(20);
    CHECK(t.vx[0] == -t.vx[1]);                 // symmetric: no directional drift
}

TEST_CASE("a bounce loses the same speed in either direction") {
    TestPool<4> a, b;
    a.pool.spawn(toSub(1), toSub(5), -toSub(3), 0, 200, 0);
    b.pool.spawn(toSub(15), toSub(5), toSub(3), 0, 200, 0);
    a.pool.step(); b.pool.step();
    a.pool.bounce(toSub(16), toSub(16), 128);
    b.pool.bounce(toSub(16), toSub(16), 128);
    CHECK(a.vx[0] == -b.vx[0]);                 // mirror images, exactly
}

// The 3.4 accumulator: a force below one velocity unit per frame would truncate to nothing, so gentle wind and weak attractors would simply be invisible. The fraction has to build up and spill.
TEST_CASE("a sub-unit force eventually moves a particle") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 0);
    for (int i = 0; i < 4; i++) t.pool.forceSmall(4, 0);   // 4/16 per frame
    CHECK(t.vx[0] == 1);                        // four quarter-steps make one whole unit
}

TEST_CASE("a sub-unit force does nothing on its first frame, then accumulates") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 0);
    t.pool.forceSmall(1, 0);                    // 1/16 of a unit
    CHECK(t.vx[0] == 0);                        // too small to move yet
    for (int i = 0; i < 15; i++) t.pool.forceSmall(1, 0);
    CHECK(t.vx[0] == 1);                        // sixteen frames make one unit
}

TEST_CASE("a sub-unit force works in both directions") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 0);
    for (int i = 0; i < 16; i++) t.pool.forceSmall(-1, 0);
    CHECK(t.vx[0] == -1);
}

// The accumulator is optional storage. An effect that does not need sub-unit forces should not have to allocate for it, and calling the function without it must degrade rather than crash.
TEST_CASE("a pool without an accumulator ignores sub-unit forces") {
    TestPool<2> t;
    t.pool.acc = nullptr;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 0);
    for (int i = 0; i < 40; i++) t.pool.forceSmall(4, 0);
    CHECK(t.vx[0] == 0);                        // dropped, not crashed
}

// --- Framerate independence -------------------------------------------------------------------

// Time, not the frame count, drives the physics: the same span of real time at any framerate lands a particle in the same place.
TEST_CASE("a particle lands in the same place at any framerate") {
    auto simulate = [](int fps) {
        TestPool<2> t;
        t.pool.spawn(toSub(0), toSub(0), toSub(2), -toSub(6), 255, 0);
        particles::FrameTime ft(60);
        for (int f = 0; f < 3 * fps; f++) {
            const uint32_t now = 1000u + static_cast<uint32_t>(static_cast<long long>(f) * 1000 / fps);
            const uint32_t s = ft.advance(now);
            if (!s) continue;
            t.pool.gravity(20, s);
            t.pool.step(s);
        }
        return Coord3D{static_cast<lengthType>(t.x[0] / draw::kSubOne),
                       static_cast<lengthType>(t.y[0] / draw::kSubOne), 0};
    };
    const auto slow = simulate(60);
    for (int fps : {200, 470, 2000, 5000}) {
        CAPTURE(fps);
        const auto fast = simulate(fps);
        // Within a pixel or two: Euler integration error shrinks as the step does, it does not grow.
        CHECK(std::abs(fast.x - slow.x) <= 3);
        CHECK(std::abs(fast.y - slow.y) <= 3);
    }
}

// A frame faster than the millisecond timer reports dt == 0. Dropping those frames' share of a force would leave a very fast device with no gravity at all, so the remainder has to carry.
TEST_CASE("gravity still acts when frames are faster than the clock can resolve") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 0);
    particles::FrameTime ft(60);
    // 10000 frames all reporting the same millisecond, then time moves on.
    for (int f = 0; f < 10000; f++) {
        const uint32_t s = ft.advance(1000);
        if (s) { t.pool.gravity(20, s); t.pool.step(s); }
    }
    const draw::pos_t afterStall = t.y[0];
    for (int f = 1; f <= 60; f++) {
        const uint32_t s = ft.advance(1000 + f * 16);
        if (s) { t.pool.gravity(20, s); t.pool.step(s); }
    }
    CHECK(t.y[0] > afterStall);          // real time passing moves it; a stalled clock does not
}

// --- Collisions and spray ---------------------------------------------------------------------

TEST_CASE("two particles approaching head-on bounce apart") {
    TestPool<4> t;
    t.pool.spawn(toSub(4), toSub(4),  toSub(1), 0, 200, 0);   // moving right
    t.pool.spawn(toSub(5), toSub(4), -toSub(1), 0, 200, 0);   // moving left, one pixel away
    t.pool.collide(toSub(2), 256);
    CHECK(t.vx[0] < 0);                      // the right-mover now heads left
    CHECK(t.vx[1] > 0);                      // and the left-mover heads right
}

TEST_CASE("particles too far apart do not interact") {
    TestPool<4> t;
    t.pool.spawn(toSub(2), toSub(2), toSub(1), 0, 200, 0);
    t.pool.spawn(toSub(12), toSub(12), -toSub(1), 0, 200, 0);
    const draw::pos_t v0 = t.vx[0], v1 = t.vx[1];
    t.pool.collide(toSub(2), 256);
    CHECK(t.vx[0] == v0);
    CHECK(t.vx[1] == v1);
}

// Already-separating pairs must be left alone, or a particle that has just bounced gets kicked again and the pair sticks together vibrating.
TEST_CASE("particles already moving apart are not kicked again") {
    TestPool<4> t;
    t.pool.spawn(toSub(4), toSub(4), -toSub(1), 0, 200, 0);   // moving away from its neighbor
    t.pool.spawn(toSub(5), toSub(4),  toSub(1), 0, 200, 0);
    const draw::pos_t v0 = t.vx[0], v1 = t.vx[1];
    t.pool.collide(toSub(2), 256);
    CHECK(t.vx[0] == v0);
    CHECK(t.vx[1] == v1);
}

// The documented trap: pushing BOTH particles apart makes each shove create the overlap the other resolves, so the pair jitters forever. Exactly one moves, and the pair must settle.
TEST_CASE("overlapping particles separate instead of jittering forever") {
    TestPool<4> t;
    t.pool.spawn(toSub(4), toSub(4), 0, 0, 200, 0);
    t.pool.spawn(toSub(4) + 8, toSub(4), 0, 0, 200, 0);       // heavily overlapped
    for (int i = 0; i < 20; i++) t.pool.collide(toSub(1), 200);
    const int32_t gap = (t.x[1] > t.x[0]) ? (t.x[1] - t.x[0]) : (t.x[0] - t.x[1]);
    CHECK(gap > 8);                          // pushed apart, not still on top of each other
}

TEST_CASE("collisions with a zero radius do nothing") {
    TestPool<4> t;
    t.pool.spawn(toSub(4), toSub(4), toSub(1), 0, 200, 0);
    t.pool.spawn(toSub(4), toSub(4), -toSub(1), 0, 200, 0);
    const draw::pos_t v0 = t.vx[0];
    t.pool.collide(0, 256);
    CHECK(t.vx[0] == v0);
}

TEST_CASE("a spray emits the requested number of particles in varied directions") {
    TestPool<16> t;
    t.pool.spray(toSub(8), toSub(8), toSub(2), 8, 200, 0, 77);
    CHECK(t.pool.liveCount() == 8);
    int varied = 0;
    for (uint16_t i = 1; i < 8; i++)
        if (t.vx[i] != t.vx[0] || t.vy[i] != t.vy[0]) varied++;
    CHECK(varied >= 5);                      // a scatter, not one shared velocity
}

TEST_CASE("the same seed produces the same spray") {
    TestPool<8> a, b;
    a.pool.spray(toSub(4), toSub(4), toSub(2), 5, 100, 7, 321);
    b.pool.spray(toSub(4), toSub(4), toSub(2), 5, 100, 7, 321);
    for (uint16_t i = 0; i < 8; i++) {
        CHECK(a.vx[i] == b.vx[i]);
        CHECK(a.vy[i] == b.vy[i]);
    }
}

// --- The base-set additions (industry audit, 2026-08-07) ----------------------------------------

// A byte capped life at 255 reference frames, about 4.25 s at 60 Hz, so slow smoke, drifting snow and long fades were not expressible at all. WLED-PS uses the same width for the same reason.
TEST_CASE("a particle can outlive the old 8-bit ceiling") {
    TestPool<2> t;
    t.pool.spawn(0, 0, 0, 0, /*life=*/3000, 0);
    CHECK(t.ttl[0] == 3000);                      // held, not truncated to 255
    for (int i = 0; i < 1000; i++) t.pool.age(1);
    CHECK(t.pool.liveCount() == 1);               // still alive after a thousand frames
    CHECK(t.ttl[0] == 2000);
}

// Wrapping is the third wall behavior beside bounce and killOutside, and the one an endless field needs. Snow, rain and marquees want to re-enter, not rattle in a box or fall off a cliff.
TEST_CASE("a particle leaving one edge re-enters the opposite one") {
    TestPool<4> t;
    const draw::pos_t w = toSub(16), h = toSub(16);
    t.pool.spawn(toSub(-2), toSub(8), 0, 0, 500, 0);     // off the left
    t.pool.spawn(toSub(18), toSub(8), 0, 0, 500, 0);     // off the right
    t.pool.wrap(w, h);
    CHECK(t.x[0] > 0);                            // came back on the right
    CHECK(t.x[0] <= w);
    CHECK(t.x[1] >= 0);                           // and this one on the left
    CHECK(t.x[1] < w);
}

TEST_CASE("wrapping can be enabled per axis") {
    TestPool<2> t;
    const draw::pos_t w = toSub(16), h = toSub(16);
    t.pool.spawn(toSub(-2), toSub(-2), 0, 0, 500, 0);
    t.pool.wrap(w, h, /*wrapX=*/true, /*wrapY=*/false);
    CHECK(t.x[0] > 0);                            // x wrapped...
    CHECK(t.y[0] < 0);                            // ...y was left alone, so a snowfall still lands
}

// Wrapping reduces by modulo, so a far throw costs what a near one does; the exact landing points are pinned, as "back inside" passes an off-by-a-span wrap.
TEST_CASE("wrapping puts a particle at an exact position, however far out it started") {
    TestPool<4> t;
    const draw::pos_t w = toSub(16), h = toSub(16);
    t.pool.spawn(w + toSub(3), toSub(8), 0, 0, 500, 0);         // 3 past the right edge
    t.pool.spawn(toSub(-3), toSub(8), 0, 0, 500, 0);            // 3 before the left edge
    t.pool.spawn(w * 50 + toSub(5), toSub(8), 0, 0, 500, 0);    // fifty grids out: one modulo, not 50 loops
    t.pool.wrap(w, h);
    CHECK(t.x[0] == toSub(3));
    CHECK(t.x[1] == w - toSub(3));
    CHECK(t.x[2] == toSub(5));
}

// The edges differ on purpose: from above a particle stops at the far edge, from below at 0, the same point on a wrapped axis, as the wall passes agree.
TEST_CASE("an exact multiple of the axis lands on the edge it approached from") {
    TestPool<4> t;
    const draw::pos_t w = toSub(16), h = toSub(16);
    t.pool.spawn(w * 2, toSub(8), 0, 0, 500, 0);       // exactly two grids to the right
    t.pool.spawn(-w, toSub(8), 0, 0, 500, 0);          // exactly one grid to the left
    t.pool.wrap(w, h);
    CHECK(t.x[0] == w);                               // reduced from above: rests on the far edge
    CHECK(t.x[1] == 0);                               // climbed from below: rests on zero
}

TEST_CASE("wrapping leaves a particle already inside the grid untouched") {
    TestPool<2> t;
    t.pool.spawn(toSub(8), toSub(8), 0, 0, 500, 0);
    t.pool.wrap(toSub(16), toSub(16));
    CHECK(t.x[0] == toSub(8));
    CHECK(t.y[0] == toSub(8));
}

// Size is what makes a pool read as blobs rather than a scatter of points, the signature look of a particle system, and its absence is the likeliest "these aren't real particles" complaint.
TEST_CASE("a sized particle draws a disc rather than a point") {
    Buffer small, big;
    small.allocate(16 * 16, 3); small.clear();
    big.allocate(16 * 16, 3);   big.clear();
    const draw::Canvas cs = draw::Canvas::of(small, 16, 16, 1);
    const draw::Canvas cb = draw::Canvas::of(big, 16, 16, 1);

    TestPool<2> a, b;
    a.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 100, /*radius=*/0);
    b.pool.spawn(toSub(8), toSub(8), 0, 0, 255, 100, /*radius=*/3);
    a.pool.render(cs);
    b.pool.render(cb);

    auto lit = [](const Buffer& x) {
        int n = 0;
        for (nrOfLightsType i = 0; i < x.count(); i++) {
            const size_t o = static_cast<size_t>(i) * 3;
            if (x.data()[o] || x.data()[o + 1] || x.data()[o + 2]) n++;   // any channel
        }
        return n;
    };
    CHECK(lit(big) > lit(small) * 4);             // a disc covers far more than a splat
}

TEST_CASE("a pool without size storage still renders every particle") {
    Buffer buf;
    buf.allocate(8 * 8, 3);
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 8, 8, 1);
    TestPool<2> t;
    t.pool.size = nullptr;                        // optional storage, absent
    t.pool.spawn(toSub(4), toSub(4), 0, 0, 255, 100);
    t.pool.render(cv, 255, RenderStyle::Hard);
    const size_t at = (4 * 8 + 4) * 3;
    CHECK((buf.data()[at] || buf.data()[at + 1] || buf.data()[at + 2]));   // drawn, not skipped
}

// FrameTime measures how much of a reference frame a frame covered, so its period is exact: `1000 / 60` truncates to 16 ms, and every setting runs 4% fast.
TEST_CASE("one second of motion is the same amount of motion however fast the device renders") {
    // One second of real time is 60 reference frames of movement, whether the device drew 30 frames or 1200.
    for (int fps : {30, 60, 240, 1200}) {
        CAPTURE(fps);
        particles::FrameTime t{60};
        uint64_t total = 0;
        // Inclusive of f == fps, so the timeline really covers 0..1000 ms. Stopping one frame short measures slightly less than a second and hides a small rate error in the shortfall.
        for (int f = 0; f <= fps; f++)
            total += t.advance(static_cast<uint32_t>(static_cast<uint64_t>(f) * 1000 / fps));
        const double refFrames = static_cast<double>(total) / particles::FrameTime::kOne;
        // One reference frame of tolerance: the first advance() seeds the time base and returns a whole unit, and the last partial unit stays in the carry. A wrong reference period is a flat 4% (2.4 frames), so it does not fit inside this band.
        CHECK(refFrames > 59.0);
        CHECK(refFrames < 62.0);
    }
}

// Slots map onto distinct lanes at any count: the interleave's step is coprime with the count, where `(i * 5) % 5` stacked Pacman's five on one row.
TEST_CASE("spreadLane gives every slot its own lane, for any count") {
    // The extent is a multiple of every count tested, so each lane lands on its own exact position and two slots sharing a lane is a real collision rather than a rounding artifact.
    constexpr mm::lengthType kExtent = 27720;   // lcm(1..12), divisible by every count below
    for (uint16_t slots = 1; slots <= 12; slots++) {
        bool seen[16] = {};
        uint16_t distinct = 0;
        for (uint16_t i = 0; i < slots; i++) {
            const mm::lengthType lane = mm::particles::spreadLane(i, slots, kExtent);
            REQUIRE(lane >= 0);
            REQUIRE(lane < kExtent);
            const int idx = lane / (kExtent / slots);
            if (!seen[idx]) { seen[idx] = true; distinct++; }
        }
        CHECK(distinct == slots);          // a collision means two sprites on the same lane
    }
}

// The interleave is the point: consecutive slots must not land on adjacent lanes, or assigning slots by species puts every fast one together regardless of the lanes being distinct.
TEST_CASE("spreadLane interleaves rather than striding in order") {
    // Every consecutive pair, since a stride of `slots - 1` is coprime yet walks consecutive slots down neighboring lanes.
    constexpr mm::lengthType kExtent = 27720;      // lcm(1..12): every count below divides it
    for (uint16_t slots = 5; slots <= 12; slots++) {
        if (slots == 6) continue;   // 6 has no coprime near 3 (2 and 3 both share a factor)
        const mm::lengthType lane = kExtent / slots;
        for (uint16_t i = 1; i < slots; i++) {
            const int a = mm::particles::spreadLane(static_cast<uint16_t>(i - 1), slots, kExtent);
            const int b = mm::particles::spreadLane(i, slots, kExtent);
            CHECK(a != b);                          // never the same lane
            CHECK(b - a != lane);                   // never the next lane up
            CHECK(a - b != lane);                   // nor the next lane down
        }
    }
}

// audio-reactive sprites: the behavior a viewer judges is "it moves with the music, and it stops when the music stops". Both halves are pinned here because both were explicit requirements.
TEST_CASE("Silence stands the sprites still") {
    mm::AudioFrame quiet;                       // levelSmoothed 0: no music playing
    for (uint16_t i = 0; i < 8; i++) CHECK(mm::particles::audioDrive(&quiet, i, 8) == 0);
}

// The pauses inside the music are part of the dance: a quiet moment slows the scene toward a stop while the gate stays open.
TEST_CASE("A quiet moment in the music slows the sprites toward a stop") {
    mm::AudioFrame playing, hush;
    playing.gate = hush.gate = 40;
    playing.gateOpen = hush.gateOpen = true;   // the music plays on, so the gate holds
    playing.level = 200;
    hush.level = 0;                            // a breath between phrases
    CHECK(mm::particles::audioDrive(&hush, 0, 8) == 0);
    CHECK(mm::particles::audioDrive(&playing, 0, 8) > 0);
}

TEST_CASE("Without an audio source the sprites keep moving normally") {
    // A device with no microphone must not end up with a frozen scene.
    CHECK(mm::particles::audioDrive(nullptr, 0, 8) == mm::particles::FrameTime::kOne);
}

TEST_CASE("Loud music moves the sprites faster than quiet music") {
    mm::AudioFrame quiet, loud;
    quiet.level = quiet.levelSmoothed = 40;
    loud.level  = loud.levelSmoothed  = 200;
    for (uint8_t b = 0; b < 16; b++) { quiet.bands[b] = 20; loud.bands[b] = 240; }
    CHECK(mm::particles::audioDrive(&loud, 0, 8) > mm::particles::audioDrive(&quiet, 0, 8));
}

// The point of a per-sprite band rather than one overall volume. A bass-heavy moment moves the bass sprites and leaves the treble ones alone, so the scene never surges as a single block.
TEST_CASE("Each sprite follows its own frequency band") {
    mm::AudioFrame f;
    f.level = f.levelSmoothed = 128;
    for (uint8_t b = 0; b < 16; b++) f.bands[b] = 0;
    f.bands[0] = 255;                            // bass only
    const uint32_t bass   = mm::particles::audioDrive(&f, 0, 8);
    const uint32_t treble = mm::particles::audioDrive(&f, 7, 8);
    CHECK(bass > treble);
    CHECK(treble > 0);                           // a quiet band drifts, it does not freeze mid-air
}

// The bands spread over the live sprites, not the pool's capacity, which would crowd every sprite into the bass and leave the treble driving nothing.
TEST_CASE("The spectrum is spread over the live sprites, not the pool capacity") {
    AudioFrame f;
    f.level = f.levelSmoothed = 128;
    for (uint8_t b = 0; b < 16; b++) f.bands[b] = 0;

    // Five live sprites in a pool sized for twelve. Spread over the LIVE count they take bands 0, 3, 6, 9, 12; spread over the capacity they would take 0, 1, 2, 4, 5 - the bottom third.
    const uint16_t live = 5, capacity = 12;
    f.bands[12] = 255;                     // energy where only the live-count spread reaches

    CHECK(particles::audioDrive(&f, live - 1, live) >
          particles::audioDrive(&f, 0, live));            // the top sprite hears it
    CHECK(particles::audioDrive(&f, live - 1, capacity) ==
          particles::audioDrive(&f, 0, capacity));        // spread over capacity: nobody hears it
}

// Pool::stepDriven is the shared entry point the sprite effects use, so the rules ride on it too.
TEST_CASE("audio-reactive stepping moves sprites by their own band, and not at all in silence") {
    draw::pos_t x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0};
    draw::pos_t vx[4] = {256, 256, 256, 256}, vy[4] = {0, 0, 0, 0};
    uint16_t ttl[4] = {1, 1, 1, 1};
    uint8_t hue[4] = {0, 0, 0, 0};
    particles::Pool p;
    p.x = x; p.y = y; p.vx = vx; p.vy = vy; p.ttl = ttl; p.hue = hue; p.count = 4;

    // soundReactive off: the pool steps together, unaffected by whatever the microphone hears.
    p.stepDriven(particles::FrameTime::kOne, /*soundReactive=*/false, 4);
    CHECK(x[0] > 0);
    const draw::pos_t moved = x[0];
    for (int i = 1; i < 4; i++) CHECK(x[i] == moved);   // one scale for the whole pool
}
