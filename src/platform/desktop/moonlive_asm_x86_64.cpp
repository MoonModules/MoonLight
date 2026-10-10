#include "moonlive_asm_host.h"
#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend

#include <cstring>

/// @defgroup moonlive_asm_x86_64 MoonLive x86-64 assembler
/// Each named instruction encoded once, composed by the shared lowering.
///
/// One architecture per file, self-guarding, the shape the device backends use; it carries its own binding of the shared walk to this assembler.
///
/// @moreinfo
///
/// ## A second axis the other host backend does not have
///
/// The two calling conventions disagree on the argument registers, the callee-saved set and the reserved stack space.
/// So those branches nest inside the architecture guard rather than splitting the file again, being a convention difference within one instruction set.
///
/// ## What the two conventions disagree on
///
/// The argument registers, the callee-saved set, and whether the caller must reserve a shadow area below the call site.
/// The encodings themselves are identical, so only the register map and the call's argument shuffle differ, switched in one place each.
///
/// ## Design choices worth stating
///
/// The frame is always emitted, unlike the sibling backend which skips it when nothing spills.
/// Registers the map uses must be saved and the shadow area reserved for any built-in call, and without a frame there is nowhere to put them.
/// It costs a short prologue and a fixed stack window per invocation, run once per frame, which is negligible against the tick budget.
///
/// The accumulator doubles as the return register, so a call stashes the return before restoring the pool and only then moves it into place.
/// Conditional branches always take the wide relative form, one width so the patcher never has to choose.
/// That wastes a few bytes on short branches and saves the range-check retry and its correctness burden.
///
/// ## The call pushes rather than moves
///
/// Pushing each register costs one or two bytes against eight for a stack-relative move, and the pool is fourteen registers while a script calls built-ins inside its light loop.
/// So the call's encoded size dominates a call-dense script, at roughly a hundred bytes per call instead of nearly three hundred.
/// That is the difference between fitting the buffers the sibling backend fits and overflowing them, which a density check in the tests holds.
///
/// ## Slots ascend, and the reason is a contract
///
/// A call hands a built-in the address of its first argument slot and the callee reads upward from there.
/// So consecutive slot indices must be consecutive ascending memory; reversing them makes every non-inlined built-in receive its arguments backwards while everything inlined keeps working.
/// They are addressed from the frame pointer rather than the stack pointer, so they survive the call moving it.
///
/// ## Narrow arguments must be zero-extended
///
/// This architecture leaves the upper bits of a register holding a narrower argument undefined, and a caller may leave anything above.
/// The intermediate form then spills each argument to a slot as a full-width store and the loop arithmetic multiplies a full-width view.
/// So a dirty channel count reaches the write as nonsense and the address lands outside the buffer.
/// The sibling architecture's convention extends them for us, which is why only this backend needs it.
///
/// ## The high multiply is alias-safe by construction
///
/// Both sources are read before the destination is written, and the only register touched is the accumulator, saved and restored around the sequence.
/// The intermediate lives on the stack rather than in a borrowed register, because no register here is safe to borrow.
/// The accumulator is itself a virtual register, and the obvious temporaries are the first ones the allocator hands out.
/// Two earlier attempts each picked a register that turned out to be allocatable, and each produced a silently wrong number rather than a crash.

namespace mm::moonlive {

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

// The register map and the call's shuffle are the only things the two conventions change here: @xref{what-the-two-conventions-disagree-on|what they disagree on} and @xref{design-choices-worth-stating|the choices made}.

// x86-64 machine register numbers, nested in a namespace so the RAX/RCX/... names cannot collide with the vreg enum's R0..R13 in the enclosing mm::moonlive scope.
namespace x64 {
enum : uint8_t {
    RAX = 0,  RCX = 1,  RDX = 2,  RBX = 3,
    RSP = 4,  RBP = 5,  RSI = 6,  RDI = 7,
    R8  = 8,  R9  = 9,  R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};
} // namespace x64
// The rest of this branch qualifies register names with `x64::` to avoid the collision with the vreg enum Reg::R8..R13 in the enclosing mm::moonlive scope. arm64 branch above never sees these constants; the #elif isolates them to the x86_64 impl.

#if defined(_WIN32)
// Microsoft x64 (Windows).
// Args in rcx/rdx/r8/r9; arg 5 arrives on the stack at [rbp+48] and is loaded into rdi (R4 = kArg4) once in prologue.
// Nonvolatiles rbx, rdi, rsi, r12-r15 are saved by prologue. rsp and rbp are reserved (rbp = frame pointer).
static constexpr uint8_t kX64Reg[kRegCount] = {
    x64::RCX, x64::RDX, x64::R8,  x64::R9,  x64::RDI,   // R0..R4  = kArg0..kArg4
    x64::R10, x64::R11,                                 // R5..R6  = volatile scratch
    x64::RBX, x64::R12, x64::R13, x64::R14, x64::R15,   // R7..R11 = nonvolatile
    x64::RSI,                                           // R12     = nonvolatile
    x64::RAX,                                           // R13     = volatile, holds every return
};
static constexpr uint8_t  kNonvolSaveBytes = 56;   // rbx+rdi+rsi+r12..r15 (7×8)
static constexpr int32_t  kArg5Offset      = 48;   // ret-addr 8 + saved-rbp 8 + shadow 32
static constexpr uint16_t kShadowSpace     = 32;   // Win64 shadow for OUR callees
#else
// System V (Linux, Intel macOS).
// Args in rdi/rsi/rdx/rcx/r8/r9, all five kArg registers arrive in registers.
// Nonvolatiles rbx, r12-r15 (5) are saved by prologue.
static constexpr uint8_t kX64Reg[kRegCount] = {
    x64::RDI, x64::RSI, x64::RDX, x64::RCX, x64::R8,    // R0..R4  = kArg0..kArg4 (all in registers)
    x64::R9,  x64::R10, x64::R11,                       // R5..R7  = volatile scratch
    x64::RBX, x64::R12, x64::R13, x64::R14, x64::R15,   // R8..R12 = nonvolatile
    x64::RAX,                                           // R13     = volatile, holds every return
};
static constexpr uint8_t  kNonvolSaveBytes = 40;   // rbx+r12..r15 (5×8)
static constexpr uint16_t kShadowSpace     = 0;    // SysV has no shadow-space obligation
#endif

// Bounds-checked vreg→machine lookup.
// Same defense the arm64 backend uses: a mapping/reservation disagreement is a bug, but reading past the map returns undefined memory, which would name a wrong-and-unrelated register in the emitted byte.
// Clamp to the last entry so the failure is wrong-but-safe.
static uint8_t xr(Reg r) { return kX64Reg[r < kRegCount ? r : kRegCount - 1]; }

// rax must not sit inside the vreg pool AND be the call-target scratch at once, or a `movabs rax, fn` in call() would clobber whatever vreg R13 held before the pool-save happened.
// Rax is R13 by design; call() saves it before it becomes scratch.
// Assert it explicitly, matching the arm64 scratch check.
static_assert(kX64Reg[kRegCount - 1] == x64::RAX,
              "rax must be the last vreg — call() relies on saving it before using it as the fn-target scratch");


// Emit bytes through the existing path, which owns the bounds check and the overflow flag; instructions here are variable-length, so a small local buffer holds any single one.
// There is no fixed-word emitter on this architecture: the header declares one for the sibling, where a fixed word is the natural unit.
// And leaving it undefined here is what says it has no meaning.
void HostAssembler::emitBytes(const uint8_t* p, size_t n) {
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}

// x86-64 encoding primitives, all inline in this file since they're one-liners used only here.
// REX prefix: W=1 for 64-bit operand size; R/X/B are the high-register bits for the reg / index / base fields.
// Emit only when at least one bit is needed OR when 64-bit operand size is required.
static inline uint8_t rex_(bool w, bool r_high, bool x_high, bool b_high) {
    return 0x40 | (w ? 0x08 : 0) | (r_high ? 0x04 : 0) | (x_high ? 0x02 : 0) | (b_high ? 0x01 : 0);
}
static inline uint8_t modrm_(uint8_t mod, uint8_t reg, uint8_t rm) {
    return uint8_t((mod & 3) << 6) | uint8_t((reg & 7) << 3) | uint8_t(rm & 7);
}
static inline uint8_t sib_(uint8_t scale, uint8_t index, uint8_t base) {
    return uint8_t((scale & 3) << 6) | uint8_t((index & 7) << 3) | uint8_t(base & 7);
}

// Mov r64, r64  (REX.W 89 /r), the workhorse register-copy instruction.
static void emitMovRegReg(HostAssembler* A, uint8_t dst, uint8_t src) {
    uint8_t b[3] = {
        rex_(true, src >= 8, false, dst >= 8),
        0x89,
        modrm_(0b11, src & 7, dst & 7),
    };
    A->emitBytes(b, 3);
}

// The shared body of the three base-plus-displacement forms, using the short displacement when it fits a signed byte and the wide one otherwise.
// The short form saves three bytes per access and the frame's hot offsets all fit it, and density matters because spill traffic is most of what a script emits.
// Two base registers require an extra addressing byte, which the encoder adds.
static void emitMemDispOp(HostAssembler* A, uint8_t opcode, uint8_t reg, uint8_t base,
                          int32_t disp) {
    const bool needsSIB = ((base & 7) == x64::RSP);
    const bool disp8 = (disp >= -128 && disp <= 127);
    uint8_t b[8]; size_t n = 0;
    b[n++] = rex_(true, reg >= 8, false, base >= 8);
    b[n++] = opcode;
    b[n++] = modrm_(disp8 ? 0b01 : 0b10, reg & 7, needsSIB ? 0b100 : (base & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, base & 7);        // scale=0, index=none (0b100)
    b[n++] = uint8_t(disp);
    if (!disp8) {
        b[n++] = uint8_t(disp >> 8); b[n++] = uint8_t(disp >> 16); b[n++] = uint8_t(disp >> 24);
    }
    A->emitBytes(b, n);
}

// Mov r64, [base + disp]  (REX.W 8B /r), the workhorse load-from-frame instruction.
static void emitMovRegMemDisp(HostAssembler* A, uint8_t dst, uint8_t base, int32_t disp) {
    emitMemDispOp(A, 0x8B, dst, base, disp);
}

// Mov [base + disp], r64  (REX.W 89 /r)
static void emitMovMemDispReg(HostAssembler* A, uint8_t base, int32_t disp, uint8_t src) {
    emitMemDispOp(A, 0x89, src, base, disp);
}

// LEA r64, [base + disp]  (REX.W 8D /r), the address-arithmetic instruction, used by slotAddr to hand a callee the address of a frame slot without materializing it via a mov.
static void emitLeaRegMemDisp(HostAssembler* A, uint8_t dst, uint8_t base, int32_t disp) {
    emitMemDispOp(A, 0x8D, dst, base, disp);
}

// Push r64  (50+r; REX.B for r8-r15). One byte per non-high register, the compact prologue path.
static void emitPushReg(HostAssembler* A, uint8_t reg) {
    uint8_t b[2]; size_t n = 0;
    if (reg >= 8) b[n++] = rex_(false, false, false, true);
    b[n++] = uint8_t(0x50 + (reg & 7));
    A->emitBytes(b, n);
}
// Pop r64  (58+r; REX.B for r8-r15).
static void emitPopReg(HostAssembler* A, uint8_t reg) {
    uint8_t b[2]; size_t n = 0;
    if (reg >= 8) b[n++] = rex_(false, false, false, true);
    b[n++] = uint8_t(0x58 + (reg & 7));
    A->emitBytes(b, n);
}

// Sub rsp, imm  (REX.W 83 /5 ib for a byte-sized amount, call()'s 32-byte shadow, 81 /5 id for the frame reservation, which exceeds 127).
static void emitSubRspImm32(HostAssembler* A, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        uint8_t b[4] = {
            rex_(true, false, false, false), 0x83, modrm_(0b11, 5, x64::RSP), uint8_t(imm),
        };
        A->emitBytes(b, 4);
        return;
    }
    uint8_t b[7] = {
        rex_(true, false, false, false),
        0x81,
        modrm_(0b11, 5, x64::RSP),                       // /5 = SUB, r/m = rsp
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    A->emitBytes(b, 7);
}

// (No emitAddRspImm32 helper: the epilogue uses `lea rsp, [rbp - kNonvolSaveBytes]` to unwind the frame in one instruction, which also drops anything call() left on the stack.)

// The call frame, laid out from the stack pointer upward.
// The shadow area callees are owed, then the outgoing argument, then the spill slots below the saved registers, then the saved frame pointer and the return address.
// Slots ascend with their index: @xref{slots-ascend-and-the-reason-is-a-contract|the contract that requires it}.
// The call saves the register pool below this frame with pushes, and nothing here is reserved for it beyond the shadow area.

// The pool is saved with pushes rather than moves: @xref{the-call-pushes-rather-than-moves|the density that buys}. A slot's offset is negative and ascends with its index, the region sized uniformly whatever the script uses, against a formula that would need the count at every call site.
static inline int32_t slotOffsetFromRbp(uint8_t slot) {
    return -int32_t(kNonvolSaveBytes) - 8 * int32_t(kTotalSlots - slot);
}

void HostAssembler::prologue(uint8_t slots) {
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // Unlike arm64, x86-64 ALWAYS emits a frame, the reasons are stated at the top of this branch (nonvols need saving, shadow space required by Win64).
    emitPushReg(this, x64::RBP);                              // push rbp
    emitMovRegReg(this, x64::RBP, x64::RSP);                       // mov rbp, rsp

    // Save nonvolatiles this ABI's vreg map uses.
#if defined(_WIN32)
    // Order: rbx, rdi, rsi, r12, r13, r14, r15, matches the offsets used at epilogue's pop.
    emitPushReg(this, x64::RBX);
    emitPushReg(this, x64::RDI);
    emitPushReg(this, x64::RSI);
    emitPushReg(this, x64::R12);
    emitPushReg(this, x64::R13);
    emitPushReg(this, x64::R14);
    emitPushReg(this, x64::R15);
#else
    emitPushReg(this, x64::RBX);
    emitPushReg(this, x64::R12);
    emitPushReg(this, x64::R13);
    emitPushReg(this, x64::R14);
    emitPushReg(this, x64::R15);
#endif

    // Reserve the outgoing area and the spill slots, aligned so the stack pointer is correctly aligned before any call. Both conventions push an odd number of registers here, which is what makes the frame size land where it does.
    uint32_t needed = uint32_t(kShadowSpace) + 8   // outgoing: callee shadow + arg-5 slot
                    + uint32_t(kTotalSlots) * 8;   // full slot region — slotOffsetFromRbp is fixed
    // Bring rsp back to 16-alignment: needed + (rsp offset mod 16) must be 0 mod 16. After the 7 (or 5) pushes above, rsp % 16 == 8, so we need frame % 16 == 8.
    if ((needed % 16) != 8) needed += (8 - (needed % 16) + 16) % 16;
    frameBytes_ = uint16_t(needed);
    emitSubRspImm32(this, int32_t(needed));

#if defined(_WIN32)
    // Load kArg4 (arg 5 = the ctrls arena pointer) from the caller's stack into R4 (= rdi). arg 5 lives at [rbp + kArg5Offset].
    // Under Win64 it's on the caller's stack because only the first four args ride registers.
    // SysV skips this because r8 IS the arg-5 register and is already in place as R4.
    emitMovRegMemDisp(this, x64::RDI, x64::RBP, kArg5Offset);
#endif

    // Zero-extend the narrow arguments, required on both conventions, which is why it sits outside the switch above: @xref{narrow-arguments-must-be-zero-extended|what a dirty one does}. The pointers are full width by definition and need nothing.
#if defined(_WIN32)
    { uint8_t b[2] = {0x89, 0xD2};             emitBytes(b, 2); }  // mov   edx, edx   (nLights)
    { uint8_t b[4] = {0x4D, 0x0F, 0xB6, 0xC0}; emitBytes(b, 4); }  // movzx r8,  r8b   (cpl)
    { uint8_t b[3] = {0x45, 0x89, 0xC9};       emitBytes(b, 3); }  // mov   r9d, r9d   (t)
#else
    { uint8_t b[2] = {0x89, 0xF6};             emitBytes(b, 2); }  // mov   esi, esi   (nLights)
    { uint8_t b[4] = {0x48, 0x0F, 0xB6, 0xD2}; emitBytes(b, 4); }  // movzx rdx, dl    (cpl)
    { uint8_t b[2] = {0x89, 0xC9};             emitBytes(b, 2); }  // mov   ecx, ecx   (t)
#endif
}

void HostAssembler::epilogue() {
    // Restore rsp to point AT the last-pushed nonvol. lea rsp, [rbp - kNonvolSaveBytes] , one instruction, drops any adjustment call() made.
    emitLeaRegMemDisp(this, x64::RSP, x64::RBP, -int32_t(kNonvolSaveBytes));
    // Pops in reverse push order.
#if defined(_WIN32)
    emitPopReg(this, x64::R15);
    emitPopReg(this, x64::R14);
    emitPopReg(this, x64::R13);
    emitPopReg(this, x64::R12);
    emitPopReg(this, x64::RSI);
    emitPopReg(this, x64::RDI);
    emitPopReg(this, x64::RBX);
#else
    emitPopReg(this, x64::R15);
    emitPopReg(this, x64::R14);
    emitPopReg(this, x64::R13);
    emitPopReg(this, x64::R12);
    emitPopReg(this, x64::RBX);
#endif
    emitPopReg(this, x64::RBP);
    ret();
}

void HostAssembler::spillStore(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emitMovMemDispReg(this, x64::RBP, slotOffsetFromRbp(slot), xr(r));
}
void HostAssembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emitMovRegMemDisp(this, xr(r), x64::RBP, slotOffsetFromRbp(slot));
}
void HostAssembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emitLeaRegMemDisp(this, xr(d), x64::RBP, slotOffsetFromRbp(slot));
}

// --- movs ---------------------------------------------------------------------------------------

// mov r64, imm64  (movabs, REX.W B8+r imm64), 10 bytes. The direct analog of arm64's movz + 3×movk sequence for materializing a full-width pointer.
void HostAssembler::movPtr(Reg d, const void* p) {
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    const uint8_t dst = xr(d);
    uint8_t b[10];
    b[0] = rex_(true, false, false, dst >= 8);
    b[1] = uint8_t(0xB8 + (dst & 7));
    for (int i = 0; i < 8; i++) b[2 + i] = uint8_t(addr >> (i * 8));
    emitBytes(b, 10);
}

// mov r64, imm32 sign-extended  (REX.W C7 /0 id), the 32-bit-immediate form.
// Handles both positive and negative int32_t immediates uniformly (sign-extended to 64 bits by the CPU).
// Wider than the imm16-based arm64 movz/movn but simpler: one instruction covers the whole int32 range, so there is no "does it fit in 16 bits?" check.
void HostAssembler::movImm(Reg d, int32_t imm) {
    const uint8_t dst = xr(d);
    uint8_t b[7] = {
        rex_(true, false, false, dst >= 8),
        0xC7,
        modrm_(0b11, 0, dst & 7),                        // /0 = MOV r/m64, imm32
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    emitBytes(b, 7);
}

// mov r64, r64 , the direct copy. Same encoding as spillStore/Load's core, exposed for the lowering (movReg is emitted anywhere the IR wants to alias one vreg to another).
void HostAssembler::movReg(Reg d, Reg a) {
    if (d == a) return;                                  // no-op copy, elide
    emitMovRegReg(this, xr(d), xr(a));
}

// The System V return register is rax. emitMovRegReg emits a 64-bit move (REX.W), so a returned pointer survives whole: tags() hands back a string address, not a number.
void HostAssembler::retValue(Reg a) {
    if (xr(a) == x64::RAX) return;                       // already there
    emitMovRegReg(this, x64::RAX, xr(a));
}

// --- arithmetic ---------------------------------------------------------------------------------

// add r64, imm  (REX.W 83 /0 ib when the immediate fits a signed byte, the common case: loop steps and channel offsets are ±1 or ±2, REX.W 81 /0 id otherwise). Sign-extended by the CPU.
void HostAssembler::addImm(Reg d, Reg a, int32_t imm) {
    // d = a + imm  →  if d != a, first mov d, a. Then add d, imm.
    if (d != a) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    if (imm >= -128 && imm <= 127) {
        uint8_t b[4] = {
            rex_(true, false, false, dst >= 8),
            0x83,
            modrm_(0b11, 0, dst & 7),                    // /0 = ADD
            uint8_t(imm),
        };
        emitBytes(b, 4);
        return;
    }
    uint8_t b[7] = {
        rex_(true, false, false, dst >= 8),
        0x81,
        modrm_(0b11, 0, dst & 7),                        // /0 = ADD
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    emitBytes(b, 7);
}

// add r64, r64  (REX.W 01 /r).
void HostAssembler::addReg(Reg d, Reg a, Reg b) {
    // d = a + b  →  if d == a: add d, b.
    // Elif d == b: add d, a.
    // Else: mov d, a; add d, b.
    if (d != a && d != b) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    const uint8_t src = (d == a || (d != a && d != b)) ? xr(b) : xr(a);
    uint8_t bytes[3] = {
        rex_(true, src >= 8, false, dst >= 8),
        0x01,
        modrm_(0b11, src & 7, dst & 7),
    };
    emitBytes(bytes, 3);
}

// imul r64, r64, imm32  (REX.W 69 /r id), the three-operand form: d = a * imm.
void HostAssembler::mulImm(Reg d, Reg a, int32_t imm) {
    const uint8_t dst = xr(d), src = xr(a);
    uint8_t b[7] = {
        rex_(true, dst >= 8, false, src >= 8),
        0x69,
        modrm_(0b11, dst & 7, src & 7),
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    emitBytes(b, 7);
}

// imul r64, r64  (REX.W 0F AF /r), two-operand: d = d * a.
void HostAssembler::mulReg(Reg d, Reg a, Reg b) {
    // d = a * b → if d == a: imul d, b.
    // Elif d == b: imul d, a.
    // Else: mov d, a; imul d, b.
    if (d != a && d != b) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    const uint8_t src = (d == a || (d != a && d != b)) ? xr(b) : xr(a);
    uint8_t bytes[4] = {
        rex_(true, dst >= 8, false, src >= 8),
        0x0F, 0xAF,
        modrm_(0b11, dst & 7, src & 7),
    };
    emitBytes(bytes, 4);
}

// --- memory ops ---------------------------------------------------------------------------------

// The shared body of the four indexed forms, unscaled, which is how this backend addresses a light, the lowering having already multiplied by the element width.
// Two encoding rules live here once instead of four times.
// Certain base registers cannot use the short addressing mode, whose slot the architecture gives to an absolute address, so an explicit zero displacement means the same thing and encodes correctly.
// The prefix is emitted unconditionally for the byte store, since without it an eight-bit operand names the high half of a different register.
void HostAssembler::emitIndexed(const uint8_t* opcode, size_t opLen, bool prefix66,
                                bool forceRex, uint8_t reg, uint8_t base, uint8_t index) {
    const bool baseNeedsDisp = ((base & 7) == 0b101);    // rbp / r13
    uint8_t b[8]; size_t n = 0;
    if (prefix66) b[n++] = 0x66;                         // operand-size override → 16-bit
    if (forceRex || reg >= 8 || index >= 8 || base >= 8)
        b[n++] = rex_(false, reg >= 8, index >= 8, base >= 8);
    for (size_t i = 0; i < opLen; i++) b[n++] = opcode[i];
    b[n++] = modrm_(baseNeedsDisp ? 0b01 : 0b00, reg & 7, 0b100);   // r/m=100 → SIB follows
    b[n++] = sib_(0, index & 7, base & 7);                          // scale=0: unscaled index
    if (baseNeedsDisp) b[n++] = 0x00;                               // the explicit zero disp8
    emitBytes(b, n);
}

// The signed high half of the product, for the fixed-point multiply: @xref{the-high-multiply-is-alias-safe-by-construction|why the intermediate lives on the stack}.
void HostAssembler::mulhi(Reg d, Reg a, Reg b) {
    const uint8_t dst = xr(d), ra = xr(a), rb = xr(b);
    const uint8_t RAX = x64::RAX;

    uint8_t save[1] = {uint8_t(0x50 | (RAX & 7))};
    emitBytes(save, 1);                                              // push rax  (save the vreg)

    // The operand that might BE rax is widened FIRST, because the other widening overwrites rax.
    // Reading a first when b == rax destroyed b before it was ever read, the third shape of the same mistake.
    // The reason both operands are now ordered rather than assumed independent.
    const uint8_t first = (rb == RAX) ? rb : ra;
    const uint8_t second = (rb == RAX) ? ra : rb;

    uint8_t ext1[3] = {rex_(true, RAX >= 8, false, first >= 8), 0x63,
                       modrm_(0b11, RAX & 7, first & 7)};
    emitBytes(ext1, 3);                                              // movsxd rax, <first>
    uint8_t park[1] = {uint8_t(0x50 | (RAX & 7))};
    emitBytes(park, 1);                                              // push rax  (park it)

    uint8_t ext2[3] = {rex_(true, RAX >= 8, false, second >= 8), 0x63,
                       modrm_(0b11, RAX & 7, second & 7)};
    emitBytes(ext2, 3);                                              // movsxd rax, <second>
    // imul rax, [rsp], the parked a. modrm mod=00 rm=100 selects a SIB byte; the SIB names rsp as base with no index, which is how [rsp] is addressed.
    uint8_t mul[5] = {rex_(true, RAX >= 8, false, false), 0x0F, 0xAF,
                      modrm_(0b00, RAX & 7, 0b100), sib_(0, 0b100, x64::RSP & 7)};
    emitBytes(mul, 5);
    uint8_t sar[4] = {rex_(true, false, false, RAX >= 8), 0xC1, modrm_(0b11, 7, RAX & 7), 32};
    emitBytes(sar, 4);                                               // sar rax, 32

    // Both sources are spent; only now does d take the result.
    uint8_t mov[3] = {rex_(true, RAX >= 8, false, dst >= 8), 0x89, modrm_(0b11, RAX & 7, dst & 7)};
    emitBytes(mov, 3);                                               // mov dD, rax

    uint8_t drop[4] = {rex_(true, false, false, false), 0x83, modrm_(0b11, 0, x64::RSP & 7), 8};
    emitBytes(drop, 4);                                              // add rsp, 8 (discard a)
    // The saved rax is restored LAST, and into rax only, if d IS rax the mov above already put the result there, so this would overwrite it. Pop into rax is therefore skipped in that case and the stack adjusted instead.
    if (dst == RAX) {
        uint8_t skip[4] = {rex_(true, false, false, false), 0x83, modrm_(0b11, 0, x64::RSP & 7), 8};
        emitBytes(skip, 4);                                          // add rsp, 8
    } else {
        uint8_t rest[1] = {uint8_t(0x58 | (RAX & 7))};
        emitBytes(rest, 1);                                          // pop rax
    }
}
// 32-bit shifts: C1 /4 ib is shl, C1 /7 ib is sar. No REX.W, a vreg is 32 bits, and the arithmetic shift must fill from bit 31, not bit 63.
void HostAssembler::shlImm(Reg d, Reg a, uint8_t n) {
    if (d != a) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    if (dst >= 8) { uint8_t r[1] = {rex_(false, false, false, true)}; emitBytes(r, 1); }
    uint8_t b[3] = {0xC1, modrm_(0b11, 4, dst & 7), n};
    emitBytes(b, 3);
}
void HostAssembler::shrImm(Reg d, Reg a, uint8_t n) {
    if (d != a) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    if (dst >= 8) { uint8_t r[1] = {rex_(false, false, false, true)}; emitBytes(r, 1); }
    uint8_t b[3] = {0xC1, modrm_(0b11, 5, dst & 7), n};      // C1 /5 ib = shr
    emitBytes(b, 3);
}
void HostAssembler::sarImm(Reg d, Reg a, uint8_t n) {
    if (d != a) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    if (dst >= 8) { uint8_t r[1] = {rex_(false, false, false, true)}; emitBytes(r, 1); }
    uint8_t b[3] = {0xC1, modrm_(0b11, 7, dst & 7), n};
    emitBytes(b, 3);
}
// The 4-byte slot access. mov r32 <- [base+disp32] (8B /r) and its store twin (89 /r); the indexed forms reuse emitIndexed, which already handles the SIB byte and the rbp/r13 base that needs an explicit zero displacement.
void HostAssembler::load32(Reg d, Reg base, int32_t imm) {
    const uint8_t dst = xr(d), b_reg = xr(base);
    const bool needsSIB = ((b_reg & 7) == x64::RSP);
    uint8_t b[9]; size_t n = 0;
    if (dst >= 8 || b_reg >= 8) b[n++] = rex_(false, dst >= 8, false, b_reg >= 8);
    b[n++] = 0x8B;
    b[n++] = modrm_(0b10, dst & 7, needsSIB ? 0b100 : (b_reg & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, b_reg & 7);
    b[n++] = uint8_t(imm); b[n++] = uint8_t(imm >> 8);
    b[n++] = uint8_t(imm >> 16); b[n++] = uint8_t(imm >> 24);
    emitBytes(b, n);
}
void HostAssembler::store32(Reg base, int32_t imm, Reg val) {
    const uint8_t src = xr(val), b_reg = xr(base);
    const bool needsSIB = ((b_reg & 7) == x64::RSP);
    uint8_t b[9]; size_t n = 0;
    if (src >= 8 || b_reg >= 8) b[n++] = rex_(false, src >= 8, false, b_reg >= 8);
    b[n++] = 0x89;
    b[n++] = modrm_(0b10, src & 7, needsSIB ? 0b100 : (b_reg & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, b_reg & 7);
    b[n++] = uint8_t(imm); b[n++] = uint8_t(imm >> 8);
    b[n++] = uint8_t(imm >> 16); b[n++] = uint8_t(imm >> 24);
    emitBytes(b, n);
}
void HostAssembler::load32Idx(Reg d, Reg base, Reg off) {
    const uint8_t op = 0x8B;
    emitIndexed(&op, 1, /*prefix66=*/false, /*forceRex=*/false, xr(d), xr(base), xr(off));
}
void HostAssembler::store32Idx(Reg base, Reg off, Reg val) {
    const uint8_t op = 0x89;
    emitIndexed(&op, 1, /*prefix66=*/false, /*forceRex=*/false, xr(val), xr(base), xr(off));
}

// mov byte ptr [base + off], val_l  (88 /r with SIB): indexed 1-byte store, the pixel write.
void HostAssembler::store8(Reg base, Reg off, Reg val) {
    const uint8_t op = 0x88;
    emitIndexed(&op, 1, /*prefix66=*/false, /*forceRex=*/true, xr(val), xr(base), xr(off));
}
// mov r64_low16, [base + off] , index-in-reg.
// Used for control byte reads. x86 zero-extends 8-bit loads to 32 bits automatically (movzx).
// The 64-bit destination is implicitly zero-extended above bit 31, matching the arm64 ldrb behavior.
void HostAssembler::load8(Reg d, Reg base, int32_t imm) {
    const uint8_t dst = xr(d), b_reg = xr(base);
    // movzx r32, byte ptr [base + disp32]  (0F B6 /r), zero-extend to 32; the r32 write clears the upper 32 bits of the r64.
    const bool needsSIB = ((b_reg & 7) == x64::RSP);
    uint8_t b[9]; size_t n = 0;
    b[n++] = rex_(false, dst >= 8, false, b_reg >= 8);   // no REX.W needed (r32 dest zero-extends)
    b[n++] = 0x0F; b[n++] = 0xB6;
    b[n++] = modrm_(0b10, dst & 7, needsSIB ? 0b100 : (b_reg & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, b_reg & 7);
    b[n++] = uint8_t(imm); b[n++] = uint8_t(imm >> 8);
    b[n++] = uint8_t(imm >> 16); b[n++] = uint8_t(imm >> 24);
    emitBytes(b, n);
}
// movzx r32, byte ptr [base + off]  (0F B6 /r SIB), indexed 8-bit zero-extending load.
void HostAssembler::load8Idx(Reg d, Reg base, Reg off) {
    const uint8_t op[2] = {0x0F, 0xB6};
    emitIndexed(op, 2, /*prefix66=*/false, /*forceRex=*/false, xr(d), xr(base), xr(off));
}

// --- compare and branch -------------------------------------------------------------------------

// A compare at the VALUE'S own width, not the register's, because a script value is narrower and the sibling backend already compares that way.
// While every value was zero-extended the two agreed, but a signed compare makes them disagree: a narrow negative in a wide register reads as a large positive.
// Comparing at the value's width keeps the backends running the same script the same way.
// The prefix is still emitted to address the upper registers; only the width bit is dropped, and the null prefix is valid, so one form holds for every pair.
void HostAssembler::cmp(Reg a, Reg b) {
    const uint8_t left = xr(a), right = xr(b);
    uint8_t bytes[3] = {
        rex_(false, right >= 8, false, left >= 8),
        0x39,
        modrm_(0b11, right & 7, left & 7),
    };
    emitBytes(bytes, 3);
}

// Conditional branch, near-32-bit-relative  (0F 8x rel32).
// Always the rel32 form, one width, so the fixup table is uniform.
// Condition tt values:  NE=0x5, HS/AE=0x3, LO/B=0x2, GE=0xD.
void HostAssembler::branchIf(Cond c, Label l) {
    uint8_t tt;
    switch (c) {
        case Cond::Ne: tt = 0x05; break;
        case Cond::Hs: tt = 0x03; break;                // aka AE: unsigned >=
        case Cond::Lo: tt = 0x02; break;                // aka B:  unsigned <
        case Cond::Ge: tt = 0x0D; break;                // SIGNED >=
        default:       tt = 0x05; break;
    }
    // Fixup site is the START of the branch instruction; patchBranches computes rel32 relative to the byte AFTER the instruction (PC-relative on x86). The fixup helper stores the site; patchBranches knows the instruction is 6 bytes long, so it applies rel32 at site+2.
    addFixup(len_, l, FixKind::Branch);
    uint8_t b[6] = { 0x0F, uint8_t(0x80 | tt), 0, 0, 0, 0 };
    emitBytes(b, 6);
}

// cbz-equivalent: test r64, r64  (REX.W 85 /r reg,reg self) then je rel32.
void HostAssembler::branchIfZero(Reg a, Label l) {
    const uint8_t reg = xr(a);
    // test reg, reg, flags reflect zero-ness of reg.
    uint8_t t[3] = {
        rex_(true, reg >= 8, false, reg >= 8),
        0x85,
        modrm_(0b11, reg & 7, reg & 7),
    };
    emitBytes(t, 3);
    // je rel32, fixup site is where the je starts.
    addFixup(len_, l, FixKind::Branch);
    uint8_t j[6] = { 0x0F, 0x84, 0, 0, 0, 0 };
    emitBytes(j, 6);
}

// The fused compare-and-branch pair.
void HostAssembler::branchGeU(Reg a, Reg b, Label l) { cmp(a, b); branchIf(Cond::Hs, l); }
void HostAssembler::branchGeS(Reg a, Reg b, Label l) { cmp(a, b); branchIf(Cond::Ge, l); }
void HostAssembler::branchNe(Reg a, Reg b, Label l)  { cmp(a, b); branchIf(Cond::Ne, l); }

// --- ret ----------------------------------------------------------------------------------------
void HostAssembler::ret() {
    uint8_t b = 0xC3;
    emitBytes(&b, 1);
}

// The call to a host built-in, in the textbook push and pop shape: push the pool, load the arguments from their pushed copies, call, then pop back.
// Loading from memory makes an argument register overlapping a source harmless, which the sibling solves with dedicated scratch.
// Pushes rather than moves: @xref{the-call-pushes-rather-than-moves|the density that buys}.
// The pool's size keeps the stack pointer aligned across the pushes.

// A pushed vreg's offset from rsp while the pool is on the stack (before the shadow sub): vreg 0 was pushed first so it sits highest.
static constexpr int32_t pushedSlot(uint8_t v) {
    return int32_t(kRegCount - 1 - v) * 8;
}

void HostAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // 1) Push the whole vreg pool, v0 first (v0 ends up at the highest address).
    for (uint8_t v = 0; v < kRegCount; v++) emitPushReg(this, kX64Reg[v]);
#if defined(_WIN32)
    // 2) Win64: reserve the callee's 32-byte shadow space. (SysV owes none.)
    emitSubRspImm32(this, int32_t(kShadowSpace));
#endif
    // 3) Load args from the pushed copies into the ABI argument registers.
    const int32_t base = int32_t(kShadowSpace);   // pushed slots sit above the shadow sub
#if defined(_WIN32)
    emitMovRegMemDisp(this, x64::RCX, x64::RSP, base + pushedSlot(a));
    emitMovRegMemDisp(this, x64::RDX, x64::RSP, base + pushedSlot(b));
    emitMovRegMemDisp(this, x64::R8,  x64::RSP, base + pushedSlot(c));
#else
    emitMovRegMemDisp(this, x64::RDI, x64::RSP, base + pushedSlot(a));
    emitMovRegMemDisp(this, x64::RSI, x64::RSP, base + pushedSlot(b));
    emitMovRegMemDisp(this, x64::RDX, x64::RSP, base + pushedSlot(c));
#endif
    // 4) movabs rax, fn, rax's vreg value (R13) is safe in its pushed slot.
    {
        const uint64_t addr = reinterpret_cast<uint64_t>(fn);
        uint8_t bytes[10];
        bytes[0] = rex_(true, false, false, false);
        bytes[1] = 0xB8;                                 // MOV rax, imm64
        for (int i = 0; i < 8; i++) bytes[2 + i] = uint8_t(addr >> (i * 8));
        emitBytes(bytes, 10);
    }
    // 5) call rax  (FF /2).
    {
        uint8_t bytes[2] = { 0xFF, modrm_(0b11, 2, x64::RAX) };
        emitBytes(bytes, 2);
    }
#if defined(_WIN32)
    // 6) Drop the shadow space so the pushed slots are back at [rsp + pushedSlot(v)]. add rsp, 32 in the imm8 form (REX.W 83 /0 ib).
    {
        uint8_t bytes[4] = {
            rex_(true, false, false, false), 0x83, modrm_(0b11, 0, x64::RSP),
            uint8_t(kShadowSpace),
        };
        emitBytes(bytes, 4);
    }
#endif
    // 7) Zero-extend the return, the ABI leaves rax's upper bits undefined for a narrower-than- 64-bit return (random16 is uint16_t), and this backend's index math is 64-bit-wide.
    // Dirty upper bits would ride into addresses.
    // `mov eax, eax` (89 C0) zeroes them.
    {
        uint8_t bytes[2] = {0x89, 0xC0};
        emitBytes(bytes, 2);
    }
    // 8) Write the return over DST's pushed slot, the pop below then restores every OTHER vreg and loads dst with the return, in one uniform restore. (If dst is R13 = rax, its slot is overwritten and the pop hands rax its own return, still correct.)
    emitMovMemDispReg(this, x64::RSP, pushedSlot(d), x64::RAX);
    // 9) Pop the pool in reverse (v13 first).
    for (uint8_t v = kRegCount; v-- > 0;) emitPopReg(this, kX64Reg[v]);
}

// Script-to-script call: bl-equivalent on x64 is `call rel32` (E8 xx xx xx xx). The callee's prologue saves rbp and nonvolatiles, exactly like our own prologue, so calls nest.
void HostAssembler::callLabel(Label l, Reg d, bool take) {
    // The same preservation call() gives a builtin: push the whole pool, call, write the result over the pushed slot of `d` so the pops deliver it. Without it a value computed before the call and used after it, `a() + b()`, read the second call's result twice.
    for (uint8_t v = 0; v < kRegCount; v++) emitPushReg(this, kX64Reg[v]);
    // Reload parked host arguments from their spill slots, the contract with IrOp::CallScript in core, mirroring arm64. On x64 this matters: the arg registers ARE vregs the callee may have consumed, so their live-across-a-called-function meaning has to be re-established.
    for (uint8_t v = 0; v < kHostArgSlots; v++) spillLoad(static_cast<Reg>(v), hostArgSlot(v));
#if defined(_WIN32)
    // One convention passes the fifth argument on the caller's stack above the shadow area, which the entry point's prologue reads from there.
    // A script-to-script call must do the same, or the callee reads whatever was in that slot and faults on the first use.
    // The pushes above buried the slot the prologue reserved, so a fresh outgoing area is opened here, padded to keep the stack aligned.
    // The other convention passes it in a register the load above already filled, so it needs no store.
    emitSubRspImm32(this, int32_t(kShadowSpace) + 16);
    emitMovMemDispReg(this, x64::RSP, kShadowSpace, xr(R4));
#endif
    addFixup(len_, l, FixKind::Call);
    uint8_t b[5] = { 0xE8, 0, 0, 0, 0 };
    emitBytes(b, 5);
#if defined(_WIN32)
    {
        uint8_t add[4] = { rex_(true, false, false, false), 0x83, modrm_(0b11, 0, x64::RSP),
                           uint8_t(kShadowSpace + 16) };
        emitBytes(add, 4);
    }
#endif
    if (take) {
        uint8_t zx[2] = {0x89, 0xC0};                    // mov eax, eax: zero-extend the 32-bit result
        emitBytes(zx, 2);
        emitMovMemDispReg(this, x64::RSP, pushedSlot(d), x64::RAX);
    }
    for (uint8_t v = kRegCount; v-- > 0;) emitPopReg(this, kX64Reg[v]);
}

// Resolve all pending fixups: overwrite the rel32 field in each branch/call with (target - site).
void HostAssembler::patchBranches() {
    // An OVERFLOWED compile is refused by lowerWith after finalize(), so patching it is pointless, and unsafe.
    // A fixup recorded just before emitBytes dropped its instruction points at the buffer's end, and the memcpy below would write up to 5 bytes past buf_.
    // "an unencodable script is refused, not truncated" is the test that reaches this path.
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] == kUnbound) continue;   // unbound label: leave the branch as-is (overflow_ already failed the compile)
        int32_t target = labelPos_[f.label];
        if (f.kind == FixKind::Call) {
            // `call rel32` at [f.at]: opcode E8 at f.at, imm32 at f.at+1. rel is relative to the byte AFTER the imm32 field, so subtract (f.at + 5).
            int32_t rel = target - (int32_t(f.at) + 5);
            uint8_t bytes[4] = {
                uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)
            };
            std::memcpy(buf_ + f.at + 1, bytes, 4);
        } else {
            // `0F 8x rel32` at [f.at]: 2-byte opcode then imm32. rel is relative to (f.at + 6).
            int32_t rel = target - (int32_t(f.at) + 6);
            uint8_t bytes[4] = {
                uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)
            };
            std::memcpy(buf_ + f.at + 2, bytes, 4);
        }
    }
}


// Bind the shared IR walk to this assembler.
// INSIDE the arch guard, with the assembler it names.
// In the `#else` it would be compiled on every OTHER host, where `HostAssembler` is the arm64 one (duplicate definition) or absent entirely. moonlive_asm_noarch.cpp owns the no-backend stub.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<HostAssembler>(ir, out, cap, squeeze, kRegCount);
}

#endif  // __x86_64__ || _M_X64

}  // namespace mm::moonlive
