/// @module MoonLive

/// The x86-64 (Windows + Linux + Intel macOS) host backend's emitted code, checked BYTE for BYTE on the development machine. Same intent as unit_moonlive_codegen_riscv.cpp and _xtensa.cpp: without this, an encoding bug is caught by an execution crash, and the only way back from a fault inside JIT-emitted bytes is a debugger session. A pinned expected-byte sequence per named instruction turns "the emitted code crashes" into "these three bytes are wrong".
///
/// Runs only on x86-64 hosts: the assembler this file exercises is the platform's HostAssembler, which compiles as x86-64 encoding on any x86-64 target (Windows/Linux/Intel-macOS) via the #elif branch in moonlive_asm_host.cpp. Skipped elsewhere (arm64 desktops) because we would be checking bytes against an assembler that isn't there, the arm64 branch of the same file uses completely different encodings, checked by the RISC-V/Xtensa/host-arm64 suites.
///
/// Naming: tests are named for the exact instruction they pin so a red one immediately says which helper to fix in moonlive_asm_host.cpp. Expected byte sequences are computed by hand against Intel SDM Vol. 2 encoding tables and marked with the human-readable assembly they represent.

#include "doctest.h"
#include <array>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

#include "platform/desktop/moonlive_asm_host.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace mm::moonlive;

#if defined(_WIN32)
// Compare an assembler's emitted bytes to a known-good sequence, with a diff-friendly failure message. doctest's built-in equality on raw arrays reads badly; this one prints the exact byte index where the mismatch is, which is the datum a reader needs to find the encoder helper.
//
// INSIDE the _WIN32 guard, with the only tests that call it. A helper defined outside the guard its callers live in is unused on every other host, and GCC makes that an error under -Werror while clang stays silent, so it builds locally and breaks CI. unit_moonlive_ir.cpp carries the same note over `place` for the same reason; this file re-learned it from a red sanitizer run.
static void checkBytes(const HostAssembler& A, const uint8_t* want, size_t n) {
    REQUIRE_FALSE(A.overflowed());
    REQUIRE(A.size() == n);
    for (size_t i = 0; i < n; i++) {
        INFO("byte ", i, " of ", n);
        CHECK(unsigned(A.bytes()[i]) == unsigned(want[i]));
    }
}
#endif

// The register-map decision this test file pins: R0..R4 = kArg0..kArg4, followed by nine general vregs. The exact machine registers behind each Reg differ by ABI (Win64 vs SysV), so the tests split two ways: those checking STRUCTURE (byte counts, opcodes, mod bits) hold for both and are unguarded, and those checking EXACT bytes name Win64's registers and are guarded.
//
// The guard wraps the WHOLE test case, not its body. A `#if` inside a TEST_CASE still registers the case on every other host, with nothing in it, and doctest counts an empty case as a pass, so the suite grows six green lines that assert nothing. Excluding the case is honest; an empty one that passes is the same false-green the `ctest --no-tests=error` fix exists to prevent.
//
// SysV's exact bytes are deliberately NOT pinned here. They would have to be hand-derived on a machine that cannot run them, which is how wrong goldens get written. The SysV path is not unguarded: CI is x86-64 Linux, so the whole MoonLive suite now executes against it, which is stronger evidence than a byte table nobody can check. What is lost is the "which encoder is wrong" precision on SysV alone; add a table here the first time a SysV-only bug appears.

// ================================================================================================= movImm, mov r64, imm32 (sign-extended) =================================================================================================

// retValue parks a script's `return` value where the ABI hands it back. STRUCTURAL, not exact bytes: which vreg holds rax differs between Win64 and SysV, and what must hold on both is that the destination IS rax. A wrong register here is silent: the host reads a plausible number.
TEST_CASE("x86_64: retValue moves the value into rax, the return register") {
    // R1 is never rax (rax is the LAST vreg by design, see the static_assert in the backend), so this always emits a real move rather than the elided self-copy.
    HostAssembler a; a.retValue(R1); a.finalize();
    REQUIRE_FALSE(a.overflowed());
    REQUIRE(a.size() == 3);                  // REX.W + 0x89 + ModRM
    CHECK((a.bytes()[0] & 0xFBu) == 0x48u);  // REX.W set; the R bit varies with the source register
    CHECK(a.bytes()[1] == 0x89u);            // MOV r/m64, r64
    // mod=11 (register direct) and rm=000 (rax): the destination is the return register.
    CHECK((a.bytes()[2] & 0xC7u) == 0xC0u);
}

TEST_CASE("x86_64: movImm(R0, 42) is mov r64, 42 (sign-extended imm32)") {
    HostAssembler A;
    A.movImm(R0, 42);
    // Win64: R0 = rcx → 48 C7 C1 2A 00 00 00 SysV : R0 = rdi → 48 C7 C7 2A 00 00 00 Both have REX.W + opcode C7 + ModR/M(mod=11, reg=/0, rm=dst) + imm32 → 7 bytes total.
    REQUIRE_FALSE(A.overflowed());
    REQUIRE(A.size() == 7);
    CHECK(A.bytes()[0] == 0x48);            // REX.W (no R/X/B bits: dst is < 8 on both ABIs)
    CHECK(A.bytes()[1] == 0xC7);            // MOV r/m64, imm32
    // ModR/M: mod=11 (register-direct), reg=0 (/0 = MOV), rm=dst&7. dst on Win64 is RCX=1, on SysV is RDI=7. Either way, mod=11 and reg=/0=000 make the high 5 bits 0xC0.
    CHECK((A.bytes()[2] & 0xF8) == 0xC0);
    // imm32 little-endian
    CHECK(A.bytes()[3] == 0x2A);
    CHECK(A.bytes()[4] == 0x00);
    CHECK(A.bytes()[5] == 0x00);
    CHECK(A.bytes()[6] == 0x00);
}

#if defined(_WIN32)
TEST_CASE("x86_64: movImm(R2, 42) uses REX.B for the r8-r15 destination on Win64") {
    HostAssembler A;
    A.movImm(R2, 42);            // R2 = r8 on Win64  → 49 C7 C0 2A 00 00 00
    const uint8_t want[] = {0x49, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00};
    checkBytes(A, want, sizeof(want));
}
#endif

TEST_CASE("x86_64: movImm(R0, -1) sign-extends via the imm32 form (no negative-imm branch)") {
    HostAssembler A;
    A.movImm(R0, -1);
    // Same shape as +42: REX.W + C7 + ModR/M + FF FF FF FF (sign-extended to 64 bits by CPU).
    REQUIRE(A.size() == 7);
    CHECK(A.bytes()[3] == 0xFF);
    CHECK(A.bytes()[4] == 0xFF);
    CHECK(A.bytes()[5] == 0xFF);
    CHECK(A.bytes()[6] == 0xFF);
}

// ================================================================================================= movReg, mov r64, r64 =================================================================================================

TEST_CASE("x86_64: movReg(R0, R0) elides the move (a no-op copy emits nothing)") {
    HostAssembler A;
    A.movReg(R0, R0);
    CHECK(A.size() == 0);
}

TEST_CASE("x86_64: movReg(R0, R1) is a 3-byte REX.W + 89 /r register-to-register mov") {
    HostAssembler A;
    A.movReg(R0, R1);
    REQUIRE(A.size() == 3);
    CHECK(A.bytes()[0] == 0x48);            // REX.W (both dst and src < 8 on both ABIs for R0/R1)
    CHECK(A.bytes()[1] == 0x89);            // MOV r/m64, r64
    // ModR/M: mod=11, reg=src, rm=dst. Win64: src=RDX(2), dst=RCX(1) → 11 010 001 = 0xD1. SysV: src=RSI(6), dst=RDI(7) → 11 110 111 = 0xF7. Only checking structural bits here.
    CHECK((A.bytes()[2] & 0xC0) == 0xC0);   // mod=11
}

// ================================================================================================= movPtr, movabs r64, imm64 =================================================================================================

TEST_CASE("x86_64: movPtr(R0, addr) is the 10-byte movabs r64, imm64 form") {
    HostAssembler A;
    // A pattern with all four byte lanes distinct so a wrong endianness or a wrong lane order shows up as a specific mismatch (not one that swaps zeros around and still checks OK).
    const uint64_t addr = 0x1122334455667788ULL;
    A.movPtr(R0, reinterpret_cast<const void*>(addr));
    REQUIRE(A.size() == 10);
    CHECK(A.bytes()[0] == 0x48);            // REX.W (dst < 8 on both ABIs)
    CHECK((A.bytes()[1] & 0xF8) == 0xB8);   // MOV r64, imm64 = B8+r (low 3 bits = dst)
    CHECK(A.bytes()[2] == 0x88);            // imm64 little-endian
    CHECK(A.bytes()[3] == 0x77);
    CHECK(A.bytes()[4] == 0x66);
    CHECK(A.bytes()[5] == 0x55);
    CHECK(A.bytes()[6] == 0x44);
    CHECK(A.bytes()[7] == 0x33);
    CHECK(A.bytes()[8] == 0x22);
    CHECK(A.bytes()[9] == 0x11);
}

// ================================================================================================= addReg, add r64, r64 =================================================================================================

TEST_CASE("x86_64: addReg(R0, R0, R1) is REX.W + 01 /r (in-place accumulate)") {
    HostAssembler A;
    A.addReg(R0, R0, R1);
    // d == a → no leading mov, just add d, b.
    REQUIRE(A.size() == 3);
    CHECK(A.bytes()[0] == 0x48);
    CHECK(A.bytes()[1] == 0x01);            // ADD r/m64, r64
    CHECK((A.bytes()[2] & 0xC0) == 0xC0);
}

TEST_CASE("x86_64: addReg(R0, R1, R2) inserts a mov first (d != a and d != b)") {
    HostAssembler A;
    A.addReg(R0, R1, R2);
    // d != a and d != b → mov d, a (3 bytes) then add d, b (3 bytes).
    REQUIRE(A.size() == 6);
    CHECK(A.bytes()[1] == 0x89);            // first: mov d, a
    CHECK(A.bytes()[4] == 0x01);            // then:  add d, b
}

// ================================================================================================= mulReg, imul r64, r64 =================================================================================================

TEST_CASE("x86_64: mulReg(R0, R0, R1) is REX.W + 0F AF /r (two-operand)") {
    HostAssembler A;
    A.mulReg(R0, R0, R1);
    // d == a → imul d, b. 4 bytes.
    REQUIRE(A.size() == 4);
    CHECK(A.bytes()[0] == 0x48);            // REX.W
    CHECK(A.bytes()[1] == 0x0F);
    CHECK(A.bytes()[2] == 0xAF);            // IMUL r64, r/m64
}

// ================================================================================================= store8, the pixel write. THE MOST HOT-PATH ENCODING; a bug here corrupts every rendered frame. =================================================================================================

#if defined(_WIN32)
TEST_CASE("x86_64: store8(R0, R1, R2) is mov [R0+R1], R2_low8 with a SIB byte") {
    HostAssembler A;
    // Win64: R0=RCX(1) base, R1=RDX(2) offset, R2=R8(8) value → REX.R (for R8) forced. Encoding: 44 88 04 11 44 = REX (W=0, R=1 for r8, X=0, B=0) 88 = MOV r/m8, r8 04 = ModR/M mod=00, reg=r8&7=0, rm=100 (=SIB indicator) 11 = SIB scale=00, index=rdx&7=010, base=rcx&7=001 → 00 010 001 = 0x11
    A.store8(R0, R1, R2);
    const uint8_t want[] = {0x44, 0x88, 0x04, 0x11};
    checkBytes(A, want, sizeof(want));
}
#endif

TEST_CASE("x86_64: store8 with a byte-half register (val is R4/R12/etc.) always emits REX") {
    // R4 on Win64 is RDI (7); its low byte without REX would encode as bh (a whole different reg). R12 on Win64 is RSI (6); without REX would encode as dh. This test would fail if store8's always-emit-REX guard were ever accidentally made conditional, see the store8 body for the reasoning behind the unconditional REX prefix.
    HostAssembler A;
    A.store8(R0, R1, R7);       // R7 on Win64 = RBX (safe as bl even without REX)
    // Structural check: the SECOND byte (after REX) must be opcode 0x88, meaning the first is REX.
    REQUIRE(A.size() == 4);
    CHECK((A.bytes()[0] & 0xF0) == 0x40);   // any REX prefix
    CHECK(A.bytes()[1] == 0x88);            // opcode
}

// ================================================================================================= load8, control-byte read at [ctrls + imm] =================================================================================================

#if defined(_WIN32)
TEST_CASE("x86_64: load8(R0, R4, 3) is a movzx r32, byte ptr [ctrls + 3] with disp32") {
    HostAssembler A;
    // Win64: R0 = RCX (dst), R4 = RDI (ctrls base). No high-bit regs, so REX prefix is 0x40.
    // Encoding: 40 0F B6 8F 03 00 00 00 40 = REX (always emitted by load8's helper, a leftover from the wider "always-emit"
    //        pattern; harmless prefix, no correctness impact).
    // 0F B6 = MOVZX r32, r/m8 8F = ModR/M mod=10 (disp32), reg=RCX(1), rm=RDI(7) → 10 001 111 = 0x8F 03 00 00 00 = disp32 = 3
    A.load8(R0, R4, 3);
    const uint8_t want[] = {0x40, 0x0F, 0xB6, 0x8F, 0x03, 0x00, 0x00, 0x00};
    checkBytes(A, want, sizeof(want));
}
#endif

// ================================================================================================= Frame ops, prologue must balance with epilogue, or `ret` returns to a corrupted address and the caller's stack cookie fails ("STATUS_STACK_BUFFER_OVERRUN" = 0xC0000409 on Windows). =================================================================================================

#if defined(_WIN32)
TEST_CASE("x86_64: prologue(0) is push rbp; mov rbp,rsp; push nonvols; sub rsp,N; (Win64: load kArg4)") {
    HostAssembler A;
    A.prologue(0);
    // Expected sequence (Win64): 55                         push rbp 48 89 E5                   mov rbp, rsp 53                         push rbx 57                         push rdi 56                         push rsi 41 54                      push r12 41 55                      push r13 41 56                      push r14 41 57                      push r15 48 81 EC XX XX XX XX       sub rsp, N (some multiple of 16 that lands rsp 16-aligned) 48 8B 7D 30                mov rdi, [rbp + 48]   ← load kArg4 (arg 5 from stack), disp8 Total: 1 + 3 + 3*1 + 4*2 + 7 + 4 = 26 bytes, then the 9-byte widening block = 35.
    REQUIRE_FALSE(A.overflowed());
    CHECK(A.bytes()[0] == 0x55);
    CHECK(A.bytes()[1] == 0x48); CHECK(A.bytes()[2] == 0x89); CHECK(A.bytes()[3] == 0xE5);
    CHECK(A.bytes()[4] == 0x53);
    CHECK(A.bytes()[5] == 0x57);
    CHECK(A.bytes()[6] == 0x56);
    CHECK(A.bytes()[7] == 0x41); CHECK(A.bytes()[8] == 0x54);
    CHECK(A.bytes()[9] == 0x41); CHECK(A.bytes()[10] == 0x55);
    CHECK(A.bytes()[11] == 0x41); CHECK(A.bytes()[12] == 0x56);
    CHECK(A.bytes()[13] == 0x41); CHECK(A.bytes()[14] == 0x57);
    CHECK(A.bytes()[15] == 0x48); CHECK(A.bytes()[16] == 0x81); CHECK(A.bytes()[17] == 0xEC);
    // bytes[18..21] = imm32 (frameBytes), variable bytes[22..25] = mov rdi, [rbp + 48] as disp8 (48 fits a signed byte)
    CHECK(A.bytes()[22] == 0x48);
    CHECK(A.bytes()[23] == 0x8B);
    CHECK(A.bytes()[24] == 0x7D);           // ModR/M mod=01 (disp8), reg=RDI(7), rm=RBP(5)
    CHECK(A.bytes()[25] == 0x30);           // disp8 = 48
    // Widening block (9 bytes): mov edx,edx  (89 D2)
    //                           movzx r8,r8b (4D 0F B6 C0)
    //                           mov r9d,r9d  (45 89 C9)
    // Zero-extends the narrow Win64 args before the shared lowering spills them 64-bit-wide; see the prologue body for the debugger investigation that found this.
    CHECK(A.bytes()[26] == 0x89); CHECK(A.bytes()[27] == 0xD2);
    CHECK(A.bytes()[28] == 0x4D); CHECK(A.bytes()[29] == 0x0F);
    CHECK(A.bytes()[30] == 0xB6); CHECK(A.bytes()[31] == 0xC0);
    CHECK(A.bytes()[32] == 0x45); CHECK(A.bytes()[33] == 0x89);
    CHECK(A.bytes()[34] == 0xC9);
    CHECK(A.size() == 35);
}
#endif

// ================================================================================================= Untested-but-critical paths: mulImm, branch fusion, load-idx, call() body. =================================================================================================

TEST_CASE("x86_64: mulImm(R0, R1, 42) is imul r64, r/m64, imm32") {
    HostAssembler A;
    A.mulImm(R0, R1, 42);
    // Encoding: REX.W + 0x69 + ModR/M(11, dst, src) + imm32.  7 bytes.
    REQUIRE(A.size() == 7);
    CHECK(A.bytes()[0] == 0x48);
    CHECK(A.bytes()[1] == 0x69);
    CHECK((A.bytes()[2] & 0xC0) == 0xC0);   // mod=11
    CHECK(A.bytes()[3] == 0x2A);            // imm32 = 42
    CHECK(A.bytes()[4] == 0x00);
    CHECK(A.bytes()[5] == 0x00);
    CHECK(A.bytes()[6] == 0x00);
}

TEST_CASE("x86_64: an unsigned comparison branches on the unsigned condition") {
    HostAssembler A;
    Label l = A.newLabel();
    A.branchGeU(R0, R1, l);
    // cmp (3 bytes) + jae rel32 (6 bytes: 0F 83 xx xx xx xx). Total 9.
    A.bind(l); A.finalize();
    REQUIRE(A.size() == 9);
    CHECK(A.bytes()[0] == 0x40);            // null REX: the compare is 32-bit, not REX.W 64-bit
    CHECK(A.bytes()[1] == 0x39);            // cmp r/m32, r32
    CHECK(A.bytes()[3] == 0x0F);            // jae opcode prefix
    CHECK(A.bytes()[4] == 0x83);            // jae rel32
}

TEST_CASE("x86_64: a signed comparison branches on the signed condition, so a negative compares below zero") {
    HostAssembler A;
    Label l = A.newLabel();
    A.branchGeS(R0, R1, l);
    A.bind(l); A.finalize();
    REQUIRE(A.size() == 9);
    CHECK(A.bytes()[4] == 0x8D);            // jge rel32, NOT jae (0x83)
}

TEST_CASE("x86_64: the compare is 32 bits wide, the width a MoonLive value actually has") {
    // Not REX.W. arm64 compares in `w` registers, so a 64-bit compare here would read a 32-bit negative (sitting in a 64-bit register as 0x00000000FFFFFFFF) as a large POSITIVE, and the two backends would run the same script differently the moment a comparison went signed.
    HostAssembler A;
    Label l = A.newLabel();
    A.branchGeS(R0, R1, l);
    A.bind(l); A.finalize();
    CHECK((A.bytes()[0] & 0x08) == 0);      // the W bit is clear
}

TEST_CASE("x86_64: branchNe emits cmp + jne (0F 85 rel32)") {
    HostAssembler A;
    Label l = A.newLabel();
    A.branchNe(R0, R1, l);
    A.bind(l); A.finalize();
    REQUIRE(A.size() == 9);
    CHECK(A.bytes()[4] == 0x85);            // jne rel32
}

#if defined(_WIN32)
TEST_CASE("x86_64: load8Idx(R0, R1, R2) is movzx r32, byte ptr [base+index] with SIB") {
    HostAssembler A;
    // Win64: R0=RCX (dst), R1=RDX (base), R2=R8 (index). r8 as the index needs REX.X. 42 = REX with X=1 0F B6 = MOVZX r32, r/m8 0C = ModR/M mod=00, reg=RCX(1), rm=100 (SIB follows) → 00 001 100 02 = SIB scale=0, index=r8&7=0, base=RDX(2)          → 00 000 010
    A.load8Idx(R0, R1, R2);
    const uint8_t want[] = {0x42, 0x0F, 0xB6, 0x0C, 0x02};
    checkBytes(A, want, sizeof(want));
}
#endif

TEST_CASE("x86_64: call() emits at least 14 vreg saves + a movabs + call rax + 14 restores") {
    HostAssembler A;
    A.prologue(1);            // reserve frame so call-save area is addressable via rsp
    const size_t before = A.size();
    A.call(R0, R1, R2, R3, reinterpret_cast<const void*>(0xDEADBEEFCAFEBABEULL));
    const size_t callLen = A.size() - before;
    REQUIRE_FALSE(A.overflowed());
    // Structural size sanity for the push/pop shape: 14 pushes (~22 B) + shadow sub (4, Win64) + 3 arg loads (8 each) + movabs (10) + call rax (2) + shadow add (4) + mov eax,eax (2) + result store (8) + 14 pops (~22) ≈ 100 B. The bound is a tripwire: an encoding that saves the pool with rsp-relative movs instead lands near 270 B, which overflows script buffers the arm64 backend fits comfortably.
    CHECK(callLen > 60);
    CHECK(callLen < 160);
    // The movabs sequence must contain 88 77 66 55 44 33 22 11 (little-endian of 0x11...88) somewhere.
    bool foundImm = false;
    for (size_t i = before; i + 10 <= A.size(); i++) {
        if (A.bytes()[i] == 0x48 && A.bytes()[i+1] == 0xB8 &&
            A.bytes()[i+2] == 0xBE && A.bytes()[i+3] == 0xBA &&
            A.bytes()[i+4] == 0xFE && A.bytes()[i+5] == 0xCA) {
            foundImm = true; break;
        }
    }
    CHECK(foundImm);
    // And a `call rax` (FF D0) must follow.
    bool foundCall = false;
    for (size_t i = before; i + 2 <= A.size(); i++) {
        if (A.bytes()[i] == 0xFF && A.bytes()[i+1] == 0xD0) { foundCall = true; break; }
    }
    CHECK(foundCall);
}

TEST_CASE("x86_64: the epilogue pops exactly what the prologue pushed, in reverse") {
    // A push/pop asymmetry returns through a stack that is off by a multiple of eight: the caller resumes at whatever that address holds, which presents as a crash nowhere near the JIT. Cheap to assert here and near-impossible to diagnose otherwise, so the test checks the property its name claims rather than that the two functions merely emitted something.
    //
    // push r64 = 50+r, pop r64 = 58+r, each optionally prefixed 41 (REX.B) for r8-r15. Scanning for those opcodes is enough: nothing else the frame code emits starts with 0x50-0x5F.
    auto pushPopSeq = [](const uint8_t* p, size_t n, uint8_t base) {
        std::vector<uint8_t> regs;
        for (size_t i = 0; i < n; i++) {
            bool high = false;
            if (p[i] == 0x41 && i + 1 < n) { high = true; i++; }
            if (p[i] >= base && p[i] < uint8_t(base + 8))
                regs.push_back(uint8_t((p[i] - base) + (high ? 8 : 0)));
            else if (high) i--;                  // 0x41 belonged to some other instruction
        }
        return regs;
    };

    HostAssembler A;
    A.prologue(0);
    const size_t prologueLen = A.size();
    A.epilogue();
    REQUIRE_FALSE(A.overflowed());

    const auto pushed = pushPopSeq(A.bytes(), prologueLen, 0x50);
    auto popped = pushPopSeq(A.bytes() + prologueLen, A.size() - prologueLen, 0x58);
    REQUIRE(pushed.size() >= 2);                 // rbp + at least one nonvolatile
    REQUIRE(popped.size() == pushed.size());
    std::reverse(popped.begin(), popped.end());
    for (size_t i = 0; i < pushed.size(); i++) {
        INFO("frame register ", i);
        CHECK(unsigned(pushed[i]) == unsigned(popped[i]));
    }
    CHECK(A.bytes()[A.size() - 1] == 0xC3);      // and it ends in ret
}

// ================================================================================================= ret =================================================================================================

TEST_CASE("x86_64: ret is a single 0xC3 byte") {
    HostAssembler A;
    A.ret();
    REQUIRE(A.size() == 1);
    CHECK(A.bytes()[0] == 0xC3);
}

// ================================================================================================= spillStore / spillLoad, frame-slot access via rbp with negative disp32 =================================================================================================

#if defined(_WIN32)
TEST_CASE("x86_64: spillStore(R0, 0) writes to slot 0 through rbp with disp32") {
    HostAssembler A;
    A.prologue(1);                            // slot 0 exists after prologue(1)
    const size_t after = A.size();
    A.spillStore(R0, 0);
    // Slots ASCEND with the index (the arg-block contract, see slotOffsetFromRbp). Slot 0 sits at the bottom of the fixed slot region: -(kNonvolSaveBytes + 8 * kMaxSpillSlots). The 56 is the Win64 nonvolatile save area (rbx+rdi+rsi+r12..r15, 7x8), a property of this ABI, so it stays written out here. The SLOT COUNT is derived rather than written out, because it has already moved once (kTotalSlots went 21 to 37) and this Windows-only test was the only thing reading it, so it went stale unnoticed until the next Windows run.
    constexpr int32_t kNonvolSaveBytes = 56;
    constexpr int32_t kSlot0Disp = -(kNonvolSaveBytes + 8 * int32_t(HostAssembler::kMaxSpillSlots));
    const uint32_t disp = uint32_t(kSlot0Disp);
    // mov [rbp + kSlot0Disp], rcx = 48 89 8D <disp32>. 48 = REX.W, 89 = MOV r/m64 r64, 8D = ModR/M mod=10 (disp32), reg=RCX(1), rm=RBP(5) -> 10 001 101.
    REQUIRE(A.size() - after == 7);
    CHECK(A.bytes()[after]     == 0x48);
    CHECK(A.bytes()[after + 1] == 0x89);
    CHECK(A.bytes()[after + 2] == 0x8D);
    CHECK(A.bytes()[after + 3] == uint8_t(disp & 0xFF));
    CHECK(A.bytes()[after + 4] == uint8_t((disp >> 8) & 0xFF));
    CHECK(A.bytes()[after + 5] == uint8_t((disp >> 16) & 0xFF));
    CHECK(A.bytes()[after + 6] == uint8_t((disp >> 24) & 0xFF));
}
#endif

// ================================================================================================= Branches, patchBranches is where rel32 fixups get resolved. =================================================================================================

TEST_CASE("x86_64: bind + branchIfZero(R0, L) patches to the correct rel32 after finalize") {
    HostAssembler A;
    Label l = A.newLabel();
    A.branchIfZero(R0, l);       // test + je rel32  = 3 + 6 = 9 bytes
    A.movImm(R1, 42);            // padding — 7 bytes; label lands at offset 16
    A.bind(l);
    A.finalize();
    // The je rel32 field starts at offset 3 + 2 = 5 (test bytes + je opcode). Target = 16. rel32 = target - (site + 6) = 16 - (3 + 6) = 7
    REQUIRE_FALSE(A.overflowed());
    // je rel32 is 0F 84 xx xx xx xx starting at offset 3.
    CHECK(A.bytes()[3] == 0x0F);
    CHECK(A.bytes()[4] == 0x84);
    int32_t rel;
    std::memcpy(&rel, A.bytes() + 5, 4);
    CHECK(rel == 7);
}

TEST_CASE("x86_64: callLabel(L) patches to rel32 target - (site + 5)") {
    HostAssembler A;
    Label l = A.newLabel();
    // Give kHostArgSlots reloads a home: prologue(1) reserves a frame so spillLoad inside callLabel doesn't set overflow_. The reloads emit BEFORE the E8 imm32, so the fixup site is at (prologue+spillLoad-block) offset, measure it dynamically.
    A.prologue(1);
    const size_t before = A.size();
    A.callLabel(l);
    // Locate the E8 opcode in the emitted bytes since spillLoad adds a variable-length preamble.
    size_t callAt = size_t(-1);
    for (size_t i = before; i + 5 <= A.size(); i++) {
        if (A.bytes()[i] == 0xE8) { callAt = i; break; }
    }
    REQUIRE(callAt != size_t(-1));
    // Bind the label after the call and finalize. rel is measured from the END of the E8 instruction to the label, so it counts whatever callLabel emits AFTER the call: the pool restore that delivers a script function's return value. Asserting rel == 0 pinned the old shape, where the call was the last thing emitted.
    const size_t afterCall = callAt + 5;
    A.bind(l);
    A.finalize();
    const int32_t expected = static_cast<int32_t>(A.size() - afterCall);
    int32_t rel;
    std::memcpy(&rel, A.bytes() + callAt + 1, 4);
    CHECK(rel == expected);
    CHECK(rel > 0);        // the restore is emitted between the call and the label
}

// ================================================================================================= End-to-end compile smoke: a script-to-script call, so callLabel, the per-function prologues and the parked-argument contract are all exercised together. Compile-only here; that the callee actually reaches the buffer and the controls is pinned by unit_moonlive_compiler's "a function the script calls can light pixels...", which EXECUTES it. =================================================================================================

TEST_CASE("x86_64: two sequential call-bearing loops stay under the density bound") {
    // The exact shape of unit_moonlive_compiler's "sequential loops reuse a name" case, the densest ordinary script the hand-sized test buffers hold on arm64, so it serves as this backend's density canary.
    const char* src =
        "class T { void tick() { "
        "for (int i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } "
        "for (int i = 0; i < 2; i = i + 1) { addLight(i, 1, 0); } "
        "} }\n";
    uint8_t out[2048];
    auto r = mm::moonlive::compileSource(src, mm::moonlive::lightBuiltins(),
                                          mm::moonlive::layoutSysVars(), out, sizeof(out),
                                          nullptr, mm::moonlive::lowerToBytes);
    INFO("error: ", std::string(r.error ? r.error : "(none)"), "  len=", r.len);
    CHECK(r.ok);
    // The density canary: measured 584 bytes with the push/pop call(), disp8 memory forms, and imm8 add/sub. The bound is headroom over that measurement, not a target, it exists to catch a size regression of the class that made the first-cut call() ~270 bytes and pushed ordinary two-call scripts past every hand-sized test buffer.
    CHECK(r.len <= 768);
}

TEST_CASE("x86_64: a class with a script-to-script call compiles") {
    const char* src =
        "class T {\n"
        "  byte level = 200;\n"
        "  void paint() { setRGB(1, level, 0, 0); }\n"
        "  void tick()  { setRGB(0, 7, 8, 9); paint(); }\n"
        "}\n";
    std::vector<uint8_t> out(mm::moonlive::codeCapFor(mm::moonlive::countTokens(src)));
    auto builtins = mm::moonlive::lightBuiltins();
    auto sys      = mm::moonlive::effectSysVars();
    auto r = mm::moonlive::compileSource(src, builtins, sys, out.data(), out.size(),
                                          nullptr, mm::moonlive::lowerToBytes);
    INFO("compile error: ", r.error ? r.error : "(none)");
    REQUIRE(r.ok);
    CHECK(r.len > 0);
    CHECK(r.entryCount == 2);
}

// The Q16.16 multiply. The sequence must survive d aliasing a or b, AND must not borrow any register the allocator can hand out.
//
// Two earlier versions failed that second rule: the first borrowed rax (vreg R13), the second r10/r11, which are R5/R6, the FIRST temps the allocator assigns, so it was strictly worse.
// Both produced a silently wrong number: `pop` restoring a stale value over the result when d aliased the scratch, or a source destroyed before it was read. The intermediate now lives on the STACK and only rax is touched, saved and restored around the whole sequence.
//
// Bytes verified against clang's assembly of the same instruction sequence.
TEST_CASE("x86_64: mulhi borrows no allocatable register") {
    HostAssembler a; a.mulhi(R0, R1, R2); a.finalize();
    const uint8_t* b = a.bytes();
    REQUIRE(a.size() >= 24);
    // No push/pop of r10 or r11 anywhere: those encode as 41 52 / 41 53 / 41 5a / 41 5b, and a 0x41 REX.B prefix on a push is the tell. Their absence is the property under test.
    for (size_t i = 0; i + 1 < a.size(); i++) {
        const bool pushPopR8plus = (b[i] == 0x41) &&
                                   ((b[i + 1] & 0xf8) == 0x50 || (b[i + 1] & 0xf8) == 0x58);
        CHECK_FALSE(pushPopR8plus);
    }
    CHECK(b[0] == 0x50);                             // opens by saving rax
    // The parked operand is discarded with `add rsp, 8`, a stack adjust, never a pop into some register. The sequence then ends either with `pop rax` (d is not rax) or a second adjust.
    bool sawAdjust = false;
    for (size_t i = 0; i + 3 < a.size(); i++)
        if (b[i] == 0x48 && b[i + 1] == 0x83 && b[i + 2] == 0xc4 && b[i + 3] == 0x08)
            sawAdjust = true;
    CHECK(sawAdjust);
}

// The destination aliasing each source, and rax itself. None may lose an operand or its result: with d == rax the saved value must NOT be popped back over the answer.
TEST_CASE("x86_64: mulhi handles every aliasing of its operands") {
    for (const auto& regs : {std::array<Reg, 3>{R0, R0, R1},    // d aliases a
                             std::array<Reg, 3>{R0, R1, R0},    // d aliases b
                             std::array<Reg, 3>{R0, R0, R0},    // all three
                             std::array<Reg, 3>{R5, R1, R2},    // d is a low temp (r10/r9)
                             std::array<Reg, 3>{R0, R5, R6},    // both sources are low temps
                             std::array<Reg, 3>{R13, R1, R2},   // d IS rax
                             std::array<Reg, 3>{R0, R13, R2},   // a is rax
                             std::array<Reg, 3>{R0, R1, R13}})  // b is rax
    {
        HostAssembler a; a.mulhi(regs[0], regs[1], regs[2]); a.finalize();
        CHECK(a.size() >= 24);
        CHECK(a.bytes()[0] == 0x50);                 // always saves rax first
        // The stack is always balanced: one save-push, one park-push, and two 8-byte adjustments (or one adjustment and one pop when the destination is not rax).
        int pushes = 0;
        for (size_t i = 0; i < a.size(); i++) if (a.bytes()[i] == 0x50) pushes++;
        CHECK(pushes >= 2);
    }
}

}  // namespace

#else   // not x86_64 host

#include <doctest.h>
TEST_CASE("x86_64 codegen test skipped: not an x86_64 host") { CHECK(true); }

#endif  // (defined(__x86_64__) || defined(_M_X64)) && !MM_MOONLIVE_FORCE_NO_HOST_JIT
