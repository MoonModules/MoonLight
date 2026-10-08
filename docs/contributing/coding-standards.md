# Coding standards

How code in this repo is written. Hard rules and process live in [CLAUDE.md](../CLAUDE.md).

How to build and run lives in [building.md](../how-to/building.md), and what is tested in [testing.md](../reference/testing.md). Design rationale lives in [the architecture](../explanation/architecture/index.md).

It runs from the smallest scale outward: the line you are typing, then naming and when to add a file, then values and the hot path they run in. After that the shape of a whole module, then the tooling that checks all of it, and last the method for when something breaks.

## Writing a line of code

Decided once, not re-derived per file.

### Language mechanics

- **`#pragma once`** for header guards. No `#ifndef … #define … #endif`.
- **`constexpr`** over `#define` for compile-time constants. `#define` is reserved for build-system flags (e.g. `MM_NO_WIFI`) that need to be visible to the preprocessor.
- **`std::span`** over pointer + length pair in function signatures. The span carries the bound; the caller can't desync.
- **Namespace `mm`** for everything in the project. Platform code lives in `mm::platform`. Tests live in `mm` (no `mm::test`, keeps the same names visible to test code as to library code).
- **No `using namespace` in headers.** In a `.cpp`, `using namespace mm;` is allowed at file scope. In a header it pollutes every translation unit that includes it.

### Naming

- **Semantic variable names.** Name variables for what they represent, beyond their type. `availableHeap` not `available`, `internalHeap` not `internal`, `lutBytes` not `bytes`. A reader should understand the variable without looking at its assignment.
- **Prefer naming over commenting.** A comment explaining WHAT a line does is usually a naming failure: extract a function, name a constant, add an explaining variable, or rename the thing. A comment explaining WHY stays, because no name carries a constraint ("the DMA cannot read PSRAM at shift clock"). Prior art: *Clean Code* ch. 4 for the naming half, Ousterhout's *A Philosophy of Software Design* for why the why-comment is load-bearing.
- **Prose, comments and docs follow [documentation-standards.md](documentation-standards.md).** Spelling, em-dashes, markdown wrapping, what a comment is for, and the MoonLive script comment budget live there, together with the page model they serve. `///` comments are the boundary case: the syntax rules below are ours, everything they SAY follows that page, because they generate it.

### Casts

Use project typedefs (`lengthType`, `nrOfLightsType`) consistently so types match and casts are unnecessary. When casts are needed:

- **`static_cast`**: converts a value between related types. Checked at compile time. Use only at system boundaries: byte protocol packing, OS API return values, overflow-prevention with wider intermediates. If you need `static_cast` between project types, make the types match instead.
- **`reinterpret_cast`**: raw byte or memory access only, such as `reinterpret_cast<const sockaddr*>` for a socket API. It converts nothing and checks nothing, so that is its whole territory.
- **`dynamic_cast`**: absent by construction. ESP32 builds with `-fno-rtti`, so a base-to-derived question is answered by a virtual method or a role check instead.

## Growing the codebase

- **Consider extending before creating.** When adding a feature, check whether an existing module extends cleanly; a new file is fine if genuinely cleaner, but justify it.
- **Reference, don't copy.** Prior art (friend repos, datasheets, our own prototype branches) holds proven approaches: study it, take the ideas, write our own code, never copy or trace the structure. Credits live in the [friend-repo digests](../friend-repos/index.md) and per-module prior-art sections.

## Values and types

**Default to integers.** Pick the smallest unsigned type that fits the natural range, `uint8_t` for percentages and small counts, `uint16_t` for pixels and ports, `uint32_t` for timestamps and byte counts, `int8_t` for signed RSSI-style values. Integers are faster, predictable, branch-free in the hot path, and one byte where they need to be.

**Use `float` only when the value is intrinsically fractional**, geometry positions on a normalized grid, audio amplitudes, ratios that would lose meaning if rounded. Even then, ask whether scaled integers (e.g. fixed-point `uint16_t` for 0..65535 mapping to 0.0..1.0) work. The render loop hits every light every frame; an integer multiply-and-shift dominates a float multiply on ESP32.

**`float` is the widest type in firmware code, and `double` has no place in it.** Xtensa (ESP32 classic) has no hardware FPU for `double`, every `double` operation runs in software emulation, ~30× slower than the same op on `float`. `1.0` is a `double` literal; write `1.0f` when you meant float. A `double` slipping into the render path silently tanks FPS.

**Store values in their native shape.** When the value is intrinsically numeric, store it as a number. When it is four octets, store it as four octets. **A long-lived string buffer earns nothing from the UI eventually showing the value as text.** Format to string at the output boundary, on the stack, then throw the buffer away. Every "I'll `char foo[12]` it now and snprintf into it" decision freezes a few bytes into the module's permanent footprint. The cheaper alternative is one int and a `snprintf` on a local stack buffer at serialization time. On ESP32 with ~180 KB free heap, dozens of those add up.

Guidelines:

- **RSSI, TX power, frame counts, percentages, temperatures, voltages**: store as `int8_t` / `uint8_t` / `uint16_t`. If the UI needs a unit suffix, carry the suffix in the control descriptor (`ControlType::ReadOnlyInt` does this, see [Control.h](../src/core/module/Control.h)), not in a per-instance string.
- **IPv4 addresses**: store and pass as `uint8_t[4]`, never a dotted-quad string. A getter that returns text forces every caller into a `char[16]` and one caller parsed it straight back to octets. Platform getters return octets; each consumer formats at its own output boundary. See `ControlType::IPv4`.
- **Mode / status labels from a small fixed set**: a `char[20]` buffer is acceptable when the label is short and `snprintf`'d at a transition. For purely constant labels a `const char*` pointed at a static literal is even cheaper. Don't combine the two: don't `snprintf` a literal into a buffer.
- **Dynamic display strings (uptime, FPS, heap KB)**: `char[N]` buffer is the established pattern (see [SystemModule.h](../src/core/system/SystemModule.h)) because the value changes every second and the UI reads it by stable pointer. Size the buffer to the longest possible value; oversized buffers are waste.

Counter-example to avoid: storing `char rssiStr_[12]` and re-`snprintf`'ing `"-58 dBm"` into it every tick. The right shape is `int8_t rssi_` (1 byte) plus a control type that knows the unit. Saves 11 bytes per metric, scales linearly across the codebase.

**A dropdown's values have names.** A Select control's member holds an index into its options, so the code that reads or sets it says which option it means, never a bare number.
Declare a plain `enum Name : uint8_t { kNameFirst, ..., kNameCount }` in the class, its members in the options' order, next to the options array it indexes, and pass its count to `addSelect`.
The enum costs no flash, since the compiler folds each name to its number, and it keeps the count beside the options, so adding one updates both.
Where an enum for the values already exists, such as `Addressing` or `platform::EthPhyType`, use it rather than declaring a second one.
A test sets the control by the same names.
See [MidiService.h](../src/core/services/MidiService.h): `profile == kProfileApc40`, not `profile == 1`.

**Width the intermediate, and the result follows.** Any `a * b` where both operands are `nrOfLightsType`, or a count times a multiplier, can overflow `uint16_t` even when each operand is small: `256 * 256 = 65536` wraps to 0 on a no-PSRAM device. Do the arithmetic in a wider type, clamp to the ceiling, then narrow. A counter derived from a cell count is the domain typedef too, never a fixed `uint16_t`. Such a path is invisible on the uint32 desktop build, so pin it with a `uint16`-typed unit test or hardware confirmation.

**When a validation field's storage is narrower than what it claims to validate, the validation is wrong, not the field.** A `uint8_t` min/max slot can't bound an `Int16` control, clamping it to `[0..0]`. The fix is a wider bound, or per-type bound slots; until then the constraint is documented at the field's declaration.

**When one control type does two jobs with different UX, that's the smell for a new type, not a range hack.** An `int16` control the UI renders as a slider can't also mean "GPIO pin number". A dedicated `Pin` type, with the smallest storage that fits the domain, is the fix rather than overloading the range.

## The hot path

**A faster device renders the same motion more smoothly, not more motion.** Anything on a tick path whose output changes between two calls with identical inputs is animating. It takes its step from wallclock, not from having been called. This holds for effects, for modifiers that scroll or rotate, and for anything else the render loop reaches. A pure fold of coordinates from controls is not animating and owes nothing. Rationale and the two-rate check: [live reconfiguration, in architecture.md](../explanation/architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame).

The shape, whichever quantity it is:

```cpp
carry_ += rate * time_.advance(elapsed());     // particles::FrameTime, 256 = one 1/60 s frame uint32_t due = carry_ / particles::FrameTime::kOne; carry_ -= due * particles::FrameTime::kOne;    // CARRY the remainder, never floor it to 1
```

**Carry the fraction.** At a high render rate the per-frame amount is legitimately below one unit, and flooring it to 1 applies many times the intended amount. That is what made trails visibly shorter on a fast device than on a slow one. Cap `due` so a long stall tops the effect up rather than bursting a frame's worth of work at once.

**Ask whether something upstream already scales it.** Do not scale a quantity twice. A fade requested through `Layer::fadeToBlackBy` is already scaled by the Layer, so an effect passes a rate and does nothing else. A fade requested only on frames that pass a wallclock gate gets throttled by the gate AND by the scale. When in doubt, write the two-rate test first.

**A compounding spatial operation is not a rate.** `draw::blur` applied twice at half strength is not one blur at full strength. The carry pattern above does not transfer to it. Gate it in time and leave it at full strength.

## Shaping a module

### File shape: header-only vs `.h` + `.cpp`

- **Light-domain modules and the `MoonModule` base: header-only.** Implementation inline in a single `.h`, so a contributor copies an effect, edits, and registers one line in `main.cpp` with no CMake question. A file that outgrows one concern extracts a helper header rather than splitting to `.h` + `.cpp`.
- **Core service modules: `.h` + `.cpp`.** A core module that bridges to the platform or carries real infrastructure splits interface from body. An implementation edit then recompiles one translation unit rather than every includer, which is 2-5x faster on the edits development makes. Small utilities that are almost all declarations stay header-only, and a template must.
- **A catalog module includes ONLY its base header**, which is the module author's standard library. It carries the render context, the domain helpers, the lifecycle primitives and the standard headers bodies use. Bundling costs nothing, since unused declarations emit no code, so add a common header to the base rather than scatter it per file. A prelude, the way Rust's `std::prelude` is.

  **The only permitted second include** is a helper outside the domain's standard surface. A network packet format, a font table, the module factory, a platform primitive or a specialized core service. It carries a one-line comment so the exception is visibly deliberate. A header that two or more modules of one kind reach for is no exception: move it into the base header, which is how `<cstdint>`, `<array>` and `<algorithm>` got there. Where a base header has an include cycle, the header itself explains the shape it takes.
- **Exceptions need a one-line comment at the top of the file naming the reason.** Without a stated reason the file is expected to follow the default for its category. When in doubt: light → header-only, core → `.h` + `.cpp`.

### Override-and-chain convention

A MoonModule that owns children gets the standard lifecycle methods (`setup`, `tick`, `tick20ms`, `tick1s`, `release`, `defineControls`, `prepare`) propagated to children by the base class default, see [MoonModule](../explanation/architecture/moonmodule.md). When a container overrides one of these to add its own work, the convention is **when in the override to call the base**:

- **`tick` / `tick20ms` / `tick1s`**: option A: parent work first, then chain. The parent prepares state that children consume.

  ```cpp
  void tick() override {
      if (layer_ && layer_->lut().hasLUT() && outputBuffer_.data()) {
          blendMap(...);                  // parent's own work
      }
      MoonModule::tick();                 // then tick children
  }
  ```

- **`setup`**: chain first, then parent work. Children must be initialized before the parent depends on them.
- **`defineControls`**: chain first, then parent work. Children register their controls before the parent appends or rewires its own. Lets a parent build a list whose order is "children's controls, then mine."
- **`prepare`**: chain first, then parent work. Children compute their dimensions and dynamic buffers before the parent reads or modifies the shared state (Layer reads child effect/modifier dimensions; Drivers reads Layer output sizing).
- **`release`**: parent work first, then chain. The parent shuts down its own state before the base reverse-iterates children.

Option B (children first on `tick`; parent first on `setup` / `defineControls` / `prepare`) or a sandwich pattern is allowed only when a specific reason justifies it. Add a one-line comment at the override explaining why.

### Per-type behavior lives with the type

A struct or enum that is the semantic owner of some data owns the functions that interpret, serialize or validate it. A control descriptor, a packet and a module role are each such an owner. They live next to the type, never at the call sites. Free functions in the same `.cpp` count; member methods on the owning class are stronger; virtual methods on a base class are strongest. The wrong shape is the same `switch (type)` repeated in every consumer: adding a variant means hunting across N files, and the compiler cannot say when one is missed.

Three concrete patterns, all already common in this codebase:

- **Discriminator + free functions in the type's own file.** `ControlType` + `writeControlValue` / `applyControlValue` / `controlTypeName` in [Control.cpp](../src/core/module/Control.cpp); `parseDottedQuad` / `formatDottedQuad` next to `ControlType::IPv4` in [Control.h](../src/core/module/Control.h); `WhiteMode` + `rebuild()` in [Correction.h](../src/light/drivers/Correction.h). Best when the discriminator is a plain enum and the operations are small.
- **Methods on the owning class.** [Buffer.h](../src/light/layers/Buffer.h)'s `allocate` / `free` / `clear`; [Scheduler.h](../src/core/module/Scheduler.h)'s `addModule` / `tick` / `prepareTree`; [ControlList](../src/core/module/Control.h)'s `addX` family. Best when the class has identity and the operations naturally form a small interface.
- **Virtual methods on a base class.** [MoonModule.h](../src/core/module/MoonModule.h)'s lifecycle (`setup`, `tick`, `tick1s`, `defineControls`, `prepare`, …). Best when polymorphism is already in play.

Counter-example to avoid: a `switch (c.type)` on `ControlType` duplicated across the HTTP server, the filesystem module and the scenario runner. That shape forces a new ControlType to be added in four places, and the compiler can't catch a missed switch on a non-exhaustive enum. The per-type dispatch instead lives next to `ControlType` in [Control.cpp](../src/core/module/Control.cpp); consumers call `writeControlValue(sink, c)` and don't need to know the enum's shape.

When a `switch (type)` outside the type's home file is legitimate: the caller has a genuinely different concern. The HTTP server mapping `ApplyResult` to status codes is a transport policy, not per-type behavior, and scenario_runner's `switch (JsonVal::type)` dispatches on *its own* discriminator. The rule is "per-type dispatch lives with the type", not "switches are banned".

**A flat parser that returns a zero sentinel for a missing key must be paired with a presence check before the value is applied as authoritative.** `json::parseInt(json, key)` returns 0 for an absent key, indistinguishable from a real 0. On a persistence-overlay load path that clobbers a non-zero default when an older or partial file omits the key. Guard with `json::hasKey()` first: an absent key leaves the control untouched (its default stands); a present key (even value 0) applies. Any "control resets to its default/0 after reboot" symptom is this overlay smell, not a control-init bug.

**A reader that decodes only a subset of the escapes its writer emits is a latent asymmetry bug.** If the writer emits `\n` / `\t` escapes, the reader must decode them, not only `\"` / `\\`. Make the escape set symmetric so multi-line text round-trips.

### Control order

- **A conditional control is registered BELOW the control it depends on.** When visibility, range or presence is gated on another control's value, `addX()` it after that control. The UI then reads top-down as cause then effect, and the source order is self-documenting.

### Defaults

**Assign a default only where the hardware, not the user's soldering iron, fixes the value.** The test is *who fixes the pin/setting*:

- **Chip-/board-fixed → default it, and you must.** The RMII Ethernet pin map, the on-board status LED, a country code per region are silicon- or PCB-wired, so a default cannot do harm. Omitting one does: a no-WiFi board with un-defaulted Ethernet pins can never connect to be configured.
- **User-soldered → leave it unset.** A MEMS mic, an LED strand or an LED-driver pin goes wherever the user ran the wire. Any default is then a guess that can drive a pin the user committed elsewhere. Empty until set; idle with a "set pins" status meanwhile (degraded is fine, crashed is not).

A "default" that is one specific board's values is bespoke masquerading as standard. Make the capability opt-in and require each consumer to state its own values, so a missing declaration fails loudly instead of inheriting a stranger's wiring. Never auto-run a peripheral whose init can block on absent hardware. The design rationale, the MCU to deviceModel provenance model, is in [architecture.md, config provenance](../explanation/architecture/mooninstaller.md#config-provenance-mcu-devicemodel).

## Tooling and checks

### Running Python

- **All Python through `uv run`**, never bare `python3`: it is not on PATH on Windows, and the macOS launcher pops a Store prompt. uv owns the project venv ([MoonDeck.md](../moondeck/MoonDeck.md)). One exception: ESP-IDF builds use IDF's own bundled Python, which IDF manages itself.

### Compiler warnings

All targets build warnings-as-errors: `-Wall -Wextra -Werror` on Clang/GCC (macOS, Linux, ESP32), `/W4 /WX` on MSVC (Windows), gated by compiler in `CMakeLists.txt`. No warning is "harmless", fix it or silence it explicitly with a `-Wno-…` (Clang/GCC) or `#pragma warning` justified in code.

**A clean local build is not proof the Windows build passes.** MSVC's `/W4` flags things Clang/GCC's `-Wall -Wextra` don't, most commonly **signed/unsigned mismatch in comparisons** (C4389), e.g. `(x & 1) == 1u` where the left side is signed: harmless logically, fatal under `/WX`. This has slipped past the macOS gate and broken Windows CI more than once. So:

- In a comparison, keep both sides the same signedness, don't mix a signed expression with an unsigned literal (`== 1`, not `== 1u`, when the other side is signed). Watch `& `, `%`, and subtraction results, which carry the signedness of their operands.
- A change that only built+passed on macOS/Linux is **not** verified for Windows. The Windows CI job (`release.yml`) is the real gate for MSVC-only warnings; let it run before considering a `src/`-touching change done, or build with MSVC locally if you have it.

Diagnostics as you type come from clangd, using the same `.clang-tidy` CI runs; once-per-machine setup is in [building.md, editor setup](../how-to/building.md#editor-setup-clangd).

### Static checks

- **Hot path check** (`moondeck/check/check_nonblocking.py`): the tick methods carry `MM_NONBLOCKING`, and Clang verifies transitively that nothing they reach allocates or blocks. Each site is a `blocking call on the render path` row in the code report below, so the count may only fall. Each finding stays a judgment: fix it, annotate the callee, or accept it with a scoped reason. An audit must sweep every syscall the path can reach, not the loudest one: a socket timeout is not a fix, it is the size of the freeze. The rule itself: [the architecture](../explanation/architecture/moonmodule.md#hot-path-discipline).
- **The code report** (`moondeck/check/check_code.py`): every function over a complexity, length, nesting or parameter limit, every block of code found elsewhere, and every file over a length limit. It also counts each crossing of the [platform and domain boundaries](../explanation/architecture/mooncore.md#platform-abstraction). Listed per file in `docs/reference/metrics/code.md`, whose committed counts may only fall, per rule and in total; a touched file clears its own rows.

### From a finding to a fix

A finding is a named code smell. Every smell has a named, behavior-preserving move from the refactoring catalog (Fowler, *Refactoring*), the vocabulary every IDE's refactor menu and every quality platform uses. The commit names the move it made, so a reviewer checks that the move was applied rather than whether the result looks simpler, and two people make the same change.

| Smell, what the report counts | The move | Where it lands |
|---|---|---|
| Long function | Extract Function | a named step a reader can skip |
| Deeply nested | Replace Nested Conditional with Guard Clauses; Extract Function on the inner loop | flat flow, one level at a time |
| Complex function: a conditional chain on one value | Replace Conditional with a table | data rows, no branches: a Command table |
| Complex function: a `switch` on a state | Replace Type Code with State: one method per state | the State pattern |
| Duplicated code | Extract Function, Pull Up Method | one home |
| Long parameter list | Introduce Parameter Object | one struct |
| Large file | Extract Class, Move Function | a module per concern |
| Core reaching into light | Move Function, or an interface in core that light implements | the dependency points inward |
| Platform code outside `src/platform` | Move Function into the platform layer; `if constexpr` on `platform_config.h` | a target's facts in one layer |
| A blocking call on the render path | Move the work off the render thread: a worker, a cached value, a deferred apply | the tick only computes |
| A name spread across files | Move Function, self-registration | a concept in one place |
| A module type named in the UI | Replace Conditional with Polymorphism: the module declares a control, a role or a flag | the UI knows no module |
| A stack frame over 512 bytes | Move the large local into a member or a `ScratchBuffer`, sized once | no task's stack pays for it |
| A buffer taken by hand in the light domain | Replace it with a `ScratchBuffer` member | owned, sized, reported and freed in one place |

Cut first: a branch that chooses between equals is data, a guard is an early return, and nesting is what a reader trips on, more than branch count. The theory behind the moves is Ousterhout's: complexity is dependencies and obscurity, so a deep module with a small interface absorbs it and its callers stay simple. A function may stay over a limit when splitting it would hide a sequence that belongs together. It says so in one line, and its count stays rather than being forced.

### When checks run

Which checks run at which lifecycle event is defined once, in the [Commit](../CLAUDE.md#commit), [Merge](../CLAUDE.md#merge) and [Release](../CLAUDE.md#release) tables. One command per check, each with an objective path trigger, so a change runs only the checks it makes applicable. CI runs the same checks on every PR ([.github/workflows/](../.github/workflows/)).

### Tests

- **Placement.** New core logic gets a module (unit) test; a full pipeline gets a scenario test. Inventory and strategy: [testing.md](../reference/testing.md).
- **Interim fixes.** When a per-module interim ships in place of the named core fix (see [CLAUDE.md § Principles](../CLAUDE.md#principles)), its tests assert *behavior* rather than the per-module mechanism. They then survive the later move into core unchanged.

## Debugging and verification

Hard-won discipline for diagnosing hardware and infrastructure failures, distilled from the war stories in [lessons.md](../work/past/lessons.md).

- **Prove the failure is *about* the change before editing code.** When something fails right after a change, re-run it isolated and probe the actual end state. Confirm the artifact under test is the one you built: process uptime, the `build` timestamp, what is bound to the port. A stale process, a loaded machine, or an async-confirmation timeout reads as a regression it isn't.
- **A status/dimension assertion does not prove the pipeline renders.** A correctness test for a mapping or effect asserts the buffer or LUT is non-empty with the expected coverage, beyond the declared dimensions looking right.
- **For a hardware bring-up, "it compiles" tells you almost nothing.** The truth is in the boot log on the actual board. A min-revision trap, a wrong PHY pin, a filename-keyed capability gate, or a Kconfig choice an incremental build silently keeps: each builds clean and fails only on hardware. `rm -rf` the build dir when a sdkconfig choice changes.
- **When test and reality disagree, enumerate what the test abstracts away and make the test transmit the genuine article.** Each closed gap either finds the bug or eliminates a theory with proof (a whole-frame loopback that sends the driver's real frame, not a synthetic pattern). Prove the firmware is *not* the cause with a measurement at each layer before editing code or buying parts.
- **A measurement tool must be faithful to the real client, or it invents and hides bugs.** A one-shot WebSocket probe that gives up on close reports stalls a reconnecting browser never sees, and misses blips it does. Match the client's real behavior.
- **Stop at the first failed fix on a working path.** Revert to the known-good state at attempt two rather than re-engineering a seam that already worked (§ *Anti-stalling* in CLAUDE.md).
- **A generated artifact's ground truth is the rendered output, not a re-derivation.** Verify a doc anchor against the built HTML (`grep id=` the `.html`), not a reimplementation of the slug algorithm; verify an emitted machine instruction against the real toolchain's disassembler before flashing.
- **A cross-boundary fact duplicated in code drifts silently; gate it.** A `docPath` in `main.cpp` pointing at a docs page, a firmware projection mirroring a build dict. The moment a check can resolve it against ground truth, that check is cheaper than the drift.

Moved to [documentation-standards.md § Module pages](documentation-standards.md#module-pages): the two reader surfaces, the `///` rules, and how `docs/moonmodules/` mirrors `src/`. It is a documentation rule rather than a coding one, and it sits beside the prose rules it depends on.

