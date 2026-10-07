/// @module MoonLive
/// @also MoonLiveLayout, MoonLiveEffect, MoonLiveModifier

/// Every script in `moonlive/` has to compile.
///
/// Those files are what a user copies into a device, and three of them ship as module defaults, so a script that stops parsing is a broken default and a broken example at once.
/// The language is young and still gaining syntax.
/// Without this, the first sign that a change broke them is someone pasting one into a running fixture and getting a parse error.
///
/// The scripts live as files rather than as string literals here so they can be read, edited and pasted without a rebuild.
/// This test walks the folder, so a new script is covered by adding it.

#include "doctest.h"
#include "../core/moonlive_script_wrap.h"
#include "core/moonlive/MoonLive.h"
#include "platform/platform.h"
#include "core/moonlive/moonlive_emit.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "core/moonlive/MoonLiveBuiltins_service.h"   // the service vocabulary a .mls compiles against
#include "core/moonlive/MoonLiveScriptFile.h"   // the role extensions the sweep filters on
#include "core/moonlive/MoonLiveScript.h"       // kMaxStatus: the status line a failure reports through
#include "core/moonlive/script_catalog.h"       // generated: what the device offers
#include "core/services/MoonLiveService.h"      // kServiceTemplate: what a new service script starts as

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>
#include <vector>

// Any role extension: one language, and the sweep compiles every script whatever role its name claims. Delegates to the one definition (MoonLiveScriptFile.h) rather than listing the extensions again: two copies of this list is exactly how a new role went missing from one sweep.
inline bool mmIsScript(const std::filesystem::path& p) {
    return mm::moonlive::isScriptExt(p.extension().string().c_str());
}


using namespace mm;

namespace {
/// The repo's script folder, found relative to this source file so the test does not depend on the working directory a runner happens to use.
std::filesystem::path scriptRoot() {
    std::filesystem::path p = std::filesystem::path(__FILE__).parent_path();   // test/unit/light
    return p.parent_path().parent_path().parent_path() / "moonlive";           // repo root
}

std::vector<std::filesystem::path> scriptsIn(const char* sub) {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path dir = scriptRoot() / sub;
    if (!std::filesystem::exists(dir)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.is_regular_file() && mmIsScript(e.path())) out.push_back(e.path());
    return out;
}

std::string read(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("every script in moonlive/ compiles") {
    int checked = 0;
    // services included: it compiles against its own table, which the isService branch below picks.
    for (const char* sub : {"layouts", "effects", "modifiers", "drivers", "palettes", "services"}) {
        for (const auto& file : scriptsIn(sub)) {
            const std::string src = read(file);
            const std::string label = std::string(sub) + "/" + file.filename().string();

            // Compile it exactly as it ships, against the system variables ITS OWN binding supplies, a layout gets the clock, an effect the grid, a modifier the grid plus a coordinate. Using one shared list here would let a script read a name its module never writes and still pass, which is the silent-zero this per-binding split exists to prevent.
            const bool isService = std::string(sub) == "services";
            // A palette is a LIGHT-domain script: it fills the active palette, so it gets the light table (where setPalEntry lives) and the effect system variables.
            const moonlive::SysVarTable sys =
                isService                       ? moonlive::serviceSysVars()  :
                std::string(sub) == "layouts"   ? moonlive::layoutSysVars()   :
                std::string(sub) == "modifiers" ? moonlive::modifierSysVars() :
                                                  moonlive::effectSysVars();
            // A SERVICE compiles against its own table: it has gpioRead and setControl and no canvas, so compiling one against the light vocabulary would prove nothing about what the device will actually run, and would accept a script calling setRGB that fails on a real service.
            const moonlive::BuiltinTable builtins =
                isService ? moonlive::serviceBuiltins() : moonlive::lightBuiltins();
            moonlive::MoonLive engine;
            const bool ok = engine.compile(src.c_str(), builtins, sys);
            if (!ok) std::printf("FAIL %-28s %s\n", label.c_str(), engine.error());
            // compile() both PARSES and emits native code, and only the second half needs a backend for this host's ISA. arm64 and x86-64 both have one.
            // A --no-jit build and any other host do not, and there requiring success would fail every script for a reason that has nothing to do with the script.
            // Without a backend the only failure allowed is the codegen one.
#if MM_MOONLIVE_HAS_HOST_JIT
            CHECK(ok);
#else
            CHECK((ok || std::string(engine.error()) == moonlive::kCodegenFailed));
#endif
            engine.free();
            checked++;
        }
    }
    MESSAGE("compiled " << checked << " scripts from moonlive/");
    CHECK(checked > 0);            // a silently empty folder would pass without this
}

// The catalog is what a DEVICE knows about: it carries these names and fetches a script's text the first time someone picks it. A script in the repo but not in the catalog is invisible on every device, and nothing else would notice, since the build succeeds and the file is right there.
TEST_CASE("the shipped catalog names every script in moonlive/") {
    std::vector<std::string> onDisk;
    for (const char* sub : {"layouts", "effects", "modifiers", "services", "palettes"})
        for (const auto& f : scriptsIn(sub)) onDisk.push_back(f.filename().string());
    std::sort(onDisk.begin(), onDisk.end());
    REQUIRE(!onDisk.empty());

    // The catalog is one array per role: the folder a script lives in is implied by its role and the role by its extension, so neither is stored per entry.
    std::vector<std::string> inCatalog;
    for (size_t i = 0; i < moonlive::kEffectCatalogCount; i++)
        inCatalog.push_back(moonlive::kEffectCatalog[i]);
    for (size_t i = 0; i < moonlive::kLayoutCatalogCount; i++)
        inCatalog.push_back(moonlive::kLayoutCatalog[i]);
    for (size_t i = 0; i < moonlive::kModifierCatalogCount; i++)
        inCatalog.push_back(moonlive::kModifierCatalog[i]);
    for (size_t i = 0; i < moonlive::kServiceCatalogCount; i++)
        inCatalog.push_back(moonlive::kServiceCatalog[i]);
    for (size_t i = 0; i < moonlive::kPaletteCatalogCount; i++)
        inCatalog.push_back(moonlive::kPaletteCatalog[i]);
    CHECK(inCatalog.size() == moonlive::kCatalogCount);
    std::sort(inCatalog.begin(), inCatalog.end());

    for (const auto& n : onDisk)
        if (!std::binary_search(inCatalog.begin(), inCatalog.end(), n))
            std::printf("MISSING from catalog: %s\n", n.c_str());
    // And the other direction: a name the catalog offers that no longer exists upstream sends a device to fetch a file that is not there, which the count check alone would miss when a script is added and another removed in the same change.
    for (const auto& n : inCatalog)
        if (!std::binary_search(onDisk.begin(), onDisk.end(), n))
            std::printf("STALE in catalog: %s\n", n.c_str());
    CHECK(inCatalog == onDisk);

    // Each array holds only its own role's extension. A modifier listed among the effects would be offered in an effect picker, compile, and then do nothing.
    for (size_t i = 0; i < moonlive::kEffectCatalogCount; i++) {
        const std::string n(moonlive::kEffectCatalog[i]);
        CHECK(n.substr(n.rfind('.')) == moonlive::kEffectExt);
    }
    for (size_t i = 0; i < moonlive::kLayoutCatalogCount; i++) {
        const std::string n(moonlive::kLayoutCatalog[i]);
        CHECK(n.substr(n.rfind('.')) == moonlive::kLayoutExt);
    }
    for (size_t i = 0; i < moonlive::kModifierCatalogCount; i++) {
        const std::string n(moonlive::kModifierCatalog[i]);
        CHECK(n.substr(n.rfind('.')) == moonlive::kModifierExt);
    }
}

// Comments are what makes a script in `moonlive/` readable, so the lexer has to treat a plain `//` line as whitespace, anywhere, including between the statements of a loop body.
// The one exception was `// @control min..max`, a comment that declared a UI slider. defineControls() replaced it, so every comment is now genuinely a comment.
// Each binding supplies the system variables it actually WRITES, and supplying a name is also what reserves it.
// That split is what keeps `x` usable as a loop counter in a layout while still making it mean "the light being folded" in a modifier, and what turns a layout reading `width` into an error instead of a silent 0 that places no lights and reports success.
// ONE vocabulary for all three roles.
// A name means the same thing in every script, and the only thing a binding decides is which slots it WRITES each frame.
//
// The per-role tables this replaced did not prevent a mistake.
// A layout reading `width` got a compile error, which is the same outcome as reading a value that is always zero.
// What they did create was a trap, because they were different vocabularies rather than nested ones, so a name was legal in one role and RESERVED in another.
// `disasm.py` compiled against the widest table and therefore refused `grid.mll`, the shipped default layout, as "name is a system variable".
TEST_CASE("every script reads the same system-variable vocabulary") {
    struct Case { const char* src; bool ok; const char* what; };
    const Case cases[] = {
        {mmScript("for (int y = 0; y < 2; y = y + 1) { for (int x = 0; x < 3; x = x + 1) { addLight(x, y, 0); } }"),
         true,  "x and y are ordinary loop counters, in EVERY role: they are the names an author "
                "reaches for, which is why the coordinate is xPos/yPos/zPos instead"},
        {mmScript("for (int i = 0; i < width; i = i + 1) { addLight(i, 0, 0); }"),
         true,  "a layout may read width: same name, same meaning, whoever asks"},
        {mmScript("setRGB(width, 0, 0, 0);"),           true,  "an effect reads the layer's width"},
        {mmScript("setXYZ(width - 1 - xPos, yPos, zPos);"),
         true,  "a modifier reads its coordinate AND the box it lives in"},
        {mmScript("setRGB(xPos, 0, 0, 0);"),
         true,  "reading a coordinate outside a modifier is legal and reads 0: no binding writes "
                "it, so there is nothing to disagree with"},
        {mmScript("byte width = 16;\nsetRGB(0, 0, 0, 0);"),
         false, "declaring one is still refused, in every role: that is what keeps a read meaningful"},
        {mmScript("byte xPos = 3;\nsetRGB(0, 0, 0, 0);"),
         false, "the coordinate names are reserved too, so a modifier cannot shadow what it is handed"},
    };
    uint8_t out[2048];
    for (const Case& c : cases) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, moonlive::lightBuiltins(), moonlive::lightSysVars(),
                                         out, sizeof(out));
        // Where a backend exists, a valid script must actually EMIT, accepting kCodegenFailed everywhere would let a codegen regression pass as a pass. Only a build with no assembler for its ISA (--no-jit, or an unsupported host) is allowed that answer.
#if MM_MOONLIVE_HAS_HOST_JIT
        if (c.ok) CHECK(r.ok);
#else
        if (c.ok) CHECK((r.ok || std::string(r.error) == moonlive::kCodegenFailed));
#endif
        else      CHECK_FALSE(r.ok);
    }
}

// The three role accessors are aliases of the one table now. Pinned so a future change that re-splits them has to say so here rather than silently reintroducing the trap above.
TEST_CASE("the three roles are handed the same table") {
    const auto layout = moonlive::layoutSysVars();
    const auto effect = moonlive::effectSysVars();
    const auto mod    = moonlive::modifierSysVars();
    CHECK(layout.count == effect.count);
    CHECK(effect.count == mod.count);
    CHECK(mod.count == moonlive::lightSysVars().count);
}

// EVERY comment is whitespace, with no exception.
// There used to be one: `// @control 1..240` declared a control's range, so a comment changed behavior and a malformed one was a compile error.
// `defineControls()` replaced it, which means a comment can no longer be wrong.
TEST_CASE("a comment is whitespace, wherever it appears") {
    struct Case { const char* src; bool ok; const char* what; };
    const Case cases[] = {
        {mmScript("// leading comment\naddLight(1, 2, 3);"), true, "a comment before the code"},
        {mmScript("addLight(1, 2, 3); // trailing comment"), true, "a comment after the code"},
        {mmScript("for (int i = 0; i < 2; i = i + 1) {\n  // inside the body\n  addLight(i, 0, 0);\n}"), true,
         "a comment inside a loop body"},
        {mmScript("// @control 1..64 is just text now\naddLight(1, 2, 3);"), true,
         "the old annotation is an ordinary comment"},
        {mmScript("byte n = 4; // anything at all !!\nfor (int i = 0; i < n; i = i + 1) { addLight(i, 0, 0); }"),
         true, "a comment after a member declaration"},
    };
    for (const Case& c : cases) {
        moonlive::MoonLive engine;
        const bool ok = engine.compile(c.src, moonlive::lightBuiltins(), moonlive::modifierSysVars());
        INFO(c.what);
        // What this case is about is the LEXER, which runs on every host.
        // Where there is no backend for this ISA a valid script still fails, at codegen, so accept that one diagnostic rather than dropping the coverage.
        // A script expected to FAIL must still fail everywhere.
        if (c.ok) CHECK((ok || std::string(engine.error()) == moonlive::kCodegenFailed));
        else      CHECK(!ok);
        engine.free();
    }
}

TEST_CASE("a comment changes nothing about what a script does") {
    // The stronger claim: commenting a script does not alter the code it produces. Needs a backend, with no code emitted, "same length" is two zeroes and proves nothing.
#if MM_MOONLIVE_HAS_HOST_JIT
    moonlive::MoonLive bare, commented;
    CHECK(bare.compile(mmScript("for (int i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"),
                       moonlive::lightBuiltins(), moonlive::modifierSysVars()));
    CHECK(commented.compile(mmScript("// place three lights in a row\n"
                            "for (int i = 0; i < 3; i = i + 1) {\n"
                            "  addLight(i, 0, 0);   // one per step\n"
                            "}"),
                            moonlive::lightBuiltins(), moonlive::modifierSysVars()));
    CHECK(bare.codeLen() == commented.codeLen());   // byte-for-byte the same program
    CHECK(bare.codeLen() > 0);                      // and a real program, not two zeroes
    bare.free();
    commented.free();
#endif
}


// `t` is the elapsed milliseconds the host passes on every run.
// Without it a script cannot animate, every frame computes the same thing, so it is the difference between a static pattern and an effect.
// It resolves to the argument register the host already fills, costing no instruction and no temp: the emitted code reads a3/a0 directly rather than loading a value.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a script reads elapsed time, so it can animate") {
    uint8_t code[2048];
    auto r = moonlive::compileSource(mmScript("setRGB(t, 200, 0, 0);"), moonlive::lightBuiltins(),
                                     moonlive::modifierSysVars(),
                                     code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<moonlive::CtrlFn>(blk);
    uint8_t arena[moonlive::kArenaBytes] = {};

    // The lit light must follow t: a frame at t=3 lights light 3, not light 0.
    for (uint32_t tv : {0u, 3u, 7u}) {
        uint8_t buf[10 * 3] = {};
        fn(buf, 10, 3, tv, arena);
        INFO("t = " << tv);
        CHECK(buf[tv * 3] == 200);                  // the light AT t is lit
        if (tv != 0) CHECK(buf[0] == 0);            // and light 0 is not, so it really moved
    }
    platform::freeExec(blk, r.len);
}
#endif

// Value noise is the primitive every organic effect (fire, clouds, plasma, lava) starts from, so a script needs it as a builtin: it cannot be written from the grammar's arithmetic. The two properties that make it noise rather than a random number are pinned here, it is SMOOTH in space (adjacent points inside one cell differ a little, not wildly) and it actually VARIES across the field (a constant would be smooth too, and useless).
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("noise is smooth across neighboring points, and varies across the field") {
    uint8_t code[4096];
    // One light per sample: light i gets the noise at x = i * 64, so the 32 lights walk 8 whole cells (256 units each) and the buffer IS a real slice of the field, not a corner of one cell.
    auto r = moonlive::compileSource(
        mmScript("for (int i = 0; i < 32; i = i + 1) { setRGB(i, noise(i * 64, 0, 0), 0, 0); }"),
        moonlive::lightBuiltins(), moonlive::modifierSysVars(), code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<moonlive::CtrlFn>(blk);
    uint8_t arena[moonlive::kArenaBytes] = {};
    uint8_t buf[32 * 3] = {};
    fn(buf, 32, 3, 0, arena);

    // Smooth: 64 units per step is a quarter cell, so neighbors move but cannot leap the range.
    int biggestJump = 0;
    for (int i = 1; i < 32; i++) {
        const int d = std::abs(int(buf[i * 3]) - int(buf[(i - 1) * 3]));
        if (d > biggestJump) biggestJump = d;
    }
    INFO("biggest neighbor-to-neighbor jump: " << biggestJump);
    CHECK(biggestJump < 64);

    // Varies: a field that returned one value everywhere would pass the smoothness check.
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < 32; i++) {
        if (buf[i * 3] < lo) lo = buf[i * 3];
        if (buf[i * 3] > hi) hi = buf[i * 3];
    }
    INFO("field spanned " << int(lo) << ".." << int(hi));
    CHECK(hi - lo > 32);   // a real field, not a near-constant one a weak test would accept
    platform::freeExec(blk, r.len);
}
#endif

// `mod` is what turns a moving pattern into a repeating one.
// `t` grows without bound, so a sweep written as `t * speed` runs off the end of the fixture once and never comes back.
// Folding it with `mod(…, width)` makes it return to the start and cycle forever.
// It is a host Call rather than an operator because no ISA here has a cheap integer divide, Xtensa has none at all.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("mod wraps a sweep, so an animation repeats instead of running off the end") {
    uint8_t code[4096];
    auto r = moonlive::compileSource(
        mmScript("byte w = 16;\n"
        "for (int yy = 0; yy < w; yy = yy + 1) { setRGB(yy * w + mod(t, w), 255, 0, 0); }"),
        moonlive::lightBuiltins(), moonlive::modifierSysVars(), code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<moonlive::CtrlFn>(blk);
    uint8_t arena[moonlive::kArenaBytes] = {16};

    // The lit column follows t, and t == width comes back to column 0 rather than off the grid.
    auto columnAt = [&](uint32_t tv) {
        uint8_t buf[256 * 3] = {};
        fn(buf, 256, 3, tv, arena);
        for (int i = 0; i < 256; i++) if (buf[i * 3]) return i % 16;
        return -1;                                  // nothing lit: the sweep left the fixture
    };
    CHECK(columnAt(0)  == 0);
    CHECK(columnAt(7)  == 7);
    CHECK(columnAt(15) == 15);
    CHECK(columnAt(16) == 0);                       // wrapped, not lost
    CHECK(columnAt(33) == 1);                       // and keeps cycling
}
#endif

// A `for` releases its counter's REGISTER when the loop ends, not just its name.
// Dropping only the name left the vreg allocated for the rest of the compile, so every loop a script wrote cost one permanently.
// Two sequential loops held two counters even though the first was long dead.
// That put an ordinary two-loop effect one register over the smallest register file (Xtensa has twelve) while each loop compiled fine on its own, the confusing part, since neither half looked too big.
TEST_CASE("sequential loops reuse the same register, so a script is not billed per loop") {
    uint8_t code[8192];
    // Four loops, each with a call in the body, comfortably over budget if counters accumulate.
    auto r = moonlive::compileSource(
        mmScript("byte w = 16;\n"
        "for (int a = 0; a < w; a = a + 1) { setRGB(a, 255, 0, 0); }\n"
        "for (int b = 0; b < w; b = b + 1) { setRGB(b, 0, 255, 0); }\n"
        "for (int c = 0; c < w; c = c + 1) { setRGB(c, 0, 0, 255); }\n"
        "for (int d = 0; d < w; d = d + 1) { setRGB(d, 255, 255, 0); }"),
        moonlive::lightBuiltins(), moonlive::modifierSysVars(), code, sizeof(code));
    if (!r.ok) INFO(r.error);
    // What this pins is REGISTER REUSE, which the front-end does on every host, but proving it needs code to come out, and only a host with an assembler for its ISA emits any. On a build without one (--no-jit, or an unsupported host) requiring success fails for the one reason that has nothing to do with register reuse.
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(r.ok);
#else
    CHECK((r.ok || std::string(r.error) == moonlive::kCodegenFailed));
#endif
}

// The DOCUMENTATION's script examples compile.
//
// A doc example is what a user copies first, so one that no longer parses is worse than no example.
// It teaches a syntax the engine rejects, and it fails on their device rather than in CI.
// The language gained declared return types and every example in four files went stale at once, which is exactly the drift this catches.
//
// Read from the .md files rather than pasted here: a pasted copy stops being the documented one the first time someone edits the real page.
TEST_CASE("every compile error fits the status line whole, its position included") {
    // A failure reaches the UI as ONE string, "<message> @<offset>": the sentence a user reads and the position the editor marks the failing line from.
    // MoonLiveScript formats it into a fixed buffer, so a message longer than that buffer loses its explanation, and a slightly longer one eats the offset and the line marking silently stops working.
    // Both happened.
    //
    // Read from the SOURCE rather than a list kept here.
    // A hand-kept copy would agree with the buffer while the compiler moved on, which is the drift this exists to prevent.
    const std::filesystem::path repo =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    size_t longest = 0;
    const char* longestText = "";
    for (const char* rel : {"src/core/moonlive/MoonLiveCompiler.cpp",
                            "src/core/moonlive/MoonLiveCompiler.h"}) {
        std::ifstream in(repo / rel);
        REQUIRE(in.good());
        std::string line;
        while (std::getline(in, line)) {
            // Both shapes a diagnostic is written in: fail("...") and a kName = "..." constant.
            for (const char* lead : {"fail(\"", "= \""}) {
                size_t at = 0;
                while ((at = line.find(lead, at)) != std::string::npos) {
                    const size_t beg = at + std::strlen(lead);
                    const size_t end = line.find('"', beg);
                    if (end == std::string::npos) break;
                    const size_t len = end - beg;
                    if (len > longest) { longest = len; }
                    at = end;
                }
            }
        }
    }
    INFO("longest diagnostic is " << longest << " chars");
    CHECK(longest >= 40);              // a control: a parse that found nothing would pass silently
    // The shadow marker and its ": " in front, then " @" + up to 5 digits + the terminator, against the buffer MoonLiveScript declares.
    // The marker is in the budget because it prefixes the longest message on the day a stale user copy fails, which is exactly when the offset matters.
    // The LONGER of the two marks: the stale one prefixes exactly when a stale user copy fails, which is the case the offset matters most in.
    const size_t shadow = std::strlen(mm::moonlive::MoonLiveScript::kShadowMark);
    const size_t stale  = std::strlen(mm::moonlive::MoonLiveScript::kStaleMark);
    const size_t mark = (stale > shadow ? stale : shadow) + 2;
    CHECK(longest + mark + 8 <= mm::moonlive::MoonLiveScript::kMaxStatus);
    (void)longestText;
}

TEST_CASE("every script example in the docs compiles") {
    const std::filesystem::path repo = scriptRoot().parent_path();
    const std::filesystem::path pages[] = {
        repo / "moonlive" / "README.md",
        // The language reference. The role cards carry their own examples, so the details sections on effects.md, layouts.md and modifiers.md are covered by the entries below.
        repo / "docs" / "moonmodules" / "light" / "moonlive.md",
        repo / "docs" / "moonmodules" / "light" / "layouts.md",
        repo / "docs" / "moonmodules" / "light" / "modifiers.md",
    };

    int checked = 0;
    for (const auto& page : pages) {
        INFO("page: ", page.string());
        REQUIRE(std::filesystem::exists(page));
        const std::string text = read(page);

        // Every fenced block that declares a class is a script.
        // A fence holding a fragment (a control table, a shell line) has no `class` and is skipped.
        // The point is to compile what a reader would paste as a whole script.
        size_t pos = 0;
        while ((pos = text.find("\n```", pos)) != std::string::npos) {
            const size_t bodyStart = text.find('\n', pos + 1);
            if (bodyStart == std::string::npos) break;
            const size_t end = text.find("\n```", bodyStart);
            if (end == std::string::npos) break;
            const std::string block = text.substr(bodyStart + 1, end - bodyStart - 1);
            pos = end + 1;
            if (block.find("class ") == std::string::npos) continue;

            INFO("block: ", block);
            moonlive::MoonLive eng;
            // The EFFECT vocabulary for every block: the three role tables are aliases of one light vocabulary (pinned by "the three roles are handed the same table"), so which one is passed is documentation rather than a behavioral choice.
            CHECK(eng.compile(block.c_str(), moonlive::lightBuiltins(), moonlive::effectSysVars()));
            checked++;
        }
    }
    // A page that stopped holding examples would make this vacuously green.
    CHECK_MESSAGE(checked > 0, "no doc examples found: the test would pass without checking anything");
    MESSAGE("compiled " << checked << " script examples from the docs");
}

// The service vocabulary is what a .mls compiles against, and it is easy to get wrong in a way no other test would catch. A missing entry shows up only as "unknown function" on a device, at the column of whichever call happened to come first.
TEST_CASE("a service script compiles against the service vocabulary") {
    const moonlive::BuiltinTable t = moonlive::serviceBuiltins();
    CHECK_FALSE(t.full());                      // a dropped registration is silent otherwise
    for (const char* name : {"gpioRead", "gpioWrite", "setControl", "addControl", "print"}) {
        INFO(name);
        CHECK(t.find(name, static_cast<uint8_t>(std::strlen(name))) != nullptr);
    }
    // And the whole template compiles. The file a user gets when they create a new service must work as handed to them, which is the one script guaranteed to be tried first.
    moonlive::MoonLive engine;
    const bool ok = engine.compile(moonlive::kServiceTemplate, moonlive::serviceBuiltins(),
                                   moonlive::serviceSysVars());
    if (!ok) std::printf("service template FAILED: %s\n", engine.error());
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok);
#else
    CHECK((ok || std::string(engine.error()) == moonlive::kCodegenFailed));
#endif
    engine.free();
}
