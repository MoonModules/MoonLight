#pragma once

#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveSpill.h"   // the register allocator, run before lowering

#include <cstring>
#include <new>       // placement new: the assembler lives on the heap

/// @defgroup moonlive_lower MoonLive lowering
/// @{
/// IR to machine bytes: the one lowering, written once for every backend.
///
/// Walking the IR is not a per-target algorithm, so the walk lives here and each backend supplies its assembler.
///
/// @moreinfo
///
/// ## What core decides
///
/// Which register holds a value, which frame slot it spills to, when a host argument is reloaded, and how an inline op expands.
/// All of it identically for every target, because what differs per target is how an instruction is encoded, and that already lives behind the assembler.
///
/// ## The assembler contract
///
/// Each backend supplies a constructor, the label and frame calls, the move, arithmetic, shift, load and store forms, the branches, the two call forms, and a spill-slot bound.
/// The branches are the fused forms, compare-and-branch as one call.
/// arm64 has no such instruction and spells each as two inside its own assembler.
///
/// ## Include order
///
/// A backend includes its own assembler header before this one.
/// `Reg` and `Label` are declared per assembler, each register file being a different size, so this header names them without declaring them.
/// That is also why it is a template rather than a compiled unit, there being no single `Reg` to compile against.

namespace mm::moonlive {

// Zero means the program cannot be encoded: degrade, never miscompile.
/// Lower `ir` into `out` using assembler `A`, returning the byte count.
template <typename A>
size_t lowerWith(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze,
                 uint8_t regCount) {
    // Deduced from the assembler, and renamed because its own `Reg` is in scope here.
    using RegId = typename A::RegType;
    // From the instance, since a <utility> include nests a second `std` in the tests' namespace.
    auto reg = [](VReg v) { return static_cast<RegId>(v); };

    // Two shared temporaries plus the host-argument reload, unconditional for the depth guard.
    constexpr uint8_t kSharedScratch = 2;
    const uint8_t scratchTotal = kSharedScratch + 1;
    lowerRefusal() = LowerRefusal::None;
    if (!out || cap == 0) return 0;   // unreachable from compileSource, which refuses this first

    // False means even the spilled form does not fit, which is a diagnostic, never a miscompile.
    uint8_t slots = 0;
    // Never `reserved`, or the allocator hands out a register this lowering then overwrites.
    const RegBudget budget = squeeze ? RegBudget{squeeze->regs, scratchTotal, squeeze->slots}
                                     : RegBudget{regCount, scratchTotal, A::kMaxSpillSlots};
    if (!spillToBudget(ir, budget, slots)) { lowerRefusal() = LowerRefusal::Spill; return 0; }
    // sAddr first: a store-only program reserves one scratch, so the shared one must be lowest.
    const RegId sAddr = static_cast<RegId>(ir.vregsUsed);       // per-channel address (both ops)
    const RegId sCtr  = static_cast<RegId>(ir.vregsUsed + 1);   // FillElems loop counter

    // On the heap, since its label and fixup tables would put about 1 KB on the stack of every task that compiles.
    struct HeapAsm {
        A* p;   ///< the assembler, built in place in its heap block
        /// Destroy it and return its block, on every exit from the lowering.
        ~HeapAsm() { if (p) { p->~A(); platform::free(p); } }
    };
    void* mem = platform::alloc(sizeof(A));
    if (!mem) { lowerRefusal() = LowerRefusal::NoMemory; return 0; }
    // Into the caller's buffer, so the assembler and the caller cannot disagree about capacity.
    const HeapAsm owned{new (mem) A(out, cap)};
    A& a = *owned.p;
    using LabelId = decltype(a.newLabel());
    // Derived from scratchTotal rather than fixed, so the reservation and the use cannot drift.
    const RegId sHost = static_cast<RegId>(ir.vregsUsed + scratchTotal - 1);
    auto host = [&](VReg v) -> RegId { a.spillLoad(sHost, hostArgSlot(v)); return sHost; };
    // Covers the parked host arguments too, which are stored before any script code runs.
    const uint8_t frameSlots = slots > kTotalSlots ? slots : kTotalSlots;
    // A class gets a prologue per function instead, since an entry's offset must be jumpable.
    if (ir.fnCount == 0) a.prologue(frameSlots);

    // Lazily, since the range up front exhausts the table; per-function labels are the exception.
    LabelId fnLabel[kMaxIrEntries];
    for (uint8_t f = 0; f < ir.fnCount; f++) fnLabel[f] = a.newLabel();

    // Only for a script whose functions call each other, which no shipped script does.
    const bool guardDepth = ir.hasScriptCall();
    // Its own epilogue, so a refusal unwinds through the same decrement as a normal exit.
    LabelId tooDeep[kMaxIrEntries];
    if (guardDepth)
        for (uint8_t f = 0; f < ir.fnCount; f++) tooDeep[f] = a.newLabel();
    // The one exit, bound ahead of the decrement; zero-initialized for GCC 14's bound analysis.
    LabelId fnExit[kMaxIrEntries] = {};
    for (uint8_t f = 0; f < ir.fnCount; f++) fnExit[f] = a.newLabel();
    // -1 until the first function opens, so a function-less program's Ret is refused.
    int curFn = -1;
    // Function number plus one while a guard is owed, since it is emitted after the parking.
    int guardPending = 0;

    // The all-ones index marks an unmade IR label, since an assembler hands out at most kAsmLabels below it.
    static_assert(kAsmLabels <= LabelId(~LabelId(0)), "the all-ones label index must stay free as the unmade mark");
    constexpr LabelId kUnmade = LabelId(~LabelId(0));
    LabelId labels[kIrLabels];
    for (auto& l : labels) l = kUnmade;
    auto  labelFor = [&](int32_t id) -> LabelId {
        if (labels[id] == kUnmade) labels[id] = a.newLabel();
        return labels[id];
    };

    // After the Spills both the guard and the epilogue read, and flushed from closeFn as well.
    auto flushGuard = [&]() {
        if (!guardPending) return;
        const uint8_t f = static_cast<uint8_t>(guardPending - 1);
        guardPending = 0;
        const RegId d = host(kArg4);              // one reload; nothing below clobbers it
        a.load8(sAddr, d, kDepthSlot);
        a.movImm(sCtr, 1);
        a.addReg(sAddr, sAddr, sCtr);             // depth + 1
        a.movImm(sCtr, kDepthSlot);
        a.store8(d, sCtr, sAddr);                 // arena[kDepthSlot] = depth + 1
        a.movImm(sCtr, kMaxCallDepth);
        a.branchGeU(sAddr, sCtr, tooDeep[f]);     // at or past the limit: unwind immediately
    };

    // The one exit every activation takes, so a refusal cannot skip the decrement and leak.
    auto closeFn = [&](uint8_t f) {
        flushGuard();          // an empty function still balances: increment, then decrement
        // Ahead of the decrement, or an early return leaks a depth level per call.
        a.bind(fnExit[f]);
        if (guardDepth) {
            a.bind(tooDeep[f]);                      // the too-deep path joins here
            const RegId d = host(kArg4);
            a.load8(sAddr, d, kDepthSlot);
            a.movImm(sCtr, -1);
            a.addReg(sAddr, sAddr, sCtr);            // depth - 1
            a.movImm(sCtr, kDepthSlot);
            a.store8(d, sCtr, sAddr);
        }
        a.epilogue();
    };

    // uint16_t matching IrProgram::count, since a uint8_t counter wrapped at 256 ops.
    for (uint16_t i = 0; i < ir.count; i++) {
        // Close the previous and open this one, so every function is independently callable.
        for (uint8_t f = 0; f < ir.fnCount; f++) {
            if (ir.fnIrStart[f] != i) continue;
            if (f > 0) closeFn(static_cast<uint8_t>(f - 1));   // the previous function returns
            curFn = f;
            // Before recording the offset: an Xtensa entry must be 4-byte aligned to be called.
            a.alignForEntry();
            // The first byte a caller executes, which is the frame setup, not the first statement.
            ir.fnOffset[f] = static_cast<uint16_t>(a.size());
            a.bind(fnLabel[f]);                      // where a CallScript to this function lands
            a.prologue(frameSlots);
            // The depth guard, one copy per function, so the deepest calls do nothing.
            guardPending = guardDepth ? int(f) + 1 : 0;   // emit it after the args are parked
        }

        if (ir.ops[i].op != IrOp::Spill) flushGuard();

        const IrInst& op = ir.ops[i];
        switch (op.op) {
            case IrOp::Const:  a.movImm(reg(op.dst), op.imm); break;
            case IrOp::ConstPtr: a.movPtr(reg(op.dst), op.ptr); break;
            case IrOp::Add:    a.addReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::AddImm: a.addImm(reg(op.dst), reg(op.a), op.imm); break;
            case IrOp::Mul:    a.mulReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::Mulhi:  a.mulhi(reg(op.dst), reg(op.a), reg(op.b)); break;
            // An immediate 1..31: Xtensa's slli field holds 32-n and cannot encode zero.
            case IrOp::Shl:
                // Outside 1..31 has no encoding, and refusing beats a wrong constant that runs.
                if (op.imm > 0 && op.imm < 32) a.shlImm(reg(op.dst), reg(op.a), uint8_t(op.imm));
                else if (op.imm == 0) { if (op.dst != op.a) a.movReg(reg(op.dst), reg(op.a)); }
                else a.shlImm(reg(op.dst), reg(op.a), 32);   // no encoding: the assembler refuses
                break;
            case IrOp::Shr:
                if (op.imm > 0 && op.imm < 32) a.shrImm(reg(op.dst), reg(op.a), uint8_t(op.imm));
                else if (op.imm == 0) { if (op.dst != op.a) a.movReg(reg(op.dst), reg(op.a)); }
                else a.shrImm(reg(op.dst), reg(op.a), 32);   // no encoding: the assembler refuses
                break;
            case IrOp::Sar:
                if (op.imm > 0 && op.imm < 32) a.sarImm(reg(op.dst), reg(op.a), uint8_t(op.imm));
                else if (op.imm == 0) { if (op.dst != op.a) a.movReg(reg(op.dst), reg(op.a)); }
                else a.sarImm(reg(op.dst), reg(op.a), 32);   // no encoding: the assembler refuses
                break;
            // A real move, not add-immediate-zero: Xtensa's addi.n reuses that slot for -1.
            case IrOp::Mov:    a.movReg(reg(op.dst), reg(op.a)); break;
            case IrOp::Label:
                if (op.imm >= 0 && op.imm < kIrLabels) a.bind(labelFor(op.imm));
                break;
            case IrOp::BranchGe:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchGeU(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::BranchGeS:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchGeS(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::BranchNe:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchNe(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::LoadCtrl: a.load8(reg(op.dst), host(kArg4), op.imm); break;   // dst = ctrls[imm]
            // The offset rides the instruction, since nothing computes a slot address at run time.
            case IrOp::LoadCtrl32: a.load32(reg(op.dst), host(kArg4), op.imm); break;
            case IrOp::StoreCtrl32: a.store32(host(kArg4), op.imm, reg(op.a)); break;
            // Clamped rather than skipped, since the depth counter shares this arena.
            case IrOp::LoadIdx:
            case IrOp::StoreIdx: {
                const uint8_t width = idxWidth(op.imm);
                const uint8_t count = idxCount(op.imm);
                const RegId idx = reg(op.a);
                // One branch, and the unsigned compare catches a negative index arriving huge.
                const LabelId inRange = a.newLabel();
                a.movImm(sCtr, count - 1);
                a.branchGeU(sCtr, idx, inRange);
                a.movReg(idx, sCtr);
                a.bind(inRange);
                // A shift, since ctrlWidth produces only 1 and 4, and this runs per array access.
                if (width == 4)      a.shlImm(idx, idx, 2);
                else if (width != 1) { a.movImm(sAddr, width); a.mulReg(idx, idx, sAddr); }
                a.addImm(idx, idx, idxBase(op.imm));    // ... plus the array's base
                // Two element widths, which is all ctrlWidth produces.
                if (op.op == IrOp::LoadIdx) {
                    if (width == 4) a.load32Idx(reg(op.dst), host(kArg4), idx);
                    else            a.load8Idx(reg(op.dst), host(kArg4), idx);
                } else {
                    if (width == 4) a.store32Idx(host(kArg4), idx, reg(op.b));
                    else            a.store8(host(kArg4), idx, reg(op.b));
                }
                break;
            }
            case IrOp::StoreCtrl: {
                // The constant goes into sCtr, not sAddr, which store8 clobbers on some backends.
                const RegId arena = host(kArg4);
                a.movImm(sCtr, op.imm);
                a.store8(arena, sCtr, reg(op.a));
                break;
            }
            // The allocator's two ops. `imm` is a slot INDEX; the assembler owns the frame layout.
            case IrOp::CallScript: {
                // A label, since the callee's address is unknown until every function is placed.
                if (op.imm < 0 || op.imm >= ir.fnCount) break;
                // The backend delivers the value into dst, since the return register is ABI.
                a.callLabel(fnLabel[op.imm], reg(op.dst), op.b != 0);
                break;
            }
            case IrOp::Ret:
                // The value first: the exit's teardown publishes the return register.
                if (op.imm) a.retValue(reg(op.a));
                // The always-taken `x >= x` idiom, on kArg0 because a bare return's `a` is unset.
                if (curFn >= 0) {
                    const RegId z = host(kArg0);
                    a.branchGeU(z, z, fnExit[curFn]);
                }
                break;
            case IrOp::Spill:  a.spillStore(reg(op.a), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Reload: a.spillLoad(reg(op.dst), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Call: {
                // Their address and count, so nothing is held in a register across the call.
                if (!op.callFn) { lowerRefusal() = LowerRefusal::NullCall; return 0; }
                const RegId argPtr = static_cast<RegId>(ir.vregsUsed);
                a.slotAddr(argPtr, static_cast<uint8_t>(op.imm));
                const RegId argN = static_cast<RegId>(ir.vregsUsed + 1);
                a.movImm(argN, static_cast<int32_t>(op.b));
                a.call(reg(op.dst), argPtr, argN,
                       host(kArg4), reinterpret_cast<const void*>(op.callFn));
                break;
            }
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        LabelId skip = a.newLabel();
                        // In scratch, not the index vreg, which a `for` step reads after the store.
                        a.branchGeU(reg(op.a), host(kArg1), skip);
                        a.mulReg(sAddr, reg(op.a), host(kArg2));        // addr = index * cpl
                        a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    // Element 0: the one slot I was given, which is not slot number zero.
                    case InlineOp::StoreFirst: {
                        LabelId skip = a.newLabel();
                        a.branchIfZero(host(kArg1), skip);
                        a.movImm(sAddr, 0);
                        a.store8(host(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        LabelId done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(host(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, host(kArg2));
                        a.store8(host(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);
                        a.branchNe(sCtr, host(kArg1), top);
                        a.bind(done);
                        break;
                    }
                }
                break;
            default: break;
        }
    }
    // The last function, or the single routine of a function-less program.
    if (ir.fnCount > 0) closeFn(static_cast<uint8_t>(ir.fnCount - 1));
    else a.epilogue();
    a.finalize();
    if (a.overflowed()) { lowerRefusal() = LowerRefusal::AsmOverflow; return 0; }
    if (a.size() > cap) { lowerRefusal() = LowerRefusal::OverCap; return 0; }
    return a.size();   // already in `out`: the assembler emitted there
}

/// @}

}  // namespace mm::moonlive
