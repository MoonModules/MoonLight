/// @module MoonLiveParticles
/// @also MoonLiveEffect, MoonLive, particles

/// A scripted particle pool: its buffers live in ScratchBuffers the binding owns, outside the 64-byte arena, and a script calls whole-pool operations.

#include "doctest.h"
#include "MoonLiveEffectScene.h"
#include "../core/moonlive_script_wrap.h"
#include "platform/platform.h"   // setTestNowMs: particle physics runs on elapsed time
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
// The shared fixture on the 8x8 grid these particle tests are sized for.
struct Scene : ScriptScene {
    Scene(int w = 8, int h = 8) : ScriptScene(w, h) {}
};
}  // namespace

TEST_CASE("a script sizes its own particle pool and is told what it got") {
    // The SAME script with and without the pool call, so the difference is the buffers alone and not the compiled program, which varies with the source text.
    Scene without, with_;
    without.run("class T { void defineControls() { addControl(\"n\", n, 0, 9); } byte n = 0; void tick() { } }");
    with_.run("class T { void defineControls() { pool(64); } void tick() { } }");
    CHECK(with_.effect.dynamicBytes() > without.effect.dynamicBytes() + 1000);   // ~1216 of buffers
}

// The pay-for-what-you-use rule, on the memory that matters most: a shader script must not carry particle buffers it never asked for, so there is no default pool.
TEST_CASE("a script that never asks for particles allocates none") {
    Scene shader;
    shader.run("class T { void tick() { fill(1, 2, 3); } }");
    // A shader script holds its compiled program and nothing else. The smallest pool a script could ask for is 23 bytes; anything under that is program alone.
    CHECK(shader.effect.dynamicBytes() < 1000);
}

// THE guarantee that keeps a malloc off the render path: sizing is reachable only from defineControls(), where the sink is installed. A script asking from tick() is told the live count and nothing is allocated, every frame, forever.
TEST_CASE("asking for a pool while the frame is running allocates nothing") {
    Scene s;
    s.run("class T { void defineControls() { pool(32); } void tick() { setRGB(0, pool(4000), 0, 0); } }");
    const size_t sized = s.effect.dynamicBytes();
    REQUIRE(sized > 0);
    for (int i = 0; i < 5; i++) s.layer.tick();
    CHECK(s.effect.dynamicBytes() == sized);     // five frames of asking changed nothing
}

// The live-edit rule applied to memory: editing the script's text recompiles, which re-runs defineControls, which resizes.
TEST_CASE("editing a script to a different pool size resizes it") {
    Scene s;
    s.run("class T { void defineControls() { pool(16); } void tick() { } }");
    const size_t small = s.effect.dynamicBytes();
    REQUIRE(small > 0);
    s.run("class T { void defineControls() { pool(128); } void tick() { } }");
    CHECK(s.effect.dynamicBytes() >= small + 112 * (4 * 4 + 2 + 1));
}

// Disabling a scripted effect must hand the memory back AND leave the pool invalid rather than pointing at freed buffers, which is the trap ParticlesEffect documents at its own prepare().
TEST_CASE("disabling a scripted effect frees its particles") {
    Scene s;
    s.run("class T { void defineControls() { pool(64); } void tick() { } }");
    REQUIRE(s.effect.dynamicBytes() > 0);
    s.effect.release();
    CHECK(s.effect.dynamicBytes() == 0);
}

// Trying effects one after another switches one module's script; on a classic ESP32 the carried buffers left too little heap to compile a large one.
TEST_CASE("switching to a script that asks for no pool or trail frees the last script's") {
    const char* bare = "class T { void tick() { fill(1, 2, 3); } }";
    const char* bareWithControls = "class T { byte n = 0; void defineControls() { addControl(\"n\", n, 0, 9); } void tick() { fill(1, 2, 3); } }";
    const char* sizing = "class T { void defineControls() { pool(64); trail(1); } void tick() { } }";
    for (const char* next : {bare, bareWithControls}) {   // without a defineControls() and with one
        CAPTURE(next);
        Scene s;
        s.run(next);
        const size_t alone = s.effect.dynamicBytes();
        s.run(sizing);
        REQUIRE(s.effect.dynamicBytes() > alone + 1000);
        s.run(next);
        CHECK(s.effect.dynamicBytes() < alone + 256);   // the same program again, with nothing carried over
    }
}

// Freed as the compile starts rather than after a successful one, so a large script's compile has that memory, and a script that fails to compile holds nothing.
TEST_CASE("a script that fails to compile holds none of the last script's pool and trail") {
    Scene s;
    s.run("class T { void defineControls() { pool(64); trail(1); } void tick() { } }");
    REQUIRE(s.effect.dynamicBytes() > 1000);
    s.run("class T { void tick() { nosuchcall(); } }");
    CHECK(s.effect.dynamicBytes() < 256);
}

// A script reaching the particle vocabulary from a layout or a modifier finds no pool installed, so the calls do nothing rather than writing through another module's buffers.
TEST_CASE("a particle call from a script with no pool does nothing") {
    Scene s;
    s.run("class T { void tick() { setRGB(0, pool(0) + 7, 0, 0); } }");
    s.layer.tick();
    CHECK(s.layer.buffer().data()[0] == 7);      // ran to completion, pool() reported 0
}

// The test that says why the feature exists: a script writes physics, not positions. Nothing here tells a spark where to go; it leaves at an angle, gravity pulls on it, and where it turns over is wherever the physics puts it.
TEST_CASE("a spark thrown upward comes back down") {
    Scene s(16, 16);
    // Straight up (angle16 49152 = three quarter turn = -y), fast, long-lived, no drag. A member counter, so the spark is thrown once and then only physics runs.
    s.run("class T {"
          "  byte fired = 0;"
          "  void defineControls() { pool(8); }"
          "  void tick() { fill(0, 0, 0);"
          "           if (fired == 0) { emit(8, 15, 49152, 260, 4, 600, 40); fired = 1; }"
          "           gravity(22); step(); age(1); render(255); } }");

    int highest = 999, lastY = 999;
    bool roseThenFell = false;
    for (int f = 0; f < 60; f++) {
        mm::platform::setTestNowMs(100000u + 16u * static_cast<uint32_t>(f));
        s.layer.tick();
        int topLit = 999;
        for (int y = 0; y < 16 && topLit == 999; y++)
            for (int x = 0; x < 16; x++)
                if (s.layer.buffer().data()[(y * 16 + x) * 3 + 1]) { topLit = y; break; }
        if (topLit == 999) continue;
        if (topLit < highest) highest = topLit;
        if (lastY != 999 && topLit > lastY && highest < 14) roseThenFell = true;
        lastY = topLit;
    }
    mm::platform::setTestNowMs(0);
    CHECK(highest < 14);        // it climbed away from the floor it was thrown from
    CHECK(roseThenFell);        // and gravity brought it back
}

// A pool is a fixed set of slots. Emitting into a full one stops rather than overwriting a living particle, so a script that over-emits degrades to "no new sparks" instead of corrupting motion.
TEST_CASE("emitting into a full pool stops rather than overwriting") {
    Scene s(16, 16);
    s.run("class T {"
          "  void defineControls() { pool(4); }"
          "  void tick() { emit(8, 8, 16384, 100, 8, 60000, 40); render(255); } }");
    for (int f = 0; f < 10; f++) s.layer.tick();
    int lit = 0;
    for (int i = 0; i < 16 * 16; i++)
        if (s.layer.buffer().data()[i * 3] || s.layer.buffer().data()[i * 3 + 1] ||
            s.layer.buffer().data()[i * 3 + 2]) lit++;
    CHECK(lit > 0);             // it drew something
    CHECK(lit <= 4 * 4);        // never more than the four slots can carry (a splat covers a few)
}

// Without aging, a long-running fountain silently stops emitting once every slot is taken. That is a bug which only shows up after a minute on the bench, so it is pinned here instead.
TEST_CASE("a script's particles die and free their slots for new ones") {
    Scene s(16, 16);
    // Life 2 with a fast age: every spark is gone within a few frames, so emit always succeeds.
    s.run("class T {"
          "  void defineControls() { pool(4); }"
          "  void tick() { emit(8, 8, 16384, 60, 2, 2, 40); age(64); step(); render(255); } }");
    for (int f = 0; f < 40; f++) s.layer.tick();
    int lit = 0;
    for (int i = 0; i < 16 * 16; i++)
        if (s.layer.buffer().data()[i * 3] || s.layer.buffer().data()[i * 3 + 1] ||
            s.layer.buffer().data()[i * 3 + 2]) lit++;
    CHECK(lit > 0);             // still emitting 40 frames in: slots were recycled
}

// Two scripted effects each own their own buffers, so one script's particles can never appear in another's layer. This is the "what does a second script asking for a pool get" question.
TEST_CASE("two scripted effects each get their own particles") {
    Scene a(16, 16), b(16, 16);
    a.run("class T { void defineControls() { pool(8); }"
          "  void tick() { emit(8, 8, 16384, 100, 4, 600, 40); render(255); } }");
    b.run("class T { void defineControls() { pool(8); } void tick() { render(255); } }");
    for (int f = 0; f < 5; f++) { a.layer.tick(); b.layer.tick(); }
    int litA = 0, litB = 0;
    for (int i = 0; i < 16 * 16; i++) {
        if (a.layer.buffer().data()[i * 3 + 1]) litA++;
        if (b.layer.buffer().data()[i * 3 + 1]) litB++;
    }
    CHECK(litA > 0);            // the emitting one drew
    CHECK(litB == 0);           // the other stayed empty
}

// Through the real binding, a fountain reaches a steady state where sparks are emitted, fly and die at one rate, rather than filling the pool once.
TEST_CASE("the fountain example keeps emitting once its pool has cycled") {
    // The SHIPPED file, staged into the test filesystem: this drives the real example rather than a copy that could drift from it.
    const std::filesystem::path src = std::filesystem::path(__FILE__).parent_path()
        .parent_path().parent_path().parent_path() / "moonlive" / "effects" / "particles" / "fountain.mle";
    std::ifstream in(src);
    REQUIRE(in.good());
    std::stringstream ss; ss << in.rdbuf();

    Scene s(24, 16);
    s.run(ss.str().c_str());
    REQUIRE(s.effect.dynamicBytes() > 1000);      // the pool(300) call landed

    int litLate = 0;
    for (int f = 0; f < 120; f++) {
        mm::platform::setTestNowMs(100000u + 16u * static_cast<uint32_t>(f));
        s.layer.tick();
    }
    for (int i = 0; i < 24 * 16; i++)
        if (s.layer.buffer().data()[i * 3] || s.layer.buffer().data()[i * 3 + 1] ||
            s.layer.buffer().data()[i * 3 + 2]) litLate++;
    mm::platform::setTestNowMs(0);
    CHECK(litLate > 5);                           // still drawing 120 frames in
}

// angleEmit hashes (index, seed) into an angle and a speed, so a seed that stood still threw identical sparks every frame, stacked into a few fixed streams.
TEST_CASE("emitting twice from the same point does not repeat the same trajectories") {
    Scene s(24, 24);
    s.run("class T {"
          "  void defineControls() { pool(64); }"
          "  void tick() { fill(0, 0, 0); emit(12, 23, 49152, 700, 6, 600, 40);"
          "           step(); render(255); } }");

    // Two frames of emission, each sampled where its own sparks landed.
    std::vector<int> firstCols, secondCols;
    for (int f = 0; f < 2; f++) {
        mm::platform::setTestNowMs(100000u + 16u * static_cast<uint32_t>(f));
        s.layer.tick();
        std::vector<int>& into = (f == 0) ? firstCols : secondCols;
        for (int x = 0; x < 24; x++)
            for (int y = 0; y < 24; y++)
                if (s.layer.buffer().data()[(y * 24 + x) * 3 + 1]) { into.push_back(x); break; }
    }
    mm::platform::setTestNowMs(0);
    REQUIRE(firstCols.size() > 1);
    CHECK(firstCols != secondCols);       // a frozen seed would make these identical
}

// Dropped down one column, balls without collide() fall through one another and stay in it; with it they shove sideways and spread, which turns a shower into a pit.
TEST_CASE("colliding balls spread sideways instead of falling through each other") {
    auto pileHeight = [](const char* collideCall) {
        Scene s(16, 16);
        std::string src = std::string(
            "class T { void defineControls() { pool(12); }"
            "  void tick() { fill(0, 0, 0);"
            "           emit(8, 0, 16384, 4, 2, 60000, 40);"
            "           gravity(20); ") + collideCall +
            " step(); bounce(120); render(1); } }";
        s.run(src.c_str());
        for (int f = 0; f < 120; f++) {
            mm::platform::setTestNowMs(100000u + 16u * static_cast<uint32_t>(f));
            s.layer.tick();
        }
        mm::platform::setTestNowMs(0);
        // The HIGHEST occupied row. Balls that pass through one another all sink to the floor; balls that collide rest on the ones below and the pile reaches further up.
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const uint8_t* px = &s.layer.buffer().data()[(y * 16 + x) * 3];
                if (px[0] || px[1] || px[2]) return 16 - y;   // pile height
            }
        return 0;
    };
    const int without = pileHeight("");
    const int with_   = pileHeight("collide(2);");
    REQUIRE(without > 0);        // both variants drew, so the comparison means something
    REQUIRE(with_ > 0);
    CHECK(with_ > without);      // the pile rests higher: balls hold each other up
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
