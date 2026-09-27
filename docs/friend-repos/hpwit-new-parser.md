# hpwit/new-parser (ESPLiveScript2): monthly activity digest

What landed on [hpwit/new-parser](https://github.com/hpwit/new-parser), month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

The library: **ESPLiveScript2**, Yves Bazin's (hpwit) from-scratch C++ rewrite of [ESPLiveScript](https://github.com/hpwit/ESPLiveScript), the same idea (a small C-like language compiled on-device to real Xtensa machine code, no interpreter, so a script runs at near-native speed) reimplemented independently rather than refactored. The library ships inside the repo as `asmparser2/` (PlatformIO name `ESPLiveScript2`, at v1.3.0). Summarised via the GitHub commits API.

**Repo note:** the repository name is `new-parser`, but the library and its README call it **ESPLiveScript2**, the name to search for. Sibling digest for v1: [hpwit-ESPLiveScript.md](hpwit-ESPLiveScript.md).

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

Added to the digest set on 2026-08-06, after the product owner flagged the rewrite. History to date, from the commit log: created March 2025, six commits across March–May 2025, then **dormant for over a year**, then **12 commits in the first days of August 2026**, the rewrite as it now stands is days old at the time of writing. July 2026 is therefore empty, and the August work is summarised above.

What the rewrite is, from its README (context for future months, not an endorsement):

- A **verifiable** compiler is the stated reason for rewriting rather than refactoring: the whole toolchain (tokenizer, parser, assembler, loader) builds and runs as an ordinary host program with no ESP32 or Arduino framework involved.
- Its tests run the **actual compiled bytes** on a real Xtensa CPU emulator (QEMU, Espressif's ESP32/ESP32-S3 machine models) and check real results, and every example script from v1's own corpus is compiled and checked against that pipeline.
- Day-to-day script authoring is said to be unchanged from v1; the differences are in the implementation and its testability.

## July 2026

No activity: no commits on `main` in July 2026, and no issues. (The repo was dormant between May 2025 and August 2026, the current rewrite work begins 2026-08-01, outside this window.)

_Checked: commits author-dated 2026-07-01..2026-07-31 on `main` (0); issues created 2026-07-01..2026-07-31 (0) and closed in the same window (0); no versioned release published in July 2026._
