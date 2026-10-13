#pragma once

#include "platform/platform.h"   // alloc/free: the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap: one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

/// @defgroup moonlive_asm_thumb2 MoonLive Thumb-2 assembler
/// @{
/// The Teensy 4.1 backend: ARMv7E-M Thumb-2 for the Cortex-M7, written against the same named-instruction interface as the other backends.
///
/// Wide encodings throughout, with no IT blocks and no literal pools: a constant is a MOVW/MOVT pair and a conditional branch is a compare plus a wide branch.
/// r0-r6, r8-r11 and lr hold values: every register a routine saving r4-r11 and lr may use, less r7, the Thumb frame pointer, and r12, the one scratch.
///
/// The AAPCS passes the fifth argument, the control arena, on the stack. The prologue loads it into r4 once r4 is saved, and a script-to-script call pushes it the same way.
/// A host call is MOVW/MOVT into r12 then BLX r12: the emitted code in OCRAM sits beyond BL's 16 MB reach of the built-ins in flash or ITCM. The C function pointer already carries the Thumb bit.

namespace mm::moonlive {

/// Twelve registers, the AAPCS argument registers plus the saved set less the frame pointer.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, kRegCount };
/// An index into the label table; bind fixes its position and branches to it are patched then.
using Label = uint8_t;

class Thumb2Assembler {
public:
    /// The register type the shared lowering works in, each backend's Reg being its own enum.
    using RegType = Reg;

    /// Frees the buffer only when this emitter owns it; a copy would double-free, so copying is deleted.
    ~Thumb2Assembler() { if (owned_) platform::free(buf_); }
    /// Allocate a `cap`-byte code buffer, sized per script because backends differ in density.
    explicit Thumb2Assembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))), owned_(true) {}
    /// Emit straight into the caller's staging buffer, which halves a compile's transient heap.
    Thumb2Assembler(uint8_t* out, size_t cap)
        : kCap(cap), buf_(out), owned_(false) {}
    /// Not copied: a copied owner would free the same buffer twice.
    Thumb2Assembler(const Thumb2Assembler&) = delete;
    /// Not copy-assigned, for the same reason.
    Thumb2Assembler& operator=(const Thumb2Assembler&) = delete;

    /// Resolve every fixup against its bound label; call it once, after the last instruction.
    void finalize() { patchBranches(); }
    /// Pad to a 4-byte boundary with a 16-bit NOP, the entry alignment every backend's shared test asserts.
    void alignForEntry();
    /// The finished bytes, valid only after finalize.
    const uint8_t* bytes() const { return buf_; }
    /// How many bytes were emitted.
    size_t size() const { return len_; }
    /// Whether any write was dropped for want of room.
    bool overflowed() const { return overflow_; }

    // --- the call frame ---
    /// Save r4-r11 and lr, open `slots` frame slots, and load the stack-passed fifth argument into r4.
    void prologue(uint8_t slots);
    /// Write a register into a frame slot.
    void spillStore(Reg r, uint8_t slot);
    /// Read a frame slot back into a register.
    void spillLoad(Reg r, uint8_t slot);
    /// Address one slot, which is how a call builds its argument block.
    void slotAddr(Reg d, uint8_t slot);
    /// The allocator's slot range plus the parked host arguments.
    static constexpr uint8_t kMaxSpillSlots = kTotalSlots;

    /// A fresh label, to be bound once and branched to any number of times.
    Label newLabel();
    /// Fix this label's position at the current offset.
    void  bind(Label l);

    /// A full-width address into a register: MOVW then MOVT.
    void movPtr(Reg d, const void* p);
    /// An immediate of any width: MOVW, plus MOVT when the top half is not zero.
    void movImm(Reg d, int32_t imm);
    /// MOV d, a.
    void movReg(Reg d, Reg a);
    /// ADDW or SUBW with a 12-bit immediate, through r12 past that.
    void addImm(Reg d, Reg a, int32_t imm);
    /// ADD.W d, a, b.
    void addReg(Reg d, Reg a, Reg b);
    /// MUL d, a, b.
    void mulReg(Reg d, Reg a, Reg b);
    /// SMMUL d, a, b: the SIGNED high 32 bits, an ARMv7E-M DSP instruction.
    void mulhi(Reg d, Reg a, Reg b);
    /// LSL.W d, a, #n.
    void shlImm(Reg d, Reg a, uint8_t n);
    /// ASR.W d, a, #n: arithmetic, sign-filling.
    void sarImm(Reg d, Reg a, uint8_t n);
    /// LSR.W d, a, #n: logical, zero-filling.
    void shrImm(Reg d, Reg a, uint8_t n);
    /// STRB.W val, [base, off].
    void store8(Reg base, Reg off, Reg val);
    /// LDRB.W d, [base, #imm]: a control read.
    void load8(Reg d, Reg base, int32_t imm);
    /// LDR.W d, [base, #imm]: a whole 4-byte slot.
    void load32(Reg d, Reg base, int32_t imm);
    /// STR.W val, [base, #imm].
    void store32(Reg base, int32_t imm, Reg val);
    /// LDR.W d, [base, off].
    void load32Idx(Reg d, Reg base, Reg off);
    /// STR.W val, [base, off].
    void store32Idx(Reg base, Reg off, Reg val);
    /// LDRB.W d, [base, off].
    void load8Idx(Reg d, Reg base, Reg off);
    /// CMP.W a, #0 then BEQ.W l.
    void branchIfZero(Reg a, Label l);
    /// CMP.W a, b then BHS.W l.
    void branchGeU(Reg a, Reg b, Label l);
    /// CMP.W a, b then BGE.W l.
    void branchGeS(Reg a, Reg b, Label l);
    /// CMP.W a, b then BNE.W l.
    void branchNe(Reg a, Reg b, Label l);
    /// Call a host built-in d = fn(a, b, c) through BLX r12, preserving every vreg across it.
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);
    /// Call a function in this block by label with BL: the script-to-script call.
    void callLabel(Label l, Reg d = R0, bool take = false);
    /// Tear down prologue's frame and return by popping the saved lr into pc.
    void epilogue();
    /// Park `a` in r0, where the AAPCS returns a value, before the epilogue runs.
    void retValue(Reg a);

private:
    // The buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core so the backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    void emit16(uint16_t hw);
    /// One 32-bit instruction written as the ARM manual prints it, first halfword on top, stored as two little-endian halfwords.
    void emit32(uint32_t ins);
    /// A wide conditional branch to `l`, the condition already set by a compare.
    void branchCond(uint8_t cond, Label l);
    void cmp(Reg a, Reg b);
    // A pending label reference; the kind matters, BL and B<cond>.W scattering the offset differently.
    enum class FixKind : uint8_t { Branch, Call };
    void addFixup(size_t at, Label label, FixKind kind);

    // Heap, as every backend's buffer is, so no task's stack carries it.
    uint8_t* buf_;
    bool     owned_;   ///< true when we allocated the buffer and free it
    size_t   len_ = 0;
    bool     overflow_ = false;
    // Bytes from the frame pointer to the caller's sp, 0 before a prologue, so a teardown cannot disagree with its setup.
    uint16_t frameBytes_ = 0;

    CodePos  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    struct Fixup { CodePos at; Label label; FixKind kind; };
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();
};

/// @}

}  // namespace mm::moonlive
