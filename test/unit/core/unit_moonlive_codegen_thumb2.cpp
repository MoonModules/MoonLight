/// @module MoonLive

/// The Thumb-2 (Teensy 4.1) backend's emitted code, checked on the development machine; moonlive_device_codegen.inc says why this exists.

#include "doctest.h"
#include "moonlive_script_wrap.h"

// System and standard headers first, at global scope, so the namespace below only ever wraps the backend.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// The shared front end at real `mm::moonlive` scope, before the backend sets the IR headers' include guards.
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveSpill.h"

namespace mm_thumb2_backend {
// The IR, the spill pass and the platform seam are shared, so only the ISA-specific classes end up local to this namespace.
namespace mm { using namespace ::mm; using namespace ::mm::moonlive;
               namespace moonlive { using namespace ::mm::moonlive;
                                    using ::mm::moonlive::spillToBudget; } }
#define __ARM_ARCH_7EM__ 1
#include "platform/teensy/moonlive_asm_thumb2.h"
#include "platform/teensy/moonlive_asm_thumb2.cpp"
#undef __ARM_ARCH_7EM__
}  // namespace mm_thumb2_backend

#define MM_ISA_NAME "Thumb-2"
// Golden values, recorded from this backend after its disassembly was read against arm-none-eabi-objdump. See the .inc for what they are and are not.
#define MM_GOLD_GRID_LEN     266u
#define MM_GOLD_FX_LEN       144u
#define MM_GOLD_FILLLOOP_LEN 328u
#define MM_GOLD_FXLOOP_LEN   236u
#define MM_GOLD_FXLOOP_HASH  3739810632u
#define MM_GOLD_FX_HASH      1651227367u
// `mov r0, rM`, the 16-bit MOV (register) with Rd = r0. Rm is the allocator's choice, so it is masked out.
#define MM_ISA_RET_WRITES_RETREG(p) (((uint32_t((p)[0]) | (uint32_t((p)[1]) << 8)) & 0xff87u) == 0x4600u)
// The caller-side stash, `mov r12, r0`, which parks the callee's value past the pool restore.
#define MM_ISA_STASHES_RESULT(p) ((p)[0] == 0x84u && (p)[1] == 0x46u)
#define MM_ISA_RET_STRIDE 2
#define MM_ISA_LOWER mm_thumb2_backend::mm::moonlive::lowerToBytes
#define MM_ISA_ASM   mm_thumb2_backend::mm::moonlive::Thumb2Assembler
#include "moonlive_device_codegen.inc"

namespace {
using Thumb2 = mm_thumb2_backend::mm::moonlive::Thumb2Assembler;
using mm_thumb2_backend::mm::moonlive::R0;
using mm_thumb2_backend::mm::moonlive::R1;
using mm_thumb2_backend::mm::moonlive::R2;
using mm_thumb2_backend::mm::moonlive::R5;
using mm_thumb2_backend::mm::moonlive::R6;
using mm_thumb2_backend::mm::moonlive::R7;

uint16_t halfword(const uint8_t* p, size_t at) { return uint16_t(p[at] | (p[at + 1] << 8)); }
// A 32-bit instruction as the ARM manual prints it, first halfword on top.
uint32_t wide(const uint8_t* p, size_t at) { return (uint32_t(halfword(p, at)) << 16) | halfword(p, at + 2); }
// The imm16 a MOVW or MOVT carries, imm4:i:imm3:imm8.
uint16_t movImm16(uint32_t ins) {
    return uint16_t(((ins >> 16) & 0xf) << 12 | ((ins >> 26) & 1) << 11 | ((ins >> 12) & 7) << 8 | (ins & 0xff));
}
}  // namespace

// The emitted block lives in OCRAM and the built-ins in flash or ITCM, further apart than BL reaches, so BL to a host address would land in the wrong place.
TEST_CASE("a Thumb-2 host call loads the address into r12 and calls it with BLX, never BL") {
    static const uint32_t kFakeFn = 0x6001a2b5u;   // a flash address with the Thumb bit set
    Thumb2 a(128);
    a.call(R5, R5, R6, R7, reinterpret_cast<const void*>(uintptr_t(kFakeFn)));
    const uint8_t* p = a.bytes();
    size_t blx = 0;
    bool found = false;
    for (size_t i = 0; i + 2 <= a.size(); i += 2)
        if (halfword(p, i) == 0x47e0u) { blx = i; found = true; break; }   // blx r12
    REQUIRE(found);
    REQUIRE(blx >= 8);
    const uint32_t movw = wide(p, blx - 8), movt = wide(p, blx - 4);
    CHECK((movw & 0xfbf08f00u) == 0xf2400c00u);    // movw r12
    CHECK((movt & 0xfbf08f00u) == 0xf2c00c00u);    // movt r12
    // The address passes through unchanged, Thumb bit included, since a C function pointer already carries it.
    CHECK(movImm16(movw) == (kFakeFn & 0xffffu));
    CHECK(movImm16(movt) == (kFakeFn >> 16));
    // No BL anywhere: first halfword 11110, second halfword 11x1.
    for (size_t i = 0; i + 4 <= a.size(); i += 2)
        CHECK_FALSE(((halfword(p, i) & 0xf800u) == 0xf000u && (halfword(p, i + 2) & 0xd000u) == 0xd000u));
}

// The AAPCS passes the fifth argument on the stack, so a prologue that loaded it before saving r4, or from the wrong offset, hands every LoadCtrl a wild pointer.
TEST_CASE("a Thumb-2 prologue saves r4-r11 and lr, then loads the fifth argument from the caller's sp") {
    Thumb2 a(128);
    a.prologue(mm::moonlive::kTotalSlots);
    const uint8_t* p = a.bytes();
    REQUIRE(a.size() == 14);
    CHECK(wide(p, 0) == 0xe92d4ff0u);                         // push.w {r4-r11, lr}
    const uint32_t sub = wide(p, 4);
    CHECK((sub & 0xfbff8f00u) == 0xf2ad0d00u);                // subw sp, sp, #slots
    const uint32_t slotBytes = ((sub >> 26) & 1) << 11 | ((sub >> 12) & 7) << 8 | (sub & 0xff);
    CHECK((36 + slotBytes) % 8 == 0);                         // sp 8-byte aligned at every call
    CHECK(halfword(p, 8) == 0x466fu);                         // mov r7, sp
    // ldr.w r4, [r7, #frame]: the saved set plus the slots puts it at the caller's sp.
    CHECK(wide(p, 10) == (0xf8d74000u | (36 + slotBytes)));
}

// SMMUL and MUL share a first-halfword prefix; the wrong one returns the low word, off by a factor of 65536.
TEST_CASE("Thumb-2 mulhi emits SMMUL, the signed high word, where mulReg emits MUL") {
    Thumb2 m(64); m.mulReg(R0, R1, R2);
    Thumb2 h(64); h.mulhi(R0, R1, R2);
    REQUIRE(m.size() == 4);
    REQUIRE(h.size() == 4);
    CHECK(wide(m.bytes(), 0) == 0xfb01f002u);                 // mul r0, r1, r2
    CHECK(wide(h.bytes(), 0) == 0xfb51f002u);                 // smmul r0, r1, r2
}

// ASR and LSR differ in one field; the logical one turns every negative fixed value into a huge positive int.
TEST_CASE("Thumb-2 sarImm emits ASR where shrImm emits LSR") {
    Thumb2 l(64); l.shrImm(R0, R1, 16);
    Thumb2 r(64); r.sarImm(R0, R1, 16);
    REQUIRE(l.size() == 4);
    REQUIRE(r.size() == 4);
    CHECK(wide(l.bytes(), 0) == 0xea4f4011u);                 // lsr.w r0, r1, #16
    CHECK(wide(r.bytes(), 0) == 0xea4f4021u);                 // asr.w r0, r1, #16
}
