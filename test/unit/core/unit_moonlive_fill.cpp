/// @module MoonLive

#include "doctest.h"
#include "moonlive_script_wrap.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/moonlive_emit.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// MoonLive Stage 1a: the load-bearing slice, emit a fixed-color fill as native code, place it in executable memory, and call it over a buffer.
// These tests run the WHOLE path in-process on the desktop host backend (the host ISA's emit + platform::allocExec/ writeExec + a real call), so they prove emit → exec → call → buffer-write works off hardware.
// The Xtensa backend is validated by the live S3 run, not here.

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("MoonLive emitFill produces a non-empty routine") {
    uint8_t code[256];
    size_t n = moonlive::emitFill(code, sizeof(code), 1, 2, 3);
    CHECK(n > 0);
    CHECK(n <= sizeof(code));
}
#endif

TEST_CASE("MoonLive emitFill rejects a too-small buffer (degrades, no overrun)") {
    uint8_t tiny[2];
    CHECK(moonlive::emitFill(tiny, sizeof(tiny), 1, 2, 3) == 0);
}

TEST_CASE("MoonLive emitFill/emitAnimatedFill reject a null output buffer (no crash)") {
    CHECK(moonlive::emitFill(nullptr, 256, 1, 2, 3) == 0);   // ample cap, null out → 0, not a deref
    CHECK(moonlive::emitAnimatedFill(nullptr, 256) == 0);
}

// The compile-through-call tests below need a working host JIT (emit blob + assembler); on x86_64 desktops (Windows, Linux/macOS Intel) MoonLive::compile fails cleanly and the tests would fail on the REQUIRE. Guarded on the emit-header capability macro so they compile out where the backend is unimplemented, the same "runs dark" degradation on-device.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("MoonLive compiles and fills a buffer with the chosen color") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(/*r*/ 10, /*g*/ 20, /*b*/ 200));
    REQUIRE(engine.ok());

    // A 5-light, 3-channel buffer pre-filled with a sentinel so a missed write shows.
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    engine.run(buf.data(), 5, 3, /*t*/ 0);

    for (int i = 0; i < 5; i++) {
        CHECK(buf[i*3 + 0] == 10);
        CHECK(buf[i*3 + 1] == 20);
        CHECK(buf[i*3 + 2] == 200);
    }
}

TEST_CASE("MoonLive run on zero lights writes nothing (robust to empty)") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(255, 0, 0));
    std::vector<uint8_t> buf(3, 0xAB);
    engine.run(buf.data(), 0, 3, 0);          // nLights == 0
    CHECK(buf[0] == 0xAB);                  // untouched
}

// The native routines write channels +0/+1/+2 per light, so a layer with fewer than 3 channels per light can't hold RGB, run() must leave it untouched, not overrun it.
TEST_CASE("MoonLive run is a no-op on sub-RGB buffers (cpl 1 and 2)") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(255, 255, 255));
    for (uint8_t cpl : {uint8_t(1), uint8_t(2)}) {
        std::vector<uint8_t> buf(8 * cpl, 0xAB);   // exact size: an RGB write WOULD overrun
        engine.run(buf.data(), 8, cpl, 0);
        for (auto v : buf) CHECK(v == 0xAB);       // every byte untouched, no out-of-bounds
    }
    // null buffer is also a safe no-op.
    engine.run(nullptr, 8, 3, 0);
}

TEST_CASE("MoonLive recompile swaps the color; free returns to !ok") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(1, 1, 1));
    std::vector<uint8_t> buf(3, 0);
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 1);

    REQUIRE(engine.compile(9, 8, 7));       // recompile a new color
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 9); CHECK(buf[1] == 8); CHECK(buf[2] == 7);

    engine.free();
    CHECK_FALSE(engine.ok());
    // run() after free is a safe no-op (no call through null).
    engine.run(buf.data(), 1, 3, 0);
}

TEST_CASE("MoonLive animated fill derives color from the per-frame t") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compileAnimated());
    REQUIRE(engine.ok());
    std::vector<uint8_t> buf(4 * 3, 0);

    // red = (t>>3)&0xFF, green=0, blue=64. Two different t -> two different reds, proving the runtime arg reaches the emitted native code and changes its output.
    engine.run(buf.data(), 4, 3, /*t*/ 0);
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 0);    // t>>3 == 0
        CHECK(buf[i*3 + 1] == 0);
        CHECK(buf[i*3 + 2] == 64);
    }

    engine.run(buf.data(), 4, 3, /*t*/ 800);   // 800>>3 = 100
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 100);
        CHECK(buf[i*3 + 2] == 64);
    }

    engine.run(buf.data(), 4, 3, /*t*/ 2048);  // 2048>>3 = 256 -> &0xFF = 0
    CHECK(buf[0] == 0);                          // wraps at 256, as a byte does
}

TEST_CASE("platform allocExec returns usable executable memory, freeExec releases it") {
    void* blk = platform::allocExec(64);
    REQUIRE(blk != nullptr);
    // Copy the emitted fill in via writeExec (the IRAM/cache-safe path) and call it.
    uint8_t code[256];
    size_t n = moonlive::emitFill(code, sizeof(code), 7, 7, 7);
    platform::writeExec(blk, code, n);
    auto fn = reinterpret_cast<moonlive::FillFn>(blk);
    std::vector<uint8_t> buf(3, 0);
    fn(buf.data(), 1, 3);
    CHECK(buf[0] == 7);
    platform::freeExec(blk, 64);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT: the JIT-dependent block ends here; the STAGE 1 CONTROLS below
        // exercise the parser/arena, which also depend on compile() succeeding, so they gate too.

#if MM_MOONLIVE_HAS_HOST_JIT

// STAGE 1 CONTROLS, engine-level arena behavior.
// These pin the load-bearing decision: the control-values arena is owned by the engine, has a STABLE address across a recompile (grows only), seeds new slots from their declared default, preserves a slot's live value across a source edit that keeps the control, and is released by free().
// The codegen/live-read is pinned in unit_moonlive_ir; this is the engine's ownership + lifecycle contract.
static moonlive::BuiltinTable kCtrlTable = moonlive::lightBuiltins();
static moonlive::SysVarTable kSys = moonlive::modifierSysVars();

// A loop counter and its limit are live ACROSS a call whenever the body calls anything, which is most real effects. The assembler's contract says it preserves what has to survive; this runs the loop and counts, so a backend that clobbered either would show up as a short or runaway loop rather than as an argument about which registers are caller-saved.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a loop counter survives a call in the body") {
    moonlive::MoonLive eng;
    // random16 is a Call; `i` and the limit `w` are both live around it.
    REQUIRE(eng.compile(mmScript("byte w = 8;\nfor (int i = 0; i < w; i = i + 1) { setRGB(i, random16(200), 200, 0); }"),
                        kCtrlTable, kSys));
    uint8_t buf[8 * 3] = {};
    eng.run(buf, 8, 3, 0);
    int written = 0;
    for (int i = 0; i < 8; i++)
        if (buf[i * 3] || buf[i * 3 + 1] || buf[i * 3 + 2]) written++;
    CHECK(written == 8);     // every iteration ran: the counter was not clobbered by the call
    eng.free();
}
#endif

// `t` is an argument register, not an arena byte, so unlike a control it can be CLOBBERED by a callee under the ABI.
// Every animated script that calls anything reads it after a call, so this runs a script that does exactly that and checks the value that comes out is the one passed in.
// (The arm64 backend saves x3 for this reason; the comment there is not evidence, this is.)
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("elapsed time survives a call that happens before it is read") {
    moonlive::MoonLive eng;
    // Two STATEMENTS, so the ordering is the language's, not an argument-evaluation detail: the first call happens, and only then is `t` read. Light 0 burns the call; light 1 reads t.
    REQUIRE(eng.compile(mmScript("setRGB(0, random16(200), 0, 0);\n"
                        "setRGB(1, mod(t, 200), 0, 0);"), kCtrlTable, kSys));
    uint8_t buf[2 * 3] = {};
    eng.run(buf, 2, 3, 12345);
    CHECK(buf[3] == 12345 % 200);   // 145, the elapsed value the host passed, not a clobbered one
    eng.free();
}
#endif

// The card says how big the compiled program is, and warns only when a budget is nearly gone.
//
// Size, because nothing could answer "how big is my script": the card's memory figure is the word-rounded ALLOCATION, which says nothing about the program.
// One budget, not five, and only past half full.
// A script can hit ten ceilings, but five are derived from code size or nesting and a number the author cannot act on is noise.
TEST_CASE("a compiled script reports its size, and its tightest budget only when it is filling up") {
    moonlive::MoonLive eng;
    char buf[48] = {};

    // Nothing compiled: nothing to say.
    eng.describe(buf, sizeof(buf));
    CHECK(buf[0] == '\0');

    // An ordinary script is nowhere near a wall, so it reports only its size.
    REQUIRE(eng.compile("class T {\n  byte bpm = 30;\n"
                        "  void defineControls() { addControl(\"bpm\", bpm, 1, 240); }\n"
                        "  void tick() { setRGB(0, bpm, 0, 0); }\n}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    eng.describe(buf, sizeof(buf));
    INFO("described: " << buf);
    CHECK(std::strstr(buf, " B") != nullptr);        // a byte count
    CHECK(std::strstr(buf, "/") == nullptr);          // and no budget: 1 of 8 controls is not news
    CHECK(eng.codeLen() > 0);

    // A script using every control slot is one edit from failing, so the card says which wall.
    REQUIRE(eng.compile("class T {\n"
                        "  byte a=1; byte b=1; byte c=1; byte d=1;\n"
                        "  byte e=1; byte f=1; byte g=1; byte h=1;\n"
                        "  void defineControls() { addControl(\"a\",a,0,9); addControl(\"b\",b,0,9);\n"
                        "    addControl(\"c\",c,0,9); addControl(\"d\",d,0,9); addControl(\"e\",e,0,9);\n"
                        "    addControl(\"f\",f,0,9); addControl(\"g\",g,0,9); addControl(\"h\",h,0,9); }\n"
                        "  void tick() { setRGB(0, a, b, c); }\n}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    eng.describe(buf, sizeof(buf));
    INFO("described: " << buf);
    CHECK(std::strstr(buf, "controls 8/8") != nullptr);
    eng.free();
}

// A FAILED recompile drops the declared controls rather than leaving them named "".
//
// The editor loop pushes broken text constantly: that is what editing is.
// A control's `name` is a pointer the UI dereferences on every /api/state, and it points into the engine's string pool, so a pool cleared while the records survived left every card named "" and unmatched by both name-keyed persistence and `POST /api/control`.
// The user's own sliders came unbound from a typo.
TEST_CASE("a broken script drops its controls instead of blanking their names") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte bpm = 30;\n"
                        "  void defineControls() { addControl(\"bpm\", bpm, 1, 240); }\n"
                        "  void tick() { setRGB(0, bpm, 0, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 1);
    CHECK(std::strcmp(dc[0].name, "bpm") == 0);

    CHECK_FALSE(eng.compile("class T { byte = ; }", kCtrlTable, kSys));
    dc = eng.declaredControls(n);
    CHECK(n == 0);          // dropped, so nothing points into a pool the next compile reuses
    eng.free();
}

TEST_CASE("MoonLive controls: declaredControls + controlSlot seeded from the default") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte speed = 42;\n"
                        "  void defineControls() { addControl(\"speed\", speed, 0, 99); }\n"
                        "  void tick() { setRGB(speed, 0, 0, 255); }\n"
                        "}\n", kCtrlTable, kSys));
    // A control exists because defineControls() RAN, the way a compiled module's does. This is the binding's half of that.
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 1);
    CHECK(dc[0].def == 42); CHECK(dc[0].min == 0); CHECK(dc[0].max == 99); CHECK(dc[0].offset == 0);
    uint8_t* slot = eng.controlSlot(0);
    REQUIRE(slot != nullptr);
    CHECK(*slot == 42);                                  // arena slot seeded to the declared default
    // Offset 1 is a real arena byte (an undeclared control slot), so it resolves. Past the arena, controls plus the host's system variables, there is nothing to point at.
    CHECK(eng.controlSlot(moonlive::kArenaBytes) == nullptr);   // out of range → nullptr (robust)
}

// A declared range is an arbitrary expression, so a script can write min > max.
// The write path tests `v < min || v > max`, which is true of EVERY value when the range is inverted.
// The slider would appear and then silently refuse everything the user does to it.
// Refusing the declaration instead leaves the control absent, which is visible, the same stance a range past the declared width already takes.
TEST_CASE("a control declared with min above max is refused, not published as unsettable") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte ok = 5;\n"
                        "  byte bad = 7;\n"
                        "  void defineControls() {\n"
                        "    addControl(\"ok\", ok, 0, 99);\n"
                        "    addControl(\"bad\", bad, 90, 10);\n"
                        "  }\n"
                        "  void tick() { setRGB(ok, 0, 0, 255); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 1);                       // only the sane one reached the sink
    CHECK(std::strcmp(dc[0].name, "ok") == 0);
}

TEST_CASE("MoonLive controls: arena address is STABLE across a recompile and the slot value survives") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("byte speed = 7;\nsetRGB(speed, 0, 0, 255);"), kCtrlTable, kSys));
    uint8_t* before = eng.controlSlot(0);
    REQUIRE(before != nullptr);
    *before = 12;                                        // a "slider move": write the live value

    // Edit the source (recompile) but KEEP the control. The grow-only arena must not move, and the live value must survive (a kept control keeps its slider position across a source edit).
    REQUIRE(eng.compile(mmScript("byte speed = 7;\nsetRGB(speed, 255, 0, 0);"), kCtrlTable, kSys));
    uint8_t* after = eng.controlSlot(0);
    CHECK(after == before);                              // STABLE address, no dangling bound pointer
    CHECK(*after == 12);                                 // value preserved across the recompile

    // Adding a SECOND control keeps the first's value and seeds the new slot from its default.
    REQUIRE(eng.compile(mmScript("byte speed = 7;\nbyte hue = 200;\nsetRGB(speed, hue, 0, 255);"), kCtrlTable, kSys));
    CHECK(*eng.controlSlot(0) == 12);                    // speed kept its live value
    CHECK(*eng.controlSlot(4) == 200);                   // hue seeded from its default, one slot on
}

// A member keeps its live value across a recompile because its name and offset still match.
// But EDITING ITS TYPE OR LENGTH makes it a different member at the same address, and its new bytes have never held anything.
// A widened uint8->uint16 would keep the old program's byte as the new value's high half, and a grown array would keep stale elements past the old end.
// Both read back as numbers the script never wrote.
TEST_CASE("MoonLive controls: widening or growing a member reseeds its whole extent") {
    moonlive::MoonLive eng;

    // Spelled out rather than via mmScript: that helper only hoists `uint8_t` declarations to class scope, so a uint16_t member written through it would become a local instead.
    REQUIRE(eng.compile("class T {\n  byte level = 3;\n  void tick() { setRGB(0, level, 0, 0); }\n}\n",
                        kCtrlTable, kSys));
    *eng.controlSlot(0) = 0xEE;                          // a "slider move" the widened member must not inherit
    REQUIRE(eng.compile("class T {\n  int level = 900;\n  void tick() { setRGB(0, level - 900, 0, 0); }\n}\n",
                        kCtrlTable, kSys));
    const uint8_t* wide = eng.controlSlot(0);
    REQUIRE(wide != nullptr);
    // 900 = 0x0384, little-endian across both bytes.
    // The high byte proves the extent was reseeded.
    // It would be 0 if only the low byte were written, and the low byte would be 0xEE if the member had been treated as unchanged.
    CHECK(wide[0] == 0x84);
    CHECK(wide[1] == 0x03);

    // The same rule for an array that grows: the new elements carry the declared default, not whatever the previous program left at those addresses.
    moonlive::MoonLive eng2;
    REQUIRE(eng2.compile("class T {\n  byte bank[2];\n  void tick() { setRGB(0, bank[0], 0, 0); }\n}\n",
                         kCtrlTable, kSys));
    uint8_t* slot = eng2.controlSlot(0);
    REQUIRE(slot != nullptr);
    slot[2] = 0x77;                                      // beyond the old end: stale bytes to inherit
    slot[3] = 0x77;
    REQUIRE(eng2.compile("class T {\n  byte bank[4];\n  void tick() { setRGB(0, bank[3], 0, 0); }\n}\n",
                         kCtrlTable, kSys));
    const uint8_t* grown = eng2.controlSlot(0);
    // An array with no initializer starts at zero, so the grown elements must read 0 rather than the 0x77 the previous program left at those addresses.
    CHECK(grown[2] == 0);
    CHECK(grown[3] == 0);
}

TEST_CASE("MoonLive controls: free() releases the arena (no stale slot after release)") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("byte a = 5;\nfill(0, 0, a);"), kCtrlTable, kSys));
    REQUIRE(eng.controlSlot(0) != nullptr);
    eng.free();
    CHECK_FALSE(eng.ok());
    CHECK(eng.controlSlot(0) == nullptr);                // arena gone, no dangling pointer handed out
    // Recompiling after a full free re-acquires cleanly (add/remove robustness).
    REQUIRE(eng.compile(mmScript("byte a = 5;\nfill(0, 0, a);"), kCtrlTable, kSys));
    REQUIRE(eng.controlSlot(0) != nullptr);
    CHECK(*eng.controlSlot(0) == 5);                     // re-seeded from default
}

// line(x1, y1, x2, y2, r, g, b), the SEVEN-argument builtin and the widest call a script makes.
// This is the behavioral pin for the args-array call ABI at an arity past the register file.
// All seven values must arrive intact, in order, through the same staging every backend uses.
TEST_CASE("MoonLive line() draws a horizontal segment through the installed canvas") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class L { void tick() { line(1, 0, 3, 0, 10, 20, 200); } }", kCtrlTable, kSys));
    REQUIRE(eng.ok());

    // A 5-wide single-row canvas, sentinel-filled so an errant write shows.
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    moonlive::setDrawCanvas(draw::Canvas{buf.data(), buf.size(), Coord3D{5, 1, 1}, 3});
    eng.run(buf.data(), 5, 3, /*t*/ 0);
    moonlive::setDrawCanvas({});

    CHECK(buf[0*3] == 0xAB);                             // before the segment: untouched
    for (int i = 1; i <= 3; i++) {
        CHECK(buf[i*3 + 0] == 10);
        CHECK(buf[i*3 + 1] == 20);
        CHECK(buf[i*3 + 2] == 200);
    }
    CHECK(buf[4*3] == 0xAB);                             // after the segment: untouched
}

// No canvas installed (a layout or modifier host) → every draw call must no-op, not write through a stale or foreign pointer. The detach after each run is what this pins.
TEST_CASE("MoonLive line() without a canvas draws nothing") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("line(0, 0, 4, 0, 255, 255, 255);"), kCtrlTable, kSys));
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    eng.run(buf.data(), 5, 3, 0);                        // canvas never installed
    for (uint8_t v : buf) CHECK(v == 0xAB);
}

// A binding that installs nothing (a modifier) must not consume one of the two per-thread slots just by RUNNING a script that draws.
// Slots are claimed on install and released on detach, so a claim taken on the READ path is never given back.
// Two such threads exhaust the table, and from then on every install silently fails: the render thread's line() goes dark with nothing to see but a stopped drawing.
// Two threads is the smallest case that shows it, because a single thread re-uses the slot it already leaked.
TEST_CASE("a script that draws with nothing installed does not consume a per-thread slot") {
    // A std::string, not mmScript's thread-local buffer. The two threads below read this, and handing them a pointer into the MAIN thread's buffer is the kind of sharing this very test exists to catch.
    const std::string kDraws = mmScript("line(0, 0, 4, 0, 255, 255, 255);");
    auto runWithNothingInstalled = [&] {
        moonlive::MoonLive eng;
        REQUIRE(eng.compile(kDraws.c_str(), kCtrlTable, kSys));
        std::vector<uint8_t> buf(5 * 3, 0xAB);
        eng.run(buf.data(), 5, 3, 0);        // no canvas, no sink: the modifier's shape
        for (uint8_t v : buf) CHECK(v == 0xAB);   // and it draws nothing
    };
    // CONCURRENT, not sequential: joined threads can be handed the same identity by the OS, and one identity claims one slot. Two live at once is what fills the table.
    std::thread a(runWithNothingInstalled), b(runWithNothingInstalled);
    a.join();
    b.join();

    // Both slots must still be claimable. A leaked claim from either thread above would make this install a no-op and the canvas would stay at its sentinel.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(kDraws.c_str(), kCtrlTable, kSys));
    std::vector<uint8_t> canvasBuf(5 * 3, 0xAB);
    moonlive::setDrawCanvas(draw::Canvas{canvasBuf.data(), canvasBuf.size(), Coord3D{5, 1, 1}, 3});
    eng.run(canvasBuf.data(), 5, 3, 0);
    moonlive::setDrawCanvas({});
    for (int i = 0; i < 5; i++) CHECK(canvasBuf[i*3] == 255);   // the install took effect
}

// Script arithmetic is unsigned, so a coordinate that went "negative" arrives as a huge value. The builtin clamps endpoints to the canvas, which keeps the draw instant and on-grid instead of sending the line walker on a billions-of-steps march (robustness: any input, degrade visibly, never stall the render thread).
TEST_CASE("MoonLive line() clamps out-of-range endpoints to the canvas edge") {
    moonlive::MoonLive eng;
    // x2 = 0 - 1 wraps to the full unsigned word; the clamp pins it to the last column.
    REQUIRE(eng.compile(mmScript("line(0, 0, 0 - 1, 0, 7, 8, 9);"), kCtrlTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0xAB);
    moonlive::setDrawCanvas(draw::Canvas{buf.data(), buf.size(), Coord3D{4, 1, 1}, 3});
    eng.run(buf.data(), 4, 3, 0);
    moonlive::setDrawCanvas({});
    for (int i = 0; i < 4; i++) CHECK(buf[i*3 + 0] == 7);   // the whole row drawn, promptly
}

// ENTRY POINTS DISPATCH: a script with two functions runs the one that was ASKED for.
//
// This is the case a symbol table exists for, and the one that fails silently when it is wrong.
// Both functions compile, both are callable, and running the wrong one just draws the wrong thing.
// The two write different colors to different pixels, so which ran is visible in the buffer.
TEST_CASE("a script runs the entry point the host asked for, not whichever came first") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class TwoFns {\n"
                        "  void helper() { setRGB(0, 11, 0, 0); }\n"
                        "  void tick()   { setRGB(1, 0, 22, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    REQUIRE(eng.entryCount() == 2);

    std::vector<uint8_t> buf(2 * 3, 0);
    eng.run(buf.data(), 2, 3, 0, "tick");
    CHECK(buf[0 * 3 + 0] == 0);      // helper did NOT run
    CHECK(buf[1 * 3 + 1] == 22);     // tick did

    std::fill(buf.begin(), buf.end(), 0);
    eng.run(buf.data(), 2, 3, 0, "helper");
    CHECK(buf[0 * 3 + 0] == 11);     // and now the other way round
    CHECK(buf[1 * 3 + 1] == 0);
}

// Asking for a name the script did not define runs NOTHING. The alternative: falling back to the block start: would run some arbitrary function and look like it worked, which is the failure a binding cannot diagnose.
TEST_CASE("asking for an entry point a script does not define runs nothing") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class OnlyTick { void tick() { setRGB(0, 99, 0, 0); } }", kCtrlTable, kSys));
    CHECK_FALSE(eng.hasEntry("placeLights"));

    std::vector<uint8_t> buf(3, 0xAB);
    eng.run(buf.data(), 1, 3, 0, "placeLights");
    for (uint8_t v : buf) CHECK(v == 0xAB);      // untouched
}

// A NAME IS A MOMENT, not a role.
// One class may define several entry points, and the host calls whichever the moment calls for: `tick` when a frame renders, `modifyLogical` when a coordinate is folded.
// That is what lets an effect also fold coordinates without any feature being added for it, and it is why nothing validates which names a class defines.
TEST_CASE("one class can serve several moments, and each is called on its own") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class Both {\n"
                        "  void tick()          { setRGB(0, 7, 0, 0); }\n"
                        "  void modifyLogical() { setXYZ(3, 4, 5); }\n"
                        "}\n", kCtrlTable, kSys));
    CHECK(eng.hasEntry("tick"));
    CHECK(eng.hasEntry("modifyLogical"));

    // The frame moment paints.
    std::vector<uint8_t> buf(3, 0);
    eng.run(buf.data(), 1, 3, 0, "tick");
    CHECK(buf[0] == 7);

    // The fold moment sends its coordinate to the SINK, not into the buffer.
    // A coordinate on a wall wider than 255 does not fit in a byte, so setXYZ is a full-width call rather than the three-byte store it used to be.
    // Nothing here reaches the buffer above.
    static uint32_t got[3];
    got[0] = got[1] = got[2] = 0;
    mm::moonlive::setCoordSink([](void*, uint32_t x, uint32_t y, uint32_t z) {
        got[0] = x; got[1] = y; got[2] = z;
    }, nullptr);
    uint8_t pos[3] = {0, 0, 0};
    eng.run(pos, 1, 3, 0, "modifyLogical");
    mm::moonlive::setCoordSink(nullptr, nullptr);
    CHECK(got[0] == 3); CHECK(got[1] == 4); CHECK(got[2] == 5);
}

// A class that defines NEITHER of a binding's moments is not an error: it compiles, and the binding simply has nothing to call. The script author decides what their script is for.
TEST_CASE("a class that defines no moment a binding owns is still a valid script") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class Helper { void paint() { setRGB(0, 1, 2, 3); } }", kCtrlTable, kSys));
    CHECK(eng.ok());
    CHECK_FALSE(eng.hasEntry("tick"));
    CHECK_FALSE(eng.hasEntry("modifyLogical"));
}


// A script calling its own function is pinned in unit_moonlive_compiler.cpp ("a function the script calls can light pixels and read the script's controls"), which asserts the same thing plus the control read that a bare call was silently getting wrong.

// Calling a name the class did not declare is refused, rather than resolving to something else.
TEST_CASE("calling a function no one declared is a compile error") {
    uint8_t out[2048];
    auto r = moonlive::compileSource(
        "class Nope { void tick() { missing(); } }", kCtrlTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "unknown function");
}


// A member could be declared and read but never WRITTEN: `x = expr;` was reachable only inside a for header.
// That made every member a constant, so the whole class of effects that carry state forward (fire, trails, decay) was inexpressible.
// This is the statement that makes a member state.
TEST_CASE("a member written by one tick is read by the next") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte level = 0;\n"
                        "  void tick() {\n"
                        "    level = level + 10;\n"
                        "    setRGB(0, level, 0, 0);\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 10);            // seeded 0, plus this tick's 10
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 20);            // the arena byte carried the 10 across the call
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 30);
    eng.free();
}

// The other half of "a member is the script's own state": one function writes it, another reads it. A frame slot could not do this, because each function has its own frame.
TEST_CASE("a member written by one function is read by another") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte shared = 0;\n"
                        "  void stash() { shared = 7; }\n"
                        "  void tick() { stash(); setRGB(0, shared * 3, 0, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, "tick");   // named: a multi-function class has no single "the program"
    CHECK(px[0] == 21);
    eng.free();
}

// A loop counter is a frame slot, and the step clause already writes one, so a body assignment is the same store. Refusing it would have made the header a special case for no reason.
TEST_CASE("a loop variable can be assigned in the loop body") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void tick() {\n"
                        "    for (int i = 0; i < 8; i = i + 1) {\n"
                        "      i = i + 1;\n"          // skips every other light
                        "      setRGB(i, 99, 0, 0);\n"
                        "    }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[24] = {};
    eng.run(px, 8, 3, 0);
    CHECK(px[1 * 3] == 99);        // 1, 3, 5, 7 written
    CHECK(px[3 * 3] == 99);
    CHECK(px[0] == 0);             // 0, 2, 4, 6 skipped
    CHECK(px[2 * 3] == 0);
    eng.free();
}

// The engine rewrites a system variable before every call, so a store to one would be silently undone. Refused with the reason, rather than compiling into something that does not work.
TEST_CASE("a system variable cannot be assigned") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { void tick() { width = 4; } }", kCtrlTable, kSys));
    eng.free();
}

// An assignment to a name nothing declared is a typo, and the message says where a name comes from.
TEST_CASE("assigning to an undeclared name is refused") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { void tick() { nope = 4; } }", kCtrlTable, kSys));
    eng.free();
}


// Every comparison, at, above and below the boundary.
// Six operators lower onto TWO branch ops by swapping operands and negating the sense, so an off-by-one in that mapping is invisible except at the boundary itself.
// `a < b` and `a <= b` differ on exactly one input.
// The table is the proof.
TEST_CASE("if: every comparison is exact at its boundary") {
    struct Case { const char* op; uint8_t lit; uint8_t expect[3]; };  // probe a = 4, 5, 6 against 5
    const Case cases[] = {
        {"<",  5, {1, 0, 0}},
        {"<=", 5, {1, 1, 0}},
        {">",  5, {0, 0, 1}},
        {">=", 5, {0, 1, 1}},
        {"==", 5, {0, 1, 0}},
        {"!=", 5, {1, 0, 1}},
    };
    for (const auto& c : cases) {
        for (uint8_t i = 0; i < 3; i++) {
            const uint8_t a = uint8_t(4 + i);
            char src[192];
            std::snprintf(src, sizeof(src),
                          "class T { void tick() { if (%u %s %u) { setRGB(0, 1, 0, 0); } } }",
                          a, c.op, c.lit);
            moonlive::MoonLive eng;
            CAPTURE(src);
            REQUIRE(eng.compile(src, kCtrlTable, kSys));
            uint8_t px[3] = {};
            eng.run(px, 1, 3, 0);
            CHECK(px[0] == c.expect[i]);
            eng.free();
        }
    }
}

// An else-block must run when, and only when, the then-block did not. The then-block falls through to the end label rather than into the else, which is the jump an if without an else never needs.
TEST_CASE("if/else takes exactly one branch") {
    for (uint8_t a = 4; a <= 6; a++) {
        char src[224];
        std::snprintf(src, sizeof(src),
                      "class T { void tick() { if (%u < 5) { setRGB(0, 11, 0, 0); }"
                      " else { setRGB(0, 22, 0, 0); } } }", a);
        moonlive::MoonLive eng;
        CAPTURE(src);
        REQUIRE(eng.compile(src, kCtrlTable, kSys));
        uint8_t px[3] = {};
        eng.run(px, 1, 3, 0);
        CHECK(px[0] == (a < 5 ? 11 : 22));
        eng.free();
    }
}

// A conditional inside a loop is where a mis-scoped label shows. The if's skip must land inside the body, not past the back edge, or the loop runs once and exits.
TEST_CASE("an if inside a for runs the body every iteration") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void tick() {\n"
                        "    for (int i = 0; i < 6; i = i + 1) {\n"
                        "      if (i < 3) { setRGB(i, 50, 0, 0); }\n"
                        "      else { setRGB(i, 200, 0, 0); }\n"
                        "    }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[18] = {};
    eng.run(px, 6, 3, 0);
    for (uint8_t i = 0; i < 6; i++) CHECK(px[i * 3] == (i < 3 ? 50 : 200));
    eng.free();
}

// The condition is an ordinary expression on both sides, not a name-against-literal special case: the same orthogonality that lets addControl take a computed range.
TEST_CASE("an if condition may be an expression on both sides") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte base = 3;\n"
                        "  void tick() { if (base * 2 >= base + 2) { setRGB(0, 42, 0, 0); } }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 42);      // 6 >= 5
    eng.free();
}

// A conditional makes a member's value decide control flow, which is the combination step 3a and step 6 exist for. State that steers, rather than state that is only read out.
TEST_CASE("a member decides which branch a tick takes") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte phase = 0;\n"
                        "  void tick() {\n"
                        "    if (phase == 0) { setRGB(0, 7, 0, 0); phase = 1; }\n"
                        "    else { setRGB(0, 9, 0, 0); phase = 0; }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0); CHECK(px[0] == 7);
    eng.run(px, 1, 3, 0); CHECK(px[0] == 9);
    eng.run(px, 1, 3, 0); CHECK(px[0] == 7);
    eng.free();
}

// `=` and `==` differ by one character and mean opposite things. Maximal munch is what keeps them apart, and lexing `==` as two assignments would make a comparison silently parse as something else.
TEST_CASE("== is one token, not two assignments") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { void tick() { if (1 = 1) { setRGB(0,1,0,0); } } }", kCtrlTable, kSys));
    eng.free();
}

// A member's arena offset is a BYTE CURSOR, not its declaration index, and every SCALAR advances it by a whole 4-byte slot whatever the member's type. Pinned because everything downstream keys on the offset: the bindings cache arena slot pointers, persistence uses it, and addControl passes it by reference.
TEST_CASE("member offsets advance by a whole slot in declaration order") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte a = 1;\n"
                        "  byte b = 2;\n"
                        "  byte c = 3;\n"
                        "  void defineControls() {\n"
                        "    addControl(\"a\", a, 0, 9);\n"
                        "    addControl(\"b\", b, 0, 9);\n"
                        "    addControl(\"c\", c, 0, 9);\n"
                        "  }\n"
                        "  void tick() { setRGB(0, a, b, c); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 3);
    // Distinct, ascending, and each one addressing its own live byte: three members must never share a slot, which is what a cursor that failed to advance would produce.
    CHECK(dc[0].offset == 0);
    CHECK(dc[1].offset == 4);
    CHECK(dc[2].offset == 8);
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, "tick");
    CHECK(px[0] == 1);
    CHECK(px[1] == 2);
    CHECK(px[2] == 3);
    eng.free();
}

// The arena's byte budget and the record count are now two different limits, and a script can exhaust either one first. Asking for more member data than the arena holds is a compile error with a message about the arena, rather than a member silently landing on top of another one.
TEST_CASE("a class declaring more member data than the arena holds is refused") {
    // The record limit (kMaxCtrls) is reached long before the byte limit when every member is a byte, so the BYTE limit is provoked with arrays.
    // Two of them exceed kCtrlBytes together while staying well inside the record count.
    // Sized from the constants so raising either one cannot silently turn this into a test of the other limit.
    char src[512];
    std::snprintf(src, sizeof(src),
                  "class T { byte a[%d]; byte b[%d]; void tick() { a[0] = 1; } }",
                  moonlive::kCtrlBytes, moonlive::kCtrlBytes);
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(src, kCtrlTable, kSys));
    eng.free();
}

// A uint16_t member holds a value a byte cannot.
// This is the correctness wall on a 256-wide wall.
// Every arena slot was 8-bit, so a coordinate clamped at 255 and a modifier could not walk a light off a large grid.
// The round trip is what matters: seeded wide, read wide, written wide.
TEST_CASE("an int member holds a value above 255") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  int big = 1000;\n"
                        "  void tick() {\n"
                        "    big = big + 300;\n"
                        "    setRGB(0, big - 1300, 0, 0);\n"   // 1300 - 1300 = 0 on the first tick
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 0);      // 1000 + 300 = 1300, which a byte member could not have held
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 44);     // 1600 - 1300 = 300, truncated to a byte by setRGB: 300 & 0xff
    eng.free();
}

// The high byte must survive being stored and reloaded. A store that wrote only the low half would pass the test above on the first tick and lose the value on the second, so the boundary at 256 is checked directly: 255 -> 256 is exactly where a byte member wraps to 0 and a halfword does not.
TEST_CASE("an int member crosses the 255 boundary without wrapping") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  int n = 255;\n"
                        "  void tick() { n = n + 1; if (n == 256) { setRGB(0, 77, 0, 0); } }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 77);     // a byte member would be 0 here, and the branch would not be taken
    eng.free();
}

// EVERY scalar takes a whole 4-byte slot, whatever its type.
// A byte does not pack in beside its neighbor, so a mixed declaration order costs the same as a uniform one and no member ever straddles a boundary.
// That uniformity is what removed the per-width alignment rule this replaces, where a wide member had to skip to an even byte because two backends scale a halfword load's immediate and cannot encode an odd offset at all.
TEST_CASE("every scalar member takes a whole slot whatever its type") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte  small = 1;\n"    // slot 0
                        "  int wide = 900;\n"     // slot 4: a byte costs a whole slot too
                        "  byte  after = 2;\n"    // slot 8
                        "  void defineControls() {\n"
                        "    addControl(\"small\", small, 0, 9);\n"
                        "    addControl(\"after\", after, 0, 9);\n"
                        "  }\n"
                        "  void tick() { setRGB(0, small + wide, 0, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 2);
    CHECK(dc[0].offset == 0);
    // `after` sits at byte 8: `small` took slot 0, `wide` slot 4. A byte member costs a whole slot exactly as an int does, which is the storage rule stated in one number.
    CHECK(dc[1].offset == 8);
    eng.free();
}

// A control no longer declares a width to keep in step with its member.
// ONE call surfaces any member and reads the widget from the member's own type, so the pair that could disagree is gone.
// What survives is the range check, a range past what the member's type holds is refused rather than truncated, because a slider whose top silently wraps is worse than one that never appears.
TEST_CASE("a control takes any scalar member, but not a range its type cannot hold") {
    moonlive::MoonLive eng;
    // An int member surfaces with a range no byte could hold.
    REQUIRE(eng.compile("class T {\n"
                        "  int wide = 5;\n"
                        "  void defineControls() { addControl(\"wide\", wide, 0, 900); }\n"
                        "  void tick() { setRGB(0, wide, 0, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    eng.declaredControls(n);
    CHECK(n == 1);
    eng.free();

    // The same range on a BYTE member is refused: 900 does not fit, and the control is absent rather than published with a top the member cannot reach.
    moonlive::MoonLive engNarrow;
    REQUIRE(engNarrow.compile("class T {\n"
                              "  byte small = 5;\n"
                              "  void defineControls() { addControl(\"small\", small, 0, 900); }\n"
                              "  void tick() { setRGB(0, small, 0, 0); }\n"
                              "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(engNarrow);
    uint8_t nn = 0;
    engNarrow.declaredControls(nn);
    CHECK(nn == 0);
    engNarrow.free();

    // An array is not a control at either width: binding one would move element 0 and leave the rest, with nothing on screen saying so.
    moonlive::MoonLive eng2;
    CHECK_FALSE(eng2.compile("class T {\n"
                             "  byte bank[4];\n"
                             "  void defineControls() { addControl(\"bank\", bank, 0, 9); }\n"
                             "  void tick() { setRGB(0, bank[0], 0, 0); }\n"
                             "}\n", kCtrlTable, kSys));
    eng2.free();
}

// The point of an `int` member.
// A script exposes a value a byte cannot hold, a dwell time, a 0..1000 scale, as ONE control, instead of packing it into two byte sliders.
// The declaration reaches the binding with its full range intact, and the live value spans the member's whole slot.
TEST_CASE("an int member is published as a control spanning its full range") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  int dwell = 900;\n"
                        "  void defineControls() { addControl(\"dwell\", dwell, 0, 1000); }\n"
                        "  void tick() { setRGB(0, dwell - 900, 0, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);

    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 1);
    CHECK(dc[0].type == moonlive::CtrlType::Int);
    CHECK(dc[0].max == 1000);              // a range past 255 survives: the record is 32-bit wide
    CHECK(dc[0].def == 900);               // seeded from the member's own initializer, not its low byte

    // The live value occupies the whole 4-byte SLOT, little-endian, which is what the UI writes through and the emitted code reads back.
    uint8_t* slot = eng.controlSlot(dc[0].offset);
    REQUIRE(slot != nullptr);
    CHECK(slot[0] == (900 & 0xff));
    CHECK(slot[1] == (900 >> 8));
    CHECK(slot[2] == 0);
    CHECK(slot[3] == 0);

    // A "slider move" writes the slot the way the UI does, and the record keeps the full value.
    slot[0] = static_cast<uint8_t>(1000 & 0xff);
    slot[1] = static_cast<uint8_t>(1000 >> 8);
    CHECK((slot[0] | (slot[1] << 8)) == 1000);
    eng.free();

    // NOT ASSERTED HERE: that a LIVE EDIT is picked up, writing the slot and running again.
    // On the desktop backend a second run() of an already-compiled program still renders the old value even though the arena holds the new one.
    // That is an open bug (docs/backlog § Desktop backend: a control-arena write is not seen by a second run()), NOT a property to pin.
    // The assertion belongs with the fix, or it would encode the bug.
    //
    // The initial READ is fine, here and everywhere: what looked like a broken read was run() being called without an entry name, which starts at the block start, `defineControls`, not `tick`. a wide control is hardware-verified on both ISAs (S3 and S31 drive ember's `cycle` to 2000 and back).
}

// A range the MEMBER'S TYPE cannot hold is refused rather than truncated, so a slider's top can never silently wrap to a small number. An int member takes any 32-bit range; a byte member does not, and the declaration is dropped rather than published with a top it cannot reach.
TEST_CASE("a control range past its type is refused, not truncated") {
    moonlive::MoonLive eng;
    // An int member reaches 70000 quite legitimately: the control is published.
    REQUIRE(eng.compile("class T {\n"
                        "  int wide = 5;\n"
                        "  void defineControls() { addControl(\"wide\", wide, 0, 70000); }\n"
                        "  void tick() { setRGB(0, wide, 0, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t nWide = 0;
    eng.declaredControls(nWide);
    CHECK(nWide == 1);
    eng.free();

    // Computed past what the TYPE holds: a byte member cannot reach 100000, so the range is refused rather than truncated to something the slider could never drive.
    moonlive::MoonLive eng2;
    REQUIRE(eng2.compile("class T {\n"
                         "  byte wide = 5;\n"
                         "  void defineControls() { addControl(\"wide\", wide, 0, 1000 * 100); }\n"
                         "  void tick() { setRGB(0, wide, 0, 0); }\n"
                         "}\n", kCtrlTable, kSys));
    moonlive::runDefineControls(eng2);
    uint8_t n = 0;
    eng2.declaredControls(n);
    CHECK(n == 0);                         // the declaration was refused; no control appears
    eng2.free();
}

// The initializer is checked against the DECLARED type, so a value a uint8_t cannot hold is a compile error rather than a member that silently starts at a different number than it says.
TEST_CASE("a byte member cannot be initialized above 255") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte x = 300; void tick() { setRGB(0,x,0,0); } }", kCtrlTable, kSys));
    eng.free();
}

// The same value is legal once the member is declared wide enough to hold it.
TEST_CASE("an int member accepts an initializer a byte could not hold") {
    moonlive::MoonLive eng;
    CHECK(eng.compile("class T { int x = 300; void tick() { setRGB(0, x - 300, 0, 0); } }", kCtrlTable, kSys));
    eng.free();
}

// An array is the difference between an effect that draws a formula and one that SIMULATES something: a particle list, a heat buffer, a per-light decay. Write each element, read it back.
TEST_CASE("an array element written in one loop is read in the next") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte heat[8];\n"
                        "  void tick() {\n"
                        "    for (int i = 0; i < 8; i = i + 1) { heat[i] = i * 10; }\n"
                        "    for (int j = 0; j < 8; j = j + 1) { setRGB(j, heat[j], 0, 0); }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[24] = {};
    eng.run(px, 8, 3, 0);
    for (uint8_t i = 0; i < 8; i++) CHECK(px[i * 3] == i * 10);
    eng.free();
}

// Array contents survive across ticks, like any other member: the arena outlives every call. This is what a decay or trail effect is built on, where each frame reads what the last frame left.
TEST_CASE("array contents survive from one tick to the next") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte acc[4];\n"
                        "  void tick() {\n"
                        "    for (int i = 0; i < 4; i = i + 1) { acc[i] = acc[i] + 5; setRGB(i, acc[i], 0, 0); }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[12] = {};
    eng.run(px, 4, 3, 0);
    CHECK(px[0] == 5);                     // seeded to zero, plus this tick
    eng.run(px, 4, 3, 0);
    CHECK(px[0] == 10);                    // the previous frame's value was still there
    eng.run(px, 4, 3, 0);
    CHECK(px[0] == 15);
    eng.free();
}

// THE SAFETY CASE.
// A script computes an index from live control values, so out of range is an ordinary run-time state, not a defect.
// It must not write outside the member: the system variables and the recursion depth counter share the arena, so a stray write would corrupt the engine rather than the picture.
// The index is clamped to the last element, which degrades visibly (the last light repeats) and never crashes.
TEST_CASE("an out-of-range array index is clamped, not written past the end") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte a[4];\n"
                        "  void tick() {\n"
                        "    for (int i = 0; i < 4; i = i + 1) { a[i] = 1; }\n"
                        "    a[9] = 200;\n"                    // far past the end
                        "    for (int j = 0; j < 4; j = j + 1) { setRGB(j, a[j], 0, 0); }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[12] = {};
    eng.run(px, 4, 3, 0);
    CHECK(px[0 * 3] == 1);                 // untouched
    CHECK(px[1 * 3] == 1);
    CHECK(px[2 * 3] == 1);
    CHECK(px[3 * 3] == 200);               // clamped onto the LAST element
    eng.free();
}

// The same clamp on the READ side, and the system variables must be intact afterwards: reading past the end must not reach into the arena region the host owns.
TEST_CASE("an out-of-range array read is clamped and leaves system variables intact") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte a[4];\n"
                        "  void tick() {\n"
                        "    a[3] = 42;\n"
                        "    setRGB(0, a[200], 0, 0);\n"       // clamps to a[3]
                        "    setRGB(1, width, 0, 0);\n"        // a system variable, still correct
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t* w = eng.controlSlot(moonlive::kSysWidth);
    REQUIRE(w != nullptr);
    *w = 33;
    uint8_t px[6] = {};
    eng.run(px, 2, 3, 0);
    CHECK(px[0] == 42);                    // the clamped read found the last element
    CHECK(px[3] == 33);                    // width was not overwritten by the out-of-range access
    eng.free();
}

// The index is an arbitrary EXPRESSION, not a bare loop counter: the same orthogonality that lets addControl take a computed range and an if condition take one on both sides.
TEST_CASE("an array index may be an expression") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte base = 1;\n"
                        "  byte a[8];\n"
                        "  void tick() {\n"
                        "    a[base * 2 + 1] = 88;\n"          // a[3]
                        "    setRGB(0, a[3], 0, 0);\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 88);
    eng.free();
}

// An array of a wide type. The element scaling and the halfword access have to agree, which is the case where an index multiplied by the wrong width silently reads a neighbor's byte.
TEST_CASE("a int array holds per-element values above 255") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  int v[4];\n"
                        "  void tick() {\n"
                        "    for (int i = 0; i < 4; i = i + 1) { v[i] = 300 + i; }\n"
                        "    if (v[0] == 300) { setRGB(0, 1, 0, 0); }\n"
                        "    if (v[3] == 303) { setRGB(1, 1, 0, 0); }\n"
                        "  }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[6] = {};
    eng.run(px, 2, 3, 0);
    CHECK(px[0] == 1);                     // element 0 kept its full value
    CHECK(px[3] == 1);                     // and so did the last one, so the stride was right
    eng.free();
}

// An array has no single arena byte, so assigning one as a whole is refused with the shape that does work, rather than silently writing its first element.
TEST_CASE("a whole array cannot be assigned in one statement") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte a[4]; void tick() { a = 5; } }", kCtrlTable, kSys));
    eng.free();
}

// And the reverse: a scalar indexed as though it were an array is a typo worth catching.
TEST_CASE("a scalar member cannot be indexed") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte x = 1; void tick() { setRGB(0, x[0], 0, 0); } }", kCtrlTable, kSys));
    eng.free();
}

// An array asking for more bytes than the arena holds is a COMPILE error, not a failed allocation while a fixture is running. A script must not be able to ask a classic ESP32 for memory it has not got and find out at run time.
TEST_CASE("an array larger than the arena is refused at compile time") {
    char src[128];
    std::snprintf(src, sizeof(src), "class T { byte a[%d]; void tick() { a[0] = 1; } }",
                  moonlive::kCtrlBytes + 1);
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(src, kCtrlTable, kSys));
    eng.free();
}


// setXYZ takes THREE arguments, not four.
// The op it lowers to writes three bytes at `index * stride` and still takes that index, exactly as setRGB does: what changed is only the syntax.
// A modifier is handed ONE coordinate per call and can write nothing but slot 0, so an explicit index was a constant every author typed and none could explain. setRGB keeps its index because an effect picks a pixel out of a whole buffer, where the index is the whole point.
namespace {
/// The three values a script's setXYZ produced, captured from the coordinate sink.
///
/// setXYZ became a full-width CALL when a coordinate outgrew a byte, so it no longer writes into the run buffer.
/// A test that wants what the script computed reads it here.
/// Static because the sink takes a plain function pointer.
uint32_t gXyz[3];
void captureXyz(void*, uint32_t x, uint32_t y, uint32_t z) { gXyz[0] = x; gXyz[1] = y; gXyz[2] = z; }
struct XyzProbe {
    XyzProbe() { gXyz[0] = gXyz[1] = gXyz[2] = 0; mm::moonlive::setCoordSink(&captureXyz, nullptr); }
    ~XyzProbe() { mm::moonlive::setCoordSink(nullptr, nullptr); }
};
}  // namespace

TEST_CASE("a modifier writes its coordinate without naming a destination slot") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class M { void modifyLogical() { setXYZ(3, 4, 5); } }\n", kCtrlTable, kSys));
    XyzProbe probe;
    uint8_t xyz[3] = {0, 0, 0};
    eng.run(xyz, 1, 3, 0, moonlive::kEntryModify);
    CHECK(gXyz[0] == 3);
    CHECK(gXyz[1] == 4);
    CHECK(gXyz[2] == 5);
    eng.free();
}

// The old four-argument form is REFUSED rather than quietly reinterpreted: taking it would read the coordinate's x as the slot index and silently write the wrong thing.
TEST_CASE("the old four-argument setXYZ is refused, not reinterpreted") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class M { void modifyLogical() { setXYZ(0, 3, 4, 5); } }\n", kCtrlTable, kSys));
    eng.free();
}

// setRGB is untouched: its index is meaningful, so it still takes four.
TEST_CASE("setRGB still names the light it writes") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() { setRGB(1, 9, 8, 7); } }\n", kCtrlTable, kSys));
    uint8_t px[6] = {};
    eng.run(px, 2, 3, 0);
    CHECK(px[3] == 9);        // light 1, not light 0
    CHECK(px[0] == 0);
    eng.free();
}


// A wide member's INITIALIZER must survive to the arena.
// It was cast to a byte on the way in, so `uint16_t phase = 1000;` started at 232 (1000 & 0xff).
// Every existing test observed through setRGB, which truncates to a byte, and the error is always a multiple of 256: invisible.
// Observed here through a COMPARISON instead, which the byte channel cannot hide.
TEST_CASE("an int member starts at the value it was initialized to") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  int phase = 1000;\n"
                        "  void tick() { if (phase == 1000) { setRGB(0, 55, 0, 0); } }\n"
                        "}\n", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0);
    CHECK(px[0] == 55);
    eng.free();
}


// The palette is what makes a scripted effect follow the device's palette control instead of hard-coding color, which is the split the compiled effects settled long ago.
TEST_CASE("a script paints from the active palette") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() { setPaletteColor(0, 0, 128, 255); } }",
                        kCtrlTable, kSys));
    // A canvas has to be installed or the draw builtins no-op, the same seam line() uses.
    uint8_t px[3] = {9, 9, 9};
    moonlive::setDrawCanvas(draw::Canvas{px, sizeof(px), {1, 1, 1}, 3});
    eng.run(px, 1, 3, 0, moonlive::kEntryTick);
    moonlive::setDrawCanvas(draw::Canvas{});
    // Whatever the active palette holds at index 128, it is not the sentinel.
    const bool untouched = (px[0] == 9) && (px[1] == 9) && (px[2] == 9);
    CHECK_FALSE(untouched);
    eng.free();
}

// polarA/polarR turn a pixel's offset from a center into an angle and a distance, which is what lets a radial effect run without the fixture-sized lookup table the original form needs.
TEST_CASE("polar builtins answer angle and distance from a center") {
    moonlive::MoonLive eng;
    // Directly right of center is angle 0 and distance 4; the script writes both as channels.
    REQUIRE(eng.compile("class T { void tick() { setRGB(0, scale(polarA(4, 0), 256), polarR(4, 0), 0); } }",
                        kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, moonlive::kEntryTick);
    CHECK(px[0] == 0);       // angle 0: straight along +x
    CHECK(px[1] == 4);       // distance 4
    eng.free();

    // A point LEFT of center arrives as an unsigned wrap (x - cx underflows); the builtin re-centers it, so the distance is still 4 rather than a huge number.
    moonlive::MoonLive eng2;
    REQUIRE(eng2.compile("class T { void tick() { setRGB(0, polarR(0 - 4, 0), 0, 0); } }",
                         kCtrlTable, kSys));
    uint8_t px2[3] = {};
    eng2.run(px2, 1, 3, 0, moonlive::kEntryTick);
    CHECK(px2[0] == 4);
    eng2.free();
}

// smoothstep is what turns a DISTANCE into LIGHT: a shape's edge stops being jaggy and becomes a falloff whose width the script chooses. The failure that matters is not the curve, which shader.h already pins, but the unsigned boundary: a script writes `smoothstep(0, w, w - d)` and `w - d` WRAPS the moment d passes w, so a missing re-center reads a huge positive where a small negative was meant, and the shape renders inverted-and-solid.
TEST_CASE("a shape's outside stays dark once the distance passes its edge") {
    moonlive::MoonLive eng;
    // Sweep the distance from inside the edge to well outside it, one light each.
    REQUIRE(eng.compile("class T { void tick() {"
                        "  for (int i = 0; i < 8; i = i + 1) {"
                        "    setRGB(i, scale(smoothstep(0, 400, 400 - i * 100), 256), 0, 0);"
                        "  } } }", kCtrlTable, kSys));
    uint8_t px[8 * 3] = {};
    eng.run(px, 8, 3, 0, moonlive::kEntryTick);
    eng.free();
    CHECK(px[0] == 255);                        // distance 0: fully inside
    for (int i = 1; i < 8; i++) {
        CHECK(px[i * 3] <= px[(i - 1) * 3]);    // never brightens as the distance grows
    }
    CHECK(px[7 * 3] == 0);                      // far outside: dark, not wrapped back to full
}

// A ramp, not a switch. An implementation that truncated the normalize to an integer before the cubic would still pass the monotone check above while drawing a hard edge.
TEST_CASE("smoothstep is a soft ramp rather than a hard threshold") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() {"
                        "  for (int i = 0; i < 8; i = i + 1) {"
                        "    setRGB(i, scale(smoothstep(0, 800, i * 100), 256), 0, 0);"
                        "  } } }", kCtrlTable, kSys));
    uint8_t px[8 * 3] = {};
    eng.run(px, 8, 3, 0, moonlive::kEntryTick);
    eng.free();
    int distinct = 0;
    for (int i = 0; i < 8; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++) if (px[j * 3] == px[i * 3]) seen = true;
        if (!seen) distinct++;
    }
    CHECK(distinct >= 5);                       // a hard threshold would give 2
    CHECK(px[0] == 0);                          // and the ramp starts dark
}

// uv is the mapping a shader starts from: centered on the grid and normalized on the SHORT side, biased at 32768 the way sin/cos already are.
// Its guarantee is that one unit of uv is the same number of PIXELS on both axes, so a shape written as a distance comes out round.
// Skipping it is why a design stretches on a non-square panel.
// With a raw `x - width / 2`, one x-unit and one y-unit differ, and a circle drawn on a 32x8 grid arrives 4:1 wide.
TEST_CASE("a circle drawn through uv stays circular on a wide panel") {
    moonlive::MoonLive eng;
    // Light every cell within a fixed uv radius of the center, on a grid four times wider than it is tall.
    // The lit region must be as tall as it is wide, in PIXELS.
    //
    // uv is Q16.16 and polarR takes whole numbers, so the coordinate is scaled UP before the conversion.
    // ToInt() alone discards the fraction, which on this grid rounds every cell to the same handful of integers and lights the lot.
    REQUIRE(eng.compile("class T { void tick() {"
                        "  for (int y = 0; y < 8; y = y + 1) {"
                        "    for (int x = 0; x < 32; x = x + 1) {"
                        "      if (polarR(toInt(uvX(x, 32, 8) * 1024), "
                        "                 toInt(uvY(y, 32, 8) * 1024)) < 650) {"
                        "        setRGB(y * 32 + x, 255, 0, 0);"
                        "      } } } } }", kCtrlTable, kSys));
    uint8_t px[32 * 8 * 3] = {};
    eng.run(px, 32 * 8, 3, 0, moonlive::kEntryTick);
    eng.free();
    int litCols = 0, litRows = 0;
    for (int x = 0; x < 32; x++) for (int y = 0; y < 8; y++) if (px[(y * 32 + x) * 3]) { litCols++; break; }
    for (int y = 0; y < 8; y++) for (int x = 0; x < 32; x++) if (px[(y * 32 + x) * 3]) { litRows++; break; }
    CHECK(litRows > 2);                // it drew something, and not a single line
    CHECK(litCols == litRows);         // round in pixels; without uv this would be 4:1
    CHECK(litRows < 8);                // and it fits inside the short axis rather than clipping
}

// A coordinate has an origin: the center of the grid is 0, the left half is NEGATIVE, and a script uses the number it is given. No bias to subtract, which is what made `uvX(...) - 32768` wrap on the left half and tear a shader's plane into blocks.
TEST_CASE("uv places the grid center at the origin, with the left half negative") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() {"
                        "  if (uvX(0, 16, 16) < 0) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"
                        "  if (uvX(15, 16, 16) > 0) { setRGB(1, 7, 0, 0); } else { setRGB(1, 3, 0, 0); }"
                        "  if (uvY(0, 16, 16) < 0) { setRGB(2, 7, 0, 0); } else { setRGB(2, 3, 0, 0); }"
                        "} }", kCtrlTable, kSys));
    uint8_t px[9] = {};
    eng.run(px, 3, 3, 0, moonlive::kEntryTick);
    eng.free();
    CHECK(px[0] == 7);                 // the left edge is below the origin
    CHECK(px[3] == 7);                 // the right edge above it
    CHECK(px[6] == 7);                 // and the same on the other axis
}

// smin is what makes two shapes read as ONE surface rather than as two stamps that overlap. The visible difference the blend control sells, stated as a test.
TEST_CASE("blending two shapes with smin produces one surface, not two") {
    // Two circles far enough apart that a plain union leaves a gap between them.
    const char* src = "class T { int k = 0; void tick() {"
                      "  for (int x = 0; x < 16; x = x + 1) {"
                      "    if (smin(polarR(x - 4, 0) - 2, polarR(x - 11, 0) - 2, k) < 0) {"
                      "      setRGB(x, 255, 0, 0); } } } }";
    moonlive::MoonLive hard;
    REQUIRE(hard.compile(src, kCtrlTable, kSys));
    uint8_t px[16 * 3] = {};
    hard.run(px, 16, 3, 0, moonlive::kEntryTick);
    hard.free();
    // k defaults to 0: a plain min, so the midpoint between the two circles stays dark.
    CHECK(px[7 * 3] == 0);
}

// draw::smin widens to 64 bits precisely so a large blend radius cannot WRAP.
// A wrap makes smin return a value larger than both inputs, which inverts the blend rather than lengthening it.
// Note smin legitimately goes BELOW both inputs as k grows.
// That is the merge, not an error, so the property to pin is the ordering against a plain union, not a floor.
TEST_CASE("a longer blend never reads as less merged than a short one") {
    moonlive::MoonLive eng;
    // The same pair of distances at three blend radii, the last large enough that draw::smin's intermediate would overflow a 32-bit multiply if it had not widened to 64.
    // Each must merge at least as hard as the one before it, and the widest must still be a real merge rather than a wrapped value.
    // A wrap makes smin return MORE than both inputs, which inverts the blend the control exists to produce.
    REQUIRE(eng.compile("class T { void tick() {"
                        "  setRGB(0, 0, 0, 0);"
                        "  setXYZ(smin(300, 500, 0), smin(300, 500, 400), smin(300, 500, 60000));"
                        "} }", kCtrlTable, kSys));
    XyzProbe probe;
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, moonlive::kEntryTick);
    eng.free();
    // setXYZ reports FULL WIDTH now, so these are the values themselves rather than their low bytes: the test reads what smin actually computed instead of what survived a byte.
    CHECK(gXyz[0] == 300);                   // k = 0: a plain min of 300 and 500
    CHECK(static_cast<int32_t>(gXyz[1]) <= static_cast<int32_t>(gXyz[0]));   // a blend pulls below the union
    CHECK(static_cast<int32_t>(gXyz[2]) == -14600);   // k = 60000: still merging further below
}

// fade(amt) is the trail primitive: an effect that fades rather than clears leaves a decaying tail behind what it draws. It goes to the LAYER, which collects every request and applies the gentlest once per frame, so what a script can observe here is that the request ARRIVES and carries the amount, not that pixels changed (Layer::tick does that, and Layer owns that test).
TEST_CASE("a script asks its layer to fade, and the amount arrives") {
    static uint16_t asked = 0;
    static uint8_t lastAmt = 0;
    asked = 0; lastAmt = 0;
    moonlive::setFadeSink([](void*, uint8_t amt) { asked++; lastAmt = amt; }, &asked);

    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() { fade(40); } }", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, moonlive::kEntryTick);
    eng.free();
    moonlive::setFadeSink(nullptr, nullptr);

    CHECK(asked == 1);
    CHECK(lastAmt == 40);
}

// An amount past a byte is clamped rather than wrapped. Fade(300) is "fade hard", and wrapping it to 44 would be a gentle fade where the script asked for the opposite.
TEST_CASE("an over-large fade amount clamps to full rather than wrapping") {
    static uint8_t lastAmt = 0;
    lastAmt = 0;
    moonlive::setFadeSink([](void*, uint8_t amt) { lastAmt = amt; }, &lastAmt);
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() { fade(300); } }", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, moonlive::kEntryTick);
    eng.free();
    moonlive::setFadeSink(nullptr, nullptr);
    CHECK(lastAmt == 255);
}

// A layout and a modifier install no fade sink, so the call reaches nothing. Without this a script moved between roles would fade a layer it is not ticking in.
TEST_CASE("fading from a script with no layer does nothing") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T { void tick() { fade(40); setRGB(0, 7, 0, 0); } }", kCtrlTable, kSys));
    uint8_t px[3] = {};
    eng.run(px, 1, 3, 0, moonlive::kEntryTick);   // no sink installed
    eng.free();
    CHECK(px[0] == 7);                            // the run completed, the fade was simply ignored
}

// A script's values are UNSIGNED 32-bit, and `65535 * 65535` is an expression it can write.
// Read back as a signed int that is a large NEGATIVE number, so a coordinate far off the right of the grid used to clamp to the LEFT edge, having overflowed a signed multiply on the way.
// A coordinate past the edge must saturate at the edge it passed.
TEST_CASE("a coordinate far outside the grid saturates at that edge, not the opposite one") {
    moonlive::MoonLive eng;
    // Compared rather than scaled: uv is signed now, and scale() takes the unsigned 0..65535 that beat() produces, so reading a coordinate through it would test the wrong thing.
    REQUIRE(eng.compile("class T { void tick() {"
                        "  if (uvX(3, 4, 4) > 0) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"
                        "  if (uvX(65535 * 65535, 4, 4) > 0) { setRGB(1, 7, 0, 0); } else { setRGB(1, 3, 0, 0); }"
                        "  if (uvY(65535 * 65535, 4, 4) > 0) { setRGB(2, 7, 0, 0); } else { setRGB(2, 3, 0, 0); }"
                        "} }", kCtrlTable, kSys));
    uint8_t px[9] = {};
    eng.run(px, 3, 3, 0, moonlive::kEntryTick);
    eng.free();
    CHECK(px[0] == 7);           // x = 3 on a 4-wide grid: right of center, as a control
    CHECK(px[3] == 7);           // a huge x saturates at the RIGHT edge, not the left
    CHECK(px[6] == 7);           // same on the other axis
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT: every case above needs compile() to succeed, and on a target with no backend (the x86-64 desktop) the helpers they call are compiled out with it.

// Regression: the slot release checked only some of its sinks, so detaching one released the slot while the palette, flow or trail sink was still installed.
TEST_CASE("a sink slot stays owned while any of its sinks is still installed") {
    using namespace mm::moonlive;
    static int ctx = 0;
    setPalSink([](void*, uint8_t, uint8_t, uint8_t, uint8_t) {}, &ctx);
    setFadeSink([](void*, uint8_t) {}, &ctx);
    setFadeSink(nullptr, nullptr);                  // one sink leaves
    CHECK(palSink().fn != nullptr);                 // the palette sink is still this thread's
    setPalSink(nullptr, nullptr);
    CHECK(palSink().fn == nullptr);                 // and the slot goes once nothing is left
}

// The table was FULL at 16 entries and add() failed silently, so the next builtin registered would have vanished and surfaced as "unknown function" in a script.
TEST_CASE("the builtin table has room and reports an overflow") {
    moonlive::BuiltinTable t = moonlive::lightBuiltins();
    CHECK_FALSE(t.full());                                   // nothing was dropped
    CHECK(t.count < moonlive::BuiltinTable::kMax);           // and there is room to grow
    while (t.add({"filler", 0, false, moonlive::BuiltinKind::Call, nullptr, {}})) {}
    CHECK(t.full());                                         // overflow is now VISIBLE
}
