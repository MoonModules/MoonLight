#pragma once

#include "platform/platform.h"   // alloc/free: the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap: one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

/// @defgroup moonlive_asm_riscv MoonLive RISC-V assembler
/// @{
/// The ESP32-P4 backend, written against the same named-instruction interface as the other two.
///
/// Fixed four-byte instructions and a standard call ABI make it the simplest of the three.
/// Every register maps into the caller-saved set, so a call saves the live pool explicitly, and branch displacements are back-patched against bound labels.
///
/// @moreinfo
///
/// ## Fourteen registers
///
/// That is what the caller-saved set alone provides, and a nested loop needs more than the twelve every backend started with.
/// It briefly reached eighteen by mapping callee-saved registers too, on the reasoning that the routine saves what it uses.
/// It does not, having no prologue, so it would have returned with four of them clobbered.
///
/// ## Slots address from the frame pointer
///
/// A call moves the stack pointer underneath them, and reading a spilled value after a built-in call is the ordinary case.
///
/// ## Why the buffer is heap
///
/// The assembler is a stack local, so a cap-sized member put 2 KB on the compile chain's stack and overflowed the task on a classic ESP32.

namespace mm::moonlive {

/// Fourteen registers, which is what the caller-saved set alone provides.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11,
                     R12, R13, kRegCount };
/// An index into the label table; bind fixes its position and branches to it are patched then.
using Label = uint8_t;
/// A branch condition, holding the ones the IR needs.
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class RiscvAssembler {
public:
    /// The register type the shared lowering works in, each backend's Reg being its own enum.
    using RegType = Reg;

    /// Frees the buffer only when this emitter owns it; a copy would double-free, so copying is deleted.
    ~RiscvAssembler() { if (owned_) platform::free(buf_); }
    /// Allocate a `cap`-byte code buffer, sized per script because backends differ by up to 1.9x.
    explicit RiscvAssembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))), owned_(true) {}
    /// Emit straight into the caller's staging buffer, which halves a compile's transient heap.
    RiscvAssembler(uint8_t* out, size_t cap)
        : kCap(cap), buf_(out), owned_(false) {}
    /// Not copied: a copied owner would free the same buffer twice.
    RiscvAssembler(const RiscvAssembler&) = delete;
    /// Not copy-assigned, for the same reason.
    RiscvAssembler& operator=(const RiscvAssembler&) = delete;

    /// Resolve every fixup against its bound label; call it once, after the last instruction.
    void finalize() { patchBranches(); }
    /// Pad to the alignment a function entry needs, which on this architecture is nothing.
    void alignForEntry() {}
    /// The finished bytes, valid only after finalize.
    const uint8_t* bytes() const { return buf_; }
    /// How many bytes were emitted.
    size_t size() const { return len_; }
    /// Whether any write was dropped for want of room.
    bool overflowed() const { return overflow_; }

    // --- the call frame ---
    /// Open a frame for `slots` spilled values; no slots emits nothing, so a plain script pays none.
    void prologue(uint8_t slots = 0);
    /// Write a register into a spill slot.
    void spillStore(Reg r, uint8_t slot);
    /// Read a spill slot back into a register.
    void spillLoad(Reg r, uint8_t slot);
    /// Address one slot, which is how a call builds its argument block.
    void slotAddr(Reg d, uint8_t slot);
    /// The allocator's slot range plus the parked host arguments.
    static constexpr uint8_t kMaxSpillSlots = kTotalSlots;

    /// A fresh label, to be bound once and branched to any number of times.
    Label newLabel();
    /// Fix this label's position at the current offset.
    void  bind(Label l);

    /// A full-width address into a register (ConstPtr).
    void movPtr(Reg d, const void* p);
    /// An immediate of any width into a register, through the small-immediate add where it fits.
    void movImm(Reg d, int32_t imm);
    /// Mv rd, ra   (addi rd, ra, 0).
    void movReg(Reg d, Reg a);
    /// Addi rd, ra, imm.
    void addImm(Reg d, Reg a, int32_t imm);
    /// add rd, ra, rb.
    void addReg(Reg d, Reg a, Reg b);
    /// Mul rd, ra, rb.
    void mulReg(Reg d, Reg a, Reg b);
    /// Mulh rd, ra, rb: the SIGNED high 32 bits.
    void mulhi(Reg d, Reg a, Reg b);
    /// slli rd, ra, #n.
    void shlImm(Reg d, Reg a, uint8_t n);
    /// srai rd, ra, #n: arithmetic, sign-filling.
    void sarImm(Reg d, Reg a, uint8_t n);
    /// srli rd, ra, #n: logical, zero-filling.
    void shrImm(Reg d, Reg a, uint8_t n);
    /// add tmp,base,off ; sb val,0(tmp).
    void store8(Reg base, Reg off, Reg val);
    /// Lbu rDst, imm(rBase): a control read.
    void load8(Reg d, Reg base, int32_t imm);
    /// Lw rDst, imm(rBase): a whole 4-byte slot.
    void load32(Reg d, Reg base, int32_t imm);
    /// sw rVal, imm(rBase) (offset IMMEDIATE).
    void store32(Reg base, int32_t imm, Reg val);
    /// add tmp,base,off ; lw d,0(tmp).
    void load32Idx(Reg d, Reg base, Reg off);
    /// add tmp,base,off ; sw val,0(tmp).
    void store32Idx(Reg base, Reg off, Reg val);
    /// add tmp,base,off ; lbu d,0(tmp).
    void load8Idx(Reg d, Reg base, Reg off);
    /// Beqz a, l  (bge x0, a... use bgeu against x0).
    void branchIfZero(Reg a, Label l);
    /// Bgeu a, b, l.
    void branchGeU(Reg a, Reg b, Label l);
    /// Bge  a, b, l.
    void branchGeS(Reg a, Reg b, Label l);
    /// Bne a, b, l.
    void branchNe(Reg a, Reg b, Label l);
    /// Standard call to a host built-in.
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);
    /// Call a function in this block by label: the script-to-script call.
    void callLabel(Label l, Reg d = R0, bool take = false);
    /// Undo prologue's frame (if any), then ret.
    void epilogue();
    /// Park `a` where the ABI returns a value, before the epilogue tears the frame down.
    void retValue(Reg a);
    /// Return to the caller.
    void ret();

private:
    // The buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core so the three backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    void emit32(uint32_t w);
    /// One conditional branch, as an inverted short branch over a jump, which is why every one takes two words.
    void branchRelaxed(uint8_t rs1, uint8_t rs2, uint8_t f3, Label l);
    // A pending label reference; the kind matters, a branch and a jump using different fields.
    enum class FixKind : uint8_t { Branch, Jal };
    void addFixup(size_t at, Label label, FixKind kind = FixKind::Branch);

    // Heap rather than a member array, which put 2 KB on the stack and overflowed a classic ESP32.
    uint8_t* buf_;
    bool     owned_;   ///< true when we allocated the buffer and free it
    size_t   len_ = 0;
    bool     overflow_ = false;
    // Frame size, 0 without a prologue, so a teardown cannot disagree with its setup about the stack.
    uint16_t frameBytes_ = 0;

    CodePos  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    struct Fixup { size_t at; Label label; FixKind kind = FixKind::Branch; };
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();
};

/// @}

}  // namespace mm::moonlive
