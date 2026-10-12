#include "core/moonlive/MoonLiveSpill.h"
#include "core/moonlive/moonlive_emit.h"

/// Linear-scan register allocation spilling to the call frame, so a nested call pushes its own.

namespace mm::moonlive {

namespace {

// Held back so a spilled operand has somewhere to land; four, because StoreElem reads four.
constexpr uint8_t kMaxReloadTemps = 4;

// A few hundred bytes of stack at kMaxVRegs, small enough to stay a local on the shared 12 KB task.
/// One value's live range, in op indices.
struct Interval {
    uint16_t start = 0;         ///< the first op index mentioning the vreg
    uint16_t end = 0;           ///< the last one, after loop extension
    VReg     vreg = 0;          ///< the vreg this range describes
    bool     live = false;      ///< whether the vreg appears at all
    bool     spilled = false;   ///< whether it lost its register to the frame
    uint8_t  slot = 0;          ///< frame slot, when spilled
    VReg     assigned = 0;      ///< compacted register number, when not
};

// A loop is exactly a BranchNe whose label was bound earlier, so no CFG has to be built.
/// A loop, as the op index its back edge lands on and the back edge itself.
struct Loop { uint16_t header;   ///< where the back edge lands
              uint16_t back; };  ///< the back edge itself

// One function, so the interval builder and the rewriter cannot disagree about the fields.
/// Every vreg an op reads, written into `out`; answers how many.
uint8_t sourcesOf(const IrInst& in, VReg* out) {
    switch (in.op) {
        case IrOp::Const:                                   return 0;
        case IrOp::ConstPtr:                                return 0;   // an address, not a value
        case IrOp::Reload:                                  return 0;
        case IrOp::Label:                                   return 0;
        case IrOp::Mov:
        case IrOp::AddImm:
        case IrOp::Spill:      out[0] = in.a;               return 1;
        // A return reads its value only when `imm` says it carries one.
        case IrOp::Ret:        if (!in.imm) return 0;
                               out[0] = in.a;               return 1;
        case IrOp::LoadCtrl:   out[0] = kArg4;              return 1;   // reads the arena pointer
        // The value alone, since the rewriter writes sources back positionally.
        case IrOp::StoreCtrl:
        case IrOp::StoreCtrl32: out[0] = in.a; return 1;
        case IrOp::LoadCtrl32:  out[0] = kArg4;               return 1;   // reads the arena pointer
        // The index, and for a store the value; the arena pointer is positional here too.
        case IrOp::LoadIdx:     out[0] = in.a;               return 1;
        case IrOp::StoreIdx:    out[0] = in.a; out[1] = in.b; return 2;
        // A shift carries its amount in `imm`, so the vreg source is the value alone.
        case IrOp::Shl:
        case IrOp::Shr:
        case IrOp::Sar:        out[0] = in.a;               return 1;
        case IrOp::Mulhi:      out[0] = in.a; out[1] = in.b; return 2;
        case IrOp::Add:
        case IrOp::Mul:
        case IrOp::BranchGe:
        case IrOp::BranchGeS:
        case IrOp::BranchNe:   out[0] = in.a; out[1] = in.b; return 2;
        // No registers: arguments were staged into frame slots, so `b` is a literal count.
        case IrOp::Call:       return 0;
        // Nor a call to the script's own function, whose `imm` is a function number.
        case IrOp::CallScript: return 0;
        case IrOp::Inline:
            // Every field the host filled in; the fixed ABI vregs need no interval.
            out[0] = in.a; out[1] = in.b; out[2] = in.c; out[3] = in.d;
            return 4;
    }
    return 0;
}

// Branches, Label, Spill and the inline ops do not: their dst is a zero the front end never fills.
/// Whether an op defines its `dst`, which is what gives it an interval and remaps it.
bool writesDst(const IrInst& in) {
    switch (in.op) {
        case IrOp::Label: case IrOp::BranchGe: case IrOp::BranchGeS: case IrOp::BranchNe:
        // A member store writes memory, so its dst is not a definition.
        case IrOp::StoreCtrl:
        case IrOp::StoreCtrl32:
        // Only when the caller wanted the value, since a statement call's dst is 0.
        case IrOp::CallScript: return in.b != 0;
        case IrOp::Spill: case IrOp::Inline: return false;
        default: return true;
    }
}

}  // namespace

/// Fit a program to a register budget, spilling to the frame; false when it cannot.
bool spillToBudget(IrProgram& ir, const RegBudget& budget, uint8_t& slotsUsed) {
    // The front end owns the low slots, so a spill from zero would land on a loop counter.
    slotsUsed = ir.localSlots;
    if (!ir.ops) { spillDetail().guard = 1; return false; };

    spillDetail() = SpillDetail{};   // every field honest for whichever guard fires, including 1-3
    const uint8_t avail = budget.allocatable();
    // Checked before the early return, since the front end allots slots without knowing the target.
    if (ir.localSlots > kMaxLocals || ir.localSlots > budget.slots) { spillDetail().guard = 2; return false; };

    // Already fits, so leave it byte-identical: this is the path every shipped script takes.
    if (ir.vregsUsed <= avail) return true;
    if (ir.vregsUsed > kMaxVRegs) { spillDetail().guard = 3; return false; };

    // Temps for the widest op present, since reserving four left zero keepable on Xtensa.
    uint8_t reloadTemps = 0;
    for (uint16_t i = 0; i < ir.count; i++) {
        VReg s[4];
        const uint8_t n = sourcesOf(ir.ops[i], s);
        uint8_t distinct = 0;
        for (uint8_t a = 0; a < n; a++) {
            if (s[a] < kFirstTemp) continue;                 // a fixed ABI vreg is always a register
            bool seen = false;
            for (uint8_t b = 0; b < a; b++) if (s[b] == s[a]) { seen = true; break; }
            if (!seen) distinct++;
        }
        if (distinct > reloadTemps) reloadTemps = distinct;
    }
    if (reloadTemps > kMaxReloadTemps) reloadTemps = kMaxReloadTemps;
    // The ABI vregs are not subtracted, since parking them runs before any temp exists.
    { auto& d = spillDetail(); d.guard = 0; d.avail = avail; d.temps = reloadTemps; d.vregs = ir.vregsUsed; d.slots = ir.localSlots; }
    if (avail <= reloadTemps) { spillDetail().guard = 4; return false; };
    const uint8_t keepable = static_cast<uint8_t>(avail - reloadTemps);

    // --- 1. Find the loops, innermost first ----------------------------------------------------

    // Half of kIrLabels, since a loop takes two labels, its head and its exit.
    Loop loops[kIrLabels / 2];
    uint8_t loopCount = 0;
    {
        int16_t labelAt[kIrLabels];   // an op index, below kMaxIrOps
        for (auto& p : labelAt) p = -1;
        for (uint16_t i = 0; i < ir.count; i++)
            if (ir.ops[i].op == IrOp::Label && ir.ops[i].imm >= 0 && ir.ops[i].imm < kIrLabels)
                labelAt[ir.ops[i].imm] = static_cast<int16_t>(i);
        for (uint16_t i = 0; i < ir.count; i++) {
            const IrInst& in = ir.ops[i];
            if (in.op != IrOp::BranchNe) continue;
            if (in.imm < 0 || in.imm >= kIrLabels) { spillDetail().guard = 5; return false; };      // an unbindable label: refuse
            const int32_t tgt = labelAt[in.imm];
            if (tgt < 0 || static_cast<uint16_t>(tgt) > i) continue;  // a forward branch, not a loop
            if (loopCount >= kIrLabels / 2) { spillDetail().guard = 6; return false; };
            loops[loopCount++] = {static_cast<uint16_t>(tgt), i};
        }
        // Proper nesting is what makes innermost-first meaningful, so anything else is refused.
        for (uint8_t x = 0; x < loopCount; x++)
            for (uint8_t y = static_cast<uint8_t>(x + 1); y < loopCount; y++) {
                const bool disjoint = loops[x].back < loops[y].header || loops[y].back < loops[x].header;
                const bool xInY = loops[y].header <= loops[x].header && loops[x].back <= loops[y].back;
                const bool yInX = loops[x].header <= loops[y].header && loops[y].back <= loops[x].back;
                if (!disjoint && !xInY && !yInX) { spillDetail().guard = 7; return false; };
            }
    }

    // --- 2. Live intervals, then loop extension -------------------------------------------------
    Interval iv[kMaxVRegs];
    for (uint8_t v = 0; v < kMaxVRegs; v++) iv[v].vreg = v;

    auto mention = [&](VReg v, uint16_t at) {
        if (v >= kMaxVRegs) return;
        if (!iv[v].live) { iv[v].live = true; iv[v].start = at; iv[v].end = at; return; }
        if (at < iv[v].start) iv[v].start = at;
        if (at > iv[v].end)   iv[v].end = at;
    };
    for (uint16_t i = 0; i < ir.count; i++) {
        const IrInst& in = ir.ops[i];
        VReg src[4];
        const uint8_t n = sourcesOf(in, src);
        for (uint8_t s = 0; s < n; s++) mention(src[s], i);
        if (writesDst(in)) mention(in.dst, i);
    }

    // A range touching a loop lives to its end, since extension can only cost a needless spill.
    for (uint8_t pass = 0; pass < loopCount; pass++) {
        // pick the innermost unprocessed loop = the one containing no other unprocessed loop
        uint8_t pickIdx = 0xff;
        for (uint8_t x = 0; x < loopCount; x++) {
            if (loops[x].header == 0xffff) continue;                  // already processed
            bool containsAnother = false;
            for (uint8_t y = 0; y < loopCount; y++) {
                if (y == x || loops[y].header == 0xffff) continue;
                if (loops[x].header <= loops[y].header && loops[y].back <= loops[x].back)
                    containsAnother = true;
            }
            if (!containsAnother) { pickIdx = x; break; }
        }
        if (pickIdx == 0xff) break;
        const Loop lp = loops[pickIdx];
        loops[pickIdx].header = 0xffff;                               // mark processed
        for (uint8_t v = 0; v < kMaxVRegs; v++)
            if (iv[v].live && iv[v].start <= lp.back && iv[v].end >= lp.header && iv[v].end < lp.back)
                iv[v].end = lp.back;
    }

    // --- 3. Linear scan (Poletto & Sarkar) ------------------------------------------------------

    // Increasing interval start, and when the active set is full the furthest end spills.
    VReg order[kMaxVRegs];
    uint8_t nOrder = 0;
    for (uint8_t v = kFirstTemp; v < ir.vregsUsed; v++) if (iv[v].live) order[nOrder++] = v;
    for (uint8_t i = 1; i < nOrder; i++) {                            // insertion sort: nOrder <= 32
        const VReg k = order[i];
        uint8_t j = i;
        while (j > 0 && iv[order[j - 1]].start > iv[k].start) { order[j] = order[j - 1]; j--; }
        order[j] = k;
    }

    VReg active[kMaxVRegs];
    uint8_t nActive = 0;
    uint8_t nSpilled = ir.localSlots;
    for (uint8_t i = 0; i < nOrder; i++) {
        const VReg cur = order[i];
        // expire: everything whose interval ended before this one starts is free again
        uint8_t w = 0;
        for (uint8_t j = 0; j < nActive; j++)
            if (iv[active[j]].end >= iv[cur].start) active[w++] = active[j];
        nActive = w;

        if (nActive < keepable) {
            active[nActive++] = cur;
            // keep `active` sorted by end so the furthest is always the last element
            for (uint8_t j = nActive - 1; j > 0 && iv[active[j - 1]].end > iv[active[j]].end; j--) {
                const VReg t = active[j]; active[j] = active[j - 1]; active[j - 1] = t;
            }
            continue;
        }
        // keepable is at least one by the guard above, or the index below reads active[-1].
        if (nActive == 0) { spillDetail().guard = 8; return false; };
        const VReg furthest = active[nActive - 1];
        if (iv[furthest].end > iv[cur].end) {
            iv[furthest].spilled = true;
            iv[furthest].slot = nSpilled++;
            active[nActive - 1] = cur;
            for (uint8_t j = nActive - 1; j > 0 && iv[active[j - 1]].end > iv[active[j]].end; j--) {
                const VReg t = active[j]; active[j] = active[j - 1]; active[j - 1] = t;
            }
        } else {
            iv[cur].spilled = true;
            iv[cur].slot = nSpilled++;
        }
    }
    // kMaxLocals rather than budget.slots, since the slots above hold the parked host arguments.
    spillDetail().spilled = nSpilled;
    if (nSpilled > kMaxLocals || nSpilled > budget.slots) { spillDetail().guard = 9; return false; };

    // --- 4. Compact the survivors ---------------------------------------------------------------

    // Numbered from the bottom, since starting at kFirstTemp reserved registers already parked.
    VReg next = 0;
    for (uint8_t v = kFirstTemp; v < ir.vregsUsed; v++)
        if (iv[v].live && !iv[v].spilled) iv[v].assigned = next++;
    for (uint8_t v = 0; v < kFirstTemp; v++) { iv[v].assigned = v; iv[v].spilled = false; }
    const VReg firstTemp = next;                            // the reload temps start here
    const VReg newHighWater = static_cast<VReg>(firstTemp + reloadTemps);

    // --- 5. Rewrite -----------------------------------------------------------------------------

    // Nothing spilled, the common case: the compacted numbering is applied in place.
    if (nSpilled == ir.localSlots) {
        for (uint16_t i = 0; i < ir.count; i++) {
            IrInst& in = ir.ops[i];
            VReg src[4];
            const uint8_t n = sourcesOf(in, src);
            auto keep = [&](VReg v) -> VReg { return (v < kMaxVRegs && iv[v].live) ? iv[v].assigned : v; };
            if (n > 0) in.a = keep(src[0]);
            if (n > 1) in.b = keep(src[1]);
            if (n > 2) in.c = keep(src[2]);
            if (n > 3) in.d = keep(src[3]);
            if (writesDst(in) && in.dst < kMaxVRegs) in.dst = keep(in.dst);
        }
        ir.vregsUsed = newHighWater;
        slotsUsed = nSpilled;
        return true;
    }

    // Into a second program, since a right-sized array cannot take an inserted Reload or Spill.
    IrProgram out;
    // Counted in a dry pass, since a worst-case reserve asked 41 KB and failed on a fragmented heap.
    uint32_t want = 0;
    for (uint16_t i = 0; i < ir.count; i++) {
        const IrInst& in = ir.ops[i];
        VReg src[4];
        const uint8_t n = sourcesOf(in, src);
        for (uint8_t s = 0; s < n; s++) {
            const VReg v = src[s];
            if (v >= kMaxVRegs || !iv[v].spilled) continue;
            bool already = false;
            for (uint8_t q = 0; q < s; q++) if (src[q] == v) { already = true; break; }
            if (!already) want++;                                   // a Reload
        }
        want++;                                                     // the op itself
        if (writesDst(in) && in.dst < kMaxVRegs && iv[in.dst].spilled) want++;   // a Spill
    }
    if (want > kMaxIrOps) { spillDetail().guard = 10; return false; };
    if (!out.reserve(static_cast<uint16_t>(want))) { spillDetail().guard = 11; return false; };

    auto emit = [&](const IrInst& in) {
        // push() re-validates every vreg, so a bad register fails here, not in a backend.
        if (!out.push(in)) { spillDetail().guard = 12; return false; };
        return true;
    };

    // The boundaries shift with every insertion, and removing this remap boot-loops an S3.
    uint16_t newFnStart[kMaxIrEntries] = {};

    for (uint16_t i = 0; i < ir.count; i++) {
        // Where this function begins in the output, before anything is emitted for its first op.
        for (uint8_t f = 0; f < ir.fnCount; f++)
            if (ir.fnIrStart[f] == i) { newFnStart[f] = out.count; }

        IrInst in = ir.ops[i];
        VReg src[4];
        const uint8_t n = sourcesOf(in, src);

        // Each distinct spilled source into its own temp, since a repeat can reuse the same one.
        VReg tempOf[4] = {0, 0, 0, 0};
        uint8_t nTemp = 0;
        for (uint8_t s = 0; s < n; s++) {
            const VReg v = src[s];
            if (v >= kMaxVRegs || !iv[v].spilled) continue;
            bool already = false;
            for (uint8_t p = 0; p < s; p++) if (src[p] == v) { tempOf[s] = tempOf[p]; already = true; break; }
            if (already) continue;
            tempOf[s] = static_cast<VReg>(firstTemp + nTemp);
            nTemp++;
            IrInst rl{};
            rl.op = IrOp::Reload;
            rl.dst = tempOf[s];
            rl.imm = iv[v].slot;
            if (!emit(rl)) { spillDetail().guard = 13; return false; };
        }

        // A spilled operand now names its temp, a kept one its compacted number.
        auto mapped = [&](VReg v, uint8_t slotIdx) -> VReg {
            if (v >= kMaxVRegs) return v;
            return iv[v].spilled ? tempOf[slotIdx] : iv[v].assigned;
        };
        if (n > 0) in.a = mapped(src[0], 0);
        if (n > 1) in.b = mapped(src[1], 1);
        if (n > 2) in.c = mapped(src[2], 2);
        if (n > 3) in.d = mapped(src[3], 3);

        const bool dstSpilled = writesDst(in) && in.dst < kMaxVRegs && iv[in.dst].spilled;
        const uint8_t dstSlot = dstSpilled ? iv[in.dst].slot : 0;
        if (writesDst(in)) {
            // Computed into a reload temp and stored; reusing a source temp is ordinary aliasing.
            in.dst = dstSpilled ? firstTemp : (in.dst < kMaxVRegs ? iv[in.dst].assigned : in.dst);
        }
        if (!emit(in)) { spillDetail().guard = 14; return false; };
        if (dstSpilled) {
            IrInst sp{};
            sp.op = IrOp::Spill;
            sp.a = firstTemp;
            sp.imm = dstSlot;
            if (!emit(sp)) { spillDetail().guard = 15; return false; };
        }
    }

    out.vregsUsed = newHighWater;
    // The function table crosses the swap with its boundaries remapped.
    out.fnCount = ir.fnCount;
    for (uint8_t f = 0; f < ir.fnCount; f++) out.fnIrStart[f] = newFnStart[f];
    ir.swap(out);
    slotsUsed = nSpilled;
    return true;
}

}  // namespace mm::moonlive
