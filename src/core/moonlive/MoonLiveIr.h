#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"   // InlineOp (a neutral opcode tag)
#include "platform/platform.h"                // alloc/free, since the op array is script-sized

/// @defgroup moonlive_ir MoonLive intermediate representation
/// @{
/// The typed form between the front-end and the per-ISA assembler.
///
/// The front-end lowers an AST to a flat list of three-address ops over virtual registers, and each backend lowers that same list to machine bytes.
/// Compile-time only, consumed during lowering and never present at run time.
///
/// @moreinfo
///
/// ## It knows operations, not targets
///
/// The IR names no instruction set and no domain: a buffer write is an inline op carrying a tag the host registered.
///
/// ## Virtual registers
///
/// Plain indices a backend maps to machine registers.
/// The host arguments arrive in fixed ones, named neutrally, so the front-end refers to them without knowing the calling convention.

namespace mm::moonlive {

using VReg = uint8_t;

// kArg4 is a per-instance data pointer the host passes at run time, which LoadCtrl reads from.
enum : VReg { kArg0 = 0, kArg1 = 1, kArg2 = 2, kArg3 = 3, kArg4 = 4, kFirstTemp = 5 };

// Larger than any register file, since this bounds the compiler's tables, not the script.
static constexpr uint8_t kMaxVRegs = 32;
// A sanity bound rather than the working limit, so a runaway source fails with a diagnostic.
static constexpr uint16_t kMaxIrOps = 4096;

// Measured at 0.75 ops per token and never above 0.85, so 1 keeps a real margin.
static constexpr uint16_t kIrOpsPerToken = 1;

// The op set, in three-address form: a destination plus up to three source operands.
enum class IrOp : uint8_t {
    Const,     // dst = imm
    Add,       // dst = a + b
    AddImm,    // dst = a + imm
    Mul,       // dst = a * b
    Mulhi,     // dst = the SIGNED high 32 bits of a * b. With Mul it spells a Q16.16 multiply:
               // the 64-bit product's middle word is (Mulhi << 16) | (Mul >>> 16).
    Shl,       // dst = a << imm, also how toFixed(v) is spelled (imm 16)
    Shr,       // dst = a >> imm, LOGICAL (zero-filling). The low word of a 64-bit product is
               // unsigned, so a fixed multiply needs this rather than Sar for its bottom half.
    Sar,       // dst = a >> imm, sign-filling: toInt(v) is this with imm 16
               // Logical would turn every negative fixed value into a large positive int.
    Call,      // dst = (*callFn)(&frame[imm], b, arena): a host-registered function
               // A position and a count rather than the values, so arity is bounded by frame slots.
    CallScript,// call the script's OWN function, the one numbered `imm` in declaration order.
               // A function number rather than an IR index, which the spill rewrite would shift.
    ConstPtr,  // dst = the pointer in `ptr`: a full-width address materialized into a register.
               // Distinct from Const because a pointer cannot ride a 32-bit immediate.
    Inline,    // a host-registered inline op (inlineOp tag); operands a/b/c/d (op-specific)
    LoadCtrl,  // dst = ((const uint8_t*)kArg4)[imm], a control byte at offset imm
    LoadCtrl32,  // dst = *(int32_t*)((const uint8_t*)kArg4 + imm): read a member's whole 4-byte
                 // Every scalar occupies a whole slot, and signed is the reading serving all four.
    StoreCtrl32, // *(int32_t*)((uint8_t*)kArg4 + imm) = a: write a member's whole slot. Takes the
                 // Offset as an immediate, which is one instruction shorter than the store path.
    LoadIdx,   // dst = arena[base + a * width]: read an ARRAY element, index in vreg `a`.
    StoreIdx,  // arena[base + a * width] = b: write an ARRAY element, index in vreg `a`.
               // Packed into `imm`, since the spill pass renumbers every vreg field.
    StoreCtrl, // ((uint8_t*)kArg4)[imm] = a: write an arena byte, which is what a MEMBER is.
               // An arena byte outlives every call and keeps its address, which a frame slot cannot.
    Mov,       // dst = a, the assignment a loop variable needs
    Label,     // a branch target; `imm` is the label id. Emits no instruction.
    BranchGe,  // if (a >= b) goto label `imm`, unsigned for its three users: the loop's entry
               // Unsigned, so the index clamp catches both ends of the range with one branch.
    BranchGeS, // if (a >= b) goto label `imm`, SIGNED: the comparison a script writes. Separate
               // Its own op, so a backend forgetting it fails to compile rather than comparing wrongly.
    BranchNe,  // if (a != b) goto label `imm`, the backward edge that closes the loop
    Ret,       // return from the enclosing function, with the value in `a` when `imm` is 1.
               // A jump to the one exit, since the epilogue decrements the depth counter.
    Spill,     // frame slot `imm` = a, a value the register file could not hold
    Reload,    // dst = frame slot `imm`, the same value brought back for one use
};

// The allocator's output: `imm` is a slot index, so the core never knows a stack layout.

// Two branches and no jump: a bottom-tested loop needs an entry guard and a back edge.

struct IrInst {
    /// Which operation this instruction performs.
    IrOp     op;
    /// The destination register.
    VReg     dst = 0;
    /// The source registers, which each opcode reads as it needs.
    VReg     a = 0, b = 0, c = 0, d = 0;
    /// The immediate, or an address offset, by opcode.
    int32_t  imm = 0;
    /// The host function a Call targets.
    HostCallFn callFn = nullptr;
    /// The address a ConstPtr materializes.
    const void* ptr = nullptr;
    /// The neutral opcode tag an Inline op carries.
    InlineOp inlineOp{};
};

// A control a script declared, whose width derives from its type rather than sitting beside it.

struct DeclaredControl {
    /// The name the script declared, pointing into the source buffer.
    const char* name = nullptr;
    /// The UI range, as wide as the widest member a control can bind.
    int32_t     min = 0, max = 255;
    // Separate from the range, since a member may be seeded outside what its slider spans.
    /// The initializer, wide enough for the widest member type.
    int32_t     def = 0;
    /// How many characters of `name` are used.
    uint8_t     nameLen = 0;
    /// The member's element type, from which its width derives.
    CtrlType    type = CtrlType::Int;
    // A running cursor rather than the declaration index, since a scalar costs a whole slot.
    /// Byte offset into the controls arena, which everything downstream keys on.
    uint8_t     offset = 0;
    /// Elements: 1 for a scalar, the length for an array.
    uint8_t     count = 1;
    /// A dropdown's option names, `max + 1` of them, or null for a slider or a switch.
    const char* const* options = nullptr;
};

// One encoder and three accessors, so the emitter and the lowering cannot disagree.
/// Pack base, width and count into `imm`, the one field the allocator does not rewrite.
constexpr int32_t idxPack(uint8_t base, uint8_t width, uint8_t count) {
    return int32_t(base) | (int32_t(width) << 8) | (int32_t(count) << 16);
}
constexpr uint8_t idxBase(int32_t p)  { return uint8_t(p & 0xff); }
constexpr uint8_t idxWidth(int32_t p) { return uint8_t((p >> 8) & 0xff); }
constexpr uint8_t idxCount(int32_t p) { return uint8_t((p >> 16) & 0xff); }


// Two per `for`, counted program-wide, so this bounds total loops rather than nesting depth.
/// Branch targets one IR program may use.
static constexpr uint8_t kIrLabels = 40;

// THE COST IS STACK: a label is a two-byte code position, since no program passes the 16 KB code cap, so 96 of them take what 48 four-byte ones did.
/// Labels and fixups an assembler's tables hold.
static constexpr uint8_t kAsmLabels = 96;
static constexpr uint8_t kAsmFixups = 96;
/// Where a label is bound in the code, or kUnbound before it is.
using CodePos = uint16_t;
static constexpr CodePos kUnbound = 0xFFFF;

// What is live at once rather than a total, since a block hands its slots back at its brace.
/// How many frame slots one program may hold at once.
static constexpr uint8_t kMaxLocals = 32;

// Bounded by the frame rather than the register file, which is what makes a wide builtin work.
/// The most arguments one call can carry.
static constexpr uint8_t kMaxCallArgs = kMaxLocals;

// Holding them in registers costs five program-wide, which on Xtensa decides a compile.
/// Where the host arguments are parked.
static constexpr uint8_t kHostArgSlots = kFirstTemp;                 // 5
constexpr uint8_t hostArgSlot(VReg v) {
    return static_cast<uint8_t>(kMaxLocals + kHostArgSlots - kFirstTemp + v);
}
/// Total frame slots a backend must address: the allocator range plus the parked host arguments.
static constexpr uint8_t kTotalSlots = kMaxLocals + kHostArgSlots;

// Mirrors kMaxEntryPoints, since the two are filled from the same parse.
/// Named functions one program may define.
static constexpr uint8_t kMaxIrEntries = 8;


static constexpr uint8_t kMaxControlName = 24;   // max control-name length (incl. NUL); the compiler
                                                 // Longer names are rejected, so none collide.

// A lowered program: the ops, sized to the script, plus the vreg high-water mark.
struct IrProgram {
    /// The lowered ops, heap-allocated and sized to the script.
    IrInst*  ops = nullptr;
    /// How many op entries are allocated.
    uint16_t cap = 0;
    /// How many ops the program holds.
    uint16_t count = 0;
    /// The high-water mark of virtual registers this program names.
    VReg     vregsUsed = kFirstTemp;
    // The allocator numbers any spill from here up, since the two share one frame.
    /// Frame slots the front end allocated for script variables.
    uint8_t  localSlots = 0;

    // The parser records the index and the lowering fills the byte, the crossing a linker makes.
    /// Where each named function's code starts, filled in as the lowering walks the ops.
    uint16_t fnIrStart[kMaxIrEntries] = {};
    /// The byte each named function starts at, which the lowering fills.
    uint16_t fnOffset[kMaxIrEntries]  = {};
    /// How many named functions the program defines.
    uint8_t  fnCount = 0;

    /// An empty program, until `reserve` sizes its op array.
    IrProgram() = default;
    /// Release the op array.
    ~IrProgram() { platform::free(ops); }
    /// Never copied: it owns a buffer, and two owners would double-free.
    IrProgram(const IrProgram&) = delete;
    IrProgram& operator=(const IrProgram&) = delete;

    /// Size the op array to `n` entries, returning false when it cannot be allocated.
    bool reserve(uint16_t n) {
        if (n == 0 || n > kMaxIrOps) return false;
        platform::free(ops);
        ops = static_cast<IrInst*>(platform::alloc(sizeof(IrInst) * n));
        cap = ops ? n : 0;
        count = 0;
        return ops != nullptr;
    }

    /// Append one op, returning false when it names a register outside the budget.
    bool push(const IrInst& i) {
        if (!ops || count >= cap) return false;
        // Opcode-specific, since not every operand field holds a vreg: a Call's `b` is a count.
        if (i.dst >= kMaxVRegs) return false;
        if (i.op == IrOp::Call) {
            if (i.b > kMaxCallArgs) return false;   // a count, not a register
        } else if (i.a >= kMaxVRegs || i.b >= kMaxVRegs ||
                   i.c >= kMaxVRegs || i.d >= kMaxVRegs) {
            return false;
        }
        ops[count++] = i;
        if (i.dst + 1 > vregsUsed) vregsUsed = static_cast<VReg>(i.dst + 1);
        return true;
    }

    // The allocator builds the rewritten program in a second one and swaps it in.
    /// Exchange contents with `o`.
    void swap(IrProgram& o) {
        IrInst* p = ops; ops = o.ops; o.ops = p;
        uint16_t t = cap; cap = o.cap; o.cap = t;
        t = count; count = o.count; o.count = t;
        VReg v = vregsUsed; vregsUsed = o.vregsUsed; o.vregsUsed = v;
        uint8_t ls = localSlots; localSlots = o.localSlots; o.localSlots = ls;
        // The function table moves with the ops: stale boundaries opened a frame two ops early.
        uint8_t fc = fnCount; fnCount = o.fnCount; o.fnCount = fc;
        for (uint8_t f = 0; f < kMaxIrEntries; f++) {
            uint16_t s = fnIrStart[f]; fnIrStart[f] = o.fnIrStart[f]; o.fnIrStart[f] = s;
            uint16_t b = fnOffset[f];  fnOffset[f]  = o.fnOffset[f];  o.fnOffset[f]  = b;
        }
    }

    // How many registers each op costs is per-ISA; which ops are present is the program's property.
    /// Which inline ops this program contains, so a backend reserves only the scratch it needs.
    bool hasInline(InlineOp which) const {
        for (uint16_t i = 0; i < count; i++)   // uint16_t: `count` is, so a uint8_t never terminates
            if (ops[i].op == IrOp::Inline && ops[i].inlineOp == which) return true;
        return false;
    }

    /// Whether any function calls another, which is when the recursion depth guard is emitted.
    bool hasScriptCall() const {
        for (uint16_t i = 0; i < count; i++)
            if (ops[i].op == IrOp::CallScript) return true;
        return false;
    }
};

/// @}

}  // namespace mm::moonlive
