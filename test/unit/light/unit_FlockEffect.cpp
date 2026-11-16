/// @module FlockEffect

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/FlockEffect.h"

#include <cstring>
#include <vector>

namespace {

/// One run of the flock: the grid, the controls, and how many ticks how far apart.
struct Flight {
    uint8_t count = 0;
    mm::lengthType w = 32, h = 32;
    uint32_t frames = 60, stepMs = 20;
    uint8_t cpl = 3;
    uint8_t size = 0;
    uint8_t speed = 60;
    uint8_t audioGain = 200;
};

/// Render `frames` ticks of a flock `stepMs` apart on a grid, and return the last frame's bytes.
std::vector<uint8_t> renderFlock(const Flight& run) {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(run.w, run.h, 1, run.cpl);
    mm::FlockEffect flock;
    flock.count = run.count;
    flock.size = run.size;
    flock.speed = run.speed;
    flock.audioGain = run.audioGain;
    fx.add(flock);
    for (uint32_t f = 0; f <= run.frames; f++) fx.tickAt(1000 + f * run.stepMs);
    return fx.frame();
}

/// How many lights carry any color.
size_t litLights(const std::vector<uint8_t>& frame) {
    size_t lit = 0;
    for (size_t i = 0; i + 2 < frame.size(); i += 3)
        if (frame[i] | frame[i + 1] | frame[i + 2]) lit++;
    return lit;
}

}  // namespace

// The trail plane (w*h*cpl) and the flock's own room, 255 boids of six floats: position, velocity and the gathered acceleration.
TEST_CASE("FlockEffect sizes the trail and room for the largest flock") {
    mm::golden::LayerFixture fx(16, 16);
    mm::FlockEffect flock;
    fx.add(flock);
    CHECK(flock.dynamicBytes() == 16 * 16 * 3 + 255 * 6 * sizeof(float));
}

TEST_CASE("FlockEffect draws the flock") {
    CHECK(litLights(renderFlock({.frames = 30})) > 0);
    CHECK(litLights(renderFlock({.w = 64, .h = 16, .frames = 30})) > 0);
}

// Each step keeps 230/255 of the trail, so a boid's path stays lit for dozens of steps.
TEST_CASE("FlockEffect leaves a trail behind each boid") {
    const uint8_t boids = 3;
    CHECK(litLights(renderFlock({.count = boids, .frames = 120})) > 2 * (boids + 1));
}

TEST_CASE("FlockEffect flies the number of boids asked for") {
    CHECK(litLights(renderFlock({.count = 60})) > litLights(renderFlock({.count = 2})));
}

// A width of 96 gives 32 boids, a third of the width as Aurora flies.
TEST_CASE("FlockEffect with no count flies a third of the grid's width") {
    CHECK(renderFlock({.w = 96}) == renderFlock({.count = 32, .w = 96}));
}

// The same flight drawn wider covers more lights, on Aurora's own grid and on a larger one.
TEST_CASE("FlockEffect draws a boid as wide as size asks") {
    CHECK(litLights(renderFlock({.count = 4, .size = 5})) > litLights(renderFlock({.count = 4, .size = 1})));
    CHECK(litLights(renderFlock({.count = 4, .w = 64, .h = 64, .size = 8})) >
          litLights(renderFlock({.count = 4, .w = 64, .h = 64, .size = 2})));
}

// After one step a lone boid and the predator have each been drawn once: one light apiece at size 0, a dot apiece at size 3.
TEST_CASE("FlockEffect with no size draws one light per boid on Aurora's grid") {
    CHECK(litLights(renderFlock({.count = 1, .frames = 0})) <= 2);
    CHECK(litLights(renderFlock({.count = 1, .frames = 0, .size = 3})) > 2);
}

// The same simulated second is the same sixty steps whether it came as fifty frames or two hundred.
TEST_CASE("FlockEffect moves with elapsed time, not with frame count") {
    CHECK(renderFlock({.count = 8, .frames = 50}) == renderFlock({.count = 8, .frames = 200, .stepMs = 5}));
}

// Speed is steps a second: twice the speed for half the time flies the same 74 steps (the first tick owes one reference frame, so 2 at 120 and 1 at 60, then 72 and 73.2 from the clock).
TEST_CASE("FlockEffect at twice the speed flies in half the time") {
    CHECK(renderFlock({.count = 8, .frames = 30, .speed = 120}) == renderFlock({.count = 8, .frames = 61, .speed = 60}));
    CHECK(renderFlock({.count = 8, .frames = 30, .speed = 120}) != renderFlock({.count = 8, .frames = 30, .speed = 60}));
}

// A full level at audioGain 255 doubles the pace, so the flock flies what speed 120 flies in silence; at audioGain 0 the pace is speed alone.
TEST_CASE("FlockEffect flies faster the louder the room") {
    mm::golden::ScopedTestClock clock(1000);
    static mm::AudioService audio;
    audio.defineControls();
    audio.setup();
    audio.tick();   // claims the seat latestFrame reads
    mm::AudioFrame& f = audio.frameForTest();
    f.level = f.levelSmoothed = 255;
    const auto loud = renderFlock({.count = 8, .frames = 30, .audioGain = 255});
    const auto deaf = renderFlock({.count = 8, .frames = 30, .audioGain = 0});
    audio.release();
    CHECK(loud == renderFlock({.count = 8, .frames = 30, .speed = 120}));
    CHECK(deaf == renderFlock({.count = 8, .frames = 30}));
}

// At speed 0 no step runs: the flock shows where it starts and stays there.
TEST_CASE("FlockEffect at speed 0 holds the flock still where it is") {
    CHECK(litLights(renderFlock({.count = 8, .speed = 0})) > 0);
    CHECK(renderFlock({.count = 8, .frames = 10, .speed = 0}) == renderFlock({.count = 8, .frames = 100, .speed = 0}));
}

// The seed is fixed, so a run is reproducible.
TEST_CASE("FlockEffect repeats itself from the same start") {
    CHECK(renderFlock({.count = 8}) == renderFlock({.count = 8}));
}

// Wrapping keeps a boid on the grid forever, so a long run keeps some lights lit and some dark.
TEST_CASE("FlockEffect keeps flying for a long run") {
    const size_t lit = litLights(renderFlock({.count = 10, .frames = 3000}));
    CHECK(lit > 0);
    CHECK(lit < 32 * 32);
}

// The first channel carries the same picture however many channels a light has, so a one-channel layout is not smeared by writes meant for three.
TEST_CASE("FlockEffect draws the same picture on lights of one, three and four channels") {
    const auto one = renderFlock({.count = 8, .cpl = 1});
    const auto three = renderFlock({.count = 8, .cpl = 3});
    const auto four = renderFlock({.count = 8, .cpl = 4});
    REQUIRE(one.size() == 32 * 32);
    for (size_t light = 0; light < 32 * 32; light++) {
        CHECK(one[light] == three[light * 3]);
        CHECK(four[light * 4] == three[light * 3]);
    }
}

// Any grid, down to a single light, draws safely, at the fastest pace too.
TEST_CASE("FlockEffect survives degenerate grids") {
    CHECK_NOTHROW(renderFlock({.w = 1, .h = 1, .frames = 5}));
    CHECK_NOTHROW(renderFlock({.count = 255, .w = 2, .h = 2, .frames = 20}));
    CHECK_NOTHROW(renderFlock({.count = 255, .w = 128, .h = 128, .frames = 5}));
    CHECK_NOTHROW(renderFlock({.count = 255, .w = 64, .h = 64, .frames = 5, .speed = 255}));
}


// Steps keep their remainder from tick to tick, so a speed that does not divide the reference rate flies the same at 50 frames a second and at 1000.
TEST_CASE("FlockEffect flies the same at any frame rate at any speed") {
    CHECK(renderFlock({.count = 8, .frames = 60, .stepMs = 20, .speed = 70}) == renderFlock({.count = 8, .frames = 1200, .stepMs = 1, .speed = 70}));
    CHECK(litLights(renderFlock({.count = 8, .frames = 1000, .stepMs = 1, .speed = 2})) > 0);   // a slow flock still moves on a fast render
    CHECK(renderFlock({.count = 8, .w = 64, .h = 64, .frames = 60, .stepMs = 20}) == renderFlock({.count = 8, .w = 64, .h = 64, .frames = 300, .stepMs = 4}));   // soft dots too
}

// A strip is a grid one light high: the flock flies along it.
TEST_CASE("FlockEffect draws on a strip") {
    CHECK(litLights(renderFlock({.w = 60, .h = 1})) > 0);
}

// A layer whose lights change under the effect before it is prepared again keeps its picture, rather than the effect writing past its trail.
TEST_CASE("FlockEffect leaves the layer as it was while the layer has changed under it and prepare has not caught up") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(16, 16);
    mm::Layer& layer = fx.layer;
    mm::FlockEffect flock;
    fx.add(flock);
    fx.tickAt(1020);
    const std::vector<uint8_t> before(layer.buffer().data(), layer.buffer().data() + 16 * 16 * 3);
    layer.setChannelsPerLight(4);   // the trail is three channels a light, the layer now four
    mm::platform::setTestNowMs(1040);
    flock.tick();
    CHECK(std::vector<uint8_t>(layer.buffer().data(), layer.buffer().data() + 16 * 16 * 3) == before);
}

namespace {

/// Two boids, `a` and `b`, given one step of Reynolds' flocking from the same moment, then moved.
void stepPair(mm::boids::Boid& a, mm::boids::Boid& b) {
    mm::boids::Boid pair[2] = {a, b};
    mm::boids::gather(pair[0], mm::boids::kBoid, pair, 0, nullptr);
    mm::boids::gather(pair[1], mm::boids::kBoid, pair, 1, nullptr);
    mm::boids::move(pair[0], mm::boids::kBoid.maxSpeed);
    mm::boids::move(pair[1], mm::boids::kBoid.maxSpeed);
    a = pair[0];
    b = pair[1];
}

}  // namespace

// Separation: two boids closer than the crowding distance move apart.
TEST_CASE("boids steer apart when they crowd each other") {
    mm::boids::Boid a{10, 10}, b{11, 10};
    stepPair(a, b);
    CHECK(b.x - a.x > 1.0f);
}

// Alignment: a boid turns toward the heading of one it sees, outside the crowding distance.
TEST_CASE("boids turn toward the heading of the boids they see") {
    mm::boids::Boid a{10, 10, 0, 0.3f}, b{10, 16, 0.3f, 0};
    stepPair(a, b);
    CHECK(a.vx > 0);
}

// Cohesion: a boid at rest is drawn toward one it sees.
TEST_CASE("boids steer toward the center of the boids they see") {
    mm::boids::Boid a{10, 10}, b{16, 10};
    stepPair(a, b);
    CHECK(a.x > 10);
    CHECK(b.x < 16);
}

// Flight: a boid near a predator steers straight away from it, and one beyond the fear distance does not.
TEST_CASE("boids flee a predator near where they are heading, and only then") {
    mm::boids::Boid near{10, 10}, predator{12, 10};
    mm::boids::gather(near, mm::boids::kBoid, std::span<const mm::boids::Boid>(&near, 1), 0, &predator);
    mm::boids::move(near, mm::boids::kBoid.maxSpeed);
    CHECK(near.vx < 0);

    mm::boids::Boid far{10, 10}, distant{25, 10};
    mm::boids::gather(far, mm::boids::kBoid, std::span<const mm::boids::Boid>(&far, 1), 0, &distant);
    CHECK(far.ax == 0);
}

// The space is a torus: two boids either side of an edge are a unit apart, so they crowd and push apart across it.
TEST_CASE("boids see each other the short way across an edge") {
    mm::boids::Boid a{0.5f, 10}, b{31.5f, 10};
    stepPair(a, b);
    CHECK(a.x > 0.5f);
    CHECK(b.x < 31.5f);
}

// A tick runs every step it owes while the clock stands, and stops once the steps have spent its budget, always after one at least.
TEST_CASE("boids stop a tick's steps once they have spent its time") {
    mm::golden::ScopedTestClock clock(1000);
    uint32_t ran = 0;
    CHECK(mm::boids::runSteps(5, [&] { ran++; }) == 5);   // the clock stands: all five
    uint32_t now = 1000;
    const uint32_t slow = mm::boids::runSteps(5, [&] { now += 3; mm::platform::setTestNowMs(now); });
    CHECK(slow == 3);                                       // 3 ms a step: the third step passes the 8 ms budget
    CHECK(mm::boids::runSteps(5, [&] { now += 20; mm::platform::setTestNowMs(now); }) == 1);   // one step at least
}

// The wind pushes one boid at a time, on about one step in 64.
TEST_CASE("boids feel the wind one at a time, now and then") {
    mm::Random8 rng(1234);
    int calls = 6400, pushes = 0;
    for (int i = 0; i < calls; i++) {
        mm::boids::Boid flock[8] = {};
        mm::boids::gust(flock, rng);
        int pushed = 0;
        for (const auto& b : flock) pushed += (b.ax != 0 || b.ay != 0);
        CHECK(pushed <= 1);
        pushes += pushed;
    }
    CHECK(pushes > 70);    // 6400 / 64 is 100
    CHECK(pushes < 130);
}

// Turning count down lets boids go: once their trails have faded, fewer lights are lit than with the larger flock.
TEST_CASE("FlockEffect drops boids when count is turned down live") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(64, 64);
    mm::FlockEffect flock;
    flock.count = 40;
    fx.add(flock);
    uint32_t now = 1000;
    auto run = [&](int frames) { for (int f = 0; f < frames; f++) { now += 20; fx.tickAt(now); } };
    auto lit = [&] { return litLights(fx.frame()); };
    run(100);
    const size_t many = lit();
    flock.count = 2;
    run(300);                   // six seconds: the dropped boids' trails have faded
    CHECK(lit() < many / 4);
}

// A prepare at the same size clears the old picture: at speed 0 afterwards only the flock's own lights show.
TEST_CASE("FlockEffect starts clean on a prepare at the same size") {
    mm::golden::ScopedTestClock clock(1000);
    mm::golden::LayerFixture fx(32, 32);
    mm::Layer& layer = fx.layer;
    mm::FlockEffect flock;
    flock.count = 8;
    fx.add(flock);
    uint32_t now = 1000;
    for (int f = 0; f < 100; f++) { now += 20; fx.tickAt(now); }
    flock.prepare();
    flock.speed = 0;
    std::memset(layer.buffer().data(), 0, layer.buffer().bytes());
    now += 20;
    fx.tickAt(now);
    CHECK(litLights(fx.frame()) <= 9);   // eight boids and the predator, one light each
}
