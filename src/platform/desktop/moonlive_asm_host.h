#pragma once

#include "platform/platform.h"   // alloc/free: the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap: one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

/// @defgroup moonlive_asm_host MoonLive host assembler
/// @{
/// The desktop backend: a named-instruction assembler for arm64 and x86-64, in the textbook MacroAssembler shape.
///
/// It appends one instruction at a time and back-patches label offsets, so the lowering composes a multi-op statement without hand-computing a branch displacement.
/// This header is the neutral surface, and the per-ISA encodings live beside it in the implementation.
///
/// @moreinfo
///
/// ## One buffer, borrowed
///
/// The emitter writes into the caller's staging buffer rather than a twin of its own.
/// Allocating a second buffer held two code caps at once, and on a classic ESP32 fragmented to a 24 KB largest block that allocation failed.
/// The compile then reported a script too large for a script that compiles fine.
///
/// ## Sized per script
///
/// The code cap is chosen per script rather than shared, the three backends differing by up to 1.9x on identical source.
///
/// ## Why the buffer is heap
///
/// The assembler is a stack local, so a cap-sized member put 2 KB on the compile chain's stack and overflowed the task on a classic ESP32.

namespace mm::moonlive {

/// An abstract register the assembler maps to a real one, which keeps the IR architecture-neutral.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9,
                     R10, R11, R12, R13, kRegCount };

/// An index into the label table; bind fixes its position and branches to it are patched then.
using Label = uint8_t;

/// A branch condition, holding the ones the IR needs.
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */, Ne /* != */,
                            Ge /* SIGNED >= */ };

class HostAssembler {
public:
    /// The register type the shared lowering works in, each backend's Reg being its own enum.
    using RegType = Reg;

    /// Frees the buffer only when this emitter owns it; a copy would double-free, so copying is deleted.
    ~HostAssembler() { if (owned_) platform::free(buf_); }
    /// Allocate a `cap`-byte code buffer, sized per script because backends differ by up to 1.9x.
    explicit HostAssembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))), owned_(true) {}
    /// Emit straight into the caller's staging buffer, which halves a compile's transient heap.
    HostAssembler(uint8_t* out, size_t cap)
        : kCap(cap), buf_(out), owned_(false) {}
    /// Not copied: a copied owner would free the same buffer twice.
    HostAssembler(const HostAssembler&) = delete;
    /// Not copy-assigned, for the same reason.
    HostAssembler& operator=(const HostAssembler&) = delete;

    // --- buffer ---
    /// Resolve every fixup against its bound label; call it once, after the last instruction.
    void finalize() { patchBranches(); }
    /// Pad to the alignment a function entry needs, which on both host architectures is nothing.
    void alignForEntry() {}
    /// The finished bytes, valid only after finalize.
    const uint8_t* bytes() const { return buf_; }
    /// How many bytes were emitted.
    size_t size() const { return len_; }
    /// Whether any write was dropped for want of room.
    bool overflowed() const { return overflow_; }

    /// Append raw bytes, which owns the bounds check and the overflow flag.
    void emitBytes(const uint8_t* p, size_t n);

    // --- labels ---
    // One definition for both host ISAs, since a table running out is the same failure as a full code buffer.
    /// A fresh label, to be bound once and branched to any number of times.
    Label newLabel() {
        if (labelCount_ == 0) for (auto& p : labelPos_) p = kUnbound;
        if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }
        const Label l = labelCount_++;
        labelPos_[l] = kUnbound;
        return l;
    }
    /// Mark l's position = current offset.
    void bind(Label l) { if (len_ >= kUnbound) overflow_ = true; else if (l < kMaxLabels) labelPos_[l] = static_cast<CodePos>(len_); }

    // --- the call frame ---
    /// Open a frame with room for `slots` spilled values, parking a frame pointer at its base.
    void prologue(uint8_t slots);
    /// Tear the frame down, then ret.
    void epilogue();
    /// Park `a` where the ABI returns a value, before the epilogue tears the frame down.
    void retValue(Reg a);
    /// Write a register into a spill slot.
    void spillStore(Reg r, uint8_t slot);
    /// Read a spill slot back into a register.
    void spillLoad(Reg r, uint8_t slot);
    /// Address one slot, which is how a call builds its argument block.
    void slotAddr(Reg d, uint8_t slot);
    /// The allocator's slot range plus the parked host arguments.
    static constexpr uint8_t kMaxSpillSlots = kTotalSlots;   // what the frame below can address

    // --- instructions, with register and immediate operands ---
    /// A full-width address into a register (ConstPtr).
    void movPtr(Reg d, const void* p);
    /// d = imm.
    void movImm(Reg d, int32_t imm);
    /// d = a + imm.
    void addImm(Reg d, Reg a, int32_t imm);
    /// d = a + b.
    void addReg(Reg d, Reg a, Reg b);
    /// d = a * imm  (index scaling by a constant).
    void mulImm(Reg d, Reg a, int32_t imm);
    /// d = a * b   (index scaling by a runtime cpl).
    void mulReg(Reg d, Reg a, Reg b);
    /// d = the SIGNED high 32 bits of a * b (Q16.16 multiply).
    void mulhi(Reg d, Reg a, Reg b);
    /// d = a << n.
    void shlImm(Reg d, Reg a, uint8_t n);
    /// d = a >> n, ARITHMETIC (sign-filling).
    void sarImm(Reg d, Reg a, uint8_t n);
    /// d = a >> n, LOGICAL (zero-filling).
    void shrImm(Reg d, Reg a, uint8_t n);
    /// Byte store: base[off] = val (low 8 bits).
    void store8(Reg base, Reg off, Reg val);
    /// d = base[imm] (zero-extended byte): control read.
    void load8(Reg d, Reg base, int32_t imm);
    /// d = base[imm..imm+3]: a whole 4-byte slot.
    void load32(Reg d, Reg base, int32_t imm);
    /// base[imm..imm+3] = val (offset IMMEDIATE).
    void store32(Reg base, int32_t imm, Reg val);
    /// d = base[off..off+3], index in a REG.
    void load32Idx(Reg d, Reg base, Reg off);
    /// base[off..off+3] = val, index in a REG.
    void store32Idx(Reg base, Reg off, Reg val);
    /// d = base[off] (zero-extended byte), index in a REG.
    void load8Idx(Reg d, Reg base, Reg off);
    /// d = a.
    void movReg(Reg d, Reg a);
    /// if a == 0 goto l.
    void branchIfZero(Reg a, Label l);
    // Fused compare-and-branch: naming the operation lets one lowering serve all three backends.
    /// if (unsigned)a >= b goto l.
    void branchGeU(Reg a, Reg b, Label l);
    /// if (signed)a >= b goto l.
    void branchGeS(Reg a, Reg b, Label l);
    /// if a != b goto l.
    void branchNe(Reg a, Reg b, Label l);
    /// Call a host built-in: d = fn(a, b, c), preserving every caller register across it.
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);
    /// Call a function in this block by label: the script-to-script call.
    void callLabel(Label l, Reg d = R0, bool take = false);
    /// Return to the caller.
    void ret();

private:
    // The flags pair those branches are built from: private, having no counterpart on the others.
    /// flags = a - b.
    void cmp(Reg a, Reg b);
    /// if flags satisfy c goto l (after cmp).
    void branchIf(Cond c, Label l);
    // The buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core so the three backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    /// Append one 32-bit instruction, arm64 only: an x64 instruction is variable-length, so those encoders call emitBytes.
    void emit32(uint32_t w);
                                         // are variable-length and call emitBytes directly)
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)
    // The four indexed memory ops share one encoder, declared only where it is defined.
    void emitIndexed(const uint8_t* opcode, size_t opLen, bool prefix66, bool forceRex,
                     uint8_t reg, uint8_t base, uint8_t index);
#endif
    // A pending label reference; the kind matters, a call and a branch using different fields.
    enum class FixKind : uint8_t { Branch, Call };
    struct Fixup { size_t at; Label label; FixKind kind = FixKind::Branch; };
    /// Bounds-checked.
    void addFixup(size_t at, Label label, FixKind kind = FixKind::Branch) {
        if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
        fixups_[fixupCount_++] = {at, label, kind};
    }

    // Heap rather than a member array, which put 2 KB on the stack and overflowed a classic ESP32.
    uint8_t* buf_;
    bool     owned_;   ///< true when we allocated the buffer and free it
    size_t   len_ = 0;
    bool     overflow_ = false;
    // Frame size, 0 without a prologue, so a teardown cannot disagree with its setup about the stack.
    uint16_t frameBytes_ = 0;

    // Label positions (kUnbound before binding) and pending branch fixups.
    CodePos  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    /// Resolve all fixups against bound labels.
    void patchBranches();
};

/// @}

}  // namespace mm::moonlive
