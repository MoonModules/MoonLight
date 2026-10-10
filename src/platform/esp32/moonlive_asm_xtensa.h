#pragma once

#include "platform/platform.h"   // alloc/free: the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap: one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

/// @defgroup moonlive_asm_xtensa MoonLive Xtensa assembler
/// @{
/// The classic ESP32 and S3 backend, written against the same named-instruction interface as the host one.
///
/// Only the encodings and the windowed ABI differ: the emitted routine opens with `entry` and returns with `retw.n`, the host arguments arriving in a2 upward.
/// Branch displacements are back-patched, so no offset is hand-computed.
///
/// @moreinfo
///
/// ## The window is the return path
///
/// Ten registers map to a2 upward, and the top of the window is not a general register.
/// A routine opened with `entry` returns through `retw.n`, which reads the caller's linkage from the window's top.
/// Using those as registers corrupted that linkage and returned to a garbage address the moment a scripted layout ran.
///
/// ## Entry alignment, twice over
///
/// The entry instruction must sit on a four-byte boundary, and a call encodes its target in four-byte units, so an unaligned callee cannot be expressed at all.
/// Instructions here are two or three bytes, so a function following another lands anywhere and needs the pad.
///
/// ## Why the buffer is heap
///
/// The assembler is a stack local, so a cap-sized member put 2 KB on the compile chain's stack and overflowed the task on a classic ESP32.

namespace mm::moonlive {

/// Ten registers, mapping to a2 upward; the top of the window is the return path, not a register.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, kRegCount };
/// An index into the label table; bind fixes its position and branches to it are patched then.
using Label = uint8_t;
/// A branch condition, holding the ones the IR needs.
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

/// The register map, for the device-codegen test.
const uint8_t* xtRegMap(uint8_t& count);

class XtensaAssembler {
public:
    /// The register type the shared lowering works in, each backend's Reg being its own enum.
    using RegType = Reg;

    /// Frees the buffer only when this emitter owns it; a copy would double-free, so copying is deleted.
    ~XtensaAssembler() { if (owned_) platform::free(buf_); }
    /// Allocate a `cap`-byte code buffer, sized per script because backends differ by up to 1.9x.
    explicit XtensaAssembler(size_t cap = kCodeCap)
        : kCap(cap), buf_(static_cast<uint8_t*>(platform::alloc(cap))), owned_(true) {}
    /// Emit straight into the caller's staging buffer, which halves a compile's transient heap.
    XtensaAssembler(uint8_t* out, size_t cap)
        : kCap(cap), buf_(out), owned_(false) {}
    /// Not copied: a copied owner would free the same buffer twice.
    XtensaAssembler(const XtensaAssembler&) = delete;
    /// Not copy-assigned, for the same reason.
    XtensaAssembler& operator=(const XtensaAssembler&) = delete;

    /// Resolve every fixup against its bound label; call it once, after the last instruction.
    void finalize() { patchBranches(); }
    /// Pad to the four-byte boundary a function entry needs, which this architecture requires twice over.
    void alignForEntry() {
        while ((len_ & 3u) != 0) { const uint8_t z = 0; emit(&z, 1); }
    }
    /// The finished bytes, valid only after finalize.
    const uint8_t* bytes() const { return buf_; }
    /// How many bytes were emitted.
    size_t size() const { return len_; }
    /// Whether any write was dropped for want of room.
    bool overflowed() const { return overflow_; }

    // --- the call frame ---
    /// Open the routine's frame, widened to carry `slots` spilled values; must be the first instruction.
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
    /// An immediate of any width into a register, through the narrow move where it fits.
    void movImm(Reg d, int32_t imm);
    /// Mov.n aD, aA.
    void movReg(Reg d, Reg a);
    /// Addi.n aD, aA, #imm (1..15).
    void addImm(Reg d, Reg a, int32_t imm);
    /// add.n aD, aA, aB.
    void addReg(Reg d, Reg a, Reg b);
    /// Mull aD, aA, aB.
    void mulReg(Reg d, Reg a, Reg b);
    /// Mulsh aD, aA, aB: the SIGNED high 32 bits.
    void mulhi(Reg d, Reg a, Reg b);
    /// slli aD, aA, #n (1..31).
    void shlImm(Reg d, Reg a, uint8_t n);
    /// srai aD, aA, #n (0..31), arithmetic.
    void sarImm(Reg d, Reg a, uint8_t n);
    /// LOGICAL right shift (srli / extui).
    void shrImm(Reg d, Reg a, uint8_t n);
    /// S8i via computed address (add then s8i,0).
    void store8(Reg base, Reg off, Reg val);
    /// L8ui aDst, aBase, #imm: a control read.
    void load8(Reg d, Reg base, int32_t imm);
    /// L32i.n aDst, aBase, #imm: a whole 4-byte slot.
    void load32(Reg d, Reg base, int32_t imm);
    /// s32i.n aVal, aBase, #imm (offset IMMEDIATE).
    void store32(Reg base, int32_t imm, Reg val);
    /// add.n tmp,base,off ; l32i.n d,tmp,0.
    void load32Idx(Reg d, Reg base, Reg off);
    /// add.n tmp,base,off ; s32i.n val,tmp,0.
    void store32Idx(Reg base, Reg off, Reg val);
    /// add.n tmp,base,off ; l8ui d,tmp,0.
    void load8Idx(Reg d, Reg base, Reg off);
    /// Beqz aA, l  (nLights==0 guard).
    void branchIfZero(Reg a, Label l);
    /// Bgeu aA, aB, l  (Bounds: skip if a>=b).
    void branchGeU(Reg a, Reg b, Label l);
    /// Bge  aA, aB, l  (a script's own comparison).
    void branchGeS(Reg a, Reg b, Label l);
    /// Bne aA, aB, l   (loop test).
    void branchNe(Reg a, Reg b, Label l);
    /// Windowed call8 to a host built-in.
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);
    /// Call a function in this block by label: the script-to-script call.
    void callLabel(Label l, Reg d = R0, bool take = false);
    /// Retw.n.
    void epilogue();
    /// Park `a` where the ABI returns a value, before the epilogue tears the frame down.
    void retValue(Reg a);

private:
    // The buffer's size, fixed for this object's life but chosen per script.
    const size_t kCap;
    // Sized in core so the three backends cannot drift apart.
    static constexpr uint8_t kMaxLabels = kAsmLabels;
    static constexpr uint8_t kMaxFixups = kAsmFixups;

    void emit(const uint8_t* p, size_t n);
    /// Narrow (16-bit) instruction.
    void emit2(uint16_t w);
    /// Wide (24-bit) instruction.
    void emit3(uint32_t w);
    // A pending label reference; the kind matters, a call's displacement being scaled and a jump's not.
    enum class FixKind : uint8_t { Jump, Call };
    struct Fixup { size_t at; Label label; FixKind kind = FixKind::Jump; };
    /// Enqueue a fixup (bounds-checked).
    void addFixup(size_t at, Label label, FixKind kind = FixKind::Jump);

    // Heap rather than a member array, which put 2 KB on the stack and overflowed a classic ESP32.
    uint8_t* buf_;
    bool     owned_;   ///< true when we allocated the buffer and free it
    size_t   len_ = 0;
    bool     overflow_ = false;

    CodePos  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    // A conditional branch emitted as an inverted condition over a jump, which reaches far enough.
    void branchRelaxed(uint8_t condNibble, Reg a, Reg b, Label l);
    void patchBranches();
};

/// @}

}  // namespace mm::moonlive
