/// @module MoonLive

#include "doctest.h"
#include "moonlive_script_wrap.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <cstdio>    // std::snprintf — clang gets it transitively, GCC does not
#include <vector>
#include <algorithm>

// MoonLive IR + assembler, exercised through the front-end + the light builtin table (the IR builders are no longer hand-written, the parser builds the IR from source). The headline check is the BEHAVIORAL GOLDEN: a compiled `fill(...)` and the hand-encoded emitFill, run over the same buffer, produce identical output (the assembler keeps its own register convention, so this is behavioral equivalence, not byte-equality).

using namespace mm;

static moonlive::BuiltinTable kT = moonlive::lightBuiltins();
static moonlive::SysVarTable kSys = moonlive::modifierSysVars();

// Every compile-through-run test in this file needs a working host JIT. The tiny-buffer degrade test lower down (which asserts !ok) is left unguarded, it passes for the right reason on arm64 (bytes exceed cap) and a compatible reason on x86_64 (no codegen).
#if MM_MOONLIVE_HAS_HOST_JIT

// Inside the guard because its only callers are: without a host JIT (CI's x86_64 Linux) the tests below vanish and `place` becomes unused, which GCC treats as an error under -Werror, while clang stays quiet. Defining a helper outside the guard that only guarded code uses is the bug.
namespace {
using FillFn = void (*)(uint8_t*, uint32_t, uint8_t);
FillFn place(const uint8_t* code, size_t n, void*& blkOut, size_t cap = 256) {
    void* blk = platform::allocExec(cap);
    blkOut = blk;
    if (!blk) return nullptr;
    platform::writeExec(blk, code, n);
    return reinterpret_cast<FillFn>(blk);
}
}

TEST_CASE("MoonLive compiled fill is BEHAVIORALLY identical to the hand-encoded emitFill (golden)") {
    const uint8_t cases[][3] = {{0, 0, 255}, {10, 20, 200}, {255, 255, 255}, {0, 0, 0}, {1, 2, 3}};
    for (auto& c : cases) {
        // hand-encoded reference
        uint8_t handCode[256];
        size_t hn = moonlive::emitFill(handCode, sizeof(handCode), c[0], c[1], c[2]);
        REQUIRE(hn > 0);
        void* hb = nullptr;
        auto handFn = place(handCode, hn, hb);
        REQUIRE(handFn != nullptr);

        // compiled-from-source via the IR + assembler
        char src[64];
        std::snprintf(src, sizeof(src), mmScript("fill(%u, %u, %u);"), c[0], c[1], c[2]);
        uint8_t irCode[256];
        auto r = moonlive::compileSource(src, kT, kSys, irCode, sizeof(irCode));
        REQUIRE(r.ok);
        void* ib = nullptr;
        auto irFn = place(irCode, r.len, ib);
        REQUIRE(irFn != nullptr);

        std::vector<uint8_t> a(8 * 3, 0xAB), b(8 * 3, 0xAB);
        handFn(a.data(), 8, 3);
        irFn(b.data(), 8, 3);
        CHECK(a == b);                          // identical output
        for (int i = 0; i < 8; i++) {
            CHECK(b[i*3+0] == c[0]); CHECK(b[i*3+1] == c[1]); CHECK(b[i*3+2] == c[2]);
        }
        platform::freeExec(hb, 256);
        platform::freeExec(ib, 256);
    }
}

TEST_CASE("MoonLive compiled fill is robust: zero lights writes nothing") {
    uint8_t code[256];
    auto r = moonlive::compileSource(mmScript("fill(255, 0, 0);"), kT, kSys, code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = nullptr;
    auto fn = place(code, r.len, blk);
    REQUIRE(fn != nullptr);
    std::vector<uint8_t> buf(3, 0xAB);
    fn(buf.data(), 0, 3);                 // nLights == 0 → loop guard skips
    CHECK(buf[0] == 0xAB);
    platform::freeExec(blk, 256);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("MoonLive compileSource degrades on a too-small code buffer") {
    uint8_t tiny[4];
    CHECK_FALSE(moonlive::compileSource(mmScript("fill(0,0,255);"), kT, kSys, tiny, sizeof(tiny)).ok);
    CHECK_FALSE(moonlive::compileSource(mmScript("fill(0,0,255);"), kT, kSys, nullptr, 0).ok);
}

#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("MoonLive compiled setRGB writes one pixel; out-of-range is bounds-rejected") {
    uint8_t code[256];
    // in-range
    auto r = moonlive::compileSource(mmScript("setRGB(3, 10, 20, 200);"), kT, kSys, code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = nullptr; auto fn = place(code, r.len, blk); REQUIRE(fn != nullptr);
    std::vector<uint8_t> buf(8 * 3, 0);
    fn(buf.data(), 8, 3);
    CHECK(buf[3*3] == 10); CHECK(buf[3*3+2] == 200);
    for (int i = 0; i < 8; i++) if (i != 3) CHECK(buf[i*3] == 0);
    platform::freeExec(blk, 256);

    // out-of-range index 99 on 8 lights → guarded, no write
    auto r2 = moonlive::compileSource(mmScript("setRGB(99, 255, 255, 255);"), kT, kSys, code, sizeof(code));
    REQUIRE(r2.ok);
    void* blk2 = nullptr; auto fn2 = place(code, r2.len, blk2); REQUIRE(fn2 != nullptr);
    std::vector<uint8_t> buf2(8 * 3, 0xAB);
    fn2(buf2.data(), 8, 3);
    for (auto v : buf2) CHECK(v == 0xAB);
    platform::freeExec(blk2, 256);
}

// STAGE 1 CONTROLS, codegen + live-read contract. A script control compiles to a LoadCtrl that reads the run-time controls arena (the 5th arg); mutating the arena changes the output with NO recompile, the live-edit guarantee, pinned at the codegen level. The `control + random16` case pins the call() save-set interaction (kArg4 must survive a host call).
namespace {
using CtrlFn = void (*)(uint8_t*, uint32_t, uint8_t, uint32_t, const uint8_t*);
int firstLit(const std::vector<uint8_t>& b) {
    for (size_t i = 0; i + 2 < b.size(); i += 3) if (b[i] || b[i+1] || b[i+2]) return static_cast<int>(i / 3);
    return -1;
}
}

TEST_CASE("MoonLive control: a declared control reads the arena live (no recompile on value change)") {
    uint8_t code[768];
    auto r = moonlive::compileSource(
        mmScript("byte speed = 50;\nsetRGB(speed, 0, 0, 255);"), kT, kSys, code, sizeof(code));
    REQUIRE(r.ok);
    REQUIRE(r.memberCount == 1);      // the declaration is a member; a control needs defineControls
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk != nullptr);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<CtrlFn>(blk);

    std::vector<uint8_t> buf(16 * 3, 0);
    // A member occupies a whole 4-byte SLOT and is read with a 32-bit load, so the arena has to hold all four bytes: a one-byte array would have the load reading three bytes past its end.
    uint8_t arena[4] = {0, 0, 0, 0};

    arena[0] = 5;  std::fill(buf.begin(), buf.end(), 0); fn(buf.data(), 16, 3, 0, arena);
    CHECK(firstLit(buf) == 5);                       // control value selects the pixel
    arena[0] = 9;  std::fill(buf.begin(), buf.end(), 0); fn(buf.data(), 16, 3, 0, arena);
    CHECK(firstLit(buf) == 9);                       // changed the arena slot only, NO recompile
    arena[0] = 0;  std::fill(buf.begin(), buf.end(), 0); fn(buf.data(), 16, 3, 0, arena);
    CHECK(firstLit(buf) == 0);
    platform::freeExec(blk, r.len);
}

TEST_CASE("MoonLive control survives a host call (kArg4 live across random16)") {
    // The control's index (arena[0]) must still be readable AFTER a random16 call clobbers the scratch pool, pins that the call() save-set protects kArg4 (the arena pointer).
    uint8_t code[768];
    auto r = moonlive::compileSource(
        mmScript("byte idx = 0;\nsetRGB(idx, random16(256), 0, 255);"), kT, kSys, code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk != nullptr);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<CtrlFn>(blk);

    // A member occupies a whole 4-byte SLOT and is read with a 32-bit load, so the arena has to hold all four bytes: a one-byte array would have the load reading past its end, and the index would come back as whatever followed it on the stack.
    uint8_t arena[4] = {7, 0, 0, 0};
    std::vector<uint8_t> buf(16 * 3, 0);
    fn(buf.data(), 16, 3, 0, arena);
    // pixel 7 is lit (its blue channel is 255), and ONLY pixel 7 (the control index held across the call)
    CHECK(buf[7*3 + 2] == 255);
    int lit = 0; for (size_t i = 0; i + 2 < buf.size(); i += 3) if (buf[i] || buf[i+1] || buf[i+2]) lit++;
    CHECK(lit == 1);
    platform::freeExec(blk, r.len);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT
