/// @module MoonLive

#include "doctest.h"

#include <cstring>
#include "moonlive_script_wrap.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLiveSpill.h"
#include "core/moonlive/moonlive_emit.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform_heap.h"   // allocExec, writeExec, freeExec

#include <cstdint>
#include <vector>

// MoonLive register allocation, spilling to the call frame (core/moonlive/MoonLiveSpill.cpp).
//
// The governing risk is that only the arm64 backend is ever EXECUTED by tests, while the register walls these tests are about are tightest on Xtensa. The answer is the squeezed budget: compiling the same script at the host's real budget and at a deliberately smaller one must render identical pixels, spilling is by definition a change of storage, not of meaning. That turns the hardest algorithm in the compiler from a hardware-only proposition into something a unit test settles.

using namespace mm;

static moonlive::BuiltinTable kT = moonlive::lightBuiltins();
static moonlive::SysVarTable kSys = moonlive::modifierSysVars();

#if MM_MOONLIVE_HAS_HOST_JIT

namespace {

using CtrlFn = void (*)(uint8_t*, uint32_t, uint8_t, uint32_t, const uint8_t*);

// Compile `src` at `budget` (null = the backend's own) and render it over `nLights` lights. `ok` reports whether the compile succeeded, so a test can assert BOTH that a squeezed budget still compiles and what it produced.
std::vector<uint8_t> renderAt(const char* src, int nLights, const moonlive::RegBudget* budget,
                              bool& ok, uint32_t t = 0) {
    uint8_t code[moonlive::kCodeCap];
    auto r = moonlive::compileSource(src, kT, kSys, code, sizeof(code), budget);
    ok = r.ok;
    std::vector<uint8_t> buf(static_cast<size_t>(nLights) * 3, 0);
    if (!r.ok) return buf;
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk != nullptr);
    platform::writeExec(blk, code, r.len);
    uint8_t arena[moonlive::kArenaBytes] = {};
    // Seeded from the MEMBERS, as the engine does: a declaration is a member, and its initializer is what the arena holds. A control is one of those members surfaced on the UI, so seeding members covers both, and a script with no defineControls still starts at its declared values.
    for (uint8_t i = 0; i < r.memberCount; i++) arena[r.members[i].offset] = r.members[i].def;
    reinterpret_cast<CtrlFn>(blk)(buf.data(), static_cast<uint32_t>(nLights), 3, t, arena);
    platform::freeExec(blk, r.len);
    return buf;
}

// A budget with FEWER registers than arm64's fourteen, so the allocator is forced to spill on the only backend a test can run. Nine leaves four values allocatable once the five fixed ABI vregs and the four reload temps are taken out, enough for a real script, tight enough that anything interesting overflows. `slots` matches what the host frame can address.
constexpr moonlive::RegBudget squeezed(uint8_t regs, uint8_t reserved) {
    return moonlive::RegBudget{regs, reserved, 16};
}

int litCount(const std::vector<uint8_t>& b) {
    int n = 0;
    for (size_t i = 0; i + 2 < b.size(); i += 3) if (b[i] || b[i + 1] || b[i + 2]) n++;
    return n;
}

}  // namespace

// THE key test: spilling changes where a value lives, never what the program computes.
TEST_CASE("a script renders identical pixels at a squeezed register budget as at the full one") {
    // Enough live values that a nine-register budget cannot hold them all: three independent color components plus two loop-carried values.
    const char* src = mmScript("for (int i = 0; i < 6; i = i + 1) { setRGB(i, i + 1, i + 2, i + 3); }");

    bool fullOk = false, tightOk = false;
    auto full = renderAt(src, 8, nullptr, fullOk);
    const auto tightBudget = squeezed(11, 1);   // 11 regs, StoreElem's one scratch
    auto tight = renderAt(src, 8, &tightBudget, tightOk);

    CHECK(fullOk);
    CHECK(tightOk);
    CHECK(full == tight);
    CHECK(litCount(full) == 6);       // and it actually did something
}

// A returned value is a DEFINITION, and the allocator has to see it as one.
//
// The rewriter remaps every operand of every op it renumbers, but only remaps a `dst` for an op that declares it writes one. A value-returning call did not, so after a compaction the call still wrote its pre-compaction register while the consumer read the new one: `a() + b()` came out as whatever that register happened to hold. Invisible at the host's own budget, which takes the already-fits path and renumbers nothing, so this drives the SQUEEZED budget through the script's `tick` entry rather than the block start a plain renderAt would call.
TEST_CASE("a value returned by a script function survives a squeezed register budget") {
    const char* src =
        "class T {\n"
        "  int a() { return 40; }\n"
        "  int b() { return 5; }\n"
        "  void tick() {\n"
        "    int p = a() + b();\n"
        "    int q = a() + a();\n"
        "    int r = b() + b();\n"
        "    int s = p + q;\n"
        "    for (int i = 0; i < 3; i = i + 1) { setRGB(i, p, q, r + s - 90); }\n"
        "  }\n"
        "}\n";

    auto renderTick = [&](const moonlive::RegBudget* budget) {
        uint8_t code[moonlive::kCodeCap];
        auto r = moonlive::compileSource(src, kT, kSys, code, sizeof(code), budget);
        REQUIRE(r.ok);
        // Enter at `tick`, not at the block start: a multi-function script begins with its helpers, and calling the block start would run `a()` as the program.
        uint16_t tickOffset = 0xFFFF;
        for (uint8_t i = 0; i < r.entryCount; i++)
            if (std::strncmp(r.entries[i].name, "tick", r.entries[i].nameLen) == 0) tickOffset = r.entries[i].offset;
        REQUIRE(tickOffset != 0xFFFF);

        std::vector<uint8_t> buf(4 * 3, 0);
        void* blk = platform::allocExec(r.len);
        REQUIRE(blk != nullptr);
        platform::writeExec(blk, code, r.len);
        uint8_t arena[moonlive::kArenaBytes] = {};
        for (uint8_t i = 0; i < r.memberCount; i++) arena[r.members[i].offset] = r.members[i].def;
        reinterpret_cast<CtrlFn>(static_cast<uint8_t*>(blk) + tickOffset)(buf.data(), 4, 3, 0, arena);
        platform::freeExec(blk, r.len);
        return buf;
    };

    const auto tightBudget = squeezed(11, 1);
    const auto full = renderTick(nullptr);
    const auto tight = renderTick(&tightBudget);

    CHECK(full == tight);
    for (int i = 0; i < 3; i++) {
        CHECK(tight[i * 3 + 0] == 45);   // a() + b()
        CHECK(tight[i * 3 + 1] == 80);   // a() + a()
        CHECK(tight[i * 3 + 2] == 45);   // (b()+b()) + (p+q) - 90
    }
}

// The back-edge case. Naive first-def-to-last-use intervals look dead early in a loop body, so the allocator would hand a counter's register away and the next iteration would read someone else's value, placing lights twice, or not at all. Nested, so the extension has to apply innermost-first.
TEST_CASE("a nested loop at a squeezed budget places every light exactly once") {
    const char* src =
        mmScript("for (int i = 0; i < 4; i = i + 1) {\n"
        "  for (int j = 0; j < 4; j = j + 1) {\n"
        "    setRGB(i * 4 + j, 200, 100, 50);\n"
        "  }\n"
        "}\n");

    bool fullOk = false, tightOk = false;
    auto full = renderAt(src, 16, nullptr, fullOk);
    const auto tightBudget = squeezed(11, 1);
    auto tight = renderAt(src, 16, &tightBudget, tightOk);

    CHECK(fullOk);
    CHECK(tightOk);
    CHECK(full == tight);
    // every one of the 16 cells written, each exactly once, a counter that lost its register across the back edge writes the wrong cells, and the count or the colors would differ
    CHECK(litCount(tight) == 16);
    for (int i = 0; i < 16; i++) {
        CHECK(tight[i * 3 + 0] == 200);
        CHECK(tight[i * 3 + 1] == 100);
        CHECK(tight[i * 3 + 2] == 50);
    }
}

// Slots live in the routine's OWN frame, and call() moves sp underneath it. Addressing a slot from the frame pointer rather than sp is what makes this hold; from sp it would read into the callee's saved registers instead.
TEST_CASE("a spilled value survives a host call and is still correct afterwards") {
    // `keep` is defined before the call and used after it, so it must be live ACROSS random16, and at a squeezed budget it is one of the values that has nowhere to live but a slot.
    const char* src =
        mmScript("byte idx = 5;\n"
        "for (int i = 0; i < 3; i = i + 1) {\n"
        "  setRGB(idx + i, random16(1) + 111, i + 1, 222);\n"
        "}\n");

    bool fullOk = false, tightOk = false;
    auto full = renderAt(src, 16, nullptr, fullOk);
    const auto tightBudget = squeezed(11, 1);
    auto tight = renderAt(src, 16, &tightBudget, tightOk);

    CHECK(fullOk);
    CHECK(tightOk);
    CHECK(full == tight);
    CHECK(litCount(tight) == 3);
    // random16(1) is always 0, so red is exactly 111, a slot clobbered by the call shows up here
    for (int i = 0; i < 3; i++) {
        CHECK(tight[(5 + i) * 3 + 0] == 111);
        CHECK(tight[(5 + i) * 3 + 1] == i + 1);
        CHECK(tight[(5 + i) * 3 + 2] == 222);
    }
}

// The control arena pointer (kArg4) is a fixed ABI vreg the allocator must never reassign or spill. If it did, a control read after a spill would load from a register holding something else.
TEST_CASE("a declared control still reads live at a squeezed budget") {
    const char* src =
        mmScript("byte pos = 0;\n"
        "for (int i = 0; i < 2; i = i + 1) {\n"
        "  setRGB(pos + i, 10, 20, 30);\n"
        "}\n");
    uint8_t code[moonlive::kCodeCap];
    const auto tightBudget = squeezed(11, 1);
    auto r = moonlive::compileSource(src, kT, kSys, code, sizeof(code), &tightBudget);
    REQUIRE(r.ok);
    REQUIRE(r.memberCount == 1);   // `pos` is a member; the arena read is what this pins
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk != nullptr);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<CtrlFn>(blk);

    uint8_t arena[moonlive::kArenaBytes] = {};
    for (uint8_t v : {uint8_t(0), uint8_t(4), uint8_t(9)}) {
        std::vector<uint8_t> buf(16 * 3, 0);
        arena[0] = v;
        fn(buf.data(), 16, 3, 0, arena);
        CHECK(buf[v * 3 + 0] == 10);            // the control still selects the pixel, live
        CHECK(buf[(v + 1) * 3 + 0] == 10);
        CHECK(litCount(buf) == 2);
    }
    platform::freeExec(blk, r.len);
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT

// FAIL, NEVER MISCOMPILE. A budget with no room for the fixed ABI vregs plus the reload temps, and one with no frame slots to spill INTO, must both refuse, silently emitting code that names a register the target does not have is the failure this whole pass exists to make impossible.
TEST_CASE("an impossible register budget refuses the compile instead of emitting wrong code") {
    // A script whose live values genuinely exceed the budgets below, so each really does have to spill and really does have nowhere to put the result.
    const char* src =
        mmScript("for (int i = 0; i < 4; i = i + 1) {\n"
        "  for (int j = 0; j < 4; j = j + 1) {\n"
        "    setRGB(i * 4 + j, 200, 100, 50);\n"
        "  }\n"
        "}\n");
    uint8_t code[2048];
#if MM_MOONLIVE_HAS_HOST_JIT
    // Only where a backend exists: on x86-64 there is none, so the default lowerer emits nothing and a normal compile legitimately fails. The refusals below still mean what they say everywhere.
    REQUIRE(moonlive::compileSource(src, kT, kSys, code, sizeof(code)).ok);   // it does compile normally
#endif

    // Fewer registers than the reload temps need. The host arguments no longer count against this, they live in frame slots and hold a register only for the parking store at entry, so the floor is the temps alone, and a budget at or below it has nothing to compute with.
    const moonlive::RegBudget noRoom{4, 1, moonlive::kTotalSlots};
    CHECK_FALSE(moonlive::compileSource(src, kT, kSys, code, sizeof(code), &noRoom).ok);

    const moonlive::RegBudget noSlots{11, 1, 0};     // room to allocate, nowhere to spill INTO
    CHECK_FALSE(moonlive::compileSource(src, kT, kSys, code, sizeof(code), &noSlots).ok);
}

// A program that fits is left byte-identical: the allocator must not exist as far as a non-spilling script is concerned, or every shipped script pays for a feature it never uses.
TEST_CASE("a program that already fits is untouched by the register allocator") {
    moonlive::IrProgram ir;
    REQUIRE(ir.reserve(8));
    REQUIRE(ir.push({moonlive::IrOp::Const, moonlive::kFirstTemp, 0, 0, 0, 0, 42, nullptr, {}}));
    REQUIRE(ir.push({moonlive::IrOp::Mov, moonlive::VReg(moonlive::kFirstTemp + 1),
                     moonlive::kFirstTemp, 0, 0, 0, 0, nullptr, {}}));
    const uint16_t before = ir.count;

    uint8_t slots = 0xff;
    CHECK(moonlive::spillToBudget(ir, moonlive::RegBudget{14, 0, 16}, slots));
    CHECK(slots == 0);                  // nothing spilled
    CHECK(ir.count == before);          // and not one op inserted
    CHECK(ir.ops[0].dst == moonlive::kFirstTemp);   // nor renumbered
}

// Loop extension, pinned on its own, the one part of the allocator no script in this grammar can currently exercise, and the part whose failure is a silent miscompile rather than a refusal.
//
// A value defined before a loop and read only EARLY in the body has a naive live range that ends at that read. Every later value then looks free to take its register, and the next iteration reads whatever took it. The IR is built by hand because the wall a script hits first is its own variable count: by the time a source program names enough live values to force allocation, the values are short-lived ones the naive analysis already gets right. Extension is therefore untestable through the front end today, and this is what stands in for it until script-local functions make longer live ranges expressible.
TEST_CASE("a value live across a loop keeps its storage for the whole loop") {
    using namespace moonlive;
    IrProgram ir;
    REQUIRE(ir.reserve(64));
    const VReg carried = kFirstTemp;                    // defined before the loop, read early inside
    const VReg ctr = VReg(kFirstTemp + 1), lim = VReg(kFirstTemp + 2);
    REQUIRE(ir.push({IrOp::Const, carried, 0, 0, 0, 0, 7, nullptr, {}}));
    REQUIRE(ir.push({IrOp::Const, ctr, 0, 0, 0, 0, 0, nullptr, {}}));
    REQUIRE(ir.push({IrOp::Const, lim, 0, 0, 0, 0, 4, nullptr, {}}));
    REQUIRE(ir.push({IrOp::Label, 0, 0, 0, 0, 0, 0, nullptr, {}}));
    REQUIRE(ir.push({IrOp::Add, VReg(kFirstTemp + 3), carried, ctr, 0, 0, 0, nullptr, {}}));
    // A tail of further values, enough that the register file cannot hold them all at once.
    for (int i = 4; i < 14; i++)
        REQUIRE(ir.push({IrOp::AddImm, VReg(kFirstTemp + i), VReg(kFirstTemp + 3),
                         0, 0, 0, i, nullptr, {}}));
    REQUIRE(ir.push({IrOp::AddImm, ctr, ctr, 0, 0, 0, 1, nullptr, {}}));
    REQUIRE(ir.push({IrOp::BranchNe, 0, ctr, lim, 0, 0, 0, nullptr, {}}));   // back edge to label 0

    const moonlive::RegBudget budget{14, 1, 16};
    REQUIRE(ir.vregsUsed > budget.allocatable());       // the program really does not fit
    uint8_t slots = 0;
    REQUIRE(spillToBudget(ir, budget, slots));

    // Nineteen values cannot occupy fourteen registers, so SOMETHING has to reach the frame. Without extension every interval ends at its early read, the allocator concludes the program fits, and it spills nothing at all, the exact wrong answer this asserts against.
    CHECK(slots > 0);

    // And the decisive property, whatever the allocator chose: no op may name a register the target does not have. This is the invariant the whole pass exists to guarantee.
    for (uint16_t i = 0; i < ir.count; i++) {
        const auto& in = ir.ops[i];
        const bool slotRef = in.op == IrOp::Spill || in.op == IrOp::Reload;
        CHECK(in.dst < budget.allocatable());
        if (!slotRef) {
            CHECK(in.a < budget.allocatable());
            CHECK(in.b < budget.allocatable());
        }
        if (slotRef) CHECK(in.imm < slots);             // and every slot is one the prologue reserves
    }
}

#if MM_MOONLIVE_HAS_HOST_JIT

// The shape that resets both Xtensa boards: a SYSTEM VARIABLE read as a loop bound, with a HOST CALL in the body. Bench-bisected, each ingredient alone is fine, and only the three together fail:
//
// loop, constant bound, call in body   -> runs loop, member-bound limit, call in body   -> runs `width` read, no loop                -> runs `width` loop, no call in body        -> runs `width` loop WITH a call in body     -> LoadProhibited inside the emitted code
//
// A system variable lives in the controls arena, reached through kArg4, a host argument that now lives in a frame slot and is reloaded at each use. Run here on the one backend a test can execute, so the failure is debuggable in a process rather than from a crash dump.
TEST_CASE("a system variable read in a loop survives a host call in that loop") {
    // `width` is the EFFECT vocabulary, not the modifier one this file's kSys uses.
    static moonlive::SysVarTable fxSys = moonlive::effectSysVars();
    // Green is a NONZERO CONSTANT, and red keeps the host call. Two reasons, both learned the hard way: random16 is one LCG shared by the whole process, so a red channel that can draw 0 makes "is this light lit" depend on how many draws earlier tests made, and a green of 0 everywhere means a loop that runs PAST width still writes 0 there, so the past-width check could not detect the runaway it is named for. A constant 7 fixes both while keeping the call, which is the ingredient this test exists for.
    const char* src = mmScript("for (int x = 0; x < width; x = x + 1) { setRGB(x, random16(256), 7, 0); }\n");

    // At the host's full budget AND at a squeezed one: Xtensa has ten registers where arm64 has fourteen, so the squeezed run is the closest a host test gets to the pressure the device is under, and pressure is what decides whether kArg4 stays in a register or goes to a slot.
    uint8_t code[2048];
    const auto tight = squeezed(11, 1);
    auto r = moonlive::compileSource(src, kT, fxSys, code, sizeof(code), &tight);
    REQUIRE(r.ok);

    void* blk = platform::allocExec(r.len);
    REQUIRE(blk != nullptr);
    platform::writeExec(blk, code, r.len);

    // The arena the binding would hand over: controls at their defaults, and `width` where MoonLiveEffect::tick writes it.
    uint8_t arena[moonlive::kArenaBytes] = {};
    for (uint8_t i = 0; i < r.memberCount; i++) arena[r.members[i].offset] = r.members[i].def;
    const uint8_t kWidth = 8;
    arena[moonlive::kSysWidth] = kWidth;

    std::vector<uint8_t> buf(16 * 3, 0);
    reinterpret_cast<CtrlFn>(blk)(buf.data(), 16, 3, 0, arena);
    platform::freeExec(blk, r.len);

    // Exactly `width` lights written, and only the first `width`: a loop that read a corrupted bound either stops early, runs away, or writes through a wrong pointer. Asserted on the CONSTANT channel, so the result does not depend on what the shared random16 sequence happens to return.
    for (int i = 0; i < kWidth; i++) {
        INFO("light ", i, " is inside width and must carry the constant");
        CHECK(buf[i * 3 + 1] == 7);
    }
    for (int i = kWidth; i < 16; i++) {
        INFO("light ", i, " is past width and must be untouched");
        CHECK(buf[i * 3 + 1] == 0);
    }
}


#endif  // MM_MOONLIVE_HAS_HOST_JIT
