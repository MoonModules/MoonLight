/// @module MoonLive

/// What parallel layers rely on: a script's sinks per thread for every render thread at once, a particle pool's own emit seed, and a palette a layer asks for applied between frames.

#include "doctest.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "light/powerfunctions/particles.h"
#include "light/util/Palette.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace mm;

namespace {
void noLight(void*, uint16_t, uint16_t, uint16_t) {}
}  // namespace

TEST_CASE("every thread that may render layers at once gets its own sinks, one more for a script compiled off the render path") {
    constexpr size_t n = platform::renderThreads + 1;
    std::atomic<size_t> ready{0}, held{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    int ctx[n] = {};
    for (size_t i = 0; i < n; i++)
        threads.emplace_back([&, i] {
            moonlive::setAddLightSink(&noLight, &ctx[i]);
            if (moonlive::addLightSink().ctx == &ctx[i]) held++;   // this thread's own, not another's
            ready++;
            while (!release.load()) std::this_thread::yield();      // every thread holds its slot at once
            moonlive::setAddLightSink(nullptr, nullptr);
        });
    while (ready.load() < n) std::this_thread::yield();
    release = true;
    for (auto& t : threads) t.join();
    CHECK(held.load() == n);
}

TEST_CASE("a particle pool's emit seed is its own, so a layer's sparks are the same on any number of threads") {
    particles::Pool a, b;
    const uint32_t a1 = a.nextSeed();
    b.nextSeed(); b.nextSeed();   // another layer's pool emitting more
    CHECK(particles::Pool{}.nextSeed() == a1);
    CHECK(a.nextSeed() != a1);    // and it moves, so each frame throws different sparks
}

TEST_CASE("a palette a layer asks for applies at the frame's start, not under the other layers") {
    const Palette before = *Palettes::active();
    Palettes::setActive(0);
    Palettes::request(2);
    CHECK(Palettes::active()->entry[0].r == Palettes::fromBuiltin(0).entry[0].r);   // unchanged while the layers render
    Palettes::applyRequest();
    const Palette two = Palettes::fromBuiltin(2);
    CHECK(Palettes::active()->entry[0].r == two.entry[0].r);
    CHECK(Palettes::active()->entry[15].b == two.entry[15].b);
    Palettes::applyRequest();     // nothing asked: it stays
    CHECK(Palettes::active()->entry[15].b == two.entry[15].b);
    Palettes::setActiveDirect(before);
}
