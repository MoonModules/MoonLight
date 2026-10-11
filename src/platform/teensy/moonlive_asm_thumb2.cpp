#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_thumb2.h"

#include <cstring>

#if defined(__ARM_ARCH_7EM__)   // the Thumb-2 assembler is only built for ARMv7E-M targets (Teensy 4.1)

/// @defgroup moonlive_asm_thumb2_impl MoonLive Thumb-2 encodings
/// Named instructions for the Cortex-M7, each checked against arm-none-eabi-as by check_encodings.py.
///
/// An encoder returns a 32-bit instruction the way the ARM manual prints it, first halfword in the top half, and branch offsets are back-patched against bound labels.

namespace mm::moonlive {

static constexpr uint8_t kSp = 13, kLr = 14, kPc = 15;
// R0..R4 are the host arguments in r0..r4, r4 loaded from the stack by the prologue.
static constexpr uint8_t kThReg[kRegCount] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, kLr};
// Bounds-checked: clamping an out-of-range vreg makes a reservation bug wrong but never a read past the table.
static uint8_t tr(Reg r) { return kThReg[r < kRegCount ? r : kRegCount - 1]; }
static constexpr uint8_t kFramePtr = 7;    // r7, the Thumb frame pointer
static constexpr uint8_t kScratch  = 12;   // r12 (ip): call target, result stash, wide immediates

// The vreg pool as a PUSH/POP register list.
constexpr uint16_t poolMask() {
    uint16_t m = 0;
    for (uint8_t r : kThReg) m = uint16_t(m | (1u << r));
    return m;
}
static constexpr uint16_t kPoolMask   = poolMask();
static constexpr uint16_t kSavedMask  = 0x0ff0 | (1u << kLr);   // r4-r11, lr: the AAPCS callee-saved set
static constexpr uint16_t kReturnMask = 0x0ff0 | (1u << kPc);   // r4-r11, pc: the same pop returns
static constexpr uint16_t kSavedBytes = 9 * 4;

// A scratch register that is also a vreg silently corrupts values across a call, so the map is checked.
constexpr bool thumbScratchOutsideMap() {
    for (uint8_t r : kThReg) if (r == kScratch || r == kFramePtr || r == kSp || r == kPc) return false;
    return true;
}
static_assert(thumbScratchOutsideMap(), "a scratch register is also a vreg: calls will corrupt it");
// PUSH stores the lowest register lowest, so an ascending map puts vreg v at [sp, #4*v] after a pool push.
constexpr bool thumbMapAscending() {
    for (uint8_t i = 1; i < kRegCount; i++) if (kThReg[i] <= kThReg[i - 1]) return false;
    return true;
}
static_assert(thumbMapAscending(), "call() reads its arguments at 4*vreg from the pool push");
static uint32_t poolOffset(Reg r) { return 4u * (r < kRegCount ? r : kRegCount - 1); }

void Thumb2Assembler::emit16(uint16_t hw) {
    if (!buf_ || len_ + 2 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(hw); buf_[len_++] = uint8_t(hw >> 8);
}
void Thumb2Assembler::emit32(uint32_t ins) {
    if (!buf_ || len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(ins >> 16); buf_[len_++] = uint8_t(ins >> 24);
    buf_[len_++] = uint8_t(ins);       buf_[len_++] = uint8_t(ins >> 8);
}

void Thumb2Assembler::alignForEntry() { if (len_ & 2) emit16(0xbf00); }   // nop

Label Thumb2Assembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = kUnbound;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }
    Label l = labelCount_++; labelPos_[l] = kUnbound; return l;
}
void Thumb2Assembler::bind(Label l) { if (len_ >= kUnbound) overflow_ = true; else if (l < kMaxLabels) labelPos_[l] = static_cast<CodePos>(len_); }

void Thumb2Assembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

// --- encoders over machine register numbers ---

// The 16-bit MOV (register), the one form that moves between any two of r0-r15, sp included.
static uint16_t encMov(uint8_t d, uint8_t m) {
    return uint16_t(0x4600 | ((d >> 3) << 7) | (m << 3) | (d & 7));
}
// The plain 12-bit immediate i:imm3:imm8 that ADDW, SUBW, MOVW and MOVT scatter over both halfwords.
static uint32_t encImm12(uint16_t op, uint8_t d, uint8_t n, uint32_t imm) {
    return (uint32_t(op | (((imm >> 11) & 1) << 10) | n) << 16) |
           (((imm >> 8) & 7) << 12) | (uint32_t(d) << 8) | (imm & 0xff);
}
static uint32_t encAddw(uint8_t d, uint8_t n, uint32_t imm) { return encImm12(0xf200, d, n, imm); }
static uint32_t encSubw(uint8_t d, uint8_t n, uint32_t imm) { return encImm12(0xf2a0, d, n, imm); }
// MOVW and MOVT carry imm16 as imm4:i:imm3:imm8, the top four bits in the first halfword's register field.
static uint32_t encMovw(uint8_t d, uint16_t imm) { return encImm12(0xf240, d, uint8_t(imm >> 12), imm & 0xfff); }
static uint32_t encMovt(uint8_t d, uint16_t imm) { return encImm12(0xf2c0, d, uint8_t(imm >> 12), imm & 0xfff); }
// Loads and stores: the 12-bit positive-offset form and the register-offset form with no shift.
static constexpr uint16_t kLdr = 0xf8d0, kStr = 0xf8c0, kLdrb = 0xf890, kStrb = 0xf880;
static uint32_t encMemImm(uint16_t op, uint8_t t, uint8_t n, uint32_t imm) {
    return (uint32_t(op | n) << 16) | (uint32_t(t) << 12) | (imm & 0xfff);
}
static uint32_t encMemReg(uint16_t op, uint8_t t, uint8_t n, uint8_t m) {
    // The register form is the immediate form's opcode with bit 7 of the first halfword clear.
    return (uint32_t((op & ~0x0080) | n) << 16) | (uint32_t(t) << 12) | m;
}
static uint32_t encDataReg(uint16_t op, uint8_t d, uint8_t n, uint8_t m) {
    return (uint32_t(op | n) << 16) | (uint32_t(d) << 8) | m;
}
// MOV.W d, m, <shift> #n: type 0 LSL, 1 LSR, 2 ASR, with imm5 split as imm3:imm2.
static uint32_t encShift(uint8_t type, uint8_t d, uint8_t m, uint8_t n) {
    return 0xea4f0000u | (uint32_t((n >> 2) & 7) << 12) | (uint32_t(d) << 8) |
           (uint32_t(n & 3) << 6) | (uint32_t(type) << 4) | m;
}
static uint32_t encPush(uint16_t mask) { return 0xe92d0000u | mask; }   // stmdb sp!, {mask}
static uint32_t encPop(uint16_t mask)  { return 0xe8bd0000u | mask; }   // ldmia sp!, {mask}
// The condition codes the fused branches need.
static constexpr uint8_t kEq = 0x0, kNe = 0x1, kHs = 0x2, kGe = 0xa;

// r7 points at slot 0, so a slot is a positive 12-bit offset that a call moving sp leaves valid, and the caller's sp is r7 + frameBytes_.
void Thumb2Assembler::prologue(uint8_t slots) {
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // The saved set is 36 bytes, so the slots round the frame to the 8 bytes the AAPCS requires of sp at a call.
    const uint16_t total = static_cast<uint16_t>((kSavedBytes + slots * 4 + 7) & ~7);
    frameBytes_ = total;
    emit32(encPush(kSavedMask));                                // push.w {r4-r11, lr}
    emit32(encSubw(kSp, kSp, total - kSavedBytes));             // sub sp, sp, #slot bytes
    emit16(encMov(kFramePtr, kSp));                             // mov r7, sp
    // The fifth argument, which the AAPCS leaves at the caller's sp, loaded once r4 is saved.
    emit32(encMemImm(kLdr, tr(R4), kFramePtr, total));          // ldr.w r4, [r7, #total]
}
void Thumb2Assembler::epilogue() {
    // Without a frame the callee-saved registers this routine uses were never saved, so refuse rather than return.
    if (frameBytes_ == 0) { overflow_ = true; return; }
    emit16(encMov(kSp, kFramePtr));                             // mov sp, r7: drops anything a call left behind
    emit32(encAddw(kSp, kSp, frameBytes_ - kSavedBytes));       // add sp, sp, #slot bytes
    emit32(encPop(kReturnMask));                                // pop.w {r4-r11, pc}
}
void Thumb2Assembler::spillStore(Reg r, uint8_t slot) {
    // No frame means no prologue; emitting anyway would address the caller's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(encMemImm(kStr, tr(r), kFramePtr, slot * 4u));
}
void Thumb2Assembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(encMemImm(kLdr, tr(r), kFramePtr, slot * 4u));
}
void Thumb2Assembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(encAddw(tr(d), kFramePtr, slot * 4u));               // addw d, r7, #off
}

void Thumb2Assembler::movImm(Reg d, int32_t imm) {
    const uint32_t u = static_cast<uint32_t>(imm);
    emit32(encMovw(tr(d), uint16_t(u)));
    if (u >> 16) emit32(encMovt(tr(d), uint16_t(u >> 16)));
}
// Always the pair, since a pointer's top half is rarely zero and a fixed length keeps it simple to read.
void Thumb2Assembler::movPtr(Reg d, const void* p) {
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    emit32(encMovw(tr(d), uint16_t(addr)));
    emit32(encMovt(tr(d), uint16_t(addr >> 16)));
}
void Thumb2Assembler::movReg(Reg d, Reg a) { emit16(encMov(tr(d), tr(a))); }
void Thumb2Assembler::retValue(Reg a) { if (a != R0) movReg(R0, a); }   // r0 is the AAPCS return register

void Thumb2Assembler::addImm(Reg d, Reg a, int32_t imm) {
    if (imm >= 0 && imm <= 4095) { emit32(encAddw(tr(d), tr(a), uint32_t(imm))); return; }
    if (imm < 0 && imm >= -4095) { emit32(encSubw(tr(d), tr(a), uint32_t(-imm))); return; }
    // Past the 12-bit field the constant goes through r12, which is outside the vreg map.
    const uint32_t u = static_cast<uint32_t>(imm);
    emit32(encMovw(kScratch, uint16_t(u)));
    emit32(encMovt(kScratch, uint16_t(u >> 16)));
    emit32(encDataReg(0xeb00, tr(d), tr(a), kScratch));
}
void Thumb2Assembler::addReg(Reg d, Reg a, Reg b) { emit32(encDataReg(0xeb00, tr(d), tr(a), tr(b))); }
void Thumb2Assembler::mulReg(Reg d, Reg a, Reg b) { emit32(encDataReg(0xfb00, tr(d), tr(a), tr(b)) | 0xf000); }
// SMMUL is SMMLA with the accumulator field all ones, and truncates rather than rounds, as mulh does.
void Thumb2Assembler::mulhi(Reg d, Reg a, Reg b)  { emit32(encDataReg(0xfb50, tr(d), tr(a), tr(b)) | 0xf000); }
void Thumb2Assembler::shlImm(Reg d, Reg a, uint8_t n) {
    if (n == 0 || n >= 32) { overflow_ = true; return; }   // a zero LSL is MOV, and 32 has no encoding
    emit32(encShift(0, tr(d), tr(a), n));
}
void Thumb2Assembler::shrImm(Reg d, Reg a, uint8_t n) {
    if (n == 0 || n >= 32) { overflow_ = true; return; }   // imm5 zero means a shift by 32
    emit32(encShift(1, tr(d), tr(a), n));
}
void Thumb2Assembler::sarImm(Reg d, Reg a, uint8_t n) {
    if (n == 0 || n >= 32) { overflow_ = true; return; }
    emit32(encShift(2, tr(d), tr(a), n));
}

void Thumb2Assembler::load8(Reg d, Reg base, int32_t imm) {
    if (imm < 0 || imm > 4095) { overflow_ = true; return; }
    emit32(encMemImm(kLdrb, tr(d), tr(base), uint32_t(imm)));
}
void Thumb2Assembler::load32(Reg d, Reg base, int32_t imm) {
    if (imm < 0 || imm > 4095) { overflow_ = true; return; }
    emit32(encMemImm(kLdr, tr(d), tr(base), uint32_t(imm)));
}
void Thumb2Assembler::store32(Reg base, int32_t imm, Reg val) {
    if (imm < 0 || imm > 4095) { overflow_ = true; return; }
    emit32(encMemImm(kStr, tr(val), tr(base), uint32_t(imm)));
}
void Thumb2Assembler::store8(Reg base, Reg off, Reg val)      { emit32(encMemReg(kStrb, tr(val), tr(base), tr(off))); }
void Thumb2Assembler::load8Idx(Reg d, Reg base, Reg off)      { emit32(encMemReg(kLdrb, tr(d), tr(base), tr(off))); }
void Thumb2Assembler::load32Idx(Reg d, Reg base, Reg off)     { emit32(encMemReg(kLdr, tr(d), tr(base), tr(off))); }
void Thumb2Assembler::store32Idx(Reg base, Reg off, Reg val)  { emit32(encMemReg(kStr, tr(val), tr(base), tr(off))); }

// B<cond>.W: a 21-bit reach of +/-1 MB, past any code cap, so no branch needs relaxing.
void Thumb2Assembler::branchCond(uint8_t cond, Label l) {
    addFixup(len_, l, FixKind::Branch);
    emit32(0xf0008000u | (uint32_t(cond) << 22));
}
void Thumb2Assembler::cmp(Reg a, Reg b) { emit32(encDataReg(0xebb0, 0x0f, tr(a), tr(b))); }   // cmp.w a, b
void Thumb2Assembler::branchIfZero(Reg a, Label l) {
    emit32(0xf1b00f00u | (uint32_t(tr(a)) << 16));             // cmp.w a, #0
    branchCond(kEq, l);
}
void Thumb2Assembler::branchGeU(Reg a, Reg b, Label l) { cmp(a, b); branchCond(kHs, l); }
void Thumb2Assembler::branchGeS(Reg a, Reg b, Label l) { cmp(a, b); branchCond(kGe, l); }
void Thumb2Assembler::branchNe(Reg a, Reg b, Label l)  { cmp(a, b); branchCond(kNe, l); }

// The pool push puts vreg v at [sp, #4*v], so the arguments load from there whichever of r0-r2 a source already is, and its 48 bytes keep sp 8-byte aligned.
void Thumb2Assembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    emit32(encPush(kPoolMask));
    emit32(encMemImm(kLdr, 0, kSp, poolOffset(a)));
    emit32(encMemImm(kLdr, 1, kSp, poolOffset(b)));
    emit32(encMemImm(kLdr, 2, kSp, poolOffset(c)));
    // The built-ins live in flash or ITCM, past BL's reach from OCRAM, and the pointer keeps its Thumb bit for BLX.
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    emit32(encMovw(kScratch, uint16_t(addr)));
    emit32(encMovt(kScratch, uint16_t(addr >> 16)));
    emit16(uint16_t(0x4780 | (kScratch << 3)));                 // blx r12
    emit16(encMov(kScratch, 0));                                // mov r12, r0: past the pool restore
    emit32(encPop(kPoolMask));
    emit16(encMov(tr(d), kScratch));
}

// A function in this block, which reads its fifth argument from the stack as any AAPCS callee does.
void Thumb2Assembler::callLabel(Label l, Reg d, bool take) {
    emit32(encPush(kPoolMask));
    for (uint8_t v = 0; v < kHostArgSlots; v++) spillLoad(static_cast<Reg>(v), hostArgSlot(v));
    // Eight bytes rather than four, so sp stays 8-byte aligned at the call.
    emit32(0xf84d0d08u | (uint32_t(tr(R4)) << 12));              // str.w r4, [sp, #-8]!
    addFixup(len_, l, FixKind::Call);
    emit32(0xf000d000u);                                        // bl, patched
    emit32(encAddw(kSp, kSp, 8));                               // add sp, sp, #8
    if (take) emit16(encMov(kScratch, 0));                      // mov r12, r0
    emit32(encPop(kPoolMask));
    if (take) emit16(encMov(tr(d), kScratch));
}

void Thumb2Assembler::patchBranches() {
    // An overflowed compile is refused afterwards, and a fixup recorded before a dropped instruction points past the end.
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] == kUnbound) continue;   // unbound: overflow_ already failed the compile
        // Relative to the branch's address plus four, the Thumb pc.
        const int32_t off = int32_t(labelPos_[f.label]) - int32_t(f.at) - 4;
        const uint32_t o = uint32_t(off);
        uint16_t hw1 = uint16_t(buf_[f.at] | (buf_[f.at + 1] << 8));
        uint16_t hw2 = uint16_t(buf_[f.at + 2] | (buf_[f.at + 3] << 8));
        const uint32_t s = (o >> 24) & 1;
        if (f.kind == FixKind::Branch) {
            // S:J2:J1:imm6:imm11, the J bits taken directly.
            if (off < -1048576 || off > 1048574) { overflow_ = true; return; }
            hw1 = uint16_t(hw1 | (((o >> 20) & 1) << 10) | ((o >> 12) & 0x3f));
            hw2 = uint16_t(hw2 | (((o >> 18) & 1) << 13) | (((o >> 19) & 1) << 11) | ((o >> 1) & 0x7ff));
        } else {
            // S:I1:I2:imm10:imm11, where J1 = NOT(I1) XOR S and likewise J2, so a short forward call has both set.
            if (off < -16777216 || off > 16777214) { overflow_ = true; return; }
            const uint32_t j1 = (((o >> 23) & 1) ^ 1) ^ s, j2 = (((o >> 22) & 1) ^ 1) ^ s;
            hw1 = uint16_t(hw1 | (s << 10) | ((o >> 12) & 0x3ff));
            hw2 = uint16_t(hw2 | (j1 << 13) | (j2 << 11) | ((o >> 1) & 0x7ff));
        }
        buf_[f.at] = uint8_t(hw1);     buf_[f.at + 1] = uint8_t(hw1 >> 8);
        buf_[f.at + 2] = uint8_t(hw2); buf_[f.at + 3] = uint8_t(hw2 >> 8);
    }
}

// The binding of core's IR walk to this assembler, here because a template instantiation lives where its argument does.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<Thumb2Assembler>(ir, out, cap, squeeze, kRegCount);
}

}  // namespace mm::moonlive

#endif  // __ARM_ARCH_7EM__
