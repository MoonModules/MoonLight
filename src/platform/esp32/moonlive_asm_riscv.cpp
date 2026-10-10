#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_riscv.h"

#include <cstring>

#if defined(__riscv)   // the RISC-V assembler is only built for RISC-V targets (ESP32-P4)

/// @defgroup moonlive_asm_riscv_impl MoonLive RISC-V encodings
/// Named instructions for the 32-bit variant, verified against the assembler.
///
/// Fixed-width little-endian instructions on the standard calling convention, with branch offsets back-patched.

namespace mm::moonlive {

// R0..R4 → a0..a4 (10..14, the host args: buf, nLights, cpl, t, ctrls, a4=kArg4 the controls arena pointer). R5..R11 → t0,t1,t2,t3,t4,t5,a5 (caller-saved temps). t6(31) is the internal scratch (store8 address, call address build + result stash), not a vreg.
static constexpr uint8_t kRvReg[kRegCount] = {10, 11, 12, 13, 14, 5, 6, 7, 28, 29, 30, 15,
                                              16, 17};
// Bounds-checked: the inline operations address their scratch past the map whenever the reservation and the map disagree.
// An out-of-bounds read returns whatever byte follows, so the emitted instruction names a register by accident; clamping makes that wrong but safe.
// The assertion below and the lowering's reservation are what stop it happening at all.
static uint8_t xr(Reg r) { return kRvReg[r < kRegCount ? r : kRegCount - 1]; }
// t6 is the ONLY caller-saved register outside kRvReg, so it is the only safe scratch: every other free register is callee-saved (s0/s1, s6..s11) and would have to be preserved. Both uses below are transient, store8 consumes it within two instructions, and call() finishes with it before any store8 can run, so one register serves both.
static constexpr uint8_t kScratchAddr = 31;   // t6 — store8 address temp
static constexpr uint8_t kScratchFn   = 31;   // t6 — call address build / result stash

// A scratch register that is ALSO a vreg silently corrupts values. kScratchFn was x16/a6, which is kRvReg[12] = vreg R12: call() stashed its result in a6, then the restore loop reloaded x16 from the frame and destroyed it, so every call returned R12's stale value. Latent only because it needs vregsUsed > 12. Checked here so the map can never grow over a scratch again.
constexpr bool rvScratchOutsideMap() {
    constexpr uint8_t scratch[] = {kScratchAddr, kScratchFn};
    for (uint8_t r : kRvReg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(rvScratchOutsideMap(), "a scratch register is also a vreg — calls will corrupt it");

void RiscvAssembler::emit32(uint32_t w) {
    if (!buf_ || len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(w); buf_[len_++] = uint8_t(w >> 8);
    buf_[len_++] = uint8_t(w >> 16); buf_[len_++] = uint8_t(w >> 24);
}

Label RiscvAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = kUnbound;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit32
    Label l = labelCount_++; labelPos_[l] = kUnbound; return l;
}
void RiscvAssembler::bind(Label l) { if (len_ >= kUnbound) overflow_ = true; else if (l < kMaxLabels) labelPos_[l] = static_cast<CodePos>(len_); }

// A call to a function in this block, the script-to-script call, linking the return address into the standard register.
// The callee's prologue saves it into its own frame, which is what lets calls nest and therefore recurse.
// The host arguments pass straight into the argument registers, there being no window rotation here.
// The encoders are declared here so this can sit with its sibling call routines rather than after them.
static uint32_t encAddi(uint8_t rd, uint8_t rs1, int32_t imm);
static uint32_t encSw(uint8_t rs2, uint8_t rs1, int32_t imm);
static uint32_t encLw(uint8_t rd, uint8_t rs1, int32_t imm);

void RiscvAssembler::callLabel(Label l, Reg d, bool take) {
    // The same preservation call() gives a builtin: the whole vreg pool and ra to the stack, the callee's a0 stashed in t6 past the restore. Without it a value computed before the call and used after it, `a() + b()`, read the second call's result twice.
    emit32(encAddi(2, 2, -80));                        // addi sp, sp, -80
    emit32(encSw(1, 2, 76));                            // sw ra, 76(sp)
    static const uint8_t saved[] = {10, 11, 12, 13, 14, 5, 6, 7, 28, 29, 30, 15, 16, 17};
    int off = 0;
    for (uint8_t r : saved) { emit32(encSw(r, 2, off)); off += 4; }
    // The host arguments are reloaded from the FRAME (s0-relative), so the sp move above does not disturb where they come from.
    for (uint8_t v = 0; v < kHostArgSlots; v++) spillLoad(static_cast<Reg>(v), hostArgSlot(v));
    addFixup(len_, l, FixKind::Jal);
    // jal x1, 0: opcode 0x6f, rd = 1. The 20-bit immediate is scattered by the patcher.
    emit32(0x000000efu);
    if (take) emit32(encAddi(kScratchFn, 10, 0));      // mv t6, a0
    off = 0;
    for (uint8_t r : saved) { emit32(encLw(r, 2, off)); off += 4; }
    emit32(encLw(1, 2, 76));
    emit32(encAddi(2, 2, 80));
    if (take) emit32(encAddi(xr(d), kScratchFn, 0));   // mv dst, t6
}

// Record a pending branch fixup, guarding the fixed table (overflow_ rather than an OOB write).
void RiscvAssembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

// --- I/R/S-type encoders (rd/rs1/rs2 are real x-register numbers) ---
static uint32_t encAddi(uint8_t rd, uint8_t rs1, int32_t imm) {
    return ((uint32_t(imm) & 0xfff) << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x13;
}
static uint32_t encAdd(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (rs2 << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x33;
}
static uint32_t encMul(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (1u << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x33;
}
// mulh rd, rs1, rs2, the same M-extension encoding as mul with funct3 = 1: the SIGNED high 32 bits of the product. Paired with mul it forms the Q16.16 multiply's middle word.
static uint32_t encMulh(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (1u << 25) | (rs2 << 20) | (rs1 << 15) | (1u << 12) | (rd << 7) | 0x33;
}
// slli / srai rd, rs1, shamt, I-type with the shift amount in the immediate. srai sets bit 30 of the immediate field, which is what makes the shift arithmetic (sign-filling) rather than logical.
static uint32_t encSlli(uint8_t rd, uint8_t rs1, uint8_t n) {
    return (uint32_t(n & 0x1f) << 20) | (rs1 << 15) | (1u << 12) | (rd << 7) | 0x13;
}
static uint32_t encSrai(uint8_t rd, uint8_t rs1, uint8_t n) {
    return (1u << 30) | (uint32_t(n & 0x1f) << 20) | (rs1 << 15) | (5u << 12) | (rd << 7) | 0x13;
}
// srli: srai without bit 30. The one bit between zero-filling and sign-filling.
static uint32_t encSrli(uint8_t rd, uint8_t rs1, uint8_t n) {
    return (uint32_t(n & 0x1f) << 20) | (rs1 << 15) | (5u << 12) | (rd << 7) | 0x13;
}
static uint32_t encSb(uint8_t rs2, uint8_t rs1, int32_t imm) {   // sb rs2, imm(rs1)
    return (((uint32_t(imm) >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) |
           ((uint32_t(imm) & 0x1f) << 7) | 0x23;
}
static uint32_t encSw(uint8_t rs2, uint8_t rs1, int32_t imm) {   // sw rs2, imm(rs1)
    return (((uint32_t(imm) >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) |
           ((uint32_t(imm) & 0x1f) << 7) | 0x23;
}
static uint32_t encLw(uint8_t rd, uint8_t rs1, int32_t imm) {    // lw rd, imm(rs1)
    return ((uint32_t(imm) & 0xfff) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03;
}
static uint32_t encLui(uint8_t rd, uint32_t imm20) {
    return (imm20 << 12) | (rd << 7) | 0x37;
}
static uint32_t encBranch(uint8_t rs1, uint8_t rs2, uint8_t f3, int32_t off) {  // B-type
    uint32_t o = uint32_t(off) & 0x1fff;
    return (((o >> 12) & 1) << 31) | (((o >> 5) & 0x3f) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (((o >> 1) & 0xf) << 8) | (((o >> 11) & 1) << 7) | 0x63;
}

// The call frame, the register allocator's overflow storage.
// The frame pointer is the standard callee-saved one, outside both the register map and the scratch, so nothing emitted here disturbs it.
// The routine saves and restores it, which is what makes using a callee-saved register legal at all.
static constexpr uint8_t kFramePtr = 8;   // s0/fp

void RiscvAssembler::prologue(uint8_t slots) {
    if (slots == 0) return;                        // no spilling: no frame, no cost
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // 16-byte aligned, as the RISC-V calling convention requires of sp at a call boundary, an unaligned sp is the kind of thing that survives every test and faults inside a callee. +8 for the two saved registers: the frame pointer AND the return address.
    const uint16_t bytes = static_cast<uint16_t>((8 + slots * 4 + 15) & ~15);
    frameBytes_ = bytes;
    emit32(encAddi(2, 2, -int32_t(bytes)));        // addi sp, sp, -bytes
    emit32(encSw(kFramePtr, 2, bytes - 4));        // sw s0, bytes-4(sp)
    // SAVE ra. A leaf routine could skip this, and every routine was one until a script could call its own function: `jal` links the return address into ra, so a callee that does not save it returns to ITSELF the moment it makes a call of its own. Measured on the S31 as a stack protection fault with the task name corrupted, which is a runaway call chain, not a bad jump.
    emit32(encSw(1, 2, bytes - 8));                // sw ra, bytes-8(sp)
    emit32(encAddi(kFramePtr, 2, bytes));          // addi s0, sp, bytes   (s0 = the caller's sp)
}
void RiscvAssembler::epilogue() {
    if (frameBytes_ != 0) {
        emit32(encLw(kFramePtr, 2, frameBytes_ - 4));   // lw s0, bytes-4(sp)
        emit32(encLw(1, 2, frameBytes_ - 8));           // lw ra, bytes-8(sp) : restore before ret
        emit32(encAddi(2, 2, frameBytes_));             // addi sp, sp, bytes
    }
    ret();
}
// sw/lw against s0. The offset is NEGATIVE (slots live below the frame pointer), which encAddi's 12-bit signed immediate and the S/I-type immediates handle directly: the whole slot file is a few hundred bytes, far inside the +/-2048 the field reaches.
void RiscvAssembler::spillStore(Reg r, uint8_t slot) {
    // No frame means prologue() bailed (slots == 0, or past kMaxSpillSlots). Emitting anyway would address s0 + slot*4, ABOVE the frame pointer, i.e. the CALLER's stack. Refuse instead.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(encSw(xr(r), kFramePtr, -int32_t(frameBytes_) + slot * 4));
}
void RiscvAssembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(encLw(xr(r), kFramePtr, -int32_t(frameBytes_) + slot * 4));
}

// The ADDRESS of a frame slot, for a host call that reads its arguments from the frame. The slots already hold the arguments; the call passes where they start rather than the values, which is what makes the number of arguments a memory question instead of a register one.
void RiscvAssembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    // Against s0, the SAME base spillStore/spillLoad use. Computing it from sp instead lands frameBytes_ below the frame (s0 == sp + frameBytes_), so the callee writes off the end of it.
    emit32(encAddi(xr(d), kFramePtr, -int32_t(frameBytes_) + slot * 4));   // addi xD, s0, off
}

void RiscvAssembler::movImm(Reg d, int32_t imm) {
    // addi sign-extends a 12-bit immediate, so it alone covers only -2048..2047. For wider constants (a uint16 like 65535) materialize the full value with lui (high 20 bits) + addi (low 12), the hi/lo split, without this, larger Const values truncate. Single addi when the value fits, to keep the common small-constant case one instruction.
    if (imm >= -2048 && imm <= 2047) {
        emit32(encAddi(xr(d), 0, imm));                    // li = addi rd, x0, imm
        return;
    }
    uint32_t v  = static_cast<uint32_t>(imm);
    uint32_t hi = (v + 0x800) >> 12;                       // round for the sign-extended addi
    int32_t  lo = static_cast<int32_t>(v) - static_cast<int32_t>(hi << 12);
    emit32(encLui(xr(d), hi & 0xfffff));                   // lui rd, hi
    emit32(encAddi(xr(d), xr(d), lo));                     // addi rd, rd, lo
}
void RiscvAssembler::movReg(Reg d, Reg a)       { emit32(encAddi(xr(d), xr(a), 0)); }   // mv = addi rd,ra,0

// The RISC-V return register is a0 (x10), which is where R0 lives: R0..R3 map to a0..a3, the host arguments. Free when the value is already there.
void RiscvAssembler::retValue(Reg a) {
    if (a == R0) return;                    // already in a0
    movReg(R0, a);
}
void RiscvAssembler::addImm(Reg d, Reg a, int32_t imm) { emit32(encAddi(xr(d), xr(a), imm)); }
void RiscvAssembler::addReg(Reg d, Reg a, Reg b) { emit32(encAdd(xr(d), xr(a), xr(b))); }
void RiscvAssembler::mulReg(Reg d, Reg a, Reg b) { emit32(encMul(xr(d), xr(a), xr(b))); }

void RiscvAssembler::mulhi(Reg d, Reg a, Reg b) { emit32(encMulh(xr(d), xr(a), xr(b))); }
void RiscvAssembler::shlImm(Reg d, Reg a, uint8_t n) {
    if (n >= 32) { overflow_ = true; return; }   // shamt is five bits
    emit32(encSlli(xr(d), xr(a), n));
}
void RiscvAssembler::sarImm(Reg d, Reg a, uint8_t n) {
    if (n >= 32) { overflow_ = true; return; }   // shamt is five bits
    emit32(encSrai(xr(d), xr(a), n));
}
void RiscvAssembler::shrImm(Reg d, Reg a, uint8_t n) {
    if (n >= 32) { overflow_ = true; return; }   // shamt is five bits
    emit32(encSrli(xr(d), xr(a), n));
}
// The 4-byte slot access. encLw/encSw already existed for spills; these give them an arbitrary base and offset, which is what a member slot needs.
void RiscvAssembler::load32(Reg d, Reg base, int32_t imm) { emit32(encLw(xr(d), xr(base), imm)); }
void RiscvAssembler::store32(Reg base, int32_t imm, Reg val) {
    emit32(encSw(xr(val), xr(base), imm));
}
void RiscvAssembler::load32Idx(Reg d, Reg base, Reg off) {
    emit32(encAdd(kScratchAddr, xr(base), xr(off)));   // t6 = base + off
    emit32(encLw(xr(d), kScratchAddr, 0));
}
void RiscvAssembler::store32Idx(Reg base, Reg off, Reg val) {
    emit32(encAdd(kScratchAddr, xr(base), xr(off)));
    emit32(encSw(xr(val), kScratchAddr, 0));
}
void RiscvAssembler::store8(Reg base, Reg off, Reg val) {
    emit32(encAdd(kScratchAddr, xr(base), xr(off)));   // t6 = base + off
    emit32(encSb(xr(val), kScratchAddr, 0));           // sb val, 0(t6)
}
void RiscvAssembler::load8(Reg d, Reg base, int32_t imm) {   // lbu rDst, imm(rBase) — control read
    emit32(((uint32_t(imm) & 0xfff) << 20) | (xr(base) << 15) | (4 << 12) | (xr(d) << 7) | 0x03);
}
// RISC-V has no register-offset addressing mode, so the address is computed first. Same shape as store8 and store32, which is why they share kScratchAddr.
void RiscvAssembler::load8Idx(Reg d, Reg base, Reg off) {
    emit32(encAdd(kScratchAddr, xr(base), xr(off)));                              // t6 = base + off
    emit32((uint32_t(kScratchAddr) << 15) | (4 << 12) | (xr(d) << 7) | 0x03);     // lbu d, 0(t6)
}
// A relaxed conditional branch: the inverted condition jumping over an unconditional jump that carries the real target.
// Two words instead of one, always: the short form reaches a few kilobytes, one shipped script outgrew it, and the patcher then truncated the offset silently and panicked a board.
// Choosing per branch needs the final layout, unknown while emitting, so this takes the uniform form and pays one extra word.
// The condition inverts by flipping the field's low bit, an encoding property rather than an arithmetic trick.
void RiscvAssembler::branchRelaxed(uint8_t rs1, uint8_t rs2, uint8_t f3, Label l) {
    emit32(encBranch(rs1, rs2, f3 ^ 1, 8));            // b<!cond> rs1, rs2, +8  (over the jal)
    addFixup(len_, l, FixKind::Jal);
    emit32(0x0000006f);                                // jal x0, l  (patched; rd = x0 discards ra)
}

void RiscvAssembler::branchIfZero(Reg a, Label l) {    // a == 0  ⇔  bgeu x0, a (unsigned 0 >= a)
    branchRelaxed(0, xr(a), 7, l);
}
void RiscvAssembler::branchGeU(Reg a, Reg b, Label l) {
    branchRelaxed(xr(a), xr(b), 7, l);
}
void RiscvAssembler::branchGeS(Reg a, Reg b, Label l) {
    branchRelaxed(xr(a), xr(b), 5, l);                 // bge (funct3 5, vs 7 unsigned)
}
void RiscvAssembler::branchNe(Reg a, Reg b, Label l) {
    branchRelaxed(xr(a), xr(b), 1, l);                 // bne
}

// A standard call to a host built-in. Every temporary is caller-saved, so the whole pool, the return address and the host arguments are preserved around it.
// The target address is built as an upper and lower half, and the rounding compensates for the lower half's sign extension.
// Without it an address whose low half has its top bit set lands one page low, the classic split-immediate bug, silent until the pointer is used.
void RiscvAssembler::movPtr(Reg d, const void* p) {
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    const uint32_t hi = (addr + 0x800) >> 12;
    const int32_t  lo = static_cast<int32_t>(addr) - static_cast<int32_t>(hi << 12);
    emit32(encLui(xr(d), hi & 0xfffff));
    emit32(encAddi(xr(d), xr(d), lo));
}

void RiscvAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // 80-byte frame, 16-byte aligned: 14 saved registers (56 bytes), three argument staging slots (56/60/64), and ra at 76. Every register the map hands out is saved here, or a value live across a call is destroyed and the caller silently computes with rubbish, which is why the list mirrors kRvReg exactly.
    emit32(encAddi(2, 2, -80));                        // addi sp, sp, -80
    emit32(encSw(1, 2, 76));                            // sw ra, 76(sp)
    static const uint8_t saved[] = {10, 11, 12, 13, 14, 5, 6, 7, 28, 29, 30, 15, 16, 17};
    int off = 0;
    for (uint8_t r : saved) { emit32(encSw(r, 2, off)); off += 4; }
    // The three args into a0/a1/a2 (the standard ABI registers a host built-in reads). Staged through the frame first: a source may itself BE a0/a1/a2, so moving them in place could overwrite a source a later move still needs. Slots 56/60/64 sit above the saved set (14 registers, offsets 0..52) and below ra at 76.
    emit32(encSw(xr(a), 2, 56));
    emit32(encSw(xr(b), 2, 60));
    emit32(encSw(xr(c), 2, 64));
    emit32(encLw(10, 2, 56));                          // a0 = arg0
    emit32(encLw(11, 2, 60));                          // a1 = arg1
    emit32(encLw(12, 2, 64));                          // a2 = arg2
    // t6 = fn address via lui + addi (hi/lo split). t6 is caller-saved, so the callee may clobber it, harmless: it is dead across the call, written before jalr and rewritten after.
    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    uint32_t hi = (addr + 0x800) >> 12;                // round for the sign-extended addi
    int32_t  lo = static_cast<int32_t>(addr) - static_cast<int32_t>(hi << 12);
    emit32(encLui(kScratchFn, hi & 0xfffff));          // lui t6, hi
    emit32(encAddi(kScratchFn, kScratchFn, lo));       // addi t6, t6, lo
    emit32((kScratchFn << 15) | (1 << 7) | 0x67);      // jalr ra, t6, 0
    // stash the result (a0) in t6 before restoring (a0 is restored to the old buf). t6 is outside the saved set, so the restore loop below cannot destroy it.
    emit32(encAddi(kScratchFn, 10, 0));                // mv t6, a0
    // restore
    off = 0;
    for (uint8_t r : saved) { emit32(encLw(r, 2, off)); off += 4; }
    emit32(encLw(1, 2, 76));                           // lw ra, 108(sp)
    emit32(encAddi(2, 2, 80));                         // addi sp, sp, 112
    emit32(encAddi(xr(d), kScratchFn, 0));             // mv dst, t6  (the result)
}

void RiscvAssembler::ret() { emit32(0x00008067u); }    // ret = jalr x0, ra, 0

void RiscvAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch, stated rather than left to be derived from a zero count. An overflowed compile is refused afterwards, so patching it is pointless and unsafe: a fixup recorded just before a dropped instruction points past the buffer's end.
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] == kUnbound) continue;                  // unbound label: leave as-is (overflow_ already failed the compile)
        int32_t off = static_cast<int32_t>(labelPos_[f.label]) - static_cast<int32_t>(f.at);
        uint32_t w; std::memcpy(&w, buf_ + f.at, 4);
        if (f.kind == FixKind::Branch) {
            // The short branch reaches a few kilobytes and no further, past which the mask below truncates the offset silently and the branch lands on whatever the low bits name.
            // That is a panic on a board and nothing at all on the host.
            // So the compile fails instead and the module reports it and renders dark: a message rather than a reboot.
            if (off < -4096 || off > 4094) { overflow_ = true; return; }
            // re-scatter the offset into the B-type immediate fields, keeping the rest.
            w &= ~((1u<<31) | (0x3fu<<25) | (0xfu<<8) | (1u<<7));
            uint32_t o = uint32_t(off) & 0x1fff;
            w |= (((o>>12)&1)<<31) | (((o>>5)&0x3f)<<25) | (((o>>1)&0xf)<<8) | (((o>>11)&1)<<7);
        } else {
            // J-type: a DIFFERENT scatter of the same signed byte offset: imm[20|10:1|11|19:12] in bits 31..12. Sharing the B-type arithmetic would silently retarget the call, which is why the fixup carries its kind.
            if (off < -1048576 || off > 1048575) { overflow_ = true; return; }
            w &= 0x00000fffu;                       // keep opcode + rd
            uint32_t o = uint32_t(off);
            w |= (((o>>20)&1)<<31) | (((o>>1)&0x3ff)<<21) | (((o>>11)&1)<<20) | (((o>>12)&0xff)<<12);
        }
        std::memcpy(buf_ + f.at, &w, 4);
    }
}


// The two-line binding of core's IR walk to THIS assembler. It lives here rather than in its own file because a template instantiation can only exist where its argument does: `lowerToBytes` for riscv is not separable from RiscvAssembler.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<RiscvAssembler>(ir, out, cap, squeeze, kRegCount);
}

}  // namespace mm::moonlive

#endif  // __riscv
