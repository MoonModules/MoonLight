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

#include <cstring>
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
#endif  // MM_MOONLIVE_HAS_HOST_JIT
