# hpwit/new-parser (ESPLiveScript2): monthly activity digest

What landed on [hpwit/new-parser](https://github.com/hpwit/new-parser), month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [index.md](index.md).

The library: **ESPLiveScript2**, Yves Bazin's (hpwit) from-scratch C++ rewrite of [ESPLiveScript](https://github.com/hpwit/ESPLiveScript), the same idea (a small C-like language compiled on-device to real Xtensa machine code, no interpreter, so a script runs at near-native speed) reimplemented independently rather than refactored. The library ships inside the repo as `asmparser2/` (PlatformIO name `ESPLiveScript2`, at v1.3.0). Summarized from the local clone (`git log --first-parent origin/main`).

**Repo note:** the repository name is `new-parser`, but the library and its README call it **ESPLiveScript2**, the name to search for. Sibling digest for v1: [hpwit-ESPLiveScript.md](hpwit-ESPLiveScript.md).

## October 2026

The rewrite's heaviest month so far: the language grows toward C, a fuzzer hardens the compiler, and generated code closes in on native speed. The details follow below.

**What script authors gain**

- `switch`/`case`/`default` works as in C: fallthrough, `break`, nested switches, and `#define`d or negative case values. A duplicate case or a second `default` is an error.
- A bare `{ }` block is a statement, and a `SwitchCase` example shows both.
- `ffloat` is a 32-bit fixed-point type written like a float: Q16.16, about plus or minus 32768 in steps of 1/65536, its arithmetic compiled to integer instructions.
- `ffloat` literals and constant subexpressions fold at compile time; multiplying or dividing by an integer, and dividing by a power of two, take fast paths. The README says to watch intermediate values against the range.
- `safe_mode` works, where the v1 port could not even assemble: an indexed store into a global or external array with declared dimensions is bounds-checked. Pointers and unsized arrays stay unchecked.
- A failed check stops the script, records the array, size and index, and reports it on a console line or an `onScriptRuntimeError` hook. It unwinds to the host call, which returns false.
- `pause()`, `restart()`, `kill()` and `state()` control a script run with `executeAsTask()`; a task whose function returns marks itself stopped instead of deleting itself, so no control call acts on a vanished task.
- `char *` arguments work: a `char *` variable, parameter, array or call result can be passed to a function (only string literals matched before). A `StringFunctions` example shows strings in and out of script and host functions.
- `_handle_` (the running executable's address) is declared in every script, so a script compiled through the raw parser path can register callbacks; the `PinInterrupt` example (a GPIO interrupt calling a script function) is ported from v1.
- `%` is signed, as in C (`-7 % 3` gave 0 before), and uninitialized globals start at 0 instead of leftover heap contents.
- GPIO registers can be bound as script variables, so a script sets pins up and toggles them with single stores: the `DirectGPIO` example builds for ESP32 and ESP32-S3 with no address or chip named in the script, and `DirectGPIOSpeed` measures 9.9 MHz on the pin, script and native within half a percent of each other.

**Fixed**

- Reload elimination dropped a load after its base register changed, so `line1[i + 1] + line1[i]` counted one neighbor twice (found writing the `GameOfLife` example).
- `if / else if / else` chains attached the trailing `else` to the first `if`, and the dangling else in `if (a) if (b) x; else y;` attached to the outer statement.
- Ternary expressions: `a < b ? x : y` parsed as `a < (b ? x : y)`, the result register was not reserved (`(c ? 1 : 1) * 6` gave 36), and a store's address load could land inside one branch.
- Call arguments overwritten in many shapes: a call inside an array-element store, a call in an argument to the left of an already placed argument (`f(f(5, 1), 8)` passed 1 for the 8), the fifth and sixth arguments of a nested call, arguments held while later ones compute (now pushed on the stack), a live `a10` temporary around a nested call, `printfln` arguments overwritten by a later argument's call, and register locals loaded through a scratch register that already held an earlier argument.
- A temporary live across a call (`fib(n-1) + fib(n-2)`, `20 + rand(200)`) was never saved while the callee ran.
- Frames reserved 16 bytes above the locals where the register-window overflow handler needs 32, so stack locals were overwritten once a call chain got deeper than the register window.
- Xtensa's short conditional branches have 8-bit offsets for most forms, so any `if` or loop body longer than about 128 bytes could jump into unrelated code. The assembler now relaxes an out-of-reach branch into its inverse over a jump and retries until everything fits. This was the cause of every crashing fuzz seed.
- `==` and `!=` against a constant were inverted in a long `if` body and in a loop's bottom-tested condition: `if (a != 1)` with a long body ran exactly when `a == 1`.
- The first global initializer in a script stored its value before loading its address.
- `++` and `--` on an int struct member inside a method stepped by 4 (taken for a pointer); this affected every script, float and int alike.
- Multi-dimensional indexes used fixed scratch registers, so a call or a 2D read inside an index lost the partial sum.
- Several copy-propagation and dead-copy cases in the optimizer dropped a copy that was read again later, or kept one after its source was overwritten.
- Function records in the binary header: a struct method's record carried a copy of whatever line came next, and per-function compile separated a function's `.global` and `.var` lines (`MultiEffectController` is 265 bytes smaller).

**Faster generated code**

- Hot locals live in registers: a pass promotes a function's most used frame slots to registers the function never names (ints to the call-surviving `a4..a7`, floats to `f3..f12` with saves around calls), weighting loop accesses 8x per nesting level. One 128x96 Mandelbrot frame on an ESP32 at 240 MHz: float 86.8 to 66.9 ms.
- Values that live across a call sit in spare window registers instead of frame slots: `fib(40)` goes from 26.94 s to 21.72 s (native 16.96 s, so 1.28x native instead of 1.59x).
- Multi-dimensional array strides fold into shifts and scaled adds instead of a `movi` plus `mull` per dimension, and leftover register copies fold into their readers.
- A last pass hoists loop-invariant loads (array addresses, constants) out of loops, gives the remaining frame slots any register free over their lines, and tidies by liveness. Game of Life 128x128 over the month: 15.24 to 8.08 ms per generation (v1: 14.67, native 5.95); Mandelbrot float 62.3 ms, `ffloat` 50.0 ms, frames identical to native.
- `ffloat` speedups: consecutive multiplies share one shift-amount register, small constants load as `movi` plus shift, division by an integer is an inline `quos`, division by a power of two is a shift. BouncingBalls with 60 balls: `ffloat` 10.83 to 6.51 ms per frame (float 7.80).
- Loop heads and jump targets get a `nop.n` when their first instruction would straddle a 4-byte fetch boundary.

**Compiler memory**

- The compiler's peak heap is roughly halved: functions compile as soon as they are parsed and their syntax tree is freed, tree nodes come from a pool, assembler lines live in one packed array, and branch relaxation runs before assembling so the text is assembled in one pass. The 730-line `MultiEffectController` script now compiles on an ESP32 without PSRAM (about 207 KB used of 230 KB free); the host model drops from 453 KB to 238 KB and fails above a 256 KB budget.

**Verification**

- A differential fuzzer generates random programs in the subset of the language that is also C, runs each compiled by ESPLiveScript2 under QEMU and as C on the host, compares the traces and reduces a failure to a few lines automatically. Its first run found seven compiler bugs; every reduced program lives in `test/fuzz/regressions/` (39 by month end) and runs as a regression suite.
- A frame-by-frame effects suite compares the procedural effect scripts and the fixed-point Mandelbrot against C versions under QEMU, and a full-system tier builds real sketches with `arduino-cli` and boots them under QEMU's esp32 machine (the `SafeModeTest` sketch covers overruns, negative indexes, recovery and the hook).
- The host run-large test tracks every allocation the compiler makes and models the ESP32's per-block cost.
- Still open per the commits: an intermittent "Cache disabled" panic during the first on-device compile, about one boot in eight, under the full-system test.

**Examples and documentation**

- New examples: `GameOfLife`, `ProceduralEffects` (plasma, fire, Voronoi, mandala and spinning squares on a 128x96 matrix, each verified byte for byte against a C reference, with frame images), `MandelbrotFixedPoint`, `MandelbrotFfloat`, `BouncingBallsFfloat`, `FloatVsFfloat`, `MandelbrotCompare` (native float and fixed against script float and `ffloat`, timed with checksums), `SafeMode`, `SwitchCase`, `StringFunctions`, `PinInterrupt`, `DirectGPIO` and `DirectGPIOSpeed`; 34 in all.
- The README is rewritten in the style of v1's: shorter, example-first, with board measurements in place of QEMU projections.

No issues were opened or closed. No release; the library stays at v1.3.0.

_Checked: `git log --first-parent origin/main` (after `git fetch --all --tags`) for author-date 2026-10-01..2026-10-31 = 37 commits (`b78375f` on 2026-10-11 through `3a8e9da` on 2026-10-31, none before the 11th; every one by Yves Bazin with a Claude co-author trailer); `main` is the only remote branch; releases published (none); issue search `repo:hpwit/new-parser is:issue created:2026-10-01..2026-10-31` and the same with `closed:` (0 results each)._

## September 2026

A short month after August's heavy push: three commits on `main`, all on 2026-09-02 and 2026-09-05, adding built-in dynamic memory for scripts and fixing three compiler bugs found by a new test.

**What script authors gain**

- `alloc()`, `free()`, `writeInt()` and `readInt()` are built into the language, like `printf()`, with no `bindFunction()` needed. The language has no pointer-dereference syntax, so a script holds the address returned by `alloc()` as a `uint32_t` and reads and writes it through these accessors. A new `MemoryAllocator` example shows the pattern.

**Fixed**

- A bare pointer declaration such as `uint16_t *arrayf;` reserved storage for the pointee (2 bytes) instead of the pointer (4 bytes).
- A `for` loop whose variable was declared before the loop (`int i; for (i = 0; ...)`) freed that variable's register when the loop ended, so a later declaration could reuse a register still in use.
- The size estimate for a call to an external function counted one instruction instead of two, which on a short script heavy in external calls could cut off the compiled binary's final return.
- The register-copy optimization could drop the only definition of a register that was read again later, silently losing a stored return value (found with an `alloc()`/`writeInt()`/`readInt()`/`free()` script). The optimizer now runs a single pass, which avoids the case and was confirmed on a real ESP32; a few extra optimizations that only a second pass found are given up.

**Verification**

- A new QEMU regression test exercises the allocator design and found the first three bugs above; the full host suite (40 tests), the sanitizer build and the optimized build all pass.

No issues were opened or closed. No release; the library stays at v1.3.0.

_Checked: `git log --first-parent origin/main` (after `git fetch --all --tags`) for author-date 2026-09-01..2026-09-30 = 3 commits (`9d3d28b`, `1c745d6`, `53671f1`, a documentation and allocator follow-up); `main` is the only remote branch; releases published (none); issue search `repo:hpwit/new-parser is:issue created:2026-09-01..2026-09-30` and the same with `closed:` (0 results each)._

## August 2026

The rewrite's first full month of work, and a heavy one: the compiler goes from a fresh port that mostly parses to something that compiles and correctly runs real scripts on real ESP32 hardware, with the QEMU test suite growing to catch the bugs that only show up when the compiled bytes actually execute.

**What script authors gain**

- `printf()` and `printfln()` are now built into the language: scripts call them directly, with no `bindFunction()` or external declaration needed.
- String literals in scripts actually work. Previously any string literal loaded as all-zero bytes at runtime, so nothing printed. `"\n"` inside a literal now becomes a real line break (CRLF), matching v1.
- Arrays of structs work. `arr[i].field` read the wrong address regardless of `i`, and `arr[i].method()` always called on a fixed target. Both are fixed, so effects that keep a per-item state array (bouncing balls, particles) behave.
- `!=` inside a conditional expression was missing from the parser and now parses.
- `json "path" as type name;` variable binding is ported from v1, alongside typed `Arguments` marshaling for calling into scripts with real int/float values.
- **Breaking change:** a struct definition now requires a closing `;`, like ordinary C and C++: `struct hh {int j,k;};`. The old optional form is gone, and every bundled example and test script is updated.

**Getting a script compiled and run**

- `parseScript()` replaces the hand-written parse/createBinary/createExecutable sequence with one call, returning a `ScriptExecutable` that cleans up after itself. Most examples are rewritten around it.
- `ScriptExecutable::free()` releases a compiled script's memory immediately, so a sketch can run one script, free it, then compile another. There is a `TwoScripts` example for that.
- `parseScriptToBinary()` and `createExecutableFromBuffer()` are the one-call save side and load side: compile once, store the bytes, run them later or on another device. A loaded script that calls `printf` now finds it, which it previously did not in a process that never parsed anything.
- `execute()` now runs a script's top-level init (global struct constructors) before the requested function, matching v1. `executeOnly()` opts out.
- `executeAsTask()` (ESP32 only) runs a script as a FreeRTOS task on a chosen core.
- Diagnostic helpers land for inspecting what was actually generated: hex dumps of a compiled binary, and dumps of live executable memory on target.
- Parse errors report the right line and position again. Three separate bugs were skewing them: stale block-comment tracking, the prelude offset, and auto-declared bindings.

**Crashes fixed on real hardware**

- A script compiled under the Arduino IDE failed to build at all: the optimizer header's include guard was named `__OPTIMIZE__`, which the compiler itself already defines whenever any optimization flag is on, so the header's body was always skipped.
- Removing the last element from an internal vector aborted the program. Reproduced on a real ESP32 with a roughly 20-line script.
- Freshly compiled code was not cache-synced before being run, invisible under emulation and a real crash on silicon.
- Internal vectors grew one element at a time, fragmenting the heap on a non-PSRAM ESP32-S3 badly enough to fail allocation.
- An unbound external left stack garbage that looked like a real function pointer.
- A pointer-stability bug in the syntax tree caused crashes that depended on heap layout.

**Faster generated code**

- Array indexing uses the Xtensa scaled-add instructions (`addx2`/`addx4`/`addx8`/`subx8`) instead of a loop of repeated adds, for element sizes 2, 3, 4, 5, 7 and 9. Applied across all the indexing paths: local, global, and external, read and write.
- Integer multiply by 3, 5 or 9 becomes a single scaled add; multiply by 2 becomes add-self.
- The optimizer gains redundant-reload elimination across more registers, dead return-instruction removal, and register-copy propagation for plain `mov`.
- A late-found optimizer bug: a function that computed a return value, passed it to another call, then returned it could silently return garbage. Fixed.

**Verification**

- The QEMU suite runs against ESP32-S3 as well as plain ESP32, with identical results, and grows to 12 cases executing the actual compiled bytes.
- All 21 real-world example scripts from the v1 repo are checked through the full compile-and-load pipeline; 16 pass, and 5 real compiler bugs were fixed in the process. The other 5 are broken in v1 too.
- A purpose-written 415-line script exercises the compiler at a size closer to a real project, and is checked against a documented ESP32-without-PSRAM budget (32 KB instructions, 96 KB data).
- Timing is measured rather than guessed: real Xtensa cycle counts around a compiled recursive `fib()` at two depths, agreeing at roughly 6 to 6.5 cycles per call, projecting `fib(40)` to about 8 to 9 seconds at 240 MHz.
- Sanitizer and optimized-build test targets were added, the first of which found 26 of 36 host tests were crashing invisibly.
- A timing harness for a full LED-matrix script is committed but unfinished: no run has completed a single 128x96 frame under emulation, so it is deliberately not part of the suite.

**Examples and packaging**

- 13 plain ESP-IDF ports of the Arduino examples, each an independent `idf.py` project, all built end-to-end against a real ESP-IDF tree.
- New examples: SimpleScript, ScriptPrintf, Factorial, FibonacciTiming, PrintFibonacciAssembly (prints the generated Xtensa assembly before running it), MultiEffectController, ExecuteAsTask, TwoScripts, PrintBinaryHex.
- The README is rewritten in v1's first-person style, documenting v2's real API and carrying the known limitations forward honestly: no multi-task scheduler, `import` does not work, no built-in `hsv()`/FastLED integration.

_Checked: 38 commits author-dated 2026-08-01..2026-08-31 on `main` (0b6aa6b..d84a389); issues created 2026-08-01..2026-08-31 (0) and closed in the same window (0); no releases or tags published in the repo, so no versioned release in August 2026 and no month split._

## Timeline note (added 2026-08-06)

Added to the digest set on 2026-08-06, after the product owner flagged the rewrite. History to date, from the commit log: created March 2025, six commits across March–May 2025, then **dormant for over a year**, then **12 commits in the first days of August 2026**, the rewrite as it now stands is days old at the time of writing. July 2026 is therefore empty, and the August work is summarized above.

What the rewrite is, from its README (context for future months, not an endorsement):

- A **verifiable** compiler is the stated reason for rewriting rather than refactoring: the whole toolchain (tokenizer, parser, assembler, loader) builds and runs as an ordinary host program with no ESP32 or Arduino framework involved.
- Its tests run the **actual compiled bytes** on a real Xtensa CPU emulator (QEMU, Espressif's ESP32/ESP32-S3 machine models) and check real results, and every example script from v1's own corpus is compiled and checked against that pipeline.
- Day-to-day script authoring is said to be unchanged from v1; the differences are in the implementation and its testability.

## July 2026

No activity: no commits on `main` in July 2026, and no issues. (The repo was dormant between May 2025 and August 2026, the current rewrite work begins 2026-08-01, outside this window.)

_Checked: commits author-dated 2026-07-01..2026-07-31 on `main` (0); issues created 2026-07-01..2026-07-31 (0) and closed in the same window (0); no versioned release published in July 2026._
