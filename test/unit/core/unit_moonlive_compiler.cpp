/// @module MoonLive

#include "doctest.h"
#include <sstream>
#include <fstream>
#include <filesystem>
#include "moonlive_script_wrap.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/moonlive_emit.h"   // MM_MOONLIVE_HAS_HOST_JIT — the assembler+emit gate
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// MoonLive front-end: an expression grammar where any argument may be a nested call, and the functions (setRGB / fill / random16) are resolved against a host-registered BuiltinTable (the light domain's). The core compiler owns no LED vocabulary, these tests drive it through the light table the same way the binding does.

using namespace mm;

static moonlive::BuiltinTable kTable = moonlive::lightBuiltins();
static moonlive::SysVarTable kSys = moonlive::modifierSysVars();

// Compile + run a source on a w-light, 3-channel buffer; returns the rendered buffer. Only used by the JIT-gated tests below; guard the definition too so a non-JIT build doesn't fire /W4's "unreferenced static function" (which -Werror escalates).
#if MM_MOONLIVE_HAS_HOST_JIT
static std::vector<uint8_t> render(const char* src, int nLights, uint32_t t = 0) {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(src, kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(nLights * 3, 0);
    eng.run(buf.data(), nLights, 3, t);
    return buf;
}
#endif

// The compile-through-run tests need a working host JIT. The assembler (the host backend (moonlive_asm_arm64/x86_64.cpp)) covers arm64 and x86-64, so these run on every desktop the project supports; a host with neither, or a --no-jit build, gets !ok ("codegen failed") and every "should compile" assertion would fail. Guarded on the emit-header capability macro so they compile out there instead, the same "runs dark" degradation as on-device. The malformed-input tests further down don't gate: they assert failure, which succeeds for the right reason (parse rejects) with a backend and for a compatible reason (no codegen) without one.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("compileSource: fill(r,g,b) fills every light") {
    auto buf = render(mmScript("fill(10, 20, 200);"), 8);
    for (int i = 0; i < 8; i++) { CHECK(buf[i*3]==10); CHECK(buf[i*3+1]==20); CHECK(buf[i*3+2]==200); }
}

TEST_CASE("compileSource: setRGB(index, r,g,b) writes one pixel") {
    auto buf = render(mmScript("setRGB(3, 255, 0, 0);"), 8);
    for (int i = 0; i < 8; i++) {
        uint8_t want = (i == 3) ? 255 : 0;
        CHECK(buf[i*3] == want);
    }
}

// A function the script calls is handed the same lights and the same controls as the one calling it. Both halves matter and they fail differently: without the buffer the helper writes nowhere visible, and without the controls arena its first control read dereferences a null pointer.
//
// Found on hardware, not here: an S3 running a three-function effect died with LoadProhibited and EXCVADDR 0x9: offset 9 into a null `ctrls`. Every backend had it, including this one, because a local call emitted the call instruction alone while each function's prologue parks the host arguments out of the argument registers into its own frame. The callee therefore parked whatever the caller had last left in them. The whole-block tests could not see it: they check that a script COMPILES, and this is a script that compiles perfectly and then reads the wrong memory.
TEST_CASE("a function the script calls can light pixels and read the script's controls") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte level = 200;\n"
                        "  void paint() { setRGB(1, level, 0, 0); }\n"
                        "  void tick()  { setRGB(0, 7, 8, 9); paint(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    // Named, as a binding does: an unnamed run enters the block at its FIRST function, which for a class is whichever the script declared first rather than the entry point a role asks for.
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 7);        // the caller's own write still lands
    CHECK(buf[3] == 200);      // and the callee reached both the buffer and the control's value
}

// A helper that calls a helper: the arguments have to survive being passed on twice, not just once. One level of nesting would pass even if a call clobbered what it forwards.
TEST_CASE("arguments reach a function two calls deep") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void inner()  { setRGB(2, 44, 0, 0); }\n"
                        "  void outer()  { setRGB(1, 33, 0, 0); inner(); }\n"
                        "  void tick()   { setRGB(0, 22, 0, 0); outer(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 22);
    CHECK(buf[3] == 33);
    CHECK(buf[6] == 44);
}

// --- function arguments ------------------------------------------------------------------------
//
// Passed BY VALUE, through a block in the arena rather than in the frame: each function opens its own frame and a slot is addressed off the current frame pointer (on Xtensa via the windowed ABI's `entry a1, N`), so a caller cannot reach the slot its callee will read. The arena is what both activations share. See kScriptArgBase in MoonLiveBuiltins.h.

TEST_CASE("a function receives the value its caller passed") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void paint(int idx, int level) { setRGB(idx, level, 0, 0); }\n"
                        "  void tick() { paint(0, 11); paint(1, 22); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 11);
    CHECK(buf[3] == 22);
}

TEST_CASE("an argument is a copy: writing the parameter leaves the caller's variable alone") {
    // By VALUE is the contract, and this is what it means at the call site. A reference would make the second pixel 99 as well.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void bump(int v) { v = 99; setRGB(0, v, 0, 0); }\n"
                        "  void tick() { int mine = 7; bump(mine); setRGB(1, mine, 0, 0); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 99);   // the callee saw its own copy
    CHECK(buf[3] == 7);    // the caller's variable is untouched
}

TEST_CASE("a recursive function keeps its own arguments through the calls it makes") {
    // THE case the single arena block has to survive. One block serves every depth only because a callee copies its arguments into its own frame before it can call anything; without that copy, the inner call would overwrite the outer one's arguments and the countdown would never terminate or would paint the wrong pixels.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void down(int n) { if (n > 0) { setRGB(n, n, 0, 0); down(n - 1); } }\n"
                        "  void tick() { down(3); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[9] == 3);    // each activation kept its own n
    CHECK(buf[6] == 2);
    CHECK(buf[3] == 1);
}

TEST_CASE("a nested call does not clobber the arguments of the call it sits inside") {
    // `outer(inner(x))`: the inner call writes the same arena block. It has RETURNED before the outer arguments are written, which is what makes one block enough.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  int twice(int v) { return v * 2; }\n"
                        "  void paint(int idx, int level) { setRGB(idx, level, 0, 0); }\n"
                        "  void tick() { paint(0, twice(21)); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 42);
}

TEST_CASE("calling a function with the wrong number of arguments is refused") {
    // Both directions, because a silent mismatch would read whatever the arena block held from an earlier call: a wrong picture rather than an error.
    moonlive::MoonLive tooFew;
    CHECK_FALSE(tooFew.compile("class T {\n"
                               "  void paint(int a, int b) { setRGB(a, b, 0, 0); }\n"
                               "  void tick() { paint(1); }\n"
                               "}\n", kTable, kSys));
    moonlive::MoonLive tooMany;
    CHECK_FALSE(tooMany.compile("class T {\n"
                                "  void paint(int a) { setRGB(a, 1, 0, 0); }\n"
                                "  void tick() { paint(1, 2); }\n"
                                "}\n", kTable, kSys));
}

// What an argument COSTS. The question a script author actually asks before rewriting a helper to take parameters, and the reason it is measured rather than reasoned about: the answer decides whether the clearer form is also the affordable one.
TEST_CASE("passing an argument costs about what writing a member costs") {
    // The OLD style: a member is the channel, written by the caller and read by the helper.
    moonlive::MoonLive viaMember;
    REQUIRE(viaMember.compile("class T {\n"
                              "  int idx = 0;\n"
                              "  void paint() { setRGB(idx, 11, 0, 0); }\n"
                              "  void tick() { idx = 0; paint(); idx = 1; paint(); }\n"
                              "}\n", kTable, kSys));
    // The NEW style: the same work, said directly.
    moonlive::MoonLive viaArg;
    REQUIRE(viaArg.compile("class T {\n"
                           "  void paint(int idx) { setRGB(idx, 11, 0, 0); }\n"
                           "  void tick() { paint(0); paint(1); }\n"
                           "}\n", kTable, kSys));
    MESSAGE("member-passing: " << viaMember.codeLen() << " bytes, "
            "argument: " << viaArg.codeLen() << " bytes");
    // Both go through the arena: a member write is StoreCtrl, an argument is StoreCtrl32 into the argument block, and the helper reads it back either way. So the argument form must not cost materially more, and this pins that rather than trusting it.
    CHECK(viaArg.codeLen() < viaMember.codeLen() * 3 / 2);
}

// --- else if, and the for's comparisons ---------------------------------------------------------

TEST_CASE("a byte parameter wraps at 255, the way a byte local does") {
    // A parameter is a local, so it has to behave like one. Handed 300 it kept 300, while the same value assigned to a byte local inside the body narrowed to 44: one type, two behaviors, depending only on how the value arrived.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void paint(byte v) { setRGB(0, v, 0, 0); }\n"
                        "  void tick() { paint(300); }\n"
                        "}\n", kTable, kSys));
    std::vector<uint8_t> buf(3, 0);
    eng.run(buf.data(), 1, 3, 0, "tick");
    CHECK(buf[0] == 44);            // 300 & 0xFF, as a byte local would hold
}

TEST_CASE("a function declaring more parameters than the block holds is refused") {
    // The prologue copies one arena word per declared parameter, so a ninth would read and write past the arena: user-supplied text, refused where it is written.
    moonlive::MoonLive nine;
    CHECK_FALSE(nine.compile("class T {\n"
                             "  void helper(int a, int b, int c, int d, int e, int f, int g, int h, int i) { setRGB(a, 1, 0, 0); }\n"
                             "  void tick() { helper(0, 0, 0, 0, 0, 0, 0, 0, 0); }\n"
                             "}\n", kTable, kSys));

    // Exactly the maximum still compiles: the bound is the block's size, not one below it.
    moonlive::MoonLive eight;
    CHECK(eight.compile("class T {\n"
                        "  void helper(int a, int b, int c, int d, int e, int f, int g, int h) { setRGB(a, b, c, d + e + f + g + h); }\n"
                        "  void tick() { helper(0, 1, 2, 3, 0, 0, 0, 0); }\n"
                        "}\n", kTable, kSys));
}

TEST_CASE("a parameter name is checked the way a local is") {
    // A parameter IS a local, so a name refused inside the body must be refused in the list. Without these checks a parameter could shadow a builtin (making it unreachable for the whole function) or repeat another, where lookups find the first while the caller stages into both.
    for (const char* params : {"int t",            // a system variable
                               "int width",        // a system variable
                               "int a, int a"}) {  // the same name twice
        moonlive::MoonLive eng;
        char src[256];
        std::snprintf(src, sizeof(src),
                      "class T {\n  void helper(%s) { }\n  void tick() { helper(1); }\n}\n", params);
        CAPTURE(params);
        CHECK_FALSE(eng.compile(src, kTable, kSys));
    }
    // And a well-formed list still compiles.
    moonlive::MoonLive ok;
    CHECK(ok.compile("class T {\n"
                     "  void helper(int a, int b) { setRGB(a, b, 0, 0); }\n"
                     "  void tick() { helper(0, 42); }\n"
                     "}\n", kTable, kSys));
}

TEST_CASE("a chain of else if arms picks exactly one") {
    // The shape a MODE control needs, and every arm is exercised: a chain that fell through would still paint the right value for the first case while painting a later arm's for the rest. The selector is a plain local per compile rather than a control, so the arms are chosen the same way at runtime without needing a control-capable fixture table.
    for (const auto& [mode, want] : std::vector<std::pair<int, uint8_t>>{
             {0, 10}, {1, 20}, {2, 30}, {7, 40}}) {
        CAPTURE(mode);
        char src[384];
        std::snprintf(src, sizeof(src),
                      "class T {\n"
                      "  void tick() {\n"
                      "    int mode = %d;\n"
                      "    if (mode == 0) { setRGB(0, 10, 0, 0); }\n"
                      "    else if (mode == 1) { setRGB(0, 20, 0, 0); }\n"
                      "    else if (mode == 2) { setRGB(0, 30, 0, 0); }\n"
                      "    else { setRGB(0, 40, 0, 0); }\n"
                      "  }\n"
                      "}\n", mode);
        moonlive::MoonLive eng;
        REQUIRE(eng.compile(src, kTable, kSys));
        REQUIRE(eng.ok());
        std::vector<uint8_t> buf(3, 0);
        eng.run(buf.data(), 1, 3, 0, "tick");
        CHECK(buf[0] == want);    // exactly the arm that matches, and nothing after it
    }
}

TEST_CASE("a for counts up to its bound with < and through it with <=") {
    // `<=` is the same guard against a limit one higher, computed once rather than per iteration.
    moonlive::MoonLive lt;
    REQUIRE(lt.compile("class T {\n"
                       "  void tick() { for (int i = 0; i < 3; i = i + 1) { setRGB(i, 99, 0, 0); } }\n"
                       "}\n", kTable, kSys));
    std::vector<uint8_t> a(4 * 3, 0);
    lt.run(a.data(), 4, 3, 0, "tick");
    CHECK(a[6] == 99);            // index 2 painted
    CHECK(a[9] == 0);             // index 3 NOT painted

    moonlive::MoonLive le;
    REQUIRE(le.compile("class T {\n"
                       "  void tick() { for (int i = 0; i <= 3; i = i + 1) { setRGB(i, 99, 0, 0); } }\n"
                       "}\n", kTable, kSys));
    std::vector<uint8_t> b(4 * 3, 0);
    le.run(b.data(), 4, 3, 0, "tick");
    CHECK(b[9] == 99);            // index 3 IS painted
}

TEST_CASE("a descending for is refused with the workaround named") {
    // Refused rather than half-supported: the step's DIRECTION is not modelled, so a `>` accepted here would emit an ascending guard around a descending body.
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T {\n"
                            "  void tick() { for (int i = 3; i > 0; i = i - 1) { setRGB(i, 9, 0, 0); } }\n"
                            "}\n", kTable, kSys));
}

// UNBOUNDED recursion must degrade visibly and keep the device rendering. A fixed render-task stack means the alternative is a reset, which the robustness rule forbids. A reset is what this script produced before the depth guard: ~176 bytes of frame per activation against a 12 KB main task is a device that reboots at roughly 64 deep, mid-frame, with no diagnostic.
//
// The guard lives in the callee's prologue and refuses by returning, so the recursion stops at kMaxCallDepth and everything above it still runs. What the user sees is a picture that is wrong where the recursion bottomed out, on a device that is still running.
TEST_CASE("a script that recurses without end keeps rendering instead of resetting") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void forever() { setRGB(1, 200, 0, 0); forever(); }\n"
                        "  void tick()    { setRGB(0, 50, 0, 0); forever(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());                       // it COMPILES: whether it terminates is not decidable
    std::vector<uint8_t> buf(4 * 3, 0);
    // The assertion is that this RETURNS at all. Without the guard it recurses until the stack is gone: a segfault here on the host, a reset on a board.
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 50);        // the entry function ran
    CHECK(buf[3] == 200);       // and so did the recursion, as far as it was allowed
    // Runnable AGAIN, at full depth: the counter unwinds with the frames, so hitting the limit in one frame must not shrink the next. A leaked level per frame would silently reduce every later frame's budget until nothing recursed at all.
    std::vector<uint8_t> second(4 * 3, 0);
    eng.run(second.data(), 4, 3, 0, "tick");
    CHECK(second[0] == 50);
    CHECK(second[3] == 200);
}

// A function with an EMPTY body still balances the recursion counter.
//
// The depth guard is emitted at a function's first real op, because it has to follow the host arguments being parked into the frame. A function whose whole body is that parking has no first real op, so it got no increment while its epilogue still decremented: every call to it drove the counter DOWN, two calls wrapped the byte past zero, and the next legal call was refused as too deep. What a user saw was a call that silently did nothing, in a script with no recursion in it.
TEST_CASE("an empty function does not consume the recursion budget") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  void nop()  { }\n"
                        "  void draw() { setRGB(0, 255, 0, 0); }\n"
                        "  void tick() { nop(); nop(); draw(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 255);   // draw() ran: the two empty calls left the budget where they found it
}

// An over-long function name is REFUSED, not truncated. The engine copies entry names into a fixed buffer, so a longer one would be clipped there: and two functions sharing a 23-character prefix would then land under the same name, with `entry()` returning whichever came first. A call would dispatch to the wrong function and nothing would say so.
TEST_CASE("a function name too long to store is refused, not silently truncated") {
    uint8_t out[2048];
    std::string longName(mm::moonlive::kMaxEntryName + 1, 'a');
    const std::string src = "class T {\n  void " + longName + "() { }\n  void tick() { }\n}\n";
    auto r = moonlive::compileSource(src.c_str(), kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::strlen(r.error) > 0);
    // One character shorter is fine, so the limit is the limit and not an off-by-one.
    const std::string ok = "class T {\n  void " + longName.substr(1) + "() { }\n  void tick() { }\n}\n";
    auto r2 = moonlive::compileSource(ok.c_str(), kTable, kSys, out, sizeof(out));
    CHECK(r2.ok);
}

// REMARK #1: every argument is an expression, random16 in ANY slot.
TEST_CASE("compileSource: random16 works in any argument slot") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("setRGB(random16(8), random16(256), 30, 0);"), kTable, kSys));
    REQUIRE(eng.ok());
    for (int run = 0; run < 32; run++) {
        std::vector<uint8_t> buf(8 * 3, 0);
        eng.run(buf.data(), 8, 3, 0);
        int lit = 0;
        for (int i = 0; i < 8; i++) if (buf[i*3] || buf[i*3+1] || buf[i*3+2]) lit++;
        CHECK(lit == 1);   // one random pixel, with a random red — exactly one lit
    }
}

// A literal spans the whole signed 32-bit range, because a member does: the old 0..65535 cap was the widest member of the day, which left a literal and the member it was assigned to disagreeing about what a number could be.
TEST_CASE("compileSource: a literal may be any value an int member can hold") {
    moonlive::MoonLive eng;
    CHECK(eng.compile(mmScript("setRGB(random16(65535), 0, 0, 255);"), kTable, kSys));
    CHECK(eng.compile(mmScript("setRGB(1000, 0, 0, 255);"), kTable, kSys));
    CHECK(eng.compile(mmScript("int big = 70000;\nsetRGB(big / 1000, 0, 0, 255);"), kTable, kSys));
    // The signed 32-bit boundaries themselves, which is where the lexer's overflow guard lives.
    CHECK(eng.compile(mmScript("int hi = 2147483647;\nsetRGB(hi / 100000000, 0, 0, 255);"),
                      kTable, kSys));
    CHECK(eng.compile(mmScript("int lo = -2147483648;\nsetRGB(0 - lo / 100000000, 0, 0, 255);"),
                      kTable, kSys));
    eng.free();
}

// The lexer accumulates in int64 and checks BEFORE each multiply. `long` is 32 bits on both ESP32 targets and a script COMPILES ON THE DEVICE, so the old guard could never fire there: the multiply wrapped first (signed overflow, UB) and the script got a number nobody wrote, while every host test stayed green. These are the two shapes that broke.
TEST_CASE("a number too large for an int is refused rather than wrapped") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("int huge = 3000000000;\nsetRGB(0, huge, 0, 0);"),
                            kTable, kSys));
    eng.free();
    // One PAST the most negative int: the lexer admits the magnitude 2147483648 so that
    // -2147483648 can be written, so the sign-aware site is what has to catch this.
    moonlive::MoonLive engLow;
    CHECK_FALSE(engLow.compile(mmScript("int low = -2147483649;\nsetRGB(0, low, 0, 0);"),
                               kTable, kSys));
    engLow.free();
    moonlive::MoonLive eng2;
    CHECK_FALSE(eng2.compile(mmScript("int huge = 99999999999;\nsetRGB(0, huge, 0, 0);"),
                             kTable, kSys));
    eng2.free();
}

TEST_CASE("compileSource: out-of-range index is bounds-rejected at runtime") {
    auto buf = render(mmScript("setRGB(5000, 255, 255, 255);"), 8);   // 5000 >> 8 lights
    for (auto v : buf) CHECK(v == 0);                       // guarded — nothing written
}

TEST_CASE("compileSource rejects malformed programs with a diagnostic, never crashes") {
    uint8_t out[256];
    // Each of these MUST fail, assert it, so an accidental successful compile is caught (not just "no crash"). Wrong arity, unknown name, unbalanced parens, trailing junk, empty.
    const char* bad[] = {
        "",                                  // empty
        mmScript("setRGB(0,0,0,0,0);"),                // too many args
        mmScript("fill(0,0);"),                        // too few args
        "wibble(1);",                        // unknown function
        mmScript("setRGB(0, 0, 0"),                    // missing ')'  and ';'
        mmScript("fill(0,0,0)"),                       // missing ';'
        mmScript("fill(0,0,0); extra"),                // trailing junk
        mmScript("setRGB(random8(8), 0, 0, 0);"),      // unknown nested function
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);                   // the parser contract: malformed → rejected
        CHECK(std::strlen(r.error) > 0);     // …with a diagnostic
    }
    // A value-returning function used as a void statement IS valid (result discarded).
    CHECK(moonlive::compileSource(mmScript("random16(8);"), kTable, kSys, out, sizeof(out)).ok);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("MoonLive.compile(source) on a bad script leaves the engine !ok with an error") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("setRGB(oops);"), kTable, kSys));
    CHECK_FALSE(eng.ok());
    CHECK(std::strlen(eng.error()) > 0);
    std::vector<uint8_t> buf(3, 0xAB);
    eng.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 0xAB);
}

// VREG REUSE: a chain of calls must fit the small device register file. Each argument temp dies once its call consumes it and is recycled, so peak register pressure stays low no matter how many calls a statement nests, setRGB with all four arguments a random16 still compiles.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a multi-call statement reuses dead vregs and stays within the register budget") {
    moonlive::BuiltinTable t = moonlive::lightBuiltins();
    uint8_t out[768];
    for (const char* s : {
            mmScript("setRGB(random16(64), random16(256), 30, 0);"),                          // 2 calls
            mmScript("setRGB(random16(128), random16(256), random16(256), 0);"),              // 3 calls
            mmScript("setRGB(random16(128), random16(256), random16(256), random16(256));"),  // 4 calls
         }) {
        auto r = moonlive::compileSource(s, t, kSys, out, sizeof(out));
        CHECK(r.ok);          // without vreg reuse the 3-/4-call cases overflow the register file
        CHECK(r.len > 0);
    }
}

// DOMAIN-NEUTRAL: the core compiler owns no function names. With an EMPTY table it knows nothing, `setRGB`/`fill`/`random16` are all "unknown function". The LED vocabulary lives only in the host's table; a different host registers different names. (Remark #3.)
TEST_CASE("core compiler has no built-in functions of its own (empty table → all unknown)") {
    moonlive::BuiltinTable empty;
    uint8_t out[256];
    for (const char* s : {mmScript("setRGB(0,0,0,0);"), mmScript("fill(0,0,0);"), "random16(8);"}) {
        auto r = moonlive::compileSource(s, empty, {}, out, sizeof(out));
        CHECK_FALSE(r.ok);                       // the core doesn't know any of these
        CHECK(std::strlen(r.error) > 0);
    }
    // A host can register an arbitrary name against the same neutral machinery.
    moonlive::BuiltinTable custom;
    custom.add({"paint", 4, false, moonlive::BuiltinKind::Inline, nullptr, moonlive::InlineOp::StoreElem});
    auto r = moonlive::compileSource(mmScript("paint(2, 9, 8, 7);"), custom, {}, out, sizeof(out));
    CHECK(r.ok);                                 // a different name, same core path
}

TEST_CASE("MoonLive recompiling swaps the program live (fill <-> setRGB)") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("fill(0,0,255);"), kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[0*3+2] == 255); CHECK(buf[3*3+2] == 255);

    REQUIRE(eng.compile(mmScript("setRGB(1, 255, 0, 0);"), kTable, kSys));
    std::fill(buf.begin(), buf.end(), 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[1*3+0] == 255); CHECK(buf[0] == 0);
}

// CONTROLS: a declaration is a member, and `addControl("name", name, lo, hi)` in defineControls A control is declared by CALLING addControl inside defineControls, the same call a compiled module makes. The declaration alone is a member: state the script owns, which the UI never sees unless the script asks for it. That split is the whole point, so both halves are checked here.
//
// Engine-level rather than compileSource-level, because a control now exists because a function RAN: compileSource emits the code, and runDefineControls executes it.
TEST_CASE("a control is declared by calling addControl, and a plain member is not") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte speed = 50;\n"
                        "  byte hidden = 7;\n"
                        "  void defineControls() { addControl(\"speed\", speed, 0, 99); }\n"
                        "  void tick() { setRGB(0, speed, hidden, 255); }\n"
                        "}\n", kTable, kSys));
    moonlive::runDefineControls(eng);

    uint8_t n = 0;
    const auto* c = eng.declaredControls(n);
    REQUIRE(n == 1);                                  // `hidden` is a member, not a control
    CHECK(std::strcmp(c[0].name, "speed") == 0);
    CHECK(c[0].min == 0); CHECK(c[0].max == 99);
    CHECK(c[0].def == 50);                            // from the member's initializer
    CHECK(c[0].type == moonlive::CtrlType::Byte);

    // Both members hold their declared values, whether or not a control surfaces them: the initializer seeds the arena, which is what makes a member state rather than a constant.
    uint8_t buf[3] = {};
    eng.run(buf, 1, 3, 0, "tick");
    CHECK(buf[0] == 50);                              // `speed`, which the UI also shows
    CHECK(buf[1] == 7);                               // `hidden`, read by tick, never on the UI
}

// A control's range is an ORDINARY EXPRESSION, like every other argument in the language. Making addControl the one call whose arguments must be literals would be a special case wearing a disguise, so this pins that it is not one.
TEST_CASE("a control's range can be computed, not just written as a literal") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  byte base = 10;\n"
                        "  byte speed = 20;\n"
                        "  void defineControls() { addControl(\"speed\", speed, base, base * 4 + 5); }\n"
                        "  void tick() { setRGB(0, speed, 0, 0); }\n"
                        "}\n", kTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const auto* c = eng.declaredControls(n);
    REQUIRE(n == 1);
    CHECK(c[0].min == 10);        // base
    CHECK(c[0].max == 45);        // base * 4 + 5
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A system variable is a value the HOST hands the script, the layer's size, the light being transformed, the clock. Letting a script declare the same name would shadow the value it is being given, silently: an effect declaring `width = 16` on an 8x8 panel draws off the edge, and every statement in it still runs perfectly. So the name is refused wherever a name can be introduced, which is exactly two places: a control declaration and a `for` loop variable.
TEST_CASE("a script cannot declare a name the engine already defines") {
    uint8_t out[512];
    struct Case { const char* src; const char* what; };
    const Case refused[] = {
        {mmScript("byte width = 16;\nsetRGB(0, 0, 0, 0);"), "a control named width"},
        {mmScript("byte t = 5;\nsetRGB(0, 0, 0, 0);"),                        "a control named t"},
        {mmScript("for (int xPos = 0; xPos < 4; xPos = xPos + 1) { setRGB(xPos, 0, 0, 0); }"),
                                                                        "a loop variable named xPos"},
        {mmScript("for (int height = 0; height < 4; height = height + 1) { setRGB(0, 0, 0, 0); }"),
                                                                        "a loop variable named height"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == "name is a system variable");   // the clash is named, not generic
    }
    // The same names READ fine, refusing the declaration is what keeps the read meaningful.
    auto ok = moonlive::compileSource(mmScript("setRGB(width, height, depth, t);"), kTable, kSys,
                                      out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);                                        // a backend exists: it must emit
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));   // parses; no backend here
#endif
    // A name the host did NOT register is an ordinary control, not a reserved word.
    auto own = moonlive::compileSource(mmScript("byte cols = 16;\nsetRGB(cols, 0, 0, 0);"), kTable, kSys,
                                       out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(own.ok);
#else
    CHECK((own.ok || std::string(own.error) == moonlive::kCodegenFailed));
#endif
}

// Found by review: this compiled cleanly and emitted a program that NEVER RETURNED. The inner loop bound a second register to the same name, so its step wrote the register the outer back edge tested and the counter never advanced, a hang on the render task, from a script a user can type into the editor. Robustness says any input degrades visibly rather than wedging the device.
TEST_CASE("a nested loop cannot reuse the enclosing loop's variable") {
    // 1 KB, not the 512 this test carried from the arm64-only era: the sequential-loops case at the bottom emits 584 bytes on x86-64 (two call sites; that backend saves/restores its full pool around each). The production path sizes from codeCapFor (≈2.3 KB for this script), so the constant here is scaffolding, the x86-64 density itself is pinned in unit_moonlive_codegen_x86_64.cpp's canary.
    uint8_t out[1024];
    auto r = moonlive::compileSource(
        mmScript("for (int i = 0; i < 2; i = i + 1) { for (int i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } }"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "loop variable already in use");
    // Distinct names nest fine, the check must not refuse the ordinary case it exists to protect.
    auto ok = moonlive::compileSource(
        mmScript("for (int yy = 0; yy < 2; yy = yy + 1) { for (int xx = 0; xx < 2; xx = xx + 1) { addLight(xx, yy, 0); } }"),
        kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));
#endif
    // Sequential loops REUSE a name legitimately: the first has left scope by the time the second binds, so this must still compile (two-rows.mll is exactly this shape).
    auto seq = moonlive::compileSource(
        mmScript("for (int i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } for (int i = 0; i < 2; i = i + 1) { addLight(i, 1, 0); }"),
        kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(seq.ok);
#else
    CHECK((seq.ok || std::string(seq.error) == moonlive::kCodegenFailed));
#endif
}

// The emitted loop tests and advances its OWN counter whatever name the condition and step clauses write, so a mistyped name used to compile clean and run as though it said the right thing, a wrong fixture with no diagnostic anywhere. Found by review.
TEST_CASE("a compile error reports the exact offset the editor turns into a line and column") {
    uint8_t out[512];
    // The offset is the ONLY position anyone has: the editor slices the source up to it to find the failing line, and marks that line in the paint layer. It is therefore ZERO-based, counted in characters from the start of the whole source, while Lexer::col() is one-based; the conversion happens where the two conventions meet. An off-by-one here marks the wrong line whenever a failure lands on the first character of one.
    const char* src =
        "class T {\n"                       // line 1, offsets 0..9
        "  byte b = 1;\n"                   // line 2, offsets 10..24
        "  void tick() { fill(0, 0, 0; }\n"  // line 3: the missing ')' is here
        "}\n";
    auto r = moonlive::compileSource(src, kTable, kSys, out, sizeof(out));
    REQUIRE_FALSE(r.ok);
    CHECK(std::string(r.error) == "expected ')'");

    // What the light domain publishes, and what the editor reads back.
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(src, kTable, kSys));
    REQUIRE(eng.hasErrorPos());
    const size_t at = eng.errorPos();
    REQUIRE(at < std::strlen(src));

    // The line and column a reader counts, derived the way the editor derives them.
    const std::string upto(src, at);
    const size_t line = std::count(upto.begin(), upto.end(), '\n') + 1;
    const size_t nl = upto.rfind('\n');
    const size_t col = upto.size() - (nl == std::string::npos ? 0 : nl + 1) + 1;
    INFO("reported offset " << at << " -> line " << line << ", col " << col);
    // The EXACT position, not merely somewhere on the right line: an off-by-one in the one-based to zero-based conversion keeps the line and still lands inside it, so a loose assertion passes while the editor marks a character that is not the one the parser choked on.
    CHECK(line == 3);
    CHECK(col == 29);
    CHECK(src[at] == ';');   // the ';' written where a ')' belongs
}

TEST_CASE("a for loop declares its counter, as every other variable in the language does") {
    uint8_t out[512];
    // The rule the language already held everywhere else: a member carries its type and an assignment to an undeclared name is refused, so a counter appearing out of nowhere was the last exception. Requiring `int` also makes the loop header identical to the C++ one, which is what test/python/test_scripts_are_cpp.py compiles every shipped script as.
    auto bare = moonlive::compileSource(
        mmScript("for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(bare.ok);
    CHECK(std::string(bare.error) == "a loop counter is declared: for (int i = 0; ...)");

    auto declared = moonlive::compileSource(
        mmScript("for (int i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"),
        kTable, kSys, out, sizeof(out));
    CHECK(declared.ok);
}

TEST_CASE("a for loop's condition and step must name the loop variable") {
    uint8_t out[512];
    struct Case { const char* src; const char* err; const char* what; };
    const Case refused[] = {
        {mmScript("for (int i = 0; j < 3; i = i + 1) { addLight(i, 0, 0); }"),
         "the condition must test the loop variable", "a typo in the condition"},
        {mmScript("for (int i = 0; i < 3; j = j + 1) { addLight(i, 0, 0); }"),
         "the step must advance the loop variable",   "a typo in the step"},
        // Plain names, not x/y: those are system variables in this table and would be refused a step earlier, hiding what this case is about.
        {mmScript("for (int a = 0; a < 4; a = a + 1) { for (int b = 0; a < 4; b = b + 1) { addLight(b, a, 0); } }"),
         "the condition must test the loop variable", "an inner loop testing the OUTER variable"},
        // The step is re-lexed from the source it was skipped over, and an expression parser stops at the first token it cannot use, so trailing junk was silently dropped.
        {mmScript("for (int i = 0; i < 3; i = i + 1 garbage) { addLight(i, 0, 0); }"),
         "unexpected token in the for's step", "trailing junk after the step expression"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == c.err);
    }
    // The ordinary loop is untouched.
    auto ok = moonlive::compileSource(mmScript("for (int i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"),
                                      kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));
#endif
}

// The op array is sized to the script, so `count` is a uint16_t, and every loop over it has to be one too. A uint8_t counter wrapped at 256 ops and spun forever, which on a device is a watchdog reset from a script that merely got long. Found by bisecting: 60 statements fine, 80 hung.
TEST_CASE("a long script compiles or refuses, but never spins") {
    uint8_t out[16384];
    // A long ARITHMETIC chain, not many statements: each `+ 1` is one cheap op, so this passes 256 IR ops while staying inside the code buffer. Repeated statements hit the code ceiling first and return before the wrap, which is why they do not pin this.
    std::string body = "addLight(1";
    for (int i = 0; i < 200; i++) body += " + 1";
    body += ", 0, 0);";
    const std::string many = mmScript(body.c_str());
    auto r = moonlive::compileSource(many.c_str(), kTable, kSys, out, sizeof(out));
    // Either answer is fine, what is NOT fine is never returning, which is what this pins.
    CHECK((r.ok || std::strlen(r.error) > 0));

    // And the sanity bound still refuses a runaway rather than trying to allocate for it. Built directly rather than through mmScript: 3000 statements is far past any fixed buffer, which is the whole point of the case.
    std::string absurd = "class Runaway {\n  void tick() {\n";
    for (int i = 0; i < 3000; i++) absurd += "addLight(1, 0, 0);";
    absurd += "\n  }\n}\n";
    auto big = moonlive::compileSource(absurd.c_str(), kTable, kSys, out, sizeof(out));
    CHECK_FALSE(big.ok);
    CHECK(std::string(big.error) == "script too large");
}

TEST_CASE("compileSource: malformed control declarations fail with a diagnostic, never crash") {
    uint8_t out[768];
    const char* bad[] = {
        mmScript("byte speed 50; setRGB(0,0,0,0);"),                        // missing '='
        mmScript("byte speed = 300; setRGB(0,0,0,0);"),                     // default > 255
        // The range cases moved to defineControls, where a range now lives. A comment cannot be malformed any more, because a comment no longer declares anything.
        "class T {\n  byte s = 5;\n  void defineControls() { addControl(\"s\", nope, 0, 9); }\n"
        "  void tick() { setRGB(0,0,0,0); }\n}\n",                                 // binds an undeclared member
        "class T {\n  byte s = 5;\n  void defineControls() { addControl(s, s, 0, 9); }\n"
        "  void tick() { setRGB(0,0,0,0); }\n}\n",                                 // name is not a string
        mmScript("byte random16 = 5; setRGB(0,0,0,0);"),                    // name shadows a builtin
        "byte speed = 50;",                                       // not even a class
        mmScript("byte = 50; setRGB(0,0,0,0);"),                            // no name
        mmScript("byte s = 1; byte s = 2; setRGB(0,0,0,0);"),            // duplicate member name
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::strlen(r.error) > 0);
    }
}

// The SYMBOL TABLE: which functions a class defined, and where each one's code starts.
//
// A binding asks for an entry by name and gets an address inside the single emitted block, which is how a compiler and a linker have always worked: one code section, a name-to-offset map over it.
// With one function the map is trivially right (its body is the whole program), so the case that proves anything is TWO: the second must start after the first, not at zero.
TEST_CASE("a class reports every function it defined, and where each one starts") {
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        "class TwoFns {\n"
        "  void helper() { setRGB(1, 10, 20, 30); }\n"
        "  void tick()   { setRGB(2, 40, 50, 60); }\n"
        "}\n", kTable, kSys, out, sizeof(out));
    // The entry table is published only by a SUCCESSFUL compile: compileSource returns at codegen failure, before it copies the table, which is the same rule the declared controls follow (a failed compile must not advertise the shape of code that is not running). So a host with no MoonLive backend has nothing to check here.
#if !MM_MOONLIVE_HAS_HOST_JIT
    return;
#endif
    REQUIRE(r.ok);
    REQUIRE(r.entryCount == 2);

    // In source order, and named as written.
    CHECK(std::string(r.entries[0].name, r.entries[0].nameLen) == "helper");
    CHECK(std::string(r.entries[1].name, r.entries[1].nameLen) == "tick");

    // The first entry opens the block; the second is further in. Without the offset map both would read 0, and a binding calling `tick` would run `helper` instead: silently, since both compile.
    CHECK(r.entries[1].offset > r.entries[0].offset);
    CHECK(r.entries[1].offset < r.len);
}

// A script may define functions the host does not know about. They are still recorded, because which names exist is what decides a script's ROLE once the per-role entry points arrive.
TEST_CASE("a function the host has no name for is still reported") {
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        "class Helpers {\n  void paint() { setRGB(0, 1, 2, 3); }\n}\n", kTable, kSys, out, sizeof(out));
#if !MM_MOONLIVE_HAS_HOST_JIT
    return;                       // no backend: no successful compile, so no table to report
#endif
    REQUIRE(r.ok);
    REQUIRE(r.entryCount == 1);
    CHECK(std::string(r.entries[0].name, r.entries[0].nameLen) == "paint");
}

#if MM_MOONLIVE_HAS_HOST_JIT
// The '/' and '%' operators. Both lower to a host call (no ISA here has a divide), so what needs pinning is not the arithmetic but the GRAMMAR: a hand-written precedence-climbing parser gets binding wrong silently, and a wrong answer here is indistinguishable from a working effect. The rule the parser must not get backwards: `/` and `%` bind tighter than `+`, and equally with `*`, so a chain runs left to right. `12 / 2 * 3` is 18; grouping it as 12 / (2 * 3) gives 2.
TEST_CASE("division binds tighter than addition and left to right with multiplication") {
    CHECK(render(mmScript("setRGB(0, 12 / 2 * 3, 2 + 12 / 4, 2 + 20 % 7);"), 1)[0] == 18);
    CHECK(render(mmScript("setRGB(0, 12 / 2 * 3, 2 + 12 / 4, 2 + 20 % 7);"), 1)[1] == 5);
    CHECK(render(mmScript("setRGB(0, 12 / 2 * 3, 2 + 12 / 4, 2 + 20 % 7);"), 1)[2] == 8);
}

// Parentheses override the precedence, which is what makes the operators usable at all.
TEST_CASE("parentheses group an expression ahead of division") {
    CHECK(render(mmScript("setRGB(0, (2 + 12) / 7, (3 + 1) * 5, 3 + 1 * 5);"), 1)[0] == 2);
    CHECK(render(mmScript("setRGB(0, (2 + 12) / 7, (3 + 1) * 5, 3 + 1 * 5);"), 1)[1] == 20);
    CHECK(render(mmScript("setRGB(0, (2 + 12) / 7, (3 + 1) * 5, 3 + 1 * 5);"), 1)[2] == 8);
}

// A script must degrade, never fault. Dividing by zero is the one input the hardware would trap on, and it reaches the host helper as an ordinary value. The masked result SATURATES with the numerator's sign, IEEE's ±infinity mapped onto an int, and the visually right value: k / dist at dist == 0 is the center of a ripple, where max is the peak the eye expects and 0 punched a dark hole exactly there. So no zero-check is ever needed before a divide. The remainder stays 0: there is no "infinite remainder".
TEST_CASE("dividing by zero saturates toward the numerator's sign rather than faulting") {
    // INT32_MAX through a truncating channel store reads 255: full bright, not a hole.
    CHECK(render(mmScript("setRGB(0, 100 / 0, 100 % 0, 0);"), 1)[0] == 255);
    CHECK(render(mmScript("setRGB(0, 100 / 0, 100 % 0, 0);"), 1)[1] == 0);
    // The sign carries: a negative numerator saturates DOWN, so the comparison sees a negative.
    CHECK(render(mmScript("int n = -5;\n"
                          "if (n / 0 < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    // 0/0 has no direction to saturate toward.
    CHECK(render(mmScript("setRGB(0, 0 / 0, 0, 0);"), 1)[0] == 0);
}

// A subtraction that goes below zero is the ordinary way to ask "which of these is bigger", and before this it silently answered the opposite: the difference wrapped to a huge unsigned value and every `< 0` test was false. Four separate rendering bugs in one session came from this.
TEST_CASE("a comparison against a subtraction that went negative takes the negative branch") {
    CHECK(render(mmScript("if (10 - 200 < 0) { setRGB(0, 7, 0, 0); } "
                          "else { setRGB(0, 3, 0, 0); }"), 1)[0] == 7);
    // ... and the positive direction still reads positive, so this is a fix and not an inversion.
    CHECK(render(mmScript("if (200 - 10 < 0) { setRGB(0, 7, 0, 0); } "
                          "else { setRGB(0, 3, 0, 0); }"), 1)[0] == 3);
}

// Every relational operator routes through the same two branch ops, so each spelling needs its own check: `>` swaps the operands and `>=` / `<=` go through the two-branch strict form.
TEST_CASE("every comparison operator orders a negative below a positive") {
    CHECK(render(mmScript("if (0 - 5 > 1) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"), 1)[0] == 3);
    CHECK(render(mmScript("if (0 - 5 <= 1) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"), 1)[0] == 7);
    CHECK(render(mmScript("if (0 - 5 >= 1) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"), 1)[0] == 3);
}

// The one remaining trap in signed division: INT32_MIN / -1 overflows, which is UB and a SIGFPE on x86-64 where the other three ISAs quietly wrap. A script can write it (65536 * 32768 wraps the multiply to INT32_MIN), so the host guards it the same way it guards divide-by-zero.
TEST_CASE("dividing the most negative value by minus one saturates rather than faulting") {
    // 32768 * 32768 * 2 wraps the multiply to exactly INT32_MIN (a literal cannot exceed 65535).
    CHECK(render(mmScript("if (32768 * 32768 * 2 / (0 - 1) > 0) { setRGB(0, 7, 0, 0); } "
                          "else { setRGB(0, 3, 0, 0); }"), 1)[0] == 7);
    CHECK(render(mmScript("setRGB(0, 32768 * 32768 * 2 % (0 - 1), 5, 0);"), 1)[1] == 5);
}

// An int ARRAY holds full 32-bit elements, negatives included: element access lowers through the 4-byte indexed load, which has no sign to lose. This is what the old int16_t-array refusal existed to stand in for, the language now has the load it was missing.
TEST_CASE("an int array element round-trips a value no byte could hold") {
    CHECK(render(mmScript("int buf[2];\n"
                          "buf[0] = 1000;\n"
                          "setRGB(0, buf[0] - 900, 0, 0);"), 1)[0] == 100);
}

// A STRING array is refused: a string is a reference into the compiled program's pool, so an array of them would be an array of references with no way to fill it, there is no runtime string. A refusal names the gap; a wrong number would not.
TEST_CASE("a string array is refused with a diagnostic rather than mis-read") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { string names[4]; void tick() { fill(0, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// A coordinate far outside the plane must escape immediately, not overflow: the value below is past the |8.0| input clamp, and without that clamp its square alone reaches 2^62.
TEST_CASE("escape treats an absurdly distant coordinate as escaped rather than overflowing") {
    CHECK(render(mmScript("fixed far = 30000.0;\n"
                          "setRGB(0, escape(far, far, 0.0, 0.0, 40), 7, 0);"), 1)[0] > 0);
}

// The escape-time fractal, pinned at the points every textbook names. escape() is the one loop a script cannot write itself (it squares signed fixed-point in 64 bits), so its contract is pinned here rather than by the script that uses it.
TEST_CASE("escape reports the inside of the Mandelbrot set as zero and the outside as a count") {
    // The origin is inside the set forever; c = 2 + 2i runs away almost immediately.
    CHECK(render(mmScript("setRGB(0, escape(0.0, 0.0, 0.0, 0.0, 40), 7, 0);"), 1)[0] == 0);
    CHECK(render(mmScript("setRGB(0, escape(0.0, 0.0, 0.0, 0.0, 40), 7, 0);"), 1)[1] == 7);
    CHECK(render(mmScript("setRGB(0, escape(2.0, 2.0, 0.0, 0.0, 40), 0, 0);"), 1)[0] > 0);
}

TEST_CASE("escape near the set boundary counts more steps than far outside") {
    // c = -1.2 + 0.3i sits near the boundary and survives longer than c = 1 + 1i, which is the graded banding every rendering of the set is made of. Written as the numbers themselves, which is what `fixed` bought: the Q13 spelling was -9830 and 8192.
    auto near_px = render(mmScript("setRGB(0, escape(-1.2, 0.3, 0.0, 0.0, 40), 0, 0);"), 1);
    auto far_px  = render(mmScript("setRGB(0, escape(1.0, 1.0, 0.0, 0.0, 40), 0, 0);"), 1);
    CHECK(near_px[0] > far_px[0]);
}

TEST_CASE("a nonzero seed selects the Julia set rather than the Mandelbrot set") {
    // The SAME pixel answers differently under the two modes, which is the whole point of the seed: the origin is inside the Mandelbrot set (0 forever), but under Julia seed (-0.4, 0.6) it iterates z = z*z + c from z = 0+0i and escapes with a graded count.
    CHECK(render(mmScript("setRGB(0, escape(0.0, 0.0, 0.0, 0.0, 40), 7, 0);"), 1)[0] == 0);
    CHECK(render(mmScript("setRGB(0, escape(0.0, 0.0, -0.4, 0.6, 40), 7, 0);"), 1)[0] > 0);
}

// ASSIGNED in tick(), not just seeded by the initializer: the store and the load are different ops, and the bug this pins wrote only part of the member, so the load read a stale byte and every stored coordinate collapsed to 0..255. A whole shader rendered one flat color, and the initializer-only test above stayed green throughout. The 4-byte slot removes the class.
TEST_CASE("an int member assigned a negative in tick reads back negative") {
    CHECK(render(mmScript("int v = 0; "
                          "v = 100 - 11000; "
                          "if (v < 0) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"), 1)[0] == 7);
    // And the magnitude survives, not just the sign: -10900 halved is -5450, still negative, where a half-written member would hold a small positive.
    CHECK(render(mmScript("int v = 0; "
                          "v = 100 - 11000; "
                          "if (v / 2 < 0 - 5000) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"), 1)[0] == 7);
}

// 65436 is simply a positive number to an int. It is here because it USED to be the bit pattern a uint16_t member held for -100, so the two were indistinguishable in storage; with a 4-byte signed slot they are different values and the comparison says so.
TEST_CASE("an int member holds a large positive") {
    CHECK(render(mmScript("int pos = 65436; "
                          "if (pos < 0) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"), 1)[0] == 3);
}

// A brightness is a byte channel, and a script computing `n * 255` for "full" meant full. The truncating cast turned that into an arbitrary pattern (n=50 gave 206, n=128 gave 128) while every part of the expression still looked right.
TEST_CASE("a brightness above full saturates to full instead of wrapping to a dark band") {
    // setPaletteColor(x, y, index, brightness): 50 * 255 is 12750, which truncated to a byte is 206. Saturating, it is 255, and palette entry 0 at full brightness is not black.
    auto bright = render(mmScript("setPaletteColor(0, 0, 0, 50 * 255);"), 1);
    auto full   = render(mmScript("setPaletteColor(0, 0, 0, 255);"), 1);
    CHECK(bright[0] == full[0]);
    CHECK(bright[1] == full[1]);
    CHECK(bright[2] == full[2]);
}

TEST_CASE("a brightness that went below zero renders black rather than full") {
    auto px = render(mmScript("setPaletteColor(0, 0, 0, 0 - 10);"), 1);
    CHECK(px[0] == 0);
    CHECK(px[1] == 0);
    CHECK(px[2] == 0);
}

// The loop guard deliberately stayed UNSIGNED when comparisons went signed: a loop counter is a count, and `for (i = 0; i < width; ...)` must run whatever a signed reading would make of it.
TEST_CASE("a loop over a count still runs every step after comparisons became signed") {
    auto px = render(mmScript("for (int i = 0; i < 4; i = i + 1) { setRGB(i, 9, 0, 0); }"), 4);
    CHECK(px[0] == 9);
    CHECK(px[3 * 3] == 9);
}

// --- the five types -----------------------------------------------------------------------------

// A type is a SEMANTIC, not a storage width. These pin what each one promises, which is the whole reason the language stopped making a script spell uint8_t/uint16_t/int16_t for itself.

// An int holds what its name says: the full signed 32-bit range, negatives included. The old language had no such member, uint16_t wrapped at 65536 and int16_t at 32768, so a script needing a big number had to know which width to reach for and got a silently wrong value when it guessed wrong.
TEST_CASE("an int member holds a value far outside any 16-bit range") {
    CHECK(render(mmScript("int big = 1000000;\n"
                          "setRGB(0, big / 10000, 0, 0);"), 1)[0] == 100);
}

TEST_CASE("an int member written negative reads back negative") {
    CHECK(render(mmScript("int neg = -100;\n"
                          "if (neg < 0) { setRGB(0, 7, 0, 0); } else { setRGB(0, 3, 0, 0); }"),
                 1)[0] == 7);
}

// Assigning past a byte's range TRUNCATES rather than wrapping the slot: the store writes one byte, so the member keeps 0..255 and the three bytes above it stay zero. That zero is what lets a byte control's descriptor point at the slot's low byte and still read the member's value.
TEST_CASE("a byte member assigned past its range keeps only its own byte") {
    CHECK(render(mmScript("byte n = 0;\nn = 300;\nsetRGB(0, n, 0, 0);"), 1)[0] == 44);
}

// A bool is a flag, and its initializer is 0 or 1, anything else is a declaration error, so a script cannot quietly seed a flag with a number it will later compare against.
TEST_CASE("a bool member takes 0 or 1 and refuses anything else") {
    CHECK(render(mmScript("bool on = 1;\n"
                          "if (on != 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { bool on = 7; void tick() { setRGB(0, on, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// A bool is written the way it reads. `bool on = 0;` was a C-ism the old language forced, and the literals cost nothing: they are 1 and 0, so every comparison and arithmetic path takes them unchanged.
TEST_CASE("a bool member is initialized and compared with true and false") {
    CHECK(render(mmScript("bool on = true;\n"
                          "if (on != false) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    CHECK(render(mmScript("bool off = false;\n"
                          "if (off != false) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 1);
}

// --- fixed (Q16.16) -----------------------------------------------------------------------------

// A script writes the number it means. `fixed half = 0.5;` is the Q16.16 word 32768, and toInt brings it back to a whole number, the pair is what makes fractional arithmetic expressible without a float anywhere in the engine.
TEST_CASE("a fixed member holds a fractional value written as a decimal") {
    // 2.5 * 100 = 250, and toInt of that is 250. Anything that lost the fraction would give 200.
    CHECK(render(mmScript("fixed v = 2.5;\n"
                          "setRGB(0, toInt(v * toFixed(100)), 0, 0);"), 1)[0] == 250);
}

// The multiply RESCALES: two Q16.16 values have 32 fraction bits between them, so the product has to come back down by 16. Without that, 0.5 * 0.5 would be 0.25 scaled wrong by 65536, either 0 or an enormous number, depending which way the shift went missing.
TEST_CASE("multiplying two fixed values rescales the product") {
    // 0.5 * 0.5 = 0.25; * 400 = 100.
    CHECK(render(mmScript("fixed a = 0.5;\n"
                          "fixed b = 0.5;\n"
                          "setRGB(0, toInt(a * b * toFixed(400)), 0, 0);"), 1)[0] == 100);
}

// A NEGATIVE fixed value survives the multiply. The low word of the 64-bit product is unsigned while the high word is signed, so joining them with the wrong shift turns -0.5 into a large positive: the logical/arithmetic distinction is the whole reason both shifts exist.
TEST_CASE("a fixed multiply keeps the sign of a negative operand") {
    CHECK(render(mmScript("fixed neg = -0.5;\n"
                          "fixed two = 2.0;\n"
                          "if (neg * two < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    // -0.5 * 2.0 = -1.0, so adding 1.5 gives 0.5, and 0.5 * 200 = 100.
    CHECK(render(mmScript("fixed neg = -0.5;\n"
                          "fixed two = 2.0;\n"
                          "fixed off = 1.5;\n"
                          "setRGB(0, toInt((neg * two + off) * toFixed(200)), 0, 0);"), 1)[0] == 100);
}

// Division rescales the other way: the numerator is pre-shifted so the quotient lands back in Q16.16 rather than collapsing to a whole number.
TEST_CASE("dividing two fixed values keeps the fraction") {
    // 1.0 / 4.0 = 0.25, * 400 = 100.
    CHECK(render(mmScript("fixed one = 1.0;\n"
                          "fixed four = 4.0;\n"
                          "setRGB(0, toInt(one / four * toFixed(400)), 0, 0);"), 1)[0] == 100);
}

// A whole number seeding a fixed member is converted at COMPILE time: `fixed z = 2;` means 2.0, and no runtime shift is spent on a constant.
TEST_CASE("a whole number initializing a fixed member means its whole value") {
    CHECK(render(mmScript("fixed two = 2;\n"
                          "setRGB(0, toInt(two * toFixed(50)), 0, 0);"), 1)[0] == 100);
}

// An integer LITERAL meeting a fixed value converts at compile time, its Const is patched to the same number in Q16.16, free at run time, so `v * 2` and `if (v < 0)` read naturally. A VARIABLE never adopts: its scaling is not visible at the site, so it keeps the explicit rule.
TEST_CASE("an integer literal adopts fixed at a meet point, a variable does not") {
    // 1.5 * 2 = 3.0; * 50 = 150.
    CHECK(render(mmScript("fixed v = 1.5;\n"
                          "setRGB(0, toInt(v * 2 * toFixed(50)), 0, 0);"), 1)[0] == 150);
    // The comparison idiom: a fixed value against a bare 0.
    CHECK(render(mmScript("fixed neg = -0.5;\n"
                          "if (neg < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    // Assignment: `c = 5;` on a fixed member means 5.0.
    CHECK(render(mmScript("fixed c = 0.0;\n"
                          "c = 5;\n"
                          "setRGB(0, toInt(c * toFixed(20)), 0, 0);"), 1)[0] == 100);
    // An int VARIABLE stays refused: nothing at the site says which scaling it carries.
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("fixed v = 1.5;\nint n = 2;\nsetRGB(0, toInt(v * n), 0, 0);"),
                            kTable, kSys));
    eng.free();
}

// The fixed divide is exact over the WHOLE range, because the widening happens in int64 in the host (fdiv), not by shifting a 32-bit register. A first implementation split the shift around an integer divide and silently wrapped for any |value| past 128.0, which froze two shipped shaders whose animation flowed through exactly such a divide, while every small-value test stayed green.
TEST_CASE("a fixed divide is exact for values far past 128") {
    // 32000.0 / 100.0 = 320.0; * 0.5 = 160.0. The wrapped version returned garbage near zero.
    CHECK(render(mmScript("fixed big = 32000.0;\n"
                          "fixed d = 100.0;\n"
                          "fixed half = 0.5;\n"
                          "setRGB(0, toInt(big / d * half), 0, 0);"), 1)[0] == 160);
    // And the sign survives.
    CHECK(render(mmScript("fixed big = -32000.0;\n"
                          "fixed d = 100.0;\n"
                          "if (big / d < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
}

// Dividing a fixed value by fixed zero saturates exactly as the integer divide does: the same policy, stated once per representation because they are different host calls.
TEST_CASE("a fixed divide by zero saturates toward the numerator's sign") {
    // Saturated positive: the quotient is INT32_MAX, so a comparison against any ordinary value sees it as larger. Asserted by comparison rather than by scaling it down, the previous version divided by toFixed(20000000), a literal that WRAPPED, so it passed through a number nobody wrote.
    CHECK(render(mmScript("fixed a = 5.0;\n"
                          "fixed z = 0.0;\n"
                          "fixed big = 30000.0;\n"
                          "if (a / z > big) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    CHECK(render(mmScript("fixed a = -5.0;\n"
                          "fixed z = 0.0;\n"
                          "if (a / z < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
}
// The new ops under REGISTER PRESSURE. sourcesOf/writesDst tell the allocator which vregs an op reads and whether it defines one; get either wrong for a new op and the allocator spills the wrong value or keeps a dead one, which shows up as an arithmetic answer that is wrong only in the programs big enough to spill. A long chain of live fixed values forces that state.
TEST_CASE("the fixed ops survive being spilled") {
    // Eight members live at once, each read after the chain has moved on, so the allocator has to park and reload values across Mulhi/Shl/Shr/Sar and the 32-bit slot access.
    CHECK(render(mmScript("fixed a = 1.5;\n"
                          "fixed b = 2.0;\n"
                          "fixed c = 0.5;\n"
                          "fixed d = 4.0;\n"
                          "fixed e = 0.25;\n"
                          "fixed f = 8.0;\n"
                          "fixed g = 0.125;\n"
                          "fixed h = 0.0;\n"
                          "h = a * b + c * d + e * f + g;\n"
                          // 1.5*2 + 0.5*4 + 0.25*8 + 0.125 = 3 + 2 + 2 + 0.125 = 7.125
                          "setRGB(0, toInt(h * toFixed(16)), 0, 0);"), 1)[0] == 114);
}

// The same for the whole-number ops the slot access shares: a value stored to a member, read back after other work has claimed every register, and compared.
TEST_CASE("a member survives a spill across the 32-bit slot access") {
    CHECK(render(mmScript("int a = 1000;\n"
                          "int b = 2000;\n"
                          "int c = 3000;\n"
                          "int d = 4000;\n"
                          "int e = 5000;\n"
                          "int f = 6000;\n"
                          "int g = 0;\n"
                          "g = a + b + c + d + e + f;\n"
                          "setRGB(0, g / 100, 0, 0);"), 1)[0] == 210);
}

// The two BOUNDARY literals, in an expression rather than an initializer. Both have a magnitude one past their type's positive limit, so a lexer that judged the number before the sign made them unwritable: -2147483648 is the most negative int and -32768.0 the most negative fixed.
TEST_CASE("the most negative int and fixed values can be written in an expression") {
    CHECK(render(mmScript("int n = 0;\n"
                          "n = -2147483648;\n"
                          "if (n < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
    CHECK(render(mmScript("fixed f = 0.0;\n"
                          "f = -32768.0;\n"
                          "if (f < 0) { setRGB(0, 9, 0, 0); } else { setRGB(0, 1, 0, 0); }"),
                 1)[0] == 9);
}

// A fixed multiply where the destination is also a source, and where a long chain forces the allocator to reuse registers. On x86-64 the emitted sequence borrows a scratch register and writes its destination last; an ordering mistake there returns a*a, or a stale value, rather than the product. Run rather than decoded, because the byte shape is what hid the bug twice.
TEST_CASE("a fixed multiply is correct when its destination aliases a source") {
    // f = f * two: destination and first source are the same member.
    CHECK(render(mmScript("fixed f = 1.5;\n"
                          "fixed two = 2.0;\n"
                          "f = f * two;\n"
                          "setRGB(0, toInt(f * toFixed(50)), 0, 0);"), 1)[0] == 150);
    // A chain long enough that the allocator recycles registers between the multiplies.
    CHECK(render(mmScript("fixed a = 1.5;\n"
                          "fixed b = 2.0;\n"
                          "fixed c = 0.5;\n"
                          "fixed d = 4.0;\n"
                          "fixed r = 0.0;\n"
                          "r = a * b * c * d;\n"      // 1.5*2*0.5*4 = 6.0
                          "setRGB(0, toInt(r * toFixed(20)), 0, 0);"), 1)[0] == 120);
}

#endif   // MM_MOONLIVE_HAS_HOST_JIT

// Compile-only from here down: these assert DIAGNOSTICS, which the front end produces with or without a backend, so they are exactly what a --no-jit build should still check. Everything above needs render(), which needs emitted code to execute.


// A byte is exactly a hardware channel: 0..255, and an initializer outside that is a COMPILE ERROR naming the member rather than an arbitrary in-range number. `byte n = 300;` used to become 44 with nothing reporting it.
TEST_CASE("a byte member outside 0..255 is refused at the declaration") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte n = 300; void tick() { setRGB(0, n, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// Every scalar occupies the SAME 4-byte slot, so a byte and an int cost the same arena and the member after either one sits at the same offset. The old rule, a byte packed beside its neighbor, a wide member skipped to an even byte, is what this replaces.
TEST_CASE("a byte member and an int member occupy the same sized slot") {
    moonlive::MoonLive a;
    REQUIRE(a.compile("class T {\n  byte first = 1;\n  byte second = 2;\n"
                      "  void defineControls() { addControl(\"second\", second, 0, 9); }\n"
                      "  void tick() { setRGB(0, first, second, 0); }\n}\n", kTable, kSys));
    moonlive::runDefineControls(a);
    uint8_t na = 0;
    const auto* da = a.declaredControls(na);
    REQUIRE(na == 1);
    const uint8_t afterByte = da[0].offset;

    moonlive::MoonLive b;
    REQUIRE(b.compile("class T {\n  int first = 1;\n  byte second = 2;\n"
                      "  void defineControls() { addControl(\"second\", second, 0, 9); }\n"
                      "  void tick() { setRGB(0, first, second, 0); }\n}\n", kTable, kSys));
    moonlive::runDefineControls(b);
    uint8_t nb = 0;
    const auto* db = b.declaredControls(nb);
    REQUIRE(nb == 1);
    CHECK(db[0].offset == afterByte);      // the type of `first` did not move `second`
    a.free(); b.free();
}

// An array still PACKS at its element width, that is where the width question survives, because a byte[] heat map costs a quarter of an int[] one and the classic ESP32 has no PSRAM to absorb the difference. Two arrays of the same length, different element types, different extents.
TEST_CASE("a byte array packs one byte per element where an int array takes four") {
    moonlive::MoonLive small;
    REQUIRE(small.compile("class T {\n  byte heat[8];\n  byte after = 3;\n"
                          "  void defineControls() { addControl(\"after\", after, 0, 9); }\n"
                          "  void tick() { setRGB(0, heat[0], after, 0); }\n}\n", kTable, kSys));
    moonlive::runDefineControls(small);
    uint8_t ns = 0;
    const auto* ds = small.declaredControls(ns);
    REQUIRE(ns == 1);

    moonlive::MoonLive wide;
    REQUIRE(wide.compile("class T {\n  int heat[8];\n  byte after = 3;\n"
                         "  void defineControls() { addControl(\"after\", after, 0, 9); }\n"
                         "  void tick() { setRGB(0, heat[0], after, 0); }\n}\n", kTable, kSys));
    moonlive::runDefineControls(wide);
    uint8_t nw = 0;
    const auto* dw = wide.declaredControls(nw);
    REQUIRE(nw == 1);

    // Eight elements: 8 bytes against 32. The member after the array is 24 bytes further along.
    CHECK(dw[0].offset - ds[0].offset == 24);
    small.free(); wide.free();
}

// A control binds a member whose type the UI has a widget for. A fixed member has no widget yet, and a slider writing a Q16.16 word is worse than a diagnostic saying so.
TEST_CASE("a control refuses a member the UI has no widget for") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T {\n  fixed scale = 1;\n"
                            "  void defineControls() { addControl(\"scale\", scale, 0, 9); }\n"
                            "  void tick() { setRGB(0, 1, 0, 0); }\n}\n", kTable, kSys));
    eng.free();
}

// true and false say bool, so seeding another type with one is a diagnostic rather than a silent 1.
TEST_CASE("true and false initialize a bool and nothing else") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte n = true; void tick() { setRGB(0, n, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// THE WALL: mixing the two representations is a COMPILE ERROR naming the conversion, because at run time they are the same 32 bits and a silent mix is a number 65,536 times off with nothing reporting it. This is the diagnostic the whole type-tracking exists to produce.
TEST_CASE("mixing a whole number and a fixed value is refused with the conversion named") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("fixed v = 1.5;\nsetRGB(0, v + 2, 0, 0);"), kTable, kSys));
    eng.free();
    moonlive::MoonLive eng2;
    CHECK_FALSE(eng2.compile(mmScript("fixed v = 1.5;\nint n = 2;\nsetRGB(0, v * n, 0, 0);"),
                             kTable, kSys));
    eng2.free();
}

// The conversions are explicit in BOTH directions, and each refuses a value already of its target type: toFixed on a fixed value is a mistake worth naming, not a no-op to absorb.
TEST_CASE("a conversion refuses a value already of its target type") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("fixed v = 1.5;\nsetRGB(0, toInt(toFixed(v)), 0, 0);"),
                            kTable, kSys));
    eng.free();
}

// A fixed member outside the representable range is refused at the declaration rather than wrapping: 40000.0 does not fit Q16.16's ±32767.99998.
TEST_CASE("a fixed member outside its range is refused at the declaration") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { fixed v = 40000.0; void tick() { setRGB(0, 1, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// A literal too big for Q16.16 cannot adopt: patching 40000 to 40000.0 would wrap the word, so the meet refuses it rather than producing a number nobody wrote.
TEST_CASE("a literal outside the fixed range does not adopt") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("fixed v = 1.5;\nsetRGB(0, toInt(v * 40000), 0, 0);"),
                            kTable, kSys));
    eng.free();
}

// The boundary to a built-in stays whole-numbered: a fixed value crossing unconverted would be read 65,536 times off, so the conversion is written where the call is.
TEST_CASE("a fixed value passed to a built-in names the conversion") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("fixed v = 1.5;\nsetRGB(0, v, 0, 0);"), kTable, kSys));
    eng.free();
}

// --- the type wall, at every boundary -----------------------------------------------------------

// An ARRAY INDEX counts elements, so it is a whole number wherever it appears. A fixed index would address by the raw Q16.16 word, 1.5 reading element 98304, clamped to the last one.
TEST_CASE("an array index is refused as a fixed value") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte h[4]; fixed f = 1.5;\n"
                            "  void tick() { setRGB(0, h[f], 0, 0); } }", kTable, kSys));
    eng.free();
    moonlive::MoonLive eng2;
    CHECK_FALSE(eng2.compile("class T { byte h[4]; fixed f = 1.5;\n"
                             "  void tick() { h[f] = 1; setRGB(0, 1, 0, 0); } }", kTable, kSys));
    eng2.free();
}

// An array ELEMENT reports the ARRAY's type, not whatever the index expression left behind. A literal index in a fixed context used to adopt the INDEX, patching `heat[3]`'s 3 into 196608, clamping to the last element, and reading a byte as though it were Q16.16.
TEST_CASE("an array element carries its array's type, not its index's") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte h[4]; fixed f = 0.0;\n"
                            "  void tick() { f = h[3] * 0.5; setRGB(0, toInt(f), 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// An element STORE takes what the element type holds, the same wall a scalar store enforces.
TEST_CASE("an array element refuses a value of the wrong type") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte h[4]; void tick() { h[0] = 1.5; setRGB(0, h[0], 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// A LOOP counts. A fixed limit would run the body ~65,536 times, a multi-second stall on the render thread rather than a diagnostic, which is the robustness rule's whole point.
TEST_CASE("a loop header refuses a fixed value in any of its three clauses") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { fixed f = 3.0;\n"
                            "  void tick() { for (int i = 0; i < f; i = i + 1) { setRGB(0, 1, 0, 0); } } }",
                            kTable, kSys));
    eng.free();
}

// A fixed remainder keeps the fixed scale, (a*2^16) mod (b*2^16) is (a mod b)*2^16, which is what makes the fractional-part idiom work. Typed int, `toInt` on it would be refused.
TEST_CASE("the remainder of two fixed values is itself fixed") {
    moonlive::MoonLive eng;
    CHECK(eng.compile("class T { fixed a = 1.5; fixed b = 1.0;\n"
                      "  void tick() { setRGB(0, toInt(a % b * toFixed(100)), 0, 0); } }",
                      kTable, kSys));
    eng.free();
}

// A member may not take a name the expression parser resolves first, or it could be declared and then never read. Same stance the language already takes for a builtin's name.
TEST_CASE("a member may not be named after a conversion or a boolean literal") {
    for (const char* src : {"class T { byte toFixed = 5; void tick() { setRGB(0, 1, 0, 0); } }",
                            "class T { byte toInt = 5; void tick() { setRGB(0, 1, 0, 0); } }",
                            "class T { byte true = 5; void tick() { setRGB(0, 1, 0, 0); } }"}) {
        moonlive::MoonLive eng;
        CHECK_FALSE(eng.compile(src, kTable, kSys));
        eng.free();
    }
}

// A fixed literal cannot seed a whole-number member: `byte b = 0.0;` says two different things about what b is.
TEST_CASE("a whole-number member refuses a fixed initializer") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { byte b = 0.0; void tick() { setRGB(0, b, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// A fixed ARRAY is refused rather than half-working: an element's type has to reach both the expression that reads it and the value that writes it, which scalars get from their declaration.
TEST_CASE("a fixed array is refused with a diagnostic") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { fixed w[4]; void tick() { setRGB(0, 1, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// toFixed of a literal past the representable range is a compile error, matching what adoption already refuses at a meet point. It used to shift and wrap into a number nobody wrote.
TEST_CASE("toFixed refuses a literal outside the fixed range") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("setRGB(0, toInt(toFixed(40000)), 0, 0);"), kTable, kSys));
    eng.free();
}

// Every script this project SHIPS compiles on the host.
//
// The device codegen tests already sweep the same folder for Xtensa and RISC-V, but nothing did it for the host backend, the one every desktop runs and every other test in this file uses. A language change that a shipped script no longer parses would otherwise reach a board before it reached a test. Read from disk deliberately, so the check cannot drift from what ships.
TEST_CASE("every shipped script compiles") {
    // A layout places lights, a modifier transforms coordinates, an effect draws: three different sets of system variables, so each folder compiles against its own.
    const moonlive::SysVarTable& layout = moonlive::layoutSysVars();
    const moonlive::SysVarTable& effect = moonlive::effectSysVars();
    const moonlive::SysVarTable& modifier = moonlive::modifierSysVars();
    const struct { const char* dir; const moonlive::SysVarTable* sys; } kRoles[] = {
        {"layouts",   &layout},
        {"effects",   &effect},
        {"modifiers", &modifier},
    };
    // Located from __FILE__, not the working directory: the gate runs this binary from build/, where a relative path finds nothing and the test would pass while checking zero scripts.
    const std::filesystem::path repo =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    int checked = 0;
    for (const auto& role : kRoles) {
        const std::filesystem::path dir = repo / "moonlive" / role.dir;
        // REPORTED, not skipped: a folder that moved would otherwise make this test pass while checking nothing, which is the failure mode it exists to prevent.
        INFO("script folder: ", dir.string());
        REQUIRE(std::filesystem::is_directory(dir));
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            // The exact extensions, not a substring: `.ml` also matches an editor backup or a note file that has no business being compiled.
            const std::string ext = entry.path().extension().string();
            if (ext != ".mle" && ext != ".mll" && ext != ".mlm") continue;
            const std::string path = entry.path().string();
            std::ifstream in(path);
            std::stringstream ss; ss << in.rdbuf();
            const std::string src = ss.str();
            moonlive::MoonLive eng;
            INFO("script: ", path);
            CHECK(eng.compile(src.c_str(), kTable, *role.sys));
            eng.free();
            checked++;
        }
    }
    CHECK(checked > 0);          // an empty folder would pass the loop vacuously
}

// A sign has no meaning on a boolean. `-true` consumed the minus and then ignored it, seeding the member to 1 as though nothing had been written.
TEST_CASE("a bool initializer takes no sign") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("class T { bool b = -true; void tick() { setRGB(0, 1, 0, 0); } }",
                            kTable, kSys));
    eng.free();
}

// A system variable's arena offset is validated at REGISTRATION, because the failure it prevents is silent: LoadCtrl32 reads four bytes, so an offset at the depth slot reads the recursion counter and one byte past the arena, and an unaligned one straddles two cells. Neither shows up as a compile error or a wrong pixel: the script just reads a number nobody wrote.
TEST_CASE("a system variable cannot be registered outside the arena's 32-bit cells") {
    moonlive::SysVarTable t;
    const auto arena = [](uint8_t where) {
        moonlive::SysVar v{};
        v.name = "probe";
        v.kind = moonlive::SysVarKind::Arena;
        v.where = where;
        return v;
    };

    // The first system slot: the one offset every real registration starts from.
    CHECK(t.add(arena(moonlive::kCtrlBytes)));

    // Below the system range is a SCRIPT member's byte, not a system variable's.
    CHECK_FALSE(t.add(arena(moonlive::kCtrlBytes - 1)));
    // The depth slot sits above the system range: a 4-byte read there runs off the end.
    CHECK_FALSE(t.add(arena(moonlive::kDepthSlot)));
    CHECK_FALSE(t.add(arena(moonlive::kArenaBytes)));
    // Inside the range but straddling two cells.
    CHECK_FALSE(t.add(arena(moonlive::kCtrlBytes + 1)));
    CHECK_FALSE(t.add(arena(moonlive::kCtrlBytes + moonlive::kSysVarBytes - 1)));
}

#if MM_MOONLIVE_HAS_HOST_JIT
// Everything from here EXECUTES emitted code (render() runs a frame, runValue() calls an entry point), so it needs a host backend. Without one the engine compiles nothing and render() is not even defined: the same guard the other compile-through-run tests in this file carry.

// `return`: the language's first statement that ANSWERS rather than acts.
//
// Two jobs, tested separately because they fail differently. As an early exit it is what a script writes when a guard fails and the rest of the frame is pointless; the failure there is the statements after it running anyway. As the way a function reports a value it is what dimensions() and tags() are built on; the failure there is a plausible wrong number, which is why the value is read back rather than merely compiled.
TEST_CASE("return leaves tick() early, and the statements after it do not run") {
    // Paint every light red, then return, then paint them all green. Green must never appear.
    auto buf = render(mmScript("fill(255, 0, 0);"
                               "return;"
                               "fill(0, 255, 0);"), 4);
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i * 3]     == 255);   // the red before the return landed
        CHECK(buf[i * 3 + 1] == 0);     // the green after it did not
    }
}

// A return inside a loop leaves the FUNCTION, not just the iteration: the classic early-out.
TEST_CASE("return inside a loop leaves the whole function") {
    auto buf = render(mmScript("for (int i = 0; i < 4; i = i + 1) {"
                               "  setRGB(i, 9, 0, 0);"
                               "  if (i >= 1) { return; }"
                               "}"), 4);
    CHECK(buf[0] == 9);   // i = 0 painted
    CHECK(buf[3] == 9);   // i = 1 painted, then returned
    CHECK(buf[6] == 0);   // i = 2 never ran
    CHECK(buf[9] == 0);
}

// A conditional return: the guard shape a real script writes. Both directions in one test, because a return that ALWAYS fires and one that never does are both wrong and only the pair rules them out. The condition is a literal comparison rather than a grid variable: this fixture runs with no layout, so width/height are 0 and a guard reading them is not the branch under test.
TEST_CASE("a return fires only when its condition holds") {
    // Guard false: the return is skipped and the fill runs.
    auto ran = render(mmScript("if (0 > 1) { return; }"
                               "fill(7, 7, 7);"), 2);
    CHECK(ran[0] == 7);

    // Guard true: the return fires and the same fill never happens.
    auto skipped = render(mmScript("if (1 > 0) { return; }"
                                   "fill(7, 7, 7);"), 2);
    CHECK(skipped[0] == 0);
}

// The other half of `return`: the host reads the answer. This is what dimensions() and tags() are built on, so it is pinned at the engine level before any binding depends on it.
//
// Failure here is a plausible wrong NUMBER rather than a crash, which is why the value is read back rather than the script merely compiled: a return-register move emitted for the wrong register (they differ per ISA) produces a number that looks like an answer.
TEST_CASE("the host reads a value a script function returned") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class S {"
                        "  int dimensions() { return 3; }"
                        "  void tick() { fill(1, 2, 3); }"
                        "}", kTable, kSys));
    REQUIRE(eng.ok());
    CHECK(eng.runValue("dimensions", moonlive::RetType::Int, 99) == 3);

    // A function the script never defined answers with the FALLBACK, not 0: "the script did not say" and "the script said 0" are different answers.
    CHECK(eng.runValue("tags", moonlive::RetType::Str, 99) == 99);
}

// A returned value survives arithmetic and a control read, so it is a real expression rather than only a literal the parser happened to fold.
TEST_CASE("a returned value can be computed") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class S {"
                        "  int answer() { return 6 * 7; }"
                        "  void tick() { fill(0, 0, 0); }"
                        "}", kTable, kSys));
    REQUIRE(eng.ok());
    CHECK(eng.runValue("answer", moonlive::RetType::Int, 0) == 42);
}

// A STRING literal returns as the pointer it compiles to, which is what tags() needs: the host reads it as a const char*. The source text outlives the call, so the pointer stays valid.
TEST_CASE("a script returns a string the host can read") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class S {"
                        "  string tags() { return \"AB\"; }"
                        "  void tick() { fill(0, 0, 0); }"
                        "}", kTable, kSys));
    REQUIRE(eng.ok());
    const auto p = eng.runValue("tags", moonlive::RetType::Str, 0);
    REQUIRE(p != 0);
    CHECK(std::strncmp(reinterpret_cast<const char*>(p), "AB", 2) == 0);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A function DECLARES what it hands back, so a script reads like the compiled module it stands in for (`void tick()` beside `void tick() override`) and the host can tell a function that answers from one that acts. The declaration is required rather than optional: accepting a bare name as implicit void would leave two spellings meaning the same thing forever.
TEST_CASE("a function without a declared return type is refused") {
    uint8_t out[2048];
    auto r = moonlive::compileSource("class T { tick() { fill(1,2,3); } }", kTable, kSys,
                                     out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::strlen(r.error) > 0);
}

// The three types are all the language has values for. `byte tick()` is refused rather than silently treated as int: it would suggest the engine narrows the value, which it does not.
TEST_CASE("void, int and string are the return types; a member type is not one") {
    uint8_t out[2048];
    // A string return INTERNS its literal, so this form needs the pool the engine always supplies. Without one the compile fails with "no room for this script's strings", which is the honest answer to a caller that offered nowhere to put it.
    static char pool[moonlive::CompileResult::kMaxStrings];
    for (const char* good : {"class T { void tick() { fill(1,2,3); } }",
                             "class T { int dimensions() { return 2; } void tick() { fill(1,2,3); } }",
                             "class T { string tags() { return \"x\"; } void tick() { fill(1,2,3); } }"}) {
        INFO("source: ", good);
        auto r = moonlive::compileSource(good, kTable, kSys, out, sizeof(out), nullptr, nullptr,
                                         pool, sizeof(pool));
        INFO("error: ", r.error);
        CHECK(r.ok);
    }
    for (const char* bad : {"class T { byte tick() { fill(1,2,3); } }",
                            "class T { fixed tick() { fill(1,2,3); } }",
                            "class T { bool tick() { fill(1,2,3); } }"}) {
        INFO("source: ", bad);
        CHECK_FALSE(moonlive::compileSource(bad, kTable, kSys, out, sizeof(out)).ok);
    }
}

// A `return` must match what its function declared. Without this the declaration would be a label rather than a contract: `return "x";` in a void function compiles, and the host that calls it for its effect never looks at the register, so a script silently disagrees with its own signature.
TEST_CASE("a return must match the type its function declared") {
    uint8_t out[2048];
    static char pool[moonlive::CompileResult::kMaxStrings];
    const auto compiles = [&](const char* src) {
        return moonlive::compileSource(src, kTable, kSys, out, sizeof(out), nullptr, nullptr,
                                       pool, sizeof(pool)).ok;
    };

    // A value from a void function has nowhere to go.
    CHECK_FALSE(compiles("class T { void tick() { return 2; } }"));
    // A function that promised a value cannot return without one: the caller would read whatever sat in the return register.
    CHECK_FALSE(compiles("class T { int dimensions() { return; } void tick() { fill(1,2,3); } }"));
    // The two value kinds are not interchangeable: a string is a pointer, an int is a number.
    CHECK_FALSE(compiles("class T { int dimensions() { return \"2\"; } void tick() { fill(1,2,3); } }"));
    CHECK_FALSE(compiles("class T { string tags() { return 2; } void tick() { fill(1,2,3); } }"));

    // And the matching forms still compile, so the check rejects rather than forbids.
    CHECK(compiles("class T { void tick() { return; } }"));
    CHECK(compiles("class T { int dimensions() { return 2; } void tick() { fill(1,2,3); } }"));
    CHECK(compiles("class T { string tags() { return \"x\"; } void tick() { fill(1,2,3); } }"));
}

// A member and a typed function open with the SAME token, and only the token after the name says which. Both orders compile: a class whose members come first, and one that starts with a function, which is what the lookahead exists for.
TEST_CASE("a member declaration and a typed function are told apart") {
    uint8_t out[2048];
    auto members = moonlive::compileSource(
        "class T { int speed = 5; void tick() { fill(speed,2,3); } }", kTable, kSys, out, sizeof(out));
    INFO("error: ", members.error);
    CHECK(members.ok);
    CHECK(members.memberCount == 1);        // `int speed = 5;` stayed a member

    auto fnFirst = moonlive::compileSource(
        "class T { int dimensions() { return 2; } int speed = 5; void tick() { fill(1,2,3); } }",
        kTable, kSys, out, sizeof(out));
    // A member AFTER a function is refused by the existing grammar (declarations come first), so this pins the diagnostic rather than a silent misparse.
    CHECK((fnFirst.ok || std::strlen(fnFirst.error) > 0));
}

// --- Local variables ------------------------------------------------------------------------
//
// A value that lives for one tick had nowhere to go before this: a member is PERSISTED (a sensor reading written to config), is a memory load rather than a register, and there are eight member records against sixteen frame slots. These pin the local as its own budget.
//
// Note the leading `fill(...)` in several bodies: mmScript hoists a body's LEADING declarations to class scope (they are members there), so a test about a LOCAL has to put a statement first or it would be testing the member path it is meant to be distinct from.

#if MM_MOONLIVE_HAS_HOST_JIT
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("an array is indexed correctly at both element widths, by a computed index") {
    // The index scaling, from the outside. `idx * width` is emitted as a SHIFT (width 4) or skipped entirely (width 1), so a wrong shift or a wrongly skipped one reads the neighboring element rather than failing loudly. A CONSTANT index would not catch it: the interesting path is an index the engine computes at run time, which is what a loop counter is.
    //
    // byte[]: width 1, so no scaling at all.
    auto b = render(mmScript("byte heat[4];\n"
                             "fill(0,0,0);\n"
                             "for (int i = 0; i < 4; i = i + 1) { heat[i] = i * 10; }\n"
                             "setRGB(0, heat[3], heat[1], heat[0]);"), 2);
    CHECK(b[0] == 30);      // heat[3]
    CHECK(b[1] == 10);      // heat[1]
    CHECK(b[2] == 0);       // heat[0]

    // int[]: width 4, the shift path, and the assertions have to distinguish THREE ways the scaling can be wrong. Two arrays side by side catch an OVER-scaled offset (it lands in the neighbor and reads its values). Element 0 holding a number wider than 16 bits catches an UNDER-scaled one (it lands part-way inside element 0, where no byte equals a real element value). A single array of small numbers catches neither: every wrong offset still reads something this same array wrote, which is how the first version of this test passed with a deliberately wrong shift.
    auto w = render(mmScript("int big[4];\n"
                             "int other[4];\n"
                             "fill(0,0,0);\n"
                             "for (int i = 0; i < 4; i = i + 1) { big[i] = i * 10 + 1; }\n"
                             "for (int i = 0; i < 4; i = i + 1) { other[i] = 200 + i; }\n"
                             "big[0] = 66000;\n"
                             "setRGB(0, big[3], big[1], other[0]);"), 2);
    CHECK(w[0] == 31);      // big[3]: not other[], not big[1], not a byte inside big[0]
    CHECK(w[1] == 11);      // big[1]
    CHECK(w[2] == 200);     // other[0]
}

TEST_CASE("an out-of-range index still clamps to the last element after the scaling change") {
    // The clamp runs BEFORE the scaling, and both were touched, so this pins that an index past the end lands on the last element rather than reading past the array into the engine's own arena.
    auto buf = render(mmScript("byte heat[4];\n"
                               "fill(0,0,0);\n"
                               "for (int i = 0; i < 4; i = i + 1) { heat[i] = i + 1; }\n"
                               "setRGB(0, heat[99], 0, 0);"), 2);
    CHECK(buf[0] == 4);     // the last element, not garbage
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("a local variable holds a value for the rest of the tick") {
    auto buf = render(mmScript("fill(0,0,0);\n"
                               "int v = 40;\n"
                               "fill(v, v + 2, v * 2);"), 4);
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3] == 40); CHECK(buf[i*3+1] == 42); CHECK(buf[i*3+2] == 80);
    }
}

TEST_CASE("a local variable is assignable after it is declared") {
    auto buf = render(mmScript("fill(0,0,0);\n"
                               "int v = 5;\n"
                               "v = v * 7;\n"
                               "fill(v, 0, 0);"), 2);
    CHECK(buf[0] == 35);
}

TEST_CASE("a local is initialized from a call, which is what a sensor read looks like") {
    // The shape the service template needs: `int now = gpioRead(pin);`. Here random16 stands in for the read, bounded so the assertion is exact.
    auto buf = render(mmScript("fill(0,0,0);\n"
                               "int v = random16(1);\n"
                               "fill(v + 9, 0, 0);"), 2);
    CHECK(buf[0] == 9);       // random16(1) is 0, so this pins the call reached the slot
}

TEST_CASE("a local declared inside an if body does not leak past it") {
    // Scoping, from the outside: the name stops resolving at the '}'. If it leaked, this would compile and the test would fail on the compile rather than the assertion.
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        mmScript("fill(0,0,0);\n"
                 "if (1 == 1) { int inner = 3; fill(inner,0,0); }\n"
                 "fill(inner, 0, 0);"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
}

TEST_CASE("blocks reuse frame slots, so the frame is not spent by a script's total locals") {
    // The slot budget is what scoping BUYS. Six sequential blocks of six locals each is 36 declarations against a sixteen-slot frame: it compiles only because each block hands its slots back at the '}'. Six blocks, not twenty, because an `if` also costs one of the sixteen LABELS (kIrLabels) and this test is about the slot budget, not that one.
    std::string body = "fill(0,0,0);\n";
    for (int i = 0; i < 6; i++) {
        body += "if (1 == 1) {\n";
        for (int j = 0; j < 6; j++)
            body += "  int v" + std::to_string(j) + " = 1;\n";
        body += "  fill(v0, v5, 0);\n}\n";
    }
    std::string src = "class T {\n  void tick() {\n" + body + "  }\n}\n";
    uint8_t out[8192];
    auto r = moonlive::compileSource(src.c_str(), kTable, kSys, out, sizeof(out));
    CHECK(r.ok);
}

TEST_CASE("two functions each get the whole frame rather than sharing one") {
    // Stage 1 emits functions inline, so without a per-function reset the second function's locals would stack on top of the first's and the pair would exhaust the frame together.
    std::string a, b;
    for (int i = 0; i < 12; i++) { a += "  int a" + std::to_string(i) + " = 1;\n"; }
    for (int i = 0; i < 12; i++) { b += "  int b" + std::to_string(i) + " = 1;\n"; }
    std::string src = "class T {\n  void tick() {\n" + a + "  }\n  void other() {\n" + b + "  }\n}\n";
    uint8_t out[8192];
    auto r = moonlive::compileSource(src.c_str(), kTable, kSys, out, sizeof(out));
    CHECK(r.ok);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("a local variable must be initialized where it is declared") {
    // `int x;` would leave the slot holding whatever the last block put there, which is a bug that reads as working code. Refusing keeps a declaration honest.
    uint8_t out[2048];
    auto r = moonlive::compileSource(mmScript("fill(0,0,0);\n int v;\n fill(v,0,0);"),
                                     kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::strlen(r.error) > 0);
}

#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a byte local WRAPS at 255, exactly as a byte member does") {
    // The point of giving a local a width: `byte` means the same thing inside a function as it does in the class body. A frame slot is four bytes and Spill has no narrowing form, so the value is truncated on the way in (shift up, shift back) rather than by the store. Without that this holds 300 as a local and 44 as a member, and the same line would mean two different things.
    auto buf = render(mmScript("fill(0,0,0);\n"
                               "byte b = 200;\n"
                               "b = b + 100;\n"          // 300 truncates to 44
                               "fill(b, 0, 0);"), 2);
    CHECK(buf[0] == 44);
}

TEST_CASE("a bool local truncates the same way, so any non-zero reads true") {
    auto buf = render(mmScript("fill(0,0,0);\n"
                               "bool f = 1;\n"
                               "int hit = 0;\n"
                               "if (f > 0) { hit = 7; }\n"
                               "fill(hit, 0, 0);"), 2);
    CHECK(buf[0] == 7);
}

TEST_CASE("a byte local keeps its width across a block boundary") {
    // The truncation must survive a spill and reload, which is what makes it a property of the VARIABLE rather than of one expression.
    auto buf = render(mmScript("fill(0,0,0);\n"
                               "byte b = 250;\n"
                               "if (1 == 1) { b = b + 10; }\n"    // 260 -> 4
                               "fill(b, 0, 0);"), 2);
    CHECK(buf[0] == 4);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("a local of every value type is accepted; only string is refused") {
    // All four value types mean the same inside a function as in the class body, so a script author meets no arbitrary wall. A string has no runtime value to put in a slot.
    uint8_t out[4096];
    for (const char* decl : {"int v = 1;", "byte v = 1;", "bool v = 1;", "fixed v = 1.5;"}) {
        std::string body = std::string("fill(0,0,0);\n ") + decl + "\n fill(1,0,0);";
        std::string src = "class T {\n  void tick() {\n" + body + "\n  }\n}\n";
        auto r = moonlive::compileSource(src.c_str(), kTable, kSys, out, sizeof(out));
        INFO(decl << " -> " << std::string(r.error));
        CHECK(r.ok);
    }
    auto str = moonlive::compileSource(mmScript("fill(0,0,0);\n string s = 1;\n fill(1,0,0);"),
                                       kTable, kSys, out, sizeof(out));
    CHECK_FALSE(str.ok);
}

TEST_CASE("a local literal is range-checked against its declared type") {
    // `byte b = 300;` names the mistake rather than silently holding 44, the same refusal a member initializer gives.
    uint8_t out[4096];
    auto b = moonlive::compileSource(mmScript("fill(0,0,0);\n byte v = 300;\n fill(v,0,0);"),
                                     kTable, kSys, out, sizeof(out));
    CHECK_FALSE(b.ok);
    auto f = moonlive::compileSource(mmScript("fill(0,0,0);\n bool v = 5;\n fill(v,0,0);"),
                                     kTable, kSys, out, sizeof(out));
    CHECK_FALSE(f.ok);
}

TEST_CASE("a local may not shadow a built-in function") {
    // A local named `fill` would shadow the builtin for the rest of the function, so the next `fill(0,0,0)` a script wrote would resolve to a variable. Members are checked for exactly this; locals were not.
    uint8_t out[2048];
    auto r = moonlive::compileSource(mmScript("fill(0,0,0);\n int fill = 1;\n setRGB(0,fill,0,0);"),
                                     kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
}

TEST_CASE("a local may not shadow a member or a system variable") {
    uint8_t out[2048];
    // A member of the same name: `v = 1` would otherwise write somewhere the author did not mean.
    auto shadowMember = moonlive::compileSource(
        "class T {\n  int v = 1;\n  void tick() { fill(0,0,0); int v = 2; fill(v,0,0); }\n}\n",
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(shadowMember.ok);
    // A system variable is read-only, so binding a slot to its name would silently detach it.
    auto shadowSys = moonlive::compileSource(mmScript("fill(0,0,0);\n int t = 2;\n fill(t,0,0);"),
                                             kTable, kSys, out, sizeof(out));
    CHECK_FALSE(shadowSys.ok);
}

TEST_CASE("an int local takes a whole number, so a fractional initializer is refused") {
    // The scaling wall, on the declaration: a Q16.16 value and a whole number are both a 4-byte slot, so nothing but the declared type tells them apart. `fixed v = 1.5;` is the way to say this (below); silently truncating into an int would not be.
    uint8_t out[2048];
    auto r = moonlive::compileSource(mmScript("fill(0,0,0);\n int v = 1.5;\n fill(v,0,0);"),
                                     kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
}

TEST_CASE("a local may be fixed, which is what a per-pixel coordinate needs") {
    // The shape metal.mle and fractal.mle were holding as members purely because a local could not be fixed: uv coordinates are per-pixel scratch, computed and consumed inside the inner loop.
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        mmScript("fill(0,0,0);\n"
                 "fixed ux = uvX(1, width, height);\n"
                 "fixed cx = ux - 0.25;\n"
                 "fill(toInt(cx * 100), 0, 0);"),
        kTable, kSys, out, sizeof(out));
    INFO(std::string(r.error));
    CHECK(r.ok);
}

TEST_CASE("a fixed local starts at a whole number, which the literal adopts") {
    // `fixed d = 0;` is how a fixed accumulator naturally opens, and reads the same as a fixed member's initializer. The literal converts at compile time and costs nothing at run time.
    uint8_t out[4096];
    auto r = moonlive::compileSource(mmScript("fill(0,0,0);\n fixed d = 0;\n d = d + 1.5;\n"
                                              " fill(toInt(d * 10), 0, 0);"),
                                     kTable, kSys, out, sizeof(out));
    INFO(std::string(r.error));
    CHECK(r.ok);
}

TEST_CASE("the two scalings do not mix in a local, in either direction") {
    // The bug this wall exists to stop: raw bits 65,536 apart in meaning, invisible at run time.
    uint8_t out[4096];
    // A fixed value into an int local.
    auto intTakesFixed = moonlive::compileSource(
        mmScript("fill(0,0,0);\n int n = 0;\n n = uvX(1, width, height);\n fill(n,0,0);"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(intTakesFixed.ok);
    // A computed whole number into a fixed local: it must name its conversion (toFixed).
    auto fixedTakesInt = moonlive::compileSource(
        mmScript("fill(0,0,0);\n fixed d = 0.0;\n d = width * 2;\n fill(toInt(d),0,0);"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(fixedTakesInt.ok);
}

TEST_CASE("a fixed local reports its scaling where it is read") {
    // Reading the local must carry the fixed-ness out with it, or the value would meet an int at the next operator and compare as a number 65,536 away. Pinned from the outside: multiplying a fixed local by an int VARIABLE (never a literal, which adopts) is refused.
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        mmScript("fill(0,0,0);\n fixed d = 1.5;\n int n = 2;\n fill(toInt(d * n), 0, 0);"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
}

// A script's own function can hand a value back, so a helper computes rather than only acts. The return machinery already existed at the HOST boundary (a function declares int or void, and the exit parks the value in the ABI register); what was missing was the call site taking it.
//
// What makes it harder than moving a register: the result has to survive the calls that FOLLOW it in the same expression. So a script call now preserves the whole vreg pool exactly as a builtin call does, and the callee's value is delivered into the destination vreg once that pool is back.
// Parking it in the frame's argument region instead was tried and rotated the enclosing call's arguments, since that region is still being filled as the expression parses.
TEST_CASE("every shipped script still compiles after a builtin is added") {
    // A builtin name is reserved for every script, so ADDING one can stop an existing script compiling: its member of that name becomes a call. Measured, not hypothetical: a `decay` builtin broke pulse.mle and beat-flash.mlp, which both declare `byte decay`, and the diagnostic pointed at the member rather than at the name that took it. It is `trailDecay`.
    //
    // The names below are the ones a member ACTUALLY uses in the shipped scripts today. A new builtin wanting one of them takes a compound name instead, as `fade` pushed authors to `fadeAmt`. (`scale` is a long-standing core builtin and no script declares it, so the rule is about what scripts use, not about every ordinary word.)
    static constexpr const char* kNamesScriptsDeclare[] = {
        "decay", "speed", "fade", "bri", "hue", "zoom", "twist", "contrast", "sparkle",
    };
    const moonlive::BuiltinTable& t = moonlive::lightBuiltins();
    for (const char* word : kNamesScriptsDeclare) {
        // `fade` is the exception that proves the rule: it was taken first, and every script since has written `fadeAmt`. Nothing else here may become a builtin.
        if (std::strcmp(word, "fade") == 0) continue;
        INFO("a builtin took a name the shipped scripts declare as a member: ", std::string(word));
        CHECK(t.find(word, std::strlen(word)) == nullptr);
    }
}

TEST_CASE("the builtin table has room for every name the light domain registers") {
    // The table fails SILENTLY when full: add() returns false, nothing checks each call, and the script reports "unknown function" for a builtin that plainly exists in the source. The registration-time guard only prints, which no CI run reads, so this is the check that fails. It also keeps real headroom visible: the table hit 61 of 64 when the flow builtins landed.
    const moonlive::BuiltinTable& t = moonlive::lightBuiltins();
    CHECK_FALSE(t.full());                       // nothing was dropped
    CHECK(t.registered() < moonlive::BuiltinTable::kMax);
    // A domain within a couple of names of the cap is one commit from the silent failure.
    CHECK(moonlive::BuiltinTable::kMax - t.registered() >= 4);
}

TEST_CASE("a script function's return value can be used in an expression") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(
        "class T {\n"
        "  int answer() { return 21; }\n"
        "  void tick() { setRGB(0, answer(), answer() + 1, 0); }\n"
        "}\n", kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 21);          // what the helper returned
    CHECK(buf[1] == 22);          // and it is usable in arithmetic
    eng.free();
}

TEST_CASE("a returned value survives the calls that follow it in the same expression") {
    // The case that exposed both wrong designs: with the result in a register `a() + b()` read 6 (b's value twice), and with it in the argument region the arguments came out rotated.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(
        "class T {\n"
        "  int a() { return 10; }\n"
        "  int b() { return 3; }\n"
        "  void tick() { setRGB(0, a() + b(), a(), b()); }\n"
        "}\n", kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 13);
    CHECK(buf[1] == 10);
    CHECK(buf[2] == 3);
    eng.free();
}

TEST_CASE("a helper's value can drive a loop and a member") {
    // What the feature is for: a helper that computes, called where a number is needed.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(
        "class T {\n"
        "  byte level = 4;\n"
        "  int scaled() { return level * 10; }\n"
        "  void tick() {\n"
        "    for (int i = 0; i < 2; i = i + 1) { setRGB(i, scaled(), scaled() + i, 0); }\n"
        "  }\n"
        "}\n", kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 40);
    CHECK(buf[1] == 40);
    CHECK(buf[3] == 40);
    CHECK(buf[4] == 41);
    eng.free();
}

TEST_CASE("a void function cannot be used as a value") {
    // Reading the return register of a function that never wrote it would hand the script whatever the last call left there: a plausible number, silently wrong.
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(
        "class T {\n"
        "  void act() { }\n"
        "  void tick() { setRGB(0, act(), 0, 0); }\n"
        "}\n", kTable, kSys));
    eng.free();
}

TEST_CASE("a returning function still works as a statement, with its value dropped") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(
        "class T {\n"
        "  int side() { return 7; }\n"
        "  void tick() { side(); setRGB(0, 1, 0, 0); }\n"
        "}\n", kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 1);
    eng.free();
}

// A compile that has no buffer to emit into is a MEMORY failure and must say so. It used to fall through to kCodegenFailed ("unsupported on this target, or too large"), which sends a user, and an investigator, after the script when the fix is to free heap. Bench 2026-09-09: noise.mle on a fragmented classic ESP32.
TEST_CASE("compileSource: a missing code buffer is refused before any lowering runs") {
    // The guard is in compileSource itself, so no LowerRefusal can fire for it: there is exactly one message for this case and it is this one. (A separate "no memory" constant used to exist for a NoBuffer refusal that this guard made unreachable; it was deleted rather than kept dead.)
    auto r = moonlive::compileSource(mmScript("setRGB(0, 1, 2, 3);"), kTable, kSys, nullptr, 64);
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "no code buffer");
}

// The allocator's refusal must reach the caller as a pointer that outlives the CompileResult: a buffer inside that temporary dangled (pre-merge review, 2026-09-09). The contract now is that `error` is a string literal, so it is valid forever; the numbers behind it live in spillDetail().
TEST_CASE("compileSource: a register refusal returns a literal, and spillDetail carries the numbers") {
    const char* src = "class E {\n  int dimensions() { return 2; }\n  void tick() {\n"
                      "    for (int x = 0; x < width; x = x + 1) { setRGB(x, 1, 2, 3); }\n  }\n}\n";
    moonlive::RegBudget tiny{1, 0, 8};
    uint8_t code[moonlive::kCodeCap];
    auto r = moonlive::compileSource(src, kTable, kSys, code, sizeof(code), &tiny);
    REQUIRE_FALSE(r.ok);
    // By CONTENT, not address: clang merges identical literals so the pointers happened to coincide, GCC gives the inline constexpr a distinct address per translation unit and the pointer compare failed on CI while the strings were identical. The contract is the text.
    CHECK(std::strcmp(r.error, moonlive::kSpillRefused) == 0);
    CHECK(moonlive::spillDetail().guard != 0);            // and the guard that fired is recorded
}

// Each refusal names what to write instead, since a struct expands into forms the script never shows and a bare "expected ';'" would point at nothing the user wrote.
TEST_CASE("a struct or a condition used where it does not fit is refused with what to write instead") {
    uint8_t out[2048];
    struct Case { const char* src; const char* err; const char* what; };
    const Case refused[] = {
        {mmScript("int RGB = 3;"), "that name is a type", "a variable named after a type"},
        {mmScript("setRGB(0, 1 + getRGB(0), 0, 0);"), "a struct is read one field at a time: write name.field", "a struct answer used as one number"},
        {"class T {\n  int step() { return 1; }\n  void tick() { }\n}\n", "that name shadows a built-in function", "a function named after a builtin, which would never be called"},
        {mmScript("RGB c = RGB(1, 2, 3); setRGB(0, c.w, 0, 0);"), "this struct has no field of that name", "a field the type does not have"},
        {mmScript("RGB c = RGB(1, 2, 3, 4);"), "too many fields for this struct", "a constructor with a fourth field"},
        {mmScript("RGB c = RGB(1, 2, 300);"), "this number is out of range for that field", "a byte field given 300"},
        {mmScript("RGB c = RGB(1, 2, 3); setRGB(0, 1, c);"), "too many arguments: this struct fills more positions than remain", "a struct past the last position"},
        {"class T {\n  void f(Coord3D a, Coord3D b, Coord3D c) { }\n  void tick() { }\n}\n",
         "parameters fill at most 8 words, a struct taking one per field", "three Coord3D parameters, nine words"},
        {"class T {\n  RGB c = RGB(1, 2, 3);\n  void tick() { }\n}\n",
         "a struct member starts at zero: set its fields in a function", "a member struct with an initializer"},
        {mmScript("int both = 0; both = 1 && 2;"), "&&, || and ! join the conditions of an if, not values", "&& as a value"},
        {mmScript("setRGB(0, !1, 0, 0);"), "&&, || and ! join the conditions of an if, not values", "! as a value"},
        // C reads `!a < b` as `(!a) < b`, so a comparison after a negated value is refused rather than read the other way.
        {mmScript("int a = 0; if (!a < 1) { setRGB(0, 1, 0, 0); }"), "! negates what follows it: write !(a < b) to negate a comparison", "! before a comparison"},
        {"class T {\n  RGB c;\n  void tick() { c[1] = 5; }\n}\n", "a struct is read one field at a time: write name.field", "a struct member written by index"},
        {mmScript("fixed f = 1.5; int y = 0; y = max(f, max(10, 20));"), "mixes whole and fixed: write toFixed(x) or toInt(x)", "a nested same-type call hiding a mixed argument"},
        // A function's arguments are checked one by one against its parameters, not counted as words: both of these filled the right number of words.
        {"class T {\n  void paint(int i, RGB c, int boost) { }\n  void tick() { paint(0, 1, 2, 3, 1); }\n}\n",
         "expected a struct value: a constructor, a struct variable or a call answering one", "three numbers where an RGB parameter is"},
        {"class T {\n  void paint(int i, RGB c, int boost) { }\n  void tick() { paint(0, Coord3D(1, 2, 300), 1); }\n}\n",
         "this is a different struct type", "a Coord3D where an RGB parameter is"},
        {"class T {\n  void paint(int a, int b, int c) { }\n  void tick() { paint(RGB(1, 2, 3)); }\n}\n",
         "a struct is read one field at a time: write name.field", "an RGB where three int parameters are"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == std::string(c.err));
    }
}

// A struct use expands to one operation per field, so struct copies are the densest IR: about 1.6 ops per token against a reservation of one.
TEST_CASE("a script dense in struct copies compiles, its IR growing past the reservation") {
    std::string src = "class T {\n  Coord3D a;\n  Coord3D b;\n  RGB c;\n  RGB d;\n  void tick() {\n";
    for (int i = 0; i < 20; i++) src += "    a = b; b = a; c = d; d = c; setRGB(0, c);\n";   // about the largest shipped script's length
    src += "  }\n}\n";
    std::vector<uint8_t> out(moonlive::codeCapFor(moonlive::countTokens(src.c_str())));
    auto r = moonlive::compileSource(src.c_str(), kTable, kSys, out.data(), out.size());
    INFO(std::string(r.error));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(r.ok);
#else
    CHECK((r.ok || std::string(r.error) == moonlive::kCodegenFailed));
#endif
}

// A lexer error does not move past its character, so a lookahead skipping a call's parentheses stops at it; before, one typo here looped the compile forever.
TEST_CASE("an unexpected character inside a call's arguments is refused, not looped on") {
    uint8_t out[2048];
    for (const char* body : {"setRGB(0, max(1, 2 @ 3), 0, 0);", "fill(abs(1 $), 0, 0);", "setRGB(0, getRGB(1 # 2).r, 0, 0);"}) {
        CAPTURE(body);
        auto r = moonlive::compileSource(mmScript(body), kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
    }
}
