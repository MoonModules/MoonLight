# Backlog: MoonLive

To-build items for the MoonLive language and engine, each with its fields on one line. The ordered roadmap and its reasoning: [the language roadmap](analysis-moonlive.md).

## Across modules

### The next language steps, in order

Kind: Feature

The [language roadmap](analysis-moonlive.md#the-order-and-why) orders them by what removing each buys: [a bigger arena and more members](analysis-moonlive.md#3-a-bigger-arena-and-more-members-check-the-handle-route-first-storage-rules-now-in-the-type-system-design-above), [what remains of the predefined structs](analysis-moonlive.md#4b-predefined-structs-what-remains-after-coord3d-and-rgb), [function arguments and return values](analysis-moonlive.md#6-function-arguments-and-return-values-moderate-removes-a-real-footgun), [user structs and arrays of them](analysis-moonlive.md#10-user-structs-and-arrays-of-them-readability-once-the-arena-is-bigger), [what the StadBeest legs and eyes hit](analysis-moonlive.md#11-what-the-stadbeest-legs-and-eyes-hit-2026-10-07) and [a one-to-one port of hpwit's scripts](analysis-moonlive.md#12-a-one-to-one-port-of-hpwits-scripts-for-a-like-for-like-comparison-2026-10-19). A step lands when it deletes a row from the roadmap's compromise table, and the predecessor's livescripts corpus compiling unchanged is the outside measure.

### The code cap, per platform (2026-10-16)

Kind: Feature

Measured across all 53 shipped scripts with `moondeck/moonlive/disasm.py` (asked on Discord by hpwit, author of ESPLiveScript): compiled bytes per source byte run Thumb-2 1.4x, Xtensa 1.7x, arm64 2.2x, RISC-V 2.8x. The largest script, `stadbeest-sauron.mle` (135 lines, 5.7 KB of source), compiles to 7.8 KB on Xtensa and 13.7 KB on RISC-V, so a script about 20% bigger is refused on a C3 or a P4.

A script's code meets three ceilings and the lowest wins: `kCodeCap`, the staging the compiler reserves per token (`codeCapFor`, capped at `kCodeCap`), and the executable memory `allocExec` finds. Only the last is the hardware: IRAM on a classic ESP32 holds tens of KB, while a P4's 768 KB of internal memory is executable and a desktop's is unbounded. The fix is the code cap as a per-platform value from the platform layer: 16 KB on a classic ESP32 and a C3, 32 KB on an S3, 64 KB on a P4 and the desktop. Pinned by a test that a script past 16 KB compiles on a platform whose cap allows it and is refused with a clear message where it does not. The members, frame slots, entry points and labels are separate budgets, costing memory per script on every chip, so they stay until a script needs more.

### A for loop whose step never advances holds the render task (2026-10-29)

Kind: Bug · Platform: all

`parseFor` in `MoonLiveCompiler.cpp` requires the step to assign the loop variable but not to change it, so `for (int i = 0; i < 1; i = i) {}` compiles, and the emitted loop compares a counter that never moves against its limit with no iteration cap. On a device the render task never returns and the task watchdog resets the board, where the rule is degrade, never crash. The cheap tier is an iteration budget per entry, checked at the loop's back-edge, that fails the script with a status and leaves the layer dark, as a parse error does. Traced in the code, not run: a test of it would hang the suite until the budget exists, so the test lands with the fix.

### A scripted driver

Kind: Feature

[MoonLive](../../explanation/architecture/moonlive.md) names a driver as a role still to come. A `MoonLiveDriver` is a `DriverBase` whose `tick()` runs compiled code over the output buffer, the way `MoonLiveLayout` answers `placeLights()`. The builtin it needs is a read of the corrected frame; the binding stays as thin as the effect's.

### A scripted control's name can disagree with the file that declares it (2026-09-04)

Kind: Bug · Platform: all

Reproduced with `Nebula.mle` on the desktop: the file declares `rate`, the UI renders `rate1`, a name the file held earlier in the session. Ruled out: the builtin table, `kMaxCtrls`, the arena, declaration order and the control name. Start at `rebuildControls()` after a recompile and whether a control that vanished from the script is cleared from `ControlList`; a test that compiles, renames one control, recompiles and asserts the old name is gone pins it. Beside it: `/moonlive/Nebula.mle` and `/.moonlive/nebula.mle` are two files on a device and one on a case-insensitive desktop.

### Desktop backend: a control-arena write is not seen by a second `run()` of an already-compiled program (2026-08-20)

Kind: Bug · Platform: desktop

The live-edit path is "move a slider, the next frame reads the new arena
byte, no recompile". On the host JIT the first render is correct, the write lands (the arena
reads back `232,3` = 1000 at the control's own `offset`), and a second `run()` of the same
compiled program still renders the OLD value.

**Ruled out — do not re-investigate these:**
- Not `LoadCtrl16` missing: it is implemented, lowers to `a.load16`, and `load16` exists in all
  three assemblers including `platform/desktop/moonlive_asm_host.cpp` (`ldrh`, immediate scaled).
- Not uint16-specific: a `uint8_t` member behaves identically.
- Not `defineControls()` or the control sink: a class with an unrelated second function fails the
  same way, and one with no members at all (a literal in `tick`) does too.
- Not the arena address or offset: `controlSlot(dc[0].offset)` is the byte the write reaches.

**A large part of the original symptom was a TEST BUG, not an engine bug.** `run()` without an
entry name starts at the BLOCK START — the first function compiled — which in a class with
`defineControls()` is `defineControls`, not `tick`. So the script rendered nothing and looked
like a broken control read. Naming `kEntryTick` makes the initial render correct in every
variant tried (with/without `defineControls`, with an unrelated second function, `tick` first or
second, uint8 and uint16). Only the re-read after a slot write is still wrong.

**Reproduce** (in `unit_moonlive_fill.cpp`, which has `kCtrlTable`/`kSys` to hand): compile
`class T { int big = 5; defineControls() { addControl("big", big, 0, 1000); } tick() {
setRGB(0, big, 0, 0); } }`, `run(..., kEntryTick)` → 5, write the slot to 7, run again → still 5.

**Impact: desktop only, nothing ships broken.** A wide control is hardware-verified on both ISAs
(S3 Xtensa and S31 RISC-V drive ember's `cycle` to 2000 and back). What is missing is DESKTOP
coverage of the live-edit loop — the path users touch most — so no host test can pin it and the
next regression there would surface only on a board. Add the runtime assertion together with the
fix, not before: asserting the current behavior would encode the bug.

### A MoonLive script's branch tables are fixed and on the stack (2026-10-08)

Kind: Bug

**Found** on the StadBeest eyes: one more `if` in a helper turned a working script into `codegen failed: assembler overflow`.
Each function takes three labels (entry, depth guard, exit) before its own `if`s, loops and divisions by a non-power-of-two.
The assembler's label table holds 96 two-byte code positions in the stack the 48 four-byte ones took (2026-10-10), which let Sauron blink.
The nearer limits are now the compiler's own `kIrLabels` at 40, which reports "too many branches", and the 96 fixups.

**The fix to build:** the label and fixup tables of all three assemblers move from members to the heap, as the code buffer already did, then `kAsmFixups` and `kIrLabels` rise (to 255 and 96); `Label` stays a byte.
The spill pass and the lowering hold `kIrLabels`-sized arrays on the stack too, so they move with it.
Verified by compiling the shipped scripts on a classic ESP32, an S3 and a P4, whose compile stacks are the ones at risk, and pinned by a host test with a script past 40 branches on each backend.

### A MoonLive control past a limit vanishes without a word (2026-10-08)

Kind: Bug

**Found** in review: an `addSelect` with a 17th option, or one whose names overflow the 96-character option store, adds no control at all, and nothing tells the script's author why.
The other limits of `addDeclaredControl` behave the same way: past `kMaxCtrls` controls, a member already bound, a slot running past the arena, or names past the engine's 128-character name store (`nameText_`, about sixteen controls of seven characters).

**The fix to build:** each refusal names itself on the effect's status line, as a compile error does, such as `control "shape": more than 16 options`, so the author sees which declaration lost and why.
Pinned by a test per limit that reads the status.

### RISC-V emits 1.7 times the code Xtensa does (2026-10-08)

Kind: Investigation · Platform: P4

**Found** on the StadBeest eyes: the script is 7 KB on an S3 and 12.4 KB on RISC-V, where one `smoothstep` call added to `tick` cost 432 bytes on its own.
A call inside a function with many live values saves and restores each of them, so every addition to a drawing loop costs RISC-V 400 to 500 bytes, and large scripts reach the 16 KB code buffer first on the P4 and the S31.

**The fix to build:** measure where the RISC-V lowering spends its bytes on the shipped scripts (`disasm.py --isa riscv`), starting with the save and restore around host calls, and keep values that live across a call in callee-saved registers rather than saving them per call.
Pinned by a size test per backend on the largest shipped script.

### A scripted module's DIMENSION chip still comes from its type, not its script

Kind: Bug

`writeModuleJson` emits an instance's `tags()` (HttpServerModule.cpp), so a MoonLive module shows
the emoji its loaded script declares. Its DIMENSION does not follow the same path: `/api/types`
carries `dim` per TYPE, captured at boot from a probe with no script loaded, and the module state
carries no `dim` at all. So a script declaring `int dimensions() { return 3; }` renders as 🟦 on the
card while behaving as D3 through `Layer::extrude`: the behavior is right and the chip lies.

The fix is not a line in the serializer. `MoonModule` deliberately has no `dimensions()`:
`ModuleFactory::registerType` detects one with `if constexpr (requires ...)` on the CONCRETE type
precisely so the light-domain `Dim` enum stays out of core (ModuleFactory.h). Core holds a
`MoonModule*` when it writes state, so emitting a per-instance dim means giving `MoonModule` a
virtual that returns a byte, which puts a light-domain concept on the domain-neutral base for the
sake of a chip.

Options, cheapest first: a `uint8_t dimByte()` on MoonModule defaulting to 0 (the enum stays in the
light domain, only the number crosses, mirroring what the probe already does); or leave it and
accept that a scripted module's dimension chip reflects its type. Worth doing when someone is
annoyed by the wrong chip, not before.

### MoonLive core/platform layering + JIT sdkconfig scoping (CodeRabbit #29, 3 findings left)

Kind: Restructure

Four 🟠 Major boundary findings from the PR #29 review are real but each is its own scoped change, not a tail on the ring branch. The Critical sibling (a `cpl<3` overflow guard in the MoonLive effect's `tick`) landed with the branch it was found on.

**One of the four is CLOSED (2026-09-07):** the scenario now uses `PreviewDriver`, the in-process
sink, instead of `NetworkSendDriver`. Its `tick_us` half was reviewed and DISMISSED rather than
fixed: a `measure` step asserts nothing, it records, and that recording is what feeds repo-health's
per-commit performance trend. A reviewer reading the file could not see that; deleting the
baselines would have blinded the trend to fix nothing. Recorded here so it is not re-raised.

The three that remain:

- **Core includes platform, compiled core in `mm_core`.** `src/core/moonlive/MoonLive.cpp` `#include`s `platform/platform.h` and calls the exec-memory API directly, and the root `CMakeLists.txt` compiles `MoonLive.cpp`/`MoonLiveCompiler.cpp` into `mm_core` and links `mm_core → mm_platform` — violating the header-only-core / no-platform-includes contract both files declare. The runtime exec-memory placement layer wants a core-neutral injected interface (or to move out of `src/core`), so the compiled/platform-dependent surface sits behind `mm_platform` and `mm_core` stays INTERFACE-only. These two are one change (same boundary).
- **W^X disabled in the board default.** `esp32/sdkconfig.defaults.esp32s3-n16r8` turns off `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` and enables `CONFIG_HEAP_HAS_EXEC_HEAP` for *every* build on that board, even with no MoonLive effect installed. The JIT genuinely needs a writable-then-executable heap, but that belongs in a dedicated MoonLive/JIT opt-in overlay or an explicit build profile, not the board default — so a stock build keeps memory protection on.

### The compile-failure latch is not provable on the host

Kind: Feature

- **The compile-failure latch is not provable on the host** (2026-08-18). `MoonLiveScript::sync`
  refuses to re-attempt a script that failed until its (name, content) changes. The latch exists for
  a device-only reason: each attempt is two LittleFS reads (~5 ms on an S3), a layout is asked from
  `lightCount()`/`placeLights()` as well as `prepare()`, and the pipeline asks repeatedly while
  sizing a fixture, so the retries starve the render task until the watchdog resets the device.

  On the host a re-read costs microseconds and nothing observable differs. Four test shapes were
  tried and each still passed with the latch REMOVED ENTIRELY, so none was kept: what survives pins
  only that a script fixed in place compiles without a rename, which is control-checked. Closing
  this needs either a counting seam (a compile counter the test can read) or a platform fake whose
  reads are observable. Until then the latch is protected by its comment and by hardware, not by a
  test.

### Catch device-backend operand defects on the host

Kind: Feature

- **Catch device-backend operand defects on the host** (2026-08-18). Two array-codegen bugs shipped
  to an S3 while all 1313 host tests stayed green, and both were control-checked: reintroducing
  either one leaves the suite fully passing. `IrInst::c`/`d` are VREG fields the spill pass
  renumbers, so a width parked there becomes a register number; and `sourcesOf` writes its sources
  back POSITIONALLY, so reporting `kArg4` first shifts every real operand one place along. arm64's
  register map absorbs both, which is exactly why the suite cannot see them.

  Three test shapes were tried and deleted for failing their control run: emitted-bytes difference
  tests (wrong bytes still differ from other wrong bytes) and a register-liveness walk (the index
  satisfies it whatever happens to the value). What DOES work is asserting an assembler primitive
  directly, as `unit_moonlive_codegen_xtensa.cpp` now does for `addImm`. The general form is
  probably an IR-level invariant check rather than a bytes-level one: assert that no op reports a
  fixed ABI vreg among its positional sources, and that non-register operands never occupy c/d.
  That is a property of the IR the host CAN evaluate, unlike the emitted code.

### Size the MoonLive control arena to the script

Kind: Feature

- **Size the MoonLive control arena to the script** (2026-08-18). `kCtrlBytes` is a fixed 64 bytes
  per engine (three engines per pipeline), so a script declaring one byte pays for 64 and one
  wanting a 128-light array is refused. The arena is already `platform::alloc`'d, so the constant is
  habit rather than necessity, and the member byte count is known at compile time.

  What blocks it: the system variables sit ABOVE the script region at compile-time constant offsets
  (`kSysWidth = kCtrlBytes + 0`), baked into emitted code as `LoadCtrl` immediates and cached as
  slot POINTERS by the bindings. A script-sized region moves every one of them. Closing it means
  putting the system variables BELOW the script region so their addresses stop depending on it,
  which touches the sysvar table, the bindings and every emitted immediate. The hard ceiling stays
  255 either way, since an arena offset is a `uint8_t` in the record, in `controlSlot` and in the
  instruction. Until then, raising the constant is one edit and the failure is a clear compile
  error naming the arena, so hitting it is visible rather than silent.

### Drain MoonLive's print() through a queue

Kind: Feature

- **Drain MoonLive's `print()` through a queue** (2026-08-09). `print(v)` writes to serial directly, and an EFFECT script runs on the render tick — so a print inside one blocks the frame for as long as the UART takes. The burst cap bounds it (a handful of writes per compile, then a compare and a return), but bounded is not free, and `tick()` is annotated `MM_NONBLOCKING`.

  **What it costs when it comes:** a small preallocated record queue the built-in writes into, drained from a housekeeping path through the existing platform output seam. The budget and the burst-spent message stay as they are; only where the bytes are written moves. Worth doing when a script is left with a print in it on a real fixture, which is the case the cap exists for.

### Downloading a scripted palette can repoint the active one (2026-09-06)

Kind: Bug

Found while testing the palette-download fixes. A `.mlp` that sorts BEFORE an already-installed one
silently changes what the current selection points at: with `fire.mlp` active at index 60,
downloading `beat-flash.mlp` re-sorts the scripted tail and index 60 becomes `beat-flash.mlp`. The
stored value never moved; what it means did.

[Palette.h](../../../src/light/util/Palette.h) sorts scripted palettes AFTER the built-ins, whose indices are fixed, and solves half of it. What is not solved is the scripted tail among itself, and
that tail grows at runtime, which is what the download path made reachable.

Nobody has reported it, and the blast radius is small: a rig with one scripted palette cannot hit it,
and the value re-reads correctly the moment the user picks again. It matters most where a preset or
an MQTT/HA automation stores the index and replays it later, which is the case where nobody is
watching the LEDs when it changes.

**What it would take:** persist the scripted palette by NAME alongside the index and re-resolve the
index on load, so a stored selection survives a re-sort. `paletteScript` already holds the resolved
file name for the editor, so the value exists; what is missing is using it as the authority when the
list changes. The alternative, appending new scripts rather than sorting them, keeps indices stable
but makes the picker unreadable as the list grows, which is the trade the sort was chosen over.

### Changing a live MoonLiveEffect's script does not rebind it (2026-09-29)

Kind: Bug

**Being worked on the macOS bench; this entry is measurement for whoever has it.** Rewriting the `script` control on an existing `MoonLiveEffect` leaves the module largely rendering what the previous script rendered. The card reports a clean status, the byte count updates, frames keep animating, so nothing in `/api/state` says anything is wrong. Only the pixels do.

Measured on Windows over the 33 shipped `.mle` effects, same scripts and same instrument, changing only the route. Each script ran 3 s while its preview frames were sampled, then scripts were compared on mean level and on frame-to-frame motion; a pair matching closely on both is a pair that cannot be telling the pixels apart:

- Reusing one module and rewriting its `script` control: **59 indistinct pairs**, in groups. Nine scripts (noise, octopus, plasma, rain, ripples, sparkle, spectrum, stadbeest-frog-eye, stadbeest-walk) all sat at mean 27.2 and motion 0.16, and six more (fountain, fractal, gradient, lines, metal, mh-ambient) at mean 42.9 and motion 0.33.
- A fresh module per script, which is what the UI's picker does: **1 indistinct pair**, and the values separate as you would expect, with fractal at 108.2, noise at 127.2, mh-aim at 255.0 and stadbeest-frog-eye at 151.5.

The picker works because it does two things rather than one: `replacePickedType` in `src/ui/app.js` calls `replaceModule` to put a NEW module in the slot and only then `setCardScript`. A user who switches scripts from the dropdown on an existing card gets the broken path; a user who picks from the module picker does not, which is why the same library looks fine one way and stale the other.

Two scripts render nothing that changes even by the working route, and may simply be correct: `gradient.mle` produces one distinct frame at mean 104.8 with zero motion, and `mh-aim.mle` one distinct frame with every sampled channel at 255. `mh-aim` aims a moving head rather than painting a grid, so full white may be the honest answer. Worth an eye rather than an assumption.

The check itself is worth keeping. Frame distinctness plus cross-script comparison catches exactly what compiling cannot, and it is what the existing test suite has no view of: `unit_MoonLiveScripts.cpp` proves all 55 scripts COMPILE, and every one of them passed while the pixels were telling this story.

**From the card (2026-09-29, desktop).** Six consecutive picks from the script dropdown: two switched, four did not, and the fourth failure recovered on its own. The file is on the device and complete, the card shows the new name, and no alert fires. The dirty-editor path is not the cause: it alerts "Not switching: this script still has unsaved changes" (`src/ui/app.js`). The duplicated download logic in the dropdown was a real defect and is fixed, pinned by `test/js/ui-script-picker-download.test.mjs`, and it did not change the symptom.

**Worth deciding alongside the fix**: whether the script control should be editable at all, or whether a scripted module's script is chosen once through the module picker. The module picker path works reliably, so a read-only control would remove the failing surface rather than repair it.

### A scripted modifier that animates every frame

Kind: Feature

- **A scripted modifier that animates every frame** (2026-10-11): `MoonLiveModifier` compiles a `modifyLive()` entry, so a script moves lights per frame with `t` (scroll, wobble, sweep) as the compiled Rotate does. Geometry only: dimming is a blend mode or an effect's `mulRGB`, per [the layer compositing plan](../present/Plan-20261011%20-%20Layer%20compositing%2C%20masks%20and%20transitions%20(2%20of%203).md). Built once a script needs it; the cost to measure is one compiled-script call per light per frame, on an S3 at its largest grid.

### A scripted modifier needs a way to drop a light

Kind: Feature

- **A scripted modifier needs a way to drop a light** (2026-08-10). A coordinate slot is a byte, so a script that computes past 255 wraps: `shift.mlv` with a large `amount` lands lights back at the left edge instead of walking them off it. The Layer already drops an out-of-bounds position, but a script has no way to SAY out-of-bounds — every value it can write is a valid coordinate. Needs a sentinel the binding recognizes (or a wider coordinate slot), at which point the "walks off the edge" behavior a scroll modifier wants becomes expressible.

### A scripted modifier that reshapes the grid

Kind: Feature

- **A scripted modifier that reshapes the grid** (2026-08-10). `ModifierBase::modifyLogicalSize` lets a modifier change the logical `width`/`height`/`depth` — a Multiply kaleidoscope grows the grid, a crop shrinks it — and a compiled modifier uses it. A SCRIPTED one cannot: system variables are read-only, so `MoonLiveModifier` writes the box in and never reads it back. Needs a writable system variable — the binding reads the slots after the script returns and reports the result through `modifyLogicalSize` — which is a new `SysVarKind` (or a mutable flag on `SysVar`) plus the read-back, not a new builtin. Until then a scripted modifier can fold coordinates but not resize the grid they live in.
