/// @module MoonLive

/// The RISC-V (ESP32-P4) backend's emitted code, checked on the development machine.
/// See moonlive_device_codegen.inc for why this exists and unit_moonlive_codegen_xtensa.cpp for how the target guard is defined so the real emitter runs on this host.

#include "doctest.h"
#include "moonlive_script_wrap.h"

// System and standard headers FIRST, at global scope. The backend below is wrapped in a namespace, and anything it includes for the first time would otherwise be declared INSIDE that namespace, which under GCC breaks both `std::memcpy` (not found where the backend calls it) and the system `ssize_t` typedef that <cstdio> drags in. Including them here means the namespace only ever wraps OUR code, which is all it is meant to wrap.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>


// The SHARED front end first, at real `mm::moonlive` scope: the backend below drags in the IR headers, and once their include guards are set the compiler header would otherwise resolve its types inside the wrapper namespace instead.
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveSpill.h"

#include "platform/platform.h"   // at global scope first, so the assembler header's own include inside the namespace below is a repeat

namespace mm_riscv_backend {
// The IR, the spill pass and the platform seam are SHARED, one definition each in the binary. Pull them into scope so the backend's unqualified references resolve to those, and only the ISA-specific classes below end up local to this namespace.
namespace mm { using namespace ::mm; using namespace ::mm::moonlive;
               namespace moonlive { using namespace ::mm::moonlive;
                                    using ::mm::moonlive::spillToBudget; } }
#define __riscv 1
#include "platform/esp32/moonlive_asm_riscv.h"
#include "platform/esp32/moonlive_asm_riscv.cpp"
#undef __riscv
}  // namespace mm_riscv_backend

#define MM_ISA_NAME "RISC-V"
// Golden values, recorded from this backend. See the .inc for what they are and are not. All six moved on 2026-08-31, by one word per CONDITIONAL BRANCH: each is now emitted as an inverted short branch over a `jal` rather than a bare B-type. A B-type reaches +/-4 KB, and metal.mle compiles to 5652 bytes, so its loop branches fell outside and the patcher truncated the offset to 13 bits: the branch landed on 0x230c, and an S31 panicked with an Illegal instruction while the same script ran correctly on the host. The uniform two-word form costs 0.9% of total emitted code (468 bytes across the 17 shipped effects) and removes the limit.
#define MM_GOLD_GRID_LEN  412u     // +24: six branches
#define MM_GOLD_FX_LEN    164u     // +4:  one branch
#define MM_GOLD_FILLLOOP_LEN 360u  // +24: fits on every backend since the host args moved to the frame
#define MM_GOLD_FXLOOP_LEN  252u   // +16: four branches
// The two hashes moved 2026-09-04 when kMaxLocals went 16 -> 32 (the first volumetric script needed 19 slots). A wider frame changes the prologue's reserve and every slot offset, so identical source emits different bytes: a frame-offset change is exactly what this hash exists to surface, and it did. The LENGTHS are unchanged, which is the evidence it is offsets rather than different code.
#define MM_GOLD_FXLOOP_HASH 3140471189u
#define MM_GOLD_FX_HASH   2676401519u
// `mv a0, xN` is `addi a0, xN, 0`: opcode 0x13, funct3 0, rd = x10 (a0), imm 0. rs1 is the allocator's choice, so it is masked out; rd and the immediate are the contract.
#define MM_ISA_RET_WRITES_RETREG(p) \
    (((uint32_t((p)[0]) | (uint32_t((p)[1]) << 8) | (uint32_t((p)[2]) << 16) | \
       (uint32_t((p)[3]) << 24)) & 0xfff07fffu) == 0x00000513u)
// The caller-side stash: `mv t6, a0`, i.e. `addi t6, a0, 0`. rd = x31 (t6), rs1 = x10 (a0), imm 0. The callee's value is parked past the pool restore, which would otherwise overwrite a0.
#define MM_ISA_STASHES_RESULT(p) \
    ((uint32_t((p)[0]) | (uint32_t((p)[1]) << 8) | (uint32_t((p)[2]) << 16) | \
      (uint32_t((p)[3]) << 24)) == 0x00050f93u)
#define MM_ISA_RET_STRIDE 4
#define MM_ISA_LOWER mm_riscv_backend::mm::moonlive::lowerToBytes
// The assembler type itself, so the stack-budget check can measure the object the compile path puts on a 12 KB task rather than re-deriving its layout from the constants.
#define MM_ISA_ASM   mm_riscv_backend::mm::moonlive::RiscvAssembler
#include "moonlive_device_codegen.inc"




// The Q16.16 primitives. mulh is mul with funct3 = 1, a single bit apart from the multiply the engine already emits, which is exactly why it is worth pinning: the wrong funct3 silently returns the LOW word, so a fixed multiply would be off by a factor of 65536 rather than fail.
TEST_CASE("RISC-V mulhi emits mulh, one funct3 from mul") {
    using Asm = mm_riscv_backend::mm::moonlive::RiscvAssembler;
    using mm_riscv_backend::mm::moonlive::R0;
    using mm_riscv_backend::mm::moonlive::R1;
    using mm_riscv_backend::mm::moonlive::R2;
    Asm m(64); m.mulReg(R0, R1, R2);
    Asm h(64); h.mulhi(R0, R1, R2);
    REQUIRE(m.size() == 4);
    REQUIRE(h.size() == 4);
    const uint32_t wm = uint32_t(m.bytes()[0]) | (uint32_t(m.bytes()[1]) << 8)
                      | (uint32_t(m.bytes()[2]) << 16) | (uint32_t(m.bytes()[3]) << 24);
    const uint32_t wh = uint32_t(h.bytes()[0]) | (uint32_t(h.bytes()[1]) << 8)
                      | (uint32_t(h.bytes()[2]) << 16) | (uint32_t(h.bytes()[3]) << 24);
    CHECK(((wm >> 12) & 7u) == 0u);                // mul:  funct3 0
    CHECK(((wh >> 12) & 7u) == 1u);                // mulh: funct3 1
    CHECK((wm & 0xfe00707fu) != (wh & 0xfe00707fu));
}

// srai sets bit 30 of the immediate field; without it the shift is srli and a negative fixed value converts to a huge positive int instead of the number the script wrote.
TEST_CASE("RISC-V sarImm sets the arithmetic-shift bit that srli lacks") {
    using Asm = mm_riscv_backend::mm::moonlive::RiscvAssembler;
    using mm_riscv_backend::mm::moonlive::R0;
    using mm_riscv_backend::mm::moonlive::R1;
    Asm l(64); l.shlImm(R0, R1, 16);
    Asm r(64); r.sarImm(R0, R1, 16);
    const uint32_t wl = uint32_t(l.bytes()[0]) | (uint32_t(l.bytes()[1]) << 8)
                      | (uint32_t(l.bytes()[2]) << 16) | (uint32_t(l.bytes()[3]) << 24);
    REQUIRE(r.size() == 4);                        // read as four bytes below, so say so first
    const uint32_t wr = uint32_t(r.bytes()[0]) | (uint32_t(r.bytes()[1]) << 8)
                      | (uint32_t(r.bytes()[2]) << 16) | (uint32_t(r.bytes()[3]) << 24);
    CHECK((wl & 0x7fu) == 0x13u);                  // OP-IMM
    CHECK(((wl >> 12) & 7u) == 1u);                // slli funct3 1
    CHECK((wr & 0x7fu) == 0x13u);                  // OP-IMM for the shift too, not just the shl
    CHECK(((wr >> 12) & 7u) == 5u);                // srai/srli funct3 5
    CHECK((wr & (1u << 30)) != 0u);                // the bit that makes it ARITHMETIC
    CHECK(((wr >> 20) & 0x1fu) == 16u);            // the shift amount
}
