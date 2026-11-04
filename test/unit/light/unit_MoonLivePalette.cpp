/// @module MoonLivePalette
/// @also Palette, Effects

/// A scripted palette's sixteen entries, recomputed every frame, are either wholly old or wholly new to the effects sampling them.

#include "doctest.h"
#include "light/moonlive/MoonLivePalette.h"
#include "light/util/Palette.h"
#include "core/moonlive/moonlive_emit.h"
#include "MoonLiveScriptFixture.h"
#include "../core/moonlive_script_wrap.h"   // mmScript: the class ceremony around a bare body
#include "core/module/MoonModule.h"
#include "core/services/AudioService.h"
#include "moonlive_script_files.h"   // scriptRoot, readText: the shipped scripts as files

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
/// A module for the binding to report status through, which is how a scripted palette's errors reach a card.
struct ScriptHost : public MoonModule {};

/// Compile `src` as a palette, run one frame, and return the active palette.
Palette runPalette(const char* src) {
    ScriptHost host;
    MoonLivePalette pal;
    pal.setScript(mmWriteScript(src));
    pal.prepare(host);
    REQUIRE(pal.ok());
    pal.tick(0);
    return *Palettes::active();
}
}  // namespace

TEST_CASE("a scripted palette fills all sixteen entries") {
    // The whole feature in one: a script writes the table every effect then samples.
    const Palette before = *Palettes::active();
    const Palette p = runPalette("class P { void tick() {\n"
                                 "  for (int i = 0; i < 16; i = i + 1) { setPalEntry(i, i * 16, 0, 0); }\n"
                                 "} }");
    for (int i = 0; i < 16; i++) {
        CHECK(p.entry[i].r == i * 16);
        CHECK(p.entry[i].g == 0);
        CHECK(p.entry[i].b == 0);
    }
    Palettes::setActiveDirect(before);   // leave the global as it was found
}

TEST_CASE("setPalEntryHSV reaches the same table, in the space a palette is reasoned in") {
    const Palette before = *Palettes::active();
    // Full-value red is hue 0: the conversion is the engine's, so this pins that the builtin is wired to it rather than writing the raw HSV bytes.
    const Palette p = runPalette("class P { void tick() { setPalEntryHSV(0, 0, 255, 255); } }");
    CHECK(p.entry[0].r > 200);
    CHECK(p.entry[0].g < 60);
    CHECK(p.entry[0].b < 60);
    Palettes::setActiveDirect(before);
}

TEST_CASE("an out-of-range entry index writes nothing at all") {
    // Bounded rather than wrapped: a wrapped index would write a NEIGHBORING entry and produce a palette nobody wrote, which reads as an engine fault rather than a script bug.
    const Palette before = *Palettes::active();
    Palette seed;
    for (int i = 0; i < 16; i++) seed.entry[i] = RGB{7, 7, 7};
    Palettes::setActiveDirect(seed);

    ScriptHost host;
    MoonLivePalette pal;
    // Pins the contract only: a missing bound writes past the entries unseen here, which the CI ASan lane catches.
    pal.setScript(mmWriteScript("class P { void tick() { setPalEntry(16, 255, 255, 255); } }"));
    pal.prepare(host);
    REQUIRE(pal.ok());
    pal.tick(0);

    const Palette* p = Palettes::active();
    for (int i = 0; i < 16; i++) CHECK(p->entry[i].r == 7);   // untouched
    Palettes::setActiveDirect(before);
}

TEST_CASE("entries the script does not write keep their previous color") {
    // Seeded from the live palette, so a script that fills four entries leaves the other twelve as they were rather than showing whatever was on the stack.
    const Palette before = *Palettes::active();
    Palette seed;
    for (int i = 0; i < 16; i++) seed.entry[i] = RGB{3, 4, 5};
    Palettes::setActiveDirect(seed);

    ScriptHost host;
    MoonLivePalette pal;
    pal.setScript(mmWriteScript("class P { void tick() { setPalEntry(0, 200, 0, 0); } }"));
    pal.prepare(host);
    REQUIRE(pal.ok());
    pal.tick(0);

    const Palette* p = Palettes::active();
    CHECK(p->entry[0].r == 200);
    for (int i = 1; i < 16; i++) { CHECK(p->entry[i].r == 3); CHECK(p->entry[i].b == 5); }
    Palettes::setActiveDirect(before);
}

TEST_CASE("a broken palette script leaves the last good palette, rather than going dark") {
    // The other bindings degrade to dark, which is honest when the script IS the picture. Here it is not: the effects still run, so a black palette would blame the effect for the palette's fault.
    const Palette before = *Palettes::active();
    Palette seed;
    for (int i = 0; i < 16; i++) seed.entry[i] = RGB{9, 9, 9};
    Palettes::setActiveDirect(seed);

    ScriptHost host;
    MoonLivePalette pal;
    pal.setScript(mmWriteScript("class P { void tick() { this is not a script } }"));
    pal.prepare(host);
    CHECK_FALSE(pal.ok());
    CHECK_FALSE(pal.tick(0));            // nothing ran
    const Palette* p = Palettes::active();
    for (int i = 0; i < 16; i++) CHECK(p->entry[i].r == 9);   // the last good palette stands
    CHECK(host.status() != nullptr);     // and the fault is reported
    Palettes::setActiveDirect(before);
}

TEST_CASE("setPalEntry does nothing outside a palette script") {
    // The sink is installed for exactly one tick, so an EFFECT calling setPalEntry changes nothing rather than corrupting the palette every other effect is sampling.
    const Palette before = *Palettes::active();
    Palette seed;
    for (int i = 0; i < 16; i++) seed.entry[i] = RGB{11, 11, 11};
    Palettes::setActiveDirect(seed);

    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("setPalEntry(0, 255, 255, 255);"),
                        moonlive::lightBuiltins(), moonlive::effectSysVars()));
    std::vector<uint8_t> buf(3 * 4, 0);
    eng.run(buf.data(), 4, 3, 0);

    const Palette* p = Palettes::active();
    for (int i = 0; i < 16; i++) CHECK(p->entry[i].r == 11);   // untouched
    Palettes::setActiveDirect(before);
}

// The harmony showcase: the palette takes the chord's colors on each chord change, each pitch class a fixed hue on the circle of fifths, so the same chord always returns as the same palette.
TEST_CASE("the harmony palette takes the chord's colors on each change, the same chord giving the same palette") {
    const Palette before = *Palettes::active();
    static AudioService audio;   // the frame the builtins read
    audio.defineControls();
    audio.setup();
    audio.tick();   // claims the seat latestFrame reads; the frames below are written straight into it
    auto chord = [&](std::initializer_list<std::pair<uint8_t, uint8_t>> classes, bool change) {
        AudioFrame& f = audio.frameForTest();
        for (uint8_t c = 0; c < 12; c++) f.chroma[c] = 0;
        for (const auto& [c, v] : classes) f.chroma[c] = v;
        f.harmonyChange = change;
    };
    ScriptHost host;
    MoonLivePalette pal;
    const std::string src = readText(scriptRoot() / "palettes" / "harmony.mlp");
    pal.setScript(mmWriteScript(src.c_str()));
    pal.prepare(host);
    REQUIRE(pal.ok());
    auto same = [](const Palette& a, const Palette& b) {
        for (int i = 0; i < 16; i++) if (a.entry[i].r != b.entry[i].r || a.entry[i].g != b.entry[i].g || a.entry[i].b != b.entry[i].b) return false;
        return true;
    };

    chord({{0, 255}, {4, 200}, {7, 180}}, true);   // C major: C, E, G
    pal.tick(1000);
    const Palette cMajor = *Palettes::active();
    CHECK(cMajor.entry[0].r > 200);   // C on the circle's start: red
    CHECK(cMajor.entry[15].g > 200);  // E, four fifths on: green

    chord({{0, 120}, {4, 255}, {7, 200}}, false);  // the same chord's notes at other strengths, no change
    pal.tick(1500);
    CHECK(same(*Palettes::active(), cMajor));      // the palette holds between changes

    chord({{0, 150}, {4, 255}, {7, 220}}, true);   // C major again, E the loudest now
    pal.tick(2000);
    pal.tick(2200);
    CHECK(same(*Palettes::active(), cMajor));      // the same chord, the same palette

    chord({{9, 255}, {0, 210}, {4, 190}}, true);   // A minor: A, C, E
    pal.tick(3000);
    pal.tick(3010);                                // a faster frame reading the same change: the crossfade goes on
    audio.frameForTest().harmonyChange = false;
    pal.tick(3075);
    const Palette midway = *Palettes::active();
    pal.tick(3200);
    const Palette aMinor = *Palettes::active();
    CHECK_FALSE(same(aMinor, cMajor));             // a new chord, new colors
    CHECK_FALSE(same(midway, aMinor));             // reached through the crossfade
    CHECK(aMinor.entry[0].r == cMajor.entry[0].r); // C, shared by both chords, keeps its color

    audio.release();
    Palettes::setActiveDirect(before);
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
