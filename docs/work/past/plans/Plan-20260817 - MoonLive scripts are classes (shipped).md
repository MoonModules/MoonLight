# Plan: MoonLive scripts are classes

Takes over from [Plan-20260813 — MoonLive on a stack machine](Plan-20260813%20-%20MoonLive%20on%20a%20stack%20machine%20%E2%80%94%20the%20frame%20is%20where%20values%20live%20(shipped).md),
whose step 7 (factor the three bindings onto one shared base) this replaces. That plan finished the
MACHINE: values live in frame slots, one lowering serves every backend, one system-variable
vocabulary serves every role. This plan changes what a script IS: how it is written (steps 1 to 5)
and what it can say (steps 6 to 10).

**MoonLive launches when all of it is there**, so the order below is the one that is best to BUILD
in, not the one that shows best soonest. The consequence worth stating out loud is that the most
visible work (`if`, reading a light back, particles, the editor) comes last, which is a deliberate
trade rather than an oversight.

## What is missing before it can launch

The engine is not the gap. Live-editing a script on a running device and watching 12,288 lights
change on the next tick, at native speed, is already true, and it is the thing nobody expects from a
microcontroller. The gap is what a script can EXPRESS: today that is smooth arithmetic over a grid,
which gives plasma, ripples and gradients and stops there. Steps 6 to 9 are that list.

Recursion is in scope and ships in step 1, but it is not on that list, because it is not what an
effect author is missing: they do not write recursive functions. It earns its place for a different
reason. It is what makes this a real language rather than a macro expander, which is a credibility
floor rather than a feature anyone points at, and the stack machine already bought it, so the cost
now is that a script function gets a real frame instead of being special-cased.

## Why the change

A MoonLive script today is a bag of declarations and statements, and a control is declared by a
COMMENT that changes behaviour:

```
uint8_t bpm = 30;   // @control 1..240
for (y = 0; y < height; y = y + 1) { ... }
```

That is not C, and it does not resemble the compiled module it stands in for. A compiled effect is a
class with `defineControls()` and `tick()`; a scripted one should read the same way, so that what a
contributor learns from one transfers to the other. The end state:

```
class PlasmaEffect {
  uint8_t bpm;
  uint8_t zoom;

  defineControls() {
    addUint8("bpm", 30, 1, 240);
    addUint8("zoom", 24, 1, 64);
  }

  tick() {
    for (y = 0; y < height; y = y + 1) { ... }
  }
}
```

Step 7 of the previous plan tried to reach the same goal from the other end, by factoring the three
BINDINGS onto a shared base. It was superseded for two reasons, and both are what this plan is built
on. The code objected: the three bindings derive from three sibling bases under `MoonModule`, so a
shared base needs virtual inheritance and changes the layout of every module in the system to serve
three of them. And the direction objected: once a script defines NAMED ENTRY POINTS, the three
bindings stop being three kinds and become one kind with different entry points present, which is a
dispatch question rather than an inheritance one. The structure falls out at the end (step 5) instead
of being designed up front.

## The enclosing class declaration

`class PlasmaEffect { ... }` is the only top-level form. It makes the class semantics VISIBLE rather
than implied: without it a script merely behaves like a class and a reader has to be told, while with
it `defineControls` and `tick` stop looking like magic top-level names and read as what they are,
members the host calls. It also gives the engine a NAME that is not the filename, for the UI, the
status line and error messages, which matters the first time somebody renames a file.

Two constraints on it:

- **The filename loads it; the class name identifies it.** The `script` control still holds the file
  name, because that is what the engine reads from the filesystem, and the class name is what the
  status line and compile errors report. This is how a C translation unit works: `plasma.c` is what
  you compile, and the diagnostics name the function inside it. Both can be renamed independently
  without breaking the other.
- **The name is just a name.** Role does NOT come from the suffix. Inferring "this is an effect" from
  `...Effect` is the kind of magic that surprises people the first time a rename changes behaviour;
  the role comes from which entry points the class defines, which is step 5's dispatch model, and it
  is also what lets one class define both `tick` and `modifyLogical`.
- **The declaration is MANDATORY.** One top-level form, not two. Optional was considered, on the
  argument that `onered.mlv` is a single `setRGB` and a class around it is ceremony, and rejected:
  it would keep a bare statement list in the grammar forever, which is a second parse path, a second
  set of rules to document, and a second thing to test, permanently, so that a handful of two-line
  scripts can stay two lines. That is more code to support less clarity, and the whole point of this
  plan is that a reader can tell what a script is by looking at it. Nothing is released yet, so the
  only cost is rewriting the shipped scripts once, which is our own work.

## Decisions taken

**The standard: a script IS a class.** Not "like" one loosely: it has members (script-level
variables) and functions, some of which the host calls from outside at known times (`tick`,
`defineControls`, `placeLights`). Syntax may be simplified, and semantics may be simplified where
that buys something, but where a reader has an expectation from any class-based language, the
behaviour meets it. Most of what follows is settled by asking "what would a class do".

**Where script-level state lives.** The moment a script has more than
one function, a variable shared between them cannot live in a frame slot, because the frame belongs
to one call and dies with it. That is this plan's own premise ("the frame is where values live")
meeting the one case it does not cover. The same mechanism is what a variable persisting ACROSS
`tick()` calls needs, which is the stateful-effect family (fire, trails, decay) the language cannot
express at all today, so it is worth solving once rather than twice.

The storage already exists: the CONTROL ARENA outlives every call, keeps a stable address across a
recompile, and is reachable from emitted code through `kArg4`. A script-level variable is close to
"a control the UI does not show", which is a good sign about the shape. What is missing is the rules,
and each is a real decision rather than a detail:

- **Scope: SETTLED by the class model.** A variable declared outside any function is a member:
  visible in every function, one per script instance.
- **Initialisation: SETTLED by the class model.** A member is initialised once when the object is
  constructed, which here is compile time, seeded exactly as a declared control already is. Persisting
  across `tick()` calls follows from that rather than needing a `setup()` entry point to explain it.
- **Width and TYPE: the open one, and the real work.** Arena slots are BYTES today, which is why a
  coordinate clamps at 255 and a shift modifier cannot walk a light off a large grid. A member must be
  able to be a scalar, a STRUCT (`Coord3D`), an ARRAY, or an array of structs. Not on day one, but the
  storage has to be designed for it, which makes this a typed addressable region rather than a wider
  row of bytes. Everything else here is downstream of this decision.

  **Correction, found when step 3 reached it.** This plan later says the storage decision is settled
  and that an element count is already on the member record. Neither is true, and building against
  the claim would have started in the wrong place:

  - `DeclaredControl` has no count and no width. `CtrlType` has exactly one value, `Uint8`.
  - The arena is `kArenaBytes` = 17 with a hardcoded three-way split, and a control's **offset IS
    its declaration index**. An array or a 16-bit member breaks that identity, and the identity is
    load-bearing outside the compiler: the bindings cache arena slot POINTERS, `addUint8` passes an
    offset as a byte, and persistence is keyed on it.
  - `LoadCtrl`/`StoreCtrl` lower to `load8`/`store8` on all three backends, so a wider member is a
    new op pair per backend, not a wider record.

  So the decision above is a DIRECTION, not a design. Steps 8 and 9 carry the design, and they are
  done together because a byte arena rebuilt for arrays would be rebuilt again for 16-bit values.
- **Cost.** Every access becomes an arena load rather than a frame slot read. Cheap at today's script
  sizes, worth measuring before it is the default for every variable.

The type question is settled before step 2 writes any of it, because `defineControls()` setting a
value that `tick()` reads is exactly this case, and finding the rules wrong late means rebuilding
whatever was laid on top of them.

**Argument passing: by value for scalars, by reference for aggregates.** The ABI already does both
with one mechanism, so neither is a special case: arguments are staged in consecutive frame slots and
the callee receives a POINTER to that block, so a value argument is "copy the value into the slot"
and a reference argument is "put the address in the slot".

Passing everything by reference was considered and rejected. It is not a simplification of the class
model but a departure from it: a script could then not have an ordinary scalar parameter, since
assigning to it would write through to the caller's variable, surprising in precisely the place this
design promises no surprises. It also collides with the host built-ins, which are compiled C
functions taking values (`sin`, `beat`, `scale`) and are not ours to change. The value/reference split
is what a contributor already predicts, and an explicit `&` can be added later without redesign.

**One exec block, an offset per entry point.** A script compiles to a single allocation holding
every function it defines, and the engine records each named entry's offset within it; the binding
gets a function pointer to that offset. This is what a symbol table is, and what every compiler and
JIT does: one code section, a name-to-address map over it.

The alternatives lose on specifics rather than on taste. A block per entry makes every
script-to-script call cross-allocation, so an ordinary relative call becomes an absolute address
fixed up at load, and it multiplies `allocExec` calls, which on ESP32 is scarce fragmenting IRAM. A
selector argument on one entry point puts a branch on every call, including `tick()` at 60 fps
forever, paying at run time for something known at compile time. With one block, a call between
script functions is just a call, and step 5's dispatch question ("which entry points does this
script define") is answered by which names are in the table.

The offset recorded must be the address in the FINAL PLACED block, not in the staging buffer:
`writeExec` copies to a different address, and the single-entry path already accounts for this, so
this is the same rule applied per name.

**Functions are REAL CALLS, and recursion works.** Not inlining, and not a later nicety. The
predecessor plan's own table lists recursion as something the stack machine BUYS ("a fixed slot file
cannot hold two activations" becomes "each activation gets its own frame"), which is one of the
reasons the rework happened at all. Inlining would satisfy `tick()` and nothing else: a recursive
function cannot be inlined, so choosing it would quietly drop the payoff. What real calls require:

- **A frame per activation, at run time.** Today the emitted routine has ONE frame from one
  `entry`/prologue, sized at compile time. A script function needs its own, so the prologue and the
  frame-slot addressing become per function rather than per program.
- **On Xtensa, a nested `call8`.** Every activation therefore owes the 32-byte window-save reserve
  the frame contract demands, and the structural checker has to see a script function's frame the
  same way it sees the entry routine's. This is the one place where the ISA makes recursion cost
  more than bookkeeping, and it is exactly the defect class that cost three days, so the checker
  extension belongs to this step rather than to a follow-up.
- **A depth bound with a clean diagnostic.** An ESP32 render task has a fixed stack, so unbounded
  recursion is a reset, which the robustness rule forbids. A general compile-time depth limit is not
  possible, so this is a runtime guard that degrades visibly.

**Inlining is NOT part of this.** It was proposed twice while writing this plan, first to keep the
hot path flat and then to get both answers at once, and it does not survive its own cost/benefit:

- **What it saves is not the cost.** Inlining removes one call and one frame setup per call site, on
  the order of a microsecond. MoonLive's time goes elsewhere: `ripples.mlv` ticks at 1695 us, nearly
  all of it in ~15 HOST calls per cell into libm. Script-to-script calls are not the bottleneck and
  are not on a path to becoming one.
- **What it costs is a pass.** A call graph, cycle detection over it, a size heuristic, and the
  substitution itself: rewriting a callee's IR into the caller with members remapped, labels
  renamed against collision, and arguments bound to caller expressions. Hundreds of lines in the
  compile path, on every device, in a compiler where only one backend executes in tests.
- **Its failure mode is worse than its win.** Getting the analysis wrong inlines a mutually
  recursive pair forever ("does it call itself" does not catch `a` calls `b` calls `a`), which hangs
  the compiler on a device rather than reporting an error.

So: real calls, always. It is the simplest thing that fully works, and it is what delivers recursion.
Inlining stays available as a pure optimisation if a measurement ever shows script-to-script calls
mattering, and it can be added then without changing any semantics, which is exactly why it does not
need to be decided now.

Note this is orthogonal to argument passing. Value-for-scalars and reference-for-aggregates holds
whether or not a call is inlined: references are about how a callee REACHES its caller's data, while
recursion is about each activation owning its OWN locals. A recursive function still passes scalars
by value, and still needs a frame per activation; using references to avoid frames would make every
activation share one set of locals and corrupt itself.

## Sequence

Steps 1 to 5 are the SHAPE: how a script is written. Steps 6 to 10 are the VOCABULARY: what it can
say. The shape comes first so that every feature in the back half lands on finished ground rather
than being retrofitted into a language still moving underneath it.

1. ✅ **The `class` declaration and script functions, together.** Done: the class form ships, every
   script and test uses it, and a script now calls its own functions and itself. `crosshair.mlv` is
   the shipped example, verified on all four boards.

   Originally: They are one change: making the
   declaration mandatory means there is no bare-statement-list form left, so the grammar's new top
   level is a class body, and a class body holds functions. `tick()` is the first named entry point
   (an effect is the simplest case), and the shipped scripts convert in the same commit, because
   there is nothing to fall back on. First rather than last: with one top-level form, everything
   after it is written inside a class, and converting `moonlive/` twice would be the alternative.
   Calls are real from the start, per the section above: a script calling its own function, and then
   calling it recursively, is the acceptance test.

   **Recursion is not a feature beside local calls; it IS local calls.** A recursive call is a local
   call whose target happens to be the running function, and the machine cannot tell the difference:
   it allocates a frame, jumps, returns. The proviso is that every value lives in the callee's own
   frame rather than a fixed location, which is exactly what the stack machine bought (the
   predecessor plan's table: "a fixed slot file cannot hold two activations" becomes "each activation
   gets its own frame"). So recursion is a TEST CASE for local calls, not separate work.

   Two things are not free, and both are robustness rather than mechanism. A runaway recursion costs
   176 bytes of stack per activation on Xtensa (48 for the host-call area + 84 for 21 slots + 32 for
   the window reserve + alignment) against a 12 KB main-task stack, so it resets the device at
   roughly 64 deep: that needs a counter, and 32 is a generous limit at 46% of the budget. And on
   Xtensa each call8 rotates the register window, so past ~8 nested frames the hardware spills to the
   stack; that is correct and automatic, and the 32-byte reserve already accounts for where the
   spills land, so it costs memory traffic rather than correctness.

   **What local calls took, as built.** All four pieces landed:

   - **A script-call IR op.** `IrOp::CallScript`, carrying the callee's FUNCTION NUMBER. It first
     carried the callee's IR index, which the spill pass invalidates: every index past its first
     inserted Reload shifts, so the call named a position that no longer started a function. A
     function number survives any rewrite of the ops.
   - **A relative call in each assembler.** `callLabel(Label)` on all three, reusing the branch
     fixup machinery with a discriminator (`FixKind::Call` on Xtensa, `Jal` on RISC-V, kind 2 on
     arm64), because a call's displacement is encoded differently from a branch's.
   - **Function-entry alignment, which was not foreseen.** Xtensa requires a 4-byte-aligned `entry`
    : the toolchain rejects anything else outright ("unaligned entry instruction") and CALLn
     encodes its target in 4-byte units, so an unaligned callee is not expressible. Instructions are
     2 or 3 bytes, so a function following another lands anywhere. `alignForEntry()` pads before
     every prologue, which is what `.align 4` does in hand-written assembly. hpwit's `new-parser`
     hits the same wall and leaves it unhandled, so this is the missing piece rather than a
     workaround.
   - **The depth guard**, in the CALLEE's prologue rather than at each call site: one copy per
     function instead of one per call, emitted only when `hasScriptCall()`, so every shipped script
     carries none of it. A refusing callee returns, so there is no branch-around at the call site
     and no counter to restore across a call; the decrement lives in the one epilogue both paths
     take. Measured: 9 instructions ≈ 25 bytes per function, one arena byte, and ~5-10% on a script
     that calls (215-233µs vs 204µs for `crosshair.mlv` on the classic).

     The counter is a byte in the CONTROL ARENA (`kDepthSlot`, above the system variables), not a
     C++ member: recursion happens entirely inside the emitted block with no C++ frame between
     activations, and every function already holds the arena pointer, so this costs one byte and no
     new argument. The host zeroes it before each run rather than trusting the block to unwind: a
     script that hit the limit would otherwise leak a level and shrink every later frame's budget.
     A stack-limit check (comparing `sp` against a bound) is the more canonical form and is cheaper
     still, but needs a per-platform stack-bound source; worth revisiting if the guard ever shows up
     in a profile.

   - **A bigger label and fixup table.** `kMaxLabels`/`kMaxFixups` were 16/32, sized when a script
     was one routine, and each backend held its own private copy of both. A class allocates a label
     per function on top of its loop and store labels, so `crosshair.mlv` exhausted the table and
     failed with the generic "too large". Now `kAsmLabels`/`kAsmFixups` (48/96) in core, so the
     three backends cannot drift into disagreeing about which scripts compile.

     **Cost: 640 bytes of STACK, and no flash.** Both tables are members of the assembler, which is
     a local in `lowerWith`, which runs on the render task. Measured on the classic ESP32 image,
     that frame went 480 -> 1120 bytes (4 per label, 8 per fixup), making it the largest on the
     compile chain: 144 + 288 + 576 + 1120 = 2128 nested, 17% of the 12 KB main task. Flash is
     unchanged, since these are stack arrays. Pinned by `the <ISA> assembler stays small enough to
     build on a render task`, a tripwire in entry counts rather than host bytes (the host's 64-bit
     size_t makes its Fixup 16 bytes against the device's 8, so sizeof here overstates the device).
     If a script ever needs more, the tables move to the heap beside the code buffer: which was
     moved off the stack for exactly this reason: rather than the constants going up again.

   **Three traps, all found on hardware and none visible to the host suite:**

   - `IrProgram::swap()` did not swap the function table, so the spill pass's remapped boundaries
     were discarded and the lowering opened a frame two ops early, mid-statement.
   - A local call passed NO arguments. Each function's prologue parks buf/nLights/cpl/t/ctrls out of
     the argument registers into its own frame, so a bare call left the callee parking garbage and
     its first control read faulted at `EXCVADDR 0x9`. On Xtensa the arguments go in a10..a14,
     because `call8` rotates the window by 8.
   - The depth guard must be emitted AFTER the host arguments are parked. Both it and the epilogue
     address the arena through the parked frame copy, and a refusing activation jumps straight to
     the epilogue: a guard placed first makes the refusal read a slot nothing wrote (SIGBUS).

   The host backend cannot pin the argument-passing contract: its R0..R4 map onto the ABI argument
   registers and `bl` leaves them alone, so removing the fix fails no test there while crashing an
   S3. The boards are the only check for that class.

2. ✅ **`defineControls()`, replacing the `// @control` comment.** Done: a control is declared by
   calling `addUint8("bpm", bpm, 1, 240)` inside a `defineControls()` the script defines, the same
   call a compiled module makes. The `ControlAnno` token and its capture path are gone, all 16
   shipped scripts and the four docs moved with it, and both boards run the new form.

   **It is ORDINARY CODE, which took more than the syntax swap this step first looked like.**
   `defineControls` is a function the binding CALLS after a successful compile, the way the
   Scheduler calls a compiled module's; `addUint8` is a builtin in the same table as `setRGB`,
   reaching the engine through a sink as `addLight` does. A compile-time reading of the arguments
   was built first and rejected: it would have made `addUint8` the one call in the language whose
   arguments must be literals, which is a special case wearing a disguise. `addUint8("speed",
   speed, base, base * 4 + 5)` works, and a test pins it.

   That required three things the step did not anticipate:

   - **`IrOp::ConstPtr` and `movPtr` on all three backends.** A label is a pointer and `IrInst::imm`
     is `int32_t`, so an address cannot ride an immediate. Each backend already materializes one for
     a host call's target (arm64 movz + 3x movk, RISC-V lui + addi, Xtensa a byte at a time), so
     this generalizes a proven sequence rather than adding a mechanism.
   - **An engine-owned string pool.** `Control::name` is a HELD pointer the UI dereferences on every
     `/api/state`, and the source buffer is freed when the compile returns, so a literal is interned
     into memory that outlives both. In the engine rather than the exec block: that block is IRAM on
     a device, which takes 32-bit stores only.
   - **`byRef` and `byStr` on the builtin descriptor.** Which argument is a member (passed as its
     arena offset, so the script reads as the reference a compiled module passes) and which must be
     a quoted name. Stated per builtin rather than special-cased by name in the parser. `byStr`
     came from a test: `addUint8(s, s, 0, 9)` compiled, reading the member's VALUE as the label and
     handing the host a pointer built from a colour byte.

   **SWAPPED with typed members, which this plan originally put first.** The stated reason for the
   old order was that "the control it declares is a member", so members had to exist to declare one
   against. Building it showed the dependency runs the other way. Every class-scope declaration is a
   member; whether the UI shows one is a separate question `defineControls()` answers. While
   `@control` is still the marker, the member rule has to be written in terms of a comment that this
   step deletes, so members built first would be built against a discriminator with no future, and
   step 3 would spend its budget unpicking that rather than on itself. Starting a member's WIP
   against the annotation is what surfaced this: the declaration rule kept wanting to ask a question
   the next step abolishes.

3. ✅ **Typed script-level members**, per *Where script-level state lives* above. Both halves are
   now done: the assignment statement below, and the types in steps 8 and 9.

   **Half of this arrived with step 2**, because a control turned out to BE a member the UI shows.
   A variable declared in the class body already lives in the arena, is visible in every function,
   is seeded once from its initializer and survives every call, and a member no `addUint8` names is
   already private state. What is missing is that a script cannot WRITE one, which is what makes it
   state rather than a constant.

   So what remains is:

   - ✅ **An assignment statement.** Done. `x = expr;` was reachable only inside a `for` header, so a
     member could be declared and read and never written. `IrOp::StoreCtrl` and its lowering were
     recovered from step 2's first attempt; the grammar is new, one token of lookahead in
     `parseStatement` separating `name =` from `name(`. A member and a script-local may be assigned
     to; a SYSTEM VARIABLE may not, because the engine rewrites it before every call and the store
     would silently vanish.

     A CONTROL is deliberately NOT refused, which is a correction to what this plan said. Whether a
     member becomes a control is decided at RUN time, by `defineControls()` calling `addUint8` on
     it, so the parser cannot know: the direct consequence of step 2 making a control an ordinary
     call. Writing one is also legitimate (an effect that ramps its own speed and lets the slider
     re-take it), and the outcome is visible rather than silent.
   - ⬜ **Types wider than a byte, and aggregates.** Scalars work; a `Coord3D` or an array does not.
     This step is now steps 8 and 9, because the storage is NOT settled: see the correction below.

   This is what makes a stateful effect (fire, trails, decay) expressible at all, and it is the one
   step where a hot-path regression is plausible, so `collect_kpi.py` runs against it.

3b. ✅ **A frame per FUNCTION, not per program.** Done: each function emits its own prologue and
   epilogue, the host arguments are parked per function (they were spilling into a frame that did
   not exist yet, which was half the segfault), and the structural checker re-reads the frame at
   every prologue rather than judging the block by its first. Verified by control: shrinking the
   Xtensa reserve to 16 makes the checker fire, restoring it passes.

   Originally: The lowering emits one prologue before the first
   op; each function needs its own, with the epilogue to match, so that its recorded offset is an
   address a caller can actually jump to. Three parts, and the second is the one this project has
   already paid for once:

   - **Prologue and epilogue per function**, sized from that function's own slots rather than the
     program's total, which is also what makes each activation independent.
   - **On Xtensa, every activation owes the 32-byte window-save reserve.** A script function calling
     a built-in is a nested `call8`, so the frame contract applies to it exactly as to the entry
     routine, and the structural checker has to see a script function's frame the same way it sees
     the entry routine's. Extending the checker belongs to this step: it is the defect class that
     cost three days, and it is silent when wrong.
   - **The block start stops being the program.** With several functions in one block, falling off
     the end of one into the next is a real hazard, so each function returns rather than running on.

4. ✅ **The remaining entry points per role.** Done, and simplified by the moment model above:
   layouts declare `placeLights`, modifiers `modifyLogical`, effects `tick`, and each binding runs
   its moment IF the script defined it. `modifyLogicalTick` is not built; it is a new moment the
   Layer would have to own, so it belongs with whatever needs it.

   Originally: once the mechanism holds: `forEachCoord`/`lightCount`
   for a layout, `modifyLogical`/`modifyLogicalSize` for a modifier, plus `modifyLogicalTick` (a
   per-drawn-light hook we never implemented; MoonLight has it, and it is what a dynamic rotation
   modifier needs).

   **Every shipped script uses `tick()` until this step**, including the layouts and modifiers, which
   is a way-station rather than the shape: a layout does not tick, it is ASKED how many lights it has
   and where they are, and a modifier is asked to fold one coordinate. Naming both `tick` hides what
   the host actually does with them. It is what step 1 could deliver while `tick` was the only entry
   point in existence.

   **The byte-offset map is DONE** (landed with step 1). The parser records the IR index each
   function starts at, the shared lowering converts it to a byte as it emits, and `CompileResult`
   reports name plus offset: a symbol table over one code section. Pinned by a two-function test
   whose second entry must start after the first, verified to FAIL when the map is stubbed back to
   zero. `MoonLive::entry(name)` turns a name into a callable address.

   **But the map is necessary and not sufficient, which the code taught us by segfaulting.** Wiring
   a binding to CALL its entry point crashes, because the lowering emits ONE prologue for the whole
   program, before any function. An entry's recorded offset therefore points PAST the frame setup,
   and calling it directly runs a routine whose frame was never established; the first frame access
   faults. Per-function frames were sequenced after this step and belong before it: a named entry
   point is not callable until each function owns its frame. That is now step 3b, and this step is
   the wiring that follows it.

**A NAME IS A MOMENT, NOT A ROLE** (PO, during step 4). The binding does not pick which entry point
belongs to its kind. The HOST owns moments and calls whatever the script defined for each: `tick`
when a frame renders, `placeLights` when lights are placed, `modifyLogical` when one coordinate is
folded. An entry a class did not define is simply not called.

This is simpler than a per-role name in every direction. There is no selection, no fallback and no
"which name is mine" question; a binding checks whether the moment it owns is defined and runs it.
Nothing validates which names a class may use, which is what leaves the author in control and
responsible: a script that defines a name no moment calls has a function that does not run, and that
is visible immediately rather than silent. It is also what makes the stretch goal free rather than a
feature: an effect that also defines `modifyLogical` gets both, because it defined both.

It settles step 5 before step 5 starts. The three bindings already differ only by which moments they
own, so there is no inheritance question left to answer, and `tick` stays available to mean something
in a layout or a modifier later without a grammar change.

5. ✅ **Consolidate the three bindings onto a HELD HELPER.** Done, in Plan-20260818, because the
   editing loop needed it: saving a script only recompiles if the bindings agree on what "changed"
   means, and they did not. `MoonLiveScript` is the held member this step describes, and it removed
   116 lines. It also settled the recompile rule the three had drifted on: an effect had NO content
   hash (so editing its text did nothing until the file was renamed), a layout cleared its hash only
   on a name change, and only a modifier re-read the file. One rule now: if the file changed,
   recompile.

   The design question this step existed
   to answer is settled: the moment model above means the bindings no longer differ in behaviour,
   only in which base they extend and which moment they own. What is left is measurable duplication,
   and the shape it should take is now concrete rather than anticipated.

   **A `MoonLiveScript` MEMBER, not a shared base.** It owns `engine_`, `script_`, the compile-and-
   report path and `defineControls`, and each binding holds one and forwards. A base class was
   re-checked against the code and is still wrong for the same structural reason: the three derive
   from three SIBLING bases under `MoonModule`, so a shared base needs virtual inheritance and would
   change the object layout of every module in the system to serve three of them. A held member
   needs no inheritance change at all.

   **What it removes, measured:** `defineControls` is already byte-identical in the layout and the
   modifier, and the compile trunk is the same in all three; roughly 75 lines of ~537. What stays
   per binding is ~20 lines of genuinely its own: the role virtuals (`placeLights`/`lightCount`,
   `modifyLogical`/`modifyLogicalSize`, `tick`/`dimensions`) and the base-class call in `release`.

   **Its own change, with its own bench pass.** Not folded into the language work: these three files
   currently work and are verified on hardware, and mixing a restructure into a grammar change means
   a reviewer cannot tell which broke what. The test of whether it worked is the scripted DRIVER as
   a fourth binding: if it needs more than the member plus its own moment, the factoring did not go
   far enough.

The shape is finished at that point. What follows decides whether MoonLive is an impressive
mechanism or a language people build with.

6. ✅ **`if` / `else`.** Done. The single largest gap between what MoonLive can express and what an
   effect IS: the language did smooth arithmetic over a grid and nothing that branches.

   Cheaper than expected, and the reason is worth recording. The six comparisons lower onto the TWO
   branch ops the loops already use, `BranchGe` (unsigned `>=`) and `BranchNe`, by negating the test
   and swapping the operands: the emitted branch skips the then-block, so `a < b` emits
   `BranchGe a, b`. Only `>=` and `<=` need a second branch, because neither is a single unsigned
   `>=`. **No new IR op, no backend change, no allocator change.**

   The last of those is the one that could have gone wrong. The spill pass identifies a loop as a
   `BranchNe` whose label was bound EARLIER, and an `if` always jumps FORWARD around its block, so a
   conditional cannot be mistaken for a loop. That property came from the emit shape rather than
   from a guard added for it.

   The lexer gained `<=`, `>=`, `==`, `!=` and `>`, matched before the one-character operators they
   contain (maximal munch): testing `=` first would have lexed `a == b` as two assignments.

   Pinned by a boundary table across all six operators at, above and below the compared value, which
   is the only place an off-by-one in the negation mapping is visible; plus an `if` inside a `for`,
   a condition that is an expression on both sides, and a member steering which branch a tick takes.
   Encodings confirmed on all three ISAs with `disasm.py`.

7. ⬜ **Reading a light back: `get(x, y)`.** One builtin, and an entire family of effects becomes
   expressible: fire, decay, trails, blur feedback all work by reading what was drawn and modifying
   it. The buffer already persists between frames, which is why every script begins with `fill` to
   clear it, so the data is there and only the read is missing. Needs a decision on how a colour
   comes back: three builtins (`red`/`green`/`blue`) or bit operators, which is the same question
   the seven-argument `line()` answered for arguments and would answer once for both.

8. ✅ **Arrays** (arrays of structs not yet) and **9. ✅ Wider values than a byte.** Both built and
   on hardware; the ceiling has its number. Arrays OF STRUCTS remain unbuilt, and are their own step
   whenever a script wants one.

   What shipped, in the order it had to be built:

   - **A member's offset is a BYTE CURSOR**, not its declaration index. The two were the same number
     while every member was a byte, which is why nothing downstream had to change: the bindings'
     cached slot pointers, persistence and `addUint8` all keyed on the offset already.
   - **The arena's byte budget split from the record count** (`kCtrlBytes` and `kMaxCtrls`). They
     answered one question while a member was a byte and two questions afterwards.
   - **`uint16_t` members**, with `LoadCtrl16`/`StoreCtrl16` and `load16`/`store16` on all three
     backends. Separate ops rather than a width field, because every backend switch is exhaustive
     over `IrOp`: a backend that forgot the width fails to COMPILE, where an ignored field would
     have emitted a byte access against a two-byte member and lost the high half at run time.
   - **Even alignment for a wide member**, because arm64 `ldrh` and Xtensa `l16ui` SCALE the
     immediate by the access size and cannot encode an odd offset at all. The ISA's rule, honored
     once in the cursor rather than worked around in two assemblers.
   - **Arrays**, with `LoadIdx`/`StoreIdx` and a `load8Idx`/`load16Idx` pair (the stores already
     took a register offset; the loads did not). An index is an arbitrary expression.
   - **An out-of-range index is CLAMPED to the last element**, one compare and one branch. Not
     refused and not allowed through: a script computes indices from live control values, so out of
     range is an ordinary run-time state, and the arena holds the system variables and the depth
     counter, so a stray write would corrupt the ENGINE rather than the picture.

   A SERPENTINE layout now compiles, which the docs had listed as the standing example of what the
   language could not express.

   **Verification, and what it missed.** 1313 unit tests, the clamp proven in both directions
   (including that a system variable survives an out-of-range access), and the emitted instruction
   sequence read on all three ISAs with `disasm.py`. All of that passed while THREE codegen defects
   were live. The bench found them:

   - `width`/`count` were parked in `IrInst::c`/`d`, which are VREG fields the spill pass renumbers.
   - `sourcesOf` reported `kArg4` as the first source, and sources are written back POSITIONALLY, so
     the index landed in the value's field and the stored value was dropped.
   - Xtensa `addi.n` encodes immediate 0 as MINUS ONE (its narrow field covers 1..15), so an array
     based at arena offset 0 shifted every element access down a byte.

   arm64's register map absorbed the first two and the third is Xtensa-only, so the host suite could
   not see any of them: reintroducing either of the first two leaves all 1313 tests passing, which
   was control-checked rather than assumed. `disasm.py` showed the wrong code without the wrongness
   being apparent, since a plausible-looking instruction sequence is what all three produced.

   Only flashing an S3 and looking at the fixture exposed them. This is the plan's own verification
   item 9 doing the job it exists for, and the strongest evidence yet that the device backends are
   verified by hardware, not by inspection. `addImm` is now pinned by a test asserting the ENCODER
   (control-checked to fail on the bug); the other two are backlogged by name, because three
   attempts at a host test each passed with the defect reintroduced.

   **The ceiling is 64 bytes, sized against a real script rather than guessed.** It was a
   placeholder 16 until the first realistic effect written against it (`ember.mle`: two byte
   controls, a `uint16_t` counter and a 16-element heat buffer) was refused at 20 bytes. 64 holds a
   `uint8_t[64]` or a `uint16_t[32]` alongside scalars. Raise it against a script that needs more,
   not on speculation: the failure is a compile error naming the arena, so hitting it is visible.

   Widening it also exposed a defect worth recording, found by the pre-merge Reviewer: the seeding
   mask was a `uint32_t` written when the budget was 16, so a member at offset 32 or beyond shifted
   past the mask's width. Undefined behaviour that in practice aliased mod 32, silently losing a
   member's live value on every recompile. A `static_assert` now ties the mask width to the budget.

   **8 and 9 were ONE step, done together.** Step 2 was expected to have designed this storage, and
   it did not (see the correction under *Where script-level state lives*): the arena is a fixed row
   of 17 bytes whose offset IS a declaration index. Both steps break that identity in the same
   place, and a byte arena rebuilt for arrays would be rebuilt again for 16-bit values, so doing
   them apart means paying for the migration twice and leaving the intermediate state on a device.

   What the two share, and therefore what the step actually decides:

   - **A member record with a width and an element count**, replacing an offset that means an index.
   - **Offsets that survive a declaration changing shape**, because the bindings cache arena slot
     POINTERS and persistence is keyed on the offset. Step 3's identity fix (a member is its name at
     an offset, not its position) is the precedent this extends.
   - **`LoadCtrl`/`StoreCtrl` widened**, which is a new op pair on all three backends: today both
     lower to `load8`/`store8` unconditionally.
   - **Where an array lives.** Per-light arrays as a `ScratchBuffer` was the direction; whether a
     small fixed array can stay in the arena is part of the ceiling question rather than separate
     from it.
   - **The ceiling and its number.** A compile error needs a budget, and what a classic ESP32 can
     spare is a product-owner decision, not one to derive from the largest script that happens to
     exist today.

### Strings: literals yes, a String TYPE not yet

`IrOp::ConstPtr` gives a script string LITERALS as arguments, which is what `addUint8("bpm", bpm,
1, 120)` needs: the text is interned into the compiled program and the emitted code carries a
pointer that outlives the source buffer, the same lifetime answer the engine already gives control
and entry-point names.

A String TYPE is deliberately NOT next, and the reason is what the light domain measures rather
than taste. Every one of the 52 compiled effects mentions `const char*`, and every use is
metadata: `name()`, `tags()`, a control label. Not one manipulates text while rendering. So
strings here are a declaration-time concern, which literals cover.

**The first thing literals buy, beyond a control name, is a real debugger.** `print(v)` writes
`[script] 42` and nothing about which value that was, so debugging a script means printing several
numbers and inferring which line each came from. `printf("y=%d x=%d\n", y, x)` is one more builtin
on the same table and the same call path once a string can be an argument, and it makes the one
script-level debugging tool actually usable.

It must be OUR formatter, not a `std::printf` passthrough. The format string comes from a script,
which is the textbook format-string vulnerability: `%s` against an integer argument dereferences a
wild pointer and `%n` writes memory. Walking the format ourselves and accepting `%d`/`%u`/`%x`/`%%`
against arguments that are known to be integers removes the class rather than documenting it. It
also keeps the existing print budget, which is what stops a serial write from sitting on the render
tick.

What a mutable String would additionally need is the hard half: somewhere to put bytes a script
assigns at run time, a length convention, and comparison/concatenation as builtins. That is the
same storage-and-ceiling question arrays face in step 8, so the two want one answer rather than
two. The one concrete use case is a text overlay in a showcase effect, and that can go a long way
on literals plus the numeric vocabulary already present.

10. ✅ **The editing loop, which is the thing people will actually see.** Done, in
   [Plan-20260818](Plan-20260818%20-%20A%20file%20editor%20control%20and%20a%20filesystem%20change%20seam%20(shipped).md).
   A card carries a file picker and an editor; typing and clicking away recompiles.

   Built EARLIER than this plan's "last step, against the finished shape" reasoning suggested, and
   that reasoning turned out to be wrong: the shape a text editor needs (a file, and a compile
   result) does not change when `get()` or arrays-of-structs arrive, and waiting for a language that
   is not finished means never building it. It paid for itself immediately, since every one of this
   plan's own codegen bugs was debugged by editing a file and re-uploading it.

   Two things it needed that were not tooling at all: a CORE seam, because a file write notified
   nothing (`requestPrepareTree` was reachable only from a control write), and step 5's helper.

## Files

Per step, the surface each touches. The pattern is that the FRONT END grows and the backends do not:
the shared lowering and the three assemblers are finished work, and a step that needs to change them
is a step whose design is wrong.

- `src/core/moonlive/MoonLiveCompiler.cpp`: every step from 1 to 9 lands here first: the class body,
  function definitions, the member symbol table, `if`, types wider than a byte.
- `src/core/moonlive/MoonLiveIr.h`: new ops as the language grows (a call to a SCRIPT function, a
  conditional branch, a typed member load/store).
- `src/core/moonlive/moonlive_lower.h`: one arm per new IR op, and nothing else. Touching more than
  that means an ISA fact leaked into the language.
- `src/core/moonlive/MoonLive.{h,cpp}`: the arena becomes typed storage (step 3) and gains its
  ceiling (step 8); entry-point discovery lives here (step 1) for the bindings to consume.
- `src/light/moonlive/MoonLive{Effect,Layout,Modifier}.h`: call an entry point instead of running
  the whole program (step 1), then collapse onto dispatch (step 5).
- `src/light/moonlive/MoonLiveBuiltins_light.h`: `addUint8` for step 3, `get`/`red`/`green`/`blue`
  for step 7.
- `src/platform/esp32/moonlive_asm_xtensa.cpp` + `test/unit/core/moonlive_structural.inc`: step 1
  only: a per-function frame means a nested `call8`, so the frame contract and the checker that
  enforces it both extend to script functions.
- `moonlive/**.mlv`: converted in step 1 (mandatory class) and again in step 3 (`defineControls`).
- `docs/moonmodules/light/MoonLive*.md`, `moonlive/README.md`: the language reference, which is what
  a user reads; it moves with each step rather than at the end.
- `src/ui/`: step 10 only.

## Verification

The governing risk is unchanged from the predecessor plan and is what shapes all of this: **only the
host backend is EXECUTED by tests**, while the constraints that bite hardest are Xtensa's. Every step
therefore needs a host test that proves the semantics and a bench run that proves the encoding.

1. ✅ **A script calling its own function, and then calling it recursively** (step 1). The recursion case
   is the one that proves a frame per activation, and it is the acceptance test for the step.
   Done: `a function the script calls can light pixels and read the script's controls`, `arguments
   reach a function two calls deep`, and `a script function can call itself, each call keeping its
   own values` in unit_moonlive_compiler.cpp, plus `every function in a class starts where a call
   can reach it` per device backend. Each was control-checked by reverting its fix.
2. ✅ **The frame contract, extended to script functions** (step 1). The structural checker must refuse a
   script function whose frame intrudes into the window-save reserve, and it must be shown FAILING on
   a deliberately wrong frame before it is trusted: the same control that caught the original bug.
   Done: the checker's case list gained a calling class and a recursive one, so every prologue it
   walks now carries the argument reload and the depth guard. Control-checked by dropping
   kWindowSaveReserve from the frame calculation, which fires the offset check as it should. The
   derived reserve resisted the first attempt to break it, which is the anti-drift design working:
   editing the static_assert alone changes nothing, because the value comes from the callx opcode.
3. ✅ **A member written by one function and read by another**, and a member that survives across
   `tick()` calls (step 3: the write is the assignment statement, which step 2 did not need). Both
   pinned by unit tests, and the second is the one a stateful effect depends on: three consecutive
   ticks read back 10, 20, 30 from a member the previous tick wrote. Proven on hardware too, since
   `ember.mlv` carries its heat array and phase across every frame on all three boards.
4. ✅ **The same script at the host's real budget and a squeezed one renders identical pixels.** The
   predecessor plan's technique, still the only way the register work is testable off hardware, and
   every new construct has to keep passing it. Holds after `ConstPtr` joined the lowering, which is
   the check that matters: a new op that disturbed the allocator would show up here first.
5. ✅ **Recursion depth degrades visibly** (step 1): a script that recurses without bound keeps the
   device rendering rather than resetting it. Pinned by `a script that recurses without end keeps
   rendering instead of resetting`, which also re-runs the script to prove the counter unwinds: a
   leaked level per frame would silently shrink every later frame's budget. Verified on the classic
   ESP32 (the tightest stack): `forever.mlv` ran 110 seconds continuously at 109 fps, no reset.

   NOT done as specified: the script does not REPORT an error. The refusal is silent, and what a
   user sees is the picture being wrong where the recursion bottomed out. Reporting it needs a
   channel from the emitted block back to the binding, which does not exist yet: worth having, and
   left for the step that gives scripts a diagnostic path.
6. ✅ **An arena ceiling reports a compile error** (step 8), not a failed allocation at run time.
   "the class declares more member data than the arena holds", pinned by a test that sizes its
   source from the constants so raising either limit cannot turn it into a test of the other. The
   ceiling proved itself immediately: the first realistic effect written against it (a 16-element
   fire buffer) was refused at the placeholder 16 bytes, which is how `kCtrlBytes` came to be 64.
7. ✅ **The bench, on all four boards**, after each step: S3 and classic (Xtensa), P4 and S31
   (RISC-V), a scripted layout and a scripted effect. Exec-block sizes compared against the previous
   step, since an unexplained jump is the cheapest signal that codegen went wrong.

   After step 2: **S3 and S31 done**, one board per ISA, each running a scripted layout
   (`grid.mlv`), effect (`plasma.mlv`) and modifier with the controls their `addUint8` calls
   declare. The classic and the P4 are NOT done, so the step is verified per ISA rather than per
   board. Note a device keeps its scripts across a flash, so a board tests the new syntax only once
   the converted files are uploaded to it: the S31 was still running the annotated `grid.mlv` after
   its firmware was current.

   After steps 3a, 6, 8 and 9: **classic, S3 and S31 done**, all running `ember.mlv`, an effect
   that uses every construct at once (a `uint16_t` counter past 255, a `uint8_t[16]` heat array read
   and written by index, member assignment carrying state between ticks, and `if`/`else` on four
   comparisons). The P4 is NOT reflashed since these fixes; it is RISC-V like the S31, so the same
   codegen, but that is an assumption rather than a check.

   | board | ISA | ember | tick | layout |
   |---|---|---:|---:|---:|
   | classic ESP32 | Xtensa | 1396 B | 81 us | 499 B |
   | S3 | Xtensa | 1396 B | 117 us | 499 B |
   | S31 | RISC-V | 1620 B | 33 us | 880 B |

   The two Xtensa targets emit BYTE-IDENTICAL code, which is the cross-check that nothing
   target-specific leaked into codegen. This is also the step where the bench earned its place in
   this list: it found three defects that the whole host suite, both clamp directions and
   `disasm.py` on all three ISAs had passed over (recorded under step 8/9 above).

   Exec blocks at this step, for the next one to compare against:

   | script | Xtensa | RISC-V |
   |---|---:|---:|
   | `grid.mlv` | 499 B | 880 B |
   | `plasma.mlv` | 1378 B | 2644 B |

   After the editing loop (Plan-20260818): **all four boards flashed, wiped and re-seeded** with the
   17 role-named scripts, each compiling its layout and effect with the pickers filtering by role.
   Save-recompile proven on hardware: writing different text into `lines.mle` took the S3 from
   1233 B to 107 B with no `/api/control` call.

   The P4 is up and holds its scripts, but it panics with `Cache error` every few minutes while
   idle. Established as PRE-EXISTING rather than a regression: it runs the default module tree with
   no MoonLive module at all, and a firmware built from a clean `main` crashes identically. Recorded
   in [backlog-platform](../../future/backlog-platform.md#esp32-p4-wifi-through-the-c6-co-processor). A SEPARATE P4 boot loop found in the same
   session WAS this branch's regression and is fixed: the engine had grown to 1440 bytes held by
   value in every scripted module, which `registerType`'s stack probe could not absorb.

8. **`collect_kpi.py` after typed members** (now step 3), because members change how EVERY variable
   is accessed. That is the one step where a hot-path regression is plausible, so it is measured
   rather than assumed. It moved with the step when 2 and 3 swapped: `defineControls()` runs once
   after a compile and emits nothing per tick, so it has no hot path to regress.

## Deliberately not in this plan

- **Inlining**, per the decision above: it optimises what is not the cost, and its failure mode hangs
  the compiler.
- **`while`, `break`, `continue`.** `for` and `if` cover what an effect does; the rest is language
  completeness rather than expressiveness, and each one costs a grammar rule and a test surface.
- **Floating point.** The render path is integer by rule ([coding-standards](../../../contributing/coding-standards.md)),
  and the Xtensa classic has no FPU, so a float in a script would be a silent softfloat call per light.
- **A scripted DRIVER as the fourth role.** It is the honest test of step 5's dispatch, but it needs
  the driver surface to be as settled as the other three are, and that is its own question.
