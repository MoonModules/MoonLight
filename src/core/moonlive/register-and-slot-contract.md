# The MoonLive register and frame-slot contract

One machine, three roles. This governs the compiler and the four backends, not a binding: a scripted
layout, effect and modifier all compile through the same front end and the same lowering, so the
ownership rules below hold identically whichever role a script plays. ("Layout" in this file means
the memory layout of the register file and the frame — never the light-placement role.)

Who owns which register index, who owns which frame slot, and who is allowed to compute what from
`vregsUsed`. Written down because four places derive numbers from each other — core's compaction,
the spill pass, each backend's inline scratch, and each assembler's map — and changing where a value
lives perturbs all four, so a change made against intuition rather than against this table
silently miscompiles on one target while the others stay green.

## Registers: one ascending layout, three owners

A vreg is an index. Each backend maps it to a machine register through its own table (`kXtReg`,
`kRvReg`, `kArm64Reg`, `kThReg`). The index space is carved as follows, **in this order, no gaps**:

| range | owner | meaning |
|---|---|---|
| `0 .. kFirstTemp-1` | **core, fixed** | the host arguments: buf, nLights, cpl, t, ctrls. They arrive in registers at entry, except the fifth on Thumb-2, which the prologue loads from the caller's stack; every backend then indexes them directly. |
| `kFirstTemp .. vregsUsed-1` | **core (parser, then spill pass)** | the program's values. The parser hands them out; the spill pass renumbers them down to what fits. |
| `vregsUsed .. vregsUsed+scratch-1` | **the backend** | the inline ops' scratch, computed per lowerer from `vregsUsed`. |

**The invariant:** `vregsUsed + scratch <= kRegCount`, and the spill pass is what guarantees it by
rewriting the program until `vregsUsed` is small enough. `RegBudget.reserved` is how a backend tells
the pass how much scratch it will add on top.

**Consequences that have each been a bug:**

- A backend must not name a register outside its own map. `ar()`/`xr()`/`mr()` index the table with
  `vregsUsed + n`, so an off-by-one reads past the array and emits a register chosen by accident.
  All three are bounds-checked for that reason.
- A register that is scratch for the assembler (an address temp, a call staging register) must not
  also appear in the vreg map. `static_assert` in each backend enforces it.
- On Xtensa the map may only contain registers that survive `call8`. The windowed ABI rotates by
  eight, so a call clobbers `a8..a15` (ESP-IDF's `coreasm.h` states it). A vreg mapped there holds a
  script value that a host call destroys.

## Frame slots: two owners, growing towards each other

One frame, indexed `0 .. budget.slots-1`, addressed by `spillStore`/`spillLoad`.

| range | owner | meaning |
|---|---|---|
| `0 .. ir.localSlots-1` | **core (parser)** | script variables (a `for`'s counter and limit) and call-argument staging. Handed out by `slotHighWater`, peak recorded in `slotsUsed`, published as `ir.localSlots`. |
| `ir.localSlots .. slotsUsed-1` | **the spill pass** | values the register file could not hold. Numbered from `ir.localSlots` UPWARD, never from zero. |

**The invariant:** the spill pass starts numbering at `ir.localSlots` so the two ranges cannot
overlap, and refuses the compile when either exceeds `budget.slots` — a slot the backend cannot
address would encode a truncated offset and write over a live value.

**`ir.localSlots` is what the FRONT END claimed, not the frame size.** Setting it to the whole
addressable range hands the allocator a full frame and leaves it nowhere to spill; that refuses every
script that needs one spill.

## What `prologue(slots)` must reserve

`slotsUsed`: the total from both owners, which `spillToBudget` returns.
The backend turns a slot index into a frame offset its own way.
Xtensa counts 4-byte words from `kFrameBase`; RISC-V, arm64 and Thumb-2 build a frame in their prologue.
Slot indices are the shared currency; offsets are private.

## Where a proposed change has to be checked

Any change to where a value lives must be checked against **all five** of:

1. `Parser` — does it still hand out slots from zero upward, and is `slotsUsed` the peak?
2. `compileSource` — is `ir.localSlots` the parser's claim only?
3. `spillToBudget` — does its reservation arithmetic still match what actually occupies registers?
4. The compaction in `spillToBudget` — does the renumbering leave the ranges above intact?
5. Each of the three lowerers — is `scratch` still the number of extra registers it adds, and does
   every register it names come from the map?

Missing any one of these produces a program that compiles and then behaves wrongly, which is far
worse than one that is refused.
