# MoonLive — live-script engine landscape analysis

> **Forward-looking research document — exception to CLAUDE.md present-tense rule.** This is a Stage-1 bottom-up survey of *live scripting* for MoonLight: running user-authored scripts (LED effects, layouts, modifiers, drivers, sensor logic) on a running device without a recompile-and-flash cycle. It deep-reads one reference implementation — the [ewowi/ESPLiveScript `fix-warnings` fork](https://github.com/ewowi/ESPLiveScript/tree/fix-warnings) of [hpwit/ESPLiveScript](https://github.com/hpwit/ESPLiveScript) — at HEAD on **2026-06-25**, surveys the comparable field (WLED ARTI-FX, embedded VMs, WASM), and extracts the architectural primitives a clean MoonLight redesign must decide. Companion to the monthly digest [friend-repos/hpwit-ESPLiveScript.md](../../friend-repos/hpwit-ESPLiveScript.md) (credits + activity log). The **top-down** redesign document ([livescripts-analysis-top-down.md](livescripts-analysis-top-down.md)) expands the decisions recorded here into the build spec. Source citations use `file:line` against the cloned fork; inferred claims are marked *(inferred)*. Modelled on [leddriver-analysis-bottom-up.md](../../work/future/leddriver-analysis-bottom-up.md).

## TL;DR

- **What live scripting is, and why MoonLight wants it.** The same itch that produced WLED ARTI-FX and ESPLiveScript: author an effect (or layout, modifier, driver, sensor rule) *as text*, push it to a running device, see it run **on the next tick** — no toolchain, no flash, no reboot. It turns MoonLight from "the effects we compiled in" into "any effect a user can write," and it's the natural home for a web-based pattern editor.
- **The design space has three corners, set by *how* a script becomes execution.** (1) **AST-walking interpreter** — parse to a tree, walk it every frame (WLED **ARTI-FX**: everything stored as `double`, flexible, slow). (2) **Bytecode VM** — compile to a compact opcode stream, run a dispatch loop (**PixelBlaze**, most embedded Lua/JS). (3) **Native JIT** — emit real machine code, call it as a function (**ESPLiveScript**). Speed climbs (1)→(3); portability and simplicity fall (1)→(3). MoonLight's "blazingly fast like ESPLiveScript" requirement points at (3), but (3) is exactly where portability dies — see below.
- **ESPLiveScript is a from-scratch C-like compiler that JIT-emits native Xtensa machine code** (confirmed, not bytecode): `tokenizer.h` → `asm_parser.h` (AST of `NodeToken`) → visitor methods emit Xtensa assembly *strings* → `asm_parser_LMbin.h` encodes them to 32-bit opcodes → the binary is copied to executable RAM (`heap_caps_malloc(..., MALLOC_CAP_EXEC)`) and **called as a function pointer** via inline `callx8` (`execute_asm.h:386-399`). That direct-call, no-dispatch-loop design is the entire "85 fps C++ vs 10 fps Gravity vs 3 fps Lua" speed story from its README.
- **The portability finding that reshapes everything: ESPLiveScript is Xtensa-only.** The agent deep-read confirmed **no RISC-V backend** — all codegen is Tensilica Xtensa LX (`asm_parser_LMbin.h`, the inline-asm executor). This matters enormously for MoonLight's target order: classic ESP32 + S3 are Xtensa (✅ ESPLiveScript runs), but **ESP32-P4 is RISC-V** (❌), as are Teensy 4.x (ARM Cortex-M7) and desktop (x86-64 / ARM64). So ESPLiveScript as-is covers exactly the *first two* targets on MoonLight's list and **none** of the rest. A native-codegen engine needs **one backend per ISA** (Xtensa, RISC-V, ARM Thumb-2, x86-64, ARM64) — that's the real cost of "blazingly fast everywhere."
- **The front-end is portable; the back-end is not.** Tokenizer + parser + AST (`NodeToken`) are CPU-agnostic; only the *visitor → opcode* tier and the *load-and-execute* tier are ISA-bound. But today they're **deeply interleaved** — visitor methods emit Xtensa strings inline, there is **no intermediate representation (IR)** between AST and machine code. A clean redesign's load-bearing decision is whether to introduce that IR seam so one front-end feeds many back-ends (the LLVM shape, scaled down).
- **The "compatible with MoonModule" requirement is the MoonLight-specific value-add.** ESPLiveScript binds to the host via `addExternalFunction(name, ret, sig, fnptr)` / `addExternalVariable(name, type, _, ptr)` (`asm_external.h`) — a flat C-pointer registry. MoonLight needs scripts to read/write **controls**, consume the **producer/consumer data structures** (a `Buffer`, an `AudioFrame`), and slot into the **module tree** as a scripted effect/layout/modifier/driver/peripheral. That binding layer — script ⇄ MoonModule — is ours to design; no surveyed engine has it.
- **Memory + sync are already partly modelled in ESPLiveScript** and align with MoonLight's constraints: compiled code lands in IRAM/PSRAM by target (`execute.h:10-15` gates PSRAM stack on S3/P4), a **save/load compiled-binary path** exists (`savebinary`/`executebinary` examples → compile once, ship the binary, skip re-compile on boot), and a `sync()` primitive coordinates concurrent script tasks. These are the right *ideas*; the redesign carries them forward against our `platform::` seam and `Scheduler`.
- **⚠️ Superseded upstream (noted 2026-08-06): hpwit has rewritten it as [ESPLiveScript2](https://github.com/hpwit/new-parser).** A from-scratch C++ reimplementation whose stated goal is precisely the gap this analysis identified below — a compiler you can *verify*: the whole toolchain builds and runs as a host program, and its tests execute the actual compiled Xtensa bytes under QEMU against v1's own example corpus. The rewrite landed in the first days of August 2026 (the repo was dormant May 2025 → August 2026), so this document's reading of v1 stands as written but is no longer a reading of hpwit's *current* work. **Before Stage 2 acts on any v1 conclusion, re-read v2** — the portability finding in particular (is codegen still Xtensa-only, or did the rewrite introduce the IR seam we concluded was missing?). Digest: [friend-repos/hpwit-new-parser.md](../../friend-repos/hpwit-new-parser.md).
- **Code-quality reality (for the redesign).** Header-only, ~18K lines across 11 headers, **pervasive global state** (`string signature; Token __t;` and dozens of file-scope compiler counters), no IR, no unit tests, a 4,100-line `Parser` and a 5,824-line `NodeToken`. It works and it's fast, but it is **not** a base to extend in place — it's the reference to learn from and rewrite against our architecture (exactly the *Industry standards, our own code* method we used for LED drivers).
- **Recommendation: build our own native engine, Xtensa-first, behind an IR seam — start small, start beautiful, no dead-ends.** Take the ESPLiveScript *approach* (native machine-code execution, near-100% speed — the standout, never-done-before-in-this-space when bound to a module system) and add the one thing our multi-target goal needs that a single-ISA engine doesn't: put an **IR seam** between a platform-independent front-end (tokenizer→parser→AST) and the code generator. **Ship one backend first — Xtensa (classic ESP32 + S3)** — exactly where ESPLiveScript already proves native speed; that's the small, beautiful, blazingly-fast first deliverable. The IR seam is the **no-dead-end guarantee**: adding RISC-V (P4), ARM (Teensy), or x86/ARM64 (desktop) later is "write another backend behind the same IR," never "go back to the drawing board." ESPLiveScript's real dead-end isn't *Xtensa-first* — it's *Xtensa-welded-in, no IR*; we start at the same fast place but with the seam it lacks. **WASM/WAMR is the named fallback, per target**: a target without a native backend yet can run the portable path through the same IR, so we're never blocked — but the *flagship* experience is native. (Detail + why-this-over-WASM-wholesale in § Recommendation.)
- **Safety the same way — climb the tiers, don't pay upfront.** A user-facing script editor means a bad script must degrade, not brick. Start with the **cheap** safety (array **bounds-checking** = a compare-branch per indexed access, low single-digit %, and removable in a trusted/fast mode; **watchdog / instruction budget** to kill a runaway loop = near-free, the task WDT already does most of it) — these catch the common bad-script cases at low cost (the kind the `fix-warnings` null-deref was). The **expensive** tier — a true memory sandbox where a script *cannot* touch memory outside its arena — is exactly what WASM gives for free and native can't cheaply; leave it as a tier we *can* climb via the IR→WASM fallback if field experience demands it, not a wall we hit. So safety is staged, not a foregone full-sandbox cost.
- **Ruled out (with reasons, so the top-down doesn't relitigate).** **FastLED's WASM** is browser *preview* (Emscripten-compiled FastLED in a Web Worker), not on-device scripting — adjacent, not it. **MicroPython/Python** is the right *edit* ergonomics but far too slow for the hot path (the Lua/Gravity wall that birthed ESPLiveScript). **Rust/TinyGo** are near-native but **AOT-compiled-and-flashed** — they remove no edit loop. **Adopting WASM/WAMR *wholesale* as the only engine** is the considered alternative, not a dead end — it wins portability + free sandbox but tops out at ~50% native (WAMR-AOT) and carries a 200KB+ runtime; we keep it as the per-target fallback rather than the flagship, because *native speed is the differentiator we're chasing*.
- **Out of scope for Stage 1.** Final VM-vs-JIT decision; IR design; the web editor; benchmarking on real hardware; the language grammar spec. All Stage 2 (top-down).

## Why this document exists

MoonLight compiles its effects, layouts, modifiers, and drivers into the firmware. Adding one means writing C++, building, and flashing. **Live scripting removes that loop**: a user writes an effect as text in a browser, the device compiles/loads it, and it runs as a first-class module on the next tick — the same leap WLED took with ARTI-FX.

The product owner's requirements (verbatim intent):

- **General in core, specific in light.** Scripts must work for domain-neutral core jobs (e.g. read/transform sensor data) *and* the light domain: a scripted **layout** (coordinate iterator), **effect** (writes the buffer), **modifier** (remaps positions), **driver** (consumes the buffer). One engine, many module roles.
- **Target order.** ESP32 classic + S3 **first**; then P4 and other ESP32 flavours; then Teensy; then macOS / Linux / Windows.
- **Blazingly fast, like ESPLiveScript** — near-native per-pixel throughput, because a script runs in the render hot path at up to 16K+ lights × 50 FPS.
- **Smart memory management** — IRAM/PSRAM-aware, no hot-path allocation, compile-once/run-many.
- **Infinitely scalable** — run *as many* live scripts concurrently as memory allows, exploiting PSRAM (each script is an independent compiled unit; the only ceiling is free heap, not a fixed slot count). Many small scripted modules — several effects across layers, a scripted modifier, a couple of core sensor rules — coexist; the device hosts what fits and degrades gracefully when it doesn't, the same way the light pipeline already scales to available memory.
- **Sync with the rest of the system** — live reconfig (no reboot), `Scheduler`-driven, robust to add/delete/replace in any order, controls and producer/consumer data wired in.
- **Compatible with the MoonModule class** — a scripted module is a MoonModule: it has controls, a `loop()`, a role, and renders generically in the UI with zero per-script UI code.

This document characterises the one reference that already hits "blazingly fast" (ESPLiveScript), maps it against the field, and surfaces every decision the redesign must make. It does **not** pick the design — that's the top-down doc.

## ESP32 — primary depth: ESPLiveScript (the reference)

Read at `ewowi/ESPLiveScript@fix-warnings`, cloned 2026-06-25. ~18,358 lines across 11 header-only files in `src/`.

### What it is, in one sentence

A hand-written **C-like language with a from-scratch tokenizer → parser → AST → native-Xtensa code generator**, where the compiled script is loaded into executable RAM and **called directly as a function** — a JIT compiler, not an interpreter. Yves Bazin (hpwit) built it because Lua (3 fps) and Gravity (10 fps) couldn't drive his 12,288-LED panel where hand-written C++ hit 85 fps (README intro).

### The pipeline (the load-bearing structure)

Source text flows through five stages; the data structure between them is `Token` → `NodeToken` (the AST node) → assembly text → binary:

1. **Tokenize** — `src/tokenizer.h` (2,394 lines). Lexes source into `Token`s with a `tokenType` enum. Also owns the user-defined-type registry (`_userDefinedTypes`, global).
2. **Parse → AST** — `src/asm_parser.h` (1,929 lines) builds a tree of `NodeToken` (`src/NodeToken.h`, 5,824 lines). `NodeToken`'s `nodeType` enum has ~47 kinds (`binOpNode`, `defFunctionNode`, `forNode`, `ifNode`, `callFunctionNode`, `returnNode`, `defAsmFunctionNode`, …). A for-loop becomes a `forNode` with init/cond/incr/body children.
3. **Generate code (visitor)** — `NodeToken::visitNode()` (`NodeToken.h:818-1001`) dispatches to `_visitbinOpNode()`, `_visitcallFunctionNode()`, etc., each of which **emits Xtensa assembly strings** into output buffers (e.g. `NodeToken.h:1858` emits `movi a%d,%d`). There is **no IR** — visitors know Xtensa directly.
4. **Encode to binary** — `src/asm_parser_LMbin.h` (592 lines) turns assembly text into 32-bit Xtensa opcodes (`bin_add`, `bin_l32i`, `bin_movi`; e.g. `bin_add` at `:97-100` emits `0x800000 | …`).
5. **Load + execute** — `src/execute_asm.h` (876 lines). `_createExcutablefromBinary()` (`:224-275`) copies the binary into executable RAM with `heap_caps_malloc(size, MALLOC_CAP_EXEC)` (`:232`), patches external references (relocation, `:44-223`), and `executeBinaryAsm()` (`:386-399`) runs it via inline asm: `l32i a15,%0,0 ; callx8 a15` — **a direct indirect call to the generated code.** No dispatch loop. That is the speed.

### Why it's blazingly fast (confirmed)

It is **native machine code called as a function** — the CPU fetches the script's own instructions from IRAM, exactly like a compiled C function. There is no per-opcode interpreter overhead (the cost a bytecode VM or AST-walker pays every operation). The README's benchmark (85 fps C++ ≈ ESPLiveScript ≫ 10 fps Gravity ≫ 3 fps Lua) is the direct consequence. Scripts can even drop to **inline Xtensa** for the hottest paths (`__ASM__ uint32_t millis() { "entry a1,32" … "retw.n" }`, `sc_examples/animwle.sc`).

### What ties it to ESP32 — the portability barrier

The agent's deep-read is unambiguous: **Xtensa-only, ESP-IDF-coupled.**

- **Codegen is 100% Xtensa LX** (`asm_parser_LMbin.h`, all visitor emission). **No RISC-V, no ARM, no x86 backend exists.** Only ~1 arch `#ifdef` in the codegen — and it's a *memory-caps* choice (S3/P4 PSRAM stack vs classic internal, `execute.h:10-15`), not a second ISA. (Note the irony: that one `#ifdef` *mentions* ESP32-P4, but only for stack allocation — P4 is RISC-V, so the **generated code wouldn't run on it.** *(inferred from "Xtensa-only codegen" + P4 being RISC-V)*.)
- **Execution is ESP-IDF-specific**: `MALLOC_CAP_EXEC` IRAM allocation, inline `callx8`, `rsr a14,234` cycle-counter reads, `xTaskCreatePinnedToCoreWithCaps` (`execute.h:590`).
- **Front-end is portable, back-end is not**: tokenizer + parser + AST are CPU-agnostic; tiers 2-4 (visit→opcode, encode, load-execute) are ISA/platform-bound and **interleaved** with no seam between them.

So on MoonLight's target list, ESPLiveScript as-is runs on **classic ESP32 + S3** and stops there. P4 (RISC-V), Teensy (ARM), and desktop (x86-64/ARM64) each need a *new code generator* — 2-3K lines per ISA *(inferred, agent estimate)* — or a different execution strategy entirely.

### Host integration — the binding model

A host C program drives it through `class Parser` (`ESPLiveScript.h:79`):

- **Compile + run**: `parseScript(&str)` → `Executable`; `Executable::execute("fn", args)` runs a function; `executeAsTask("fn", core, args)` runs it pinned to a FreeRTOS core; `suspend()/restart()/kill()/free()` manage its lifetime (`execute.h:352+`).
- **Expose a C function to scripts**: `addExternalFunction("calc","float","int",(void*)calcul)` → script calls `float h = calc(52);` (`asm_external.h`, README example).
- **Expose a C variable**: `addExternalVariable("value","int","",(void*)&v)` and `("array","int *","",(void*)arr)` → script does `value = value + 2; array[i] = 10;`.
- **Arguments**: `int` and `float` only (`ESPLivescriptRuntime.h:150-176`).
- **JSON path** (`__JSON__OPTION__`): scripts exchange JSON with the host (`execute_asm.h:400-471`) — the `enjoy json` feature from the digest; the bridge a web editor would lean on.
- **Precompiled binaries**: `parseScriptBinary()` → `saveBinary()/loadBinary()` → `createExecutableFromBinary()` (`execute_asm.h:276-384`). Compile once (on a desktop or web service), persist the `ESPLiveScript1.0.1`-format binary, load it on the device — but **external pointers are not serialized**, they re-bind at load (README). This is the seed of a smart compile-once memory strategy.

This binding is a **flat C-pointer registry** — exactly what MoonLight must *replace* with a MoonModule-aware layer (controls, producer/consumer structs, the module tree).

### Memory model

- Generated code → executable RAM via `MALLOC_CAP_EXEC` (IRAM on classic; PSRAM-capable on S3/P4 via `__LS_STACK_CAPS`, `execute.h:10-15`).
- Script globals → a malloc'd `data` buffer; locals → the Xtensa ABI stack frame (`entry a1, size`); params → registers a2-a7.
- Precompiled-binary persistence (above) = compile-once.
- `Executable::free()` releases both code and data.
- No PSRAM is *forced* for the data section; that's a knob the redesign would make policy.

### The language (what a user writes)

C-like, LED-oriented. From `sc_examples/*.sc` + README:

- **Types**: `int`/`s_int` (32/16-bit), `uint8_t..uint32_t`, `float`, `char`, `bool`, plus **`CRGB`/`CRGBW`** (LED color) as first-class; user `struct`s with fields, methods, constructors; multi-dimensional arrays (`int g[z][y][x]`).
- **Control flow**: `if/else`, ternary, `while`, C-style `for`, `break`/`continue`, `return`; recursion (`sc_examples/fibonacci.sc`).
- **Built-ins**: `printf`/`printfln` (int only), `millis()`, `rand`/`copy`/`memset`/`fill` (inline-asm in `functionlib.h`), `hsv()` and FastLED math when `USE_FASTLED` is set.
- **Escape hatch**: `__ASM__ … @` blocks for hand-written Xtensa.
- **Preprocessor**: `#define TOKEN value` (substitution only).
- **Flavour example** — `sc_examples/animwle.sc` is a **Mandelbrot effect ported from an existing pattern** (`#define width 128`, float `cR/cI`, nested grid loops, inline-asm `millis()`), which tells you the target audience: people porting effects from other LED-scripting environments.

### What the `fix-warnings` fork changed

Despite the branch name, the fork's substantive change is **one commit (`4871509`, 2026-04-02): a null-pointer crash fix**, not a `-Wall` cleanup. `findMaxArgumentSize()` in `NodeToken.h` dereferenced `getChildAtPos(1)`/`(2)` unconditionally; a **nested external-function argument** (`setRGB(random16(NUM_LEDS), CRGB(0,0,255))`) produces a node without those children → `LoadProhibited` crash on device. The fix adds null guards (return 0 — a scalar needs no pre-call stack spill). *Relevance to MoonLight*: this is precisely the class of bug a from-scratch hand-written parser breeds (no test harness caught it), and a data point for "rewrite with tests" over "extend in place."

### Structural observations (what a multi-target rewrite must account for — not a verdict on the original)

These are the differences between ESPLiveScript's design (one author, one ISA, maximum speed) and what MoonLight's *different* goals (multi-target, module-bound, tested) need. They are reasons to write our own against our architecture, not faults — ESPLiveScript meets its own goals well.

- **Header-only**, ~18K lines, 11 files; the two biggest (`NodeToken.h` 5,824, `ESPLiveScript.h` 4,100) carry several jobs each (tree + metadata + 47 visitors + asm emission). Fine for a single-include library; we'd split for testability.
- **File-scope state**: `string signature; Token __t;` (`ESPLiveScript.h:29-30`), plus global register-allocation stacks, output buffers, compiler counters. A consequence: one compilation at a time. Acceptable on a device that compiles one script; we'd encapsulate it.
- **No IR** between AST and Xtensa — the one structural thing that makes multi-target hard, and the single highest-leverage change our redesign makes. (ESPLiveScript didn't need it — it targets one ISA.)
- **`.ino` integration examples, no unit suite** — natural for an Arduino library; we'd add unit + scenario tests because robustness is pinned by tests here.

The lesson is the LED-driver lesson: **study it hard, credit it (see § Prior art & credits), write our own against our architecture and goals.**

## The comparable field (what else to learn from)

MoonLight's "industry standards" rule says: name the prior art, take the textbook approach. The live-scripting field has three established design points; ESPLiveScript is the extreme of one.

### WLED ARTI-FX — the AST-walking interpreter (our sibling project)

[ARTI-FX](https://mm.kno.wled.ge/moonmodules/arti-fx/) (MoonModules, by ewowi — the author of this analysis — built on the **ARTI** runtime, a PEG-grammar-driven interpreter) parses a script and **walks the AST every frame**. Every value is stored as a **`double`**, converted to int when needed; scripts define `renderFrame` (per-frame) + `renderLed` (per-LED) callbacks and call `setPixelColor`/`setRange`/`fill`. Host binding is `arti_external_function` / `arti_set/get_external_variable` (the same flat-registry shape as ESPLiveScript). **What it contributes:** it proved live scripting works end-to-end in this ecosystem, and its design is maximally flexible and portable — pure C++ tree-walking, runs anywhere unchanged. The `double`-everything per-LED walk trades per-frame speed for that flexibility, which is the trade MoonLight's 16K hot path can't take (and exactly the gap ESPLiveScript's native path closes). So the two are complementary baselines: ARTI-FX is the **reach** baseline (runs everywhere, the product shape proven), ESPLiveScript is the **speed** baseline (native, Xtensa) — and MoonLight wants both, which is why neither alone is the answer.

### WASM on ESP32 — the strongest off-the-shelf portable-runtime candidate (answering "is WASM what we want?")

WebAssembly is a portable **bytecode standard** with mature small runtimes that **already run on the ESP32**, which makes it the most serious "don't build the engine, adopt one" option for the multi-target problem ESPLiveScript can't solve. Two runtimes matter:

- **wasm3** — an ultra-light **interpreter** in C (~64 KB code, ~10 KB RAM), runs Arduino-class MCUs upward. Pure interpretation, so *slower* (the per-opcode dispatch cost, same class as a bytecode VM) — fine for control logic, questionable for a 16K-pixel inner loop.
- **WAMR (WebAssembly Micro Runtime)** — supports interpreter **and AOT/JIT**; in **AOT mode WAMR reaches ~50% of native speed**, "quite acceptable" for embedded use, at a larger footprint. Rule of thumb from the field: **RAM < 256 KB → wasm3, > 256 KB → WAMR**; the classic ESP32's 320 KB+ puts it in WAMR's range. ([arXiv survey](https://arxiv.org/html/2512.00035v1), [WAMR-ESP32](https://registry.platformio.org/libraries/mlaass/WAMR-ESP32))

**Why this is genuinely relevant to MoonLight**, and arguably *the* answer to "runs everywhere":

- **One artifact, every target.** A script compiled to `.wasm` runs on classic/S3/P4/Teensy/desktop through the same runtime — no per-ISA backend, which is exactly ESPLiveScript's missing piece. WASM **is** the portable IR + VM, off the shelf.
- **WAMR-AOT is the "blazingly fast" bridge.** AOT-compiling the `.wasm` to native on the device (or on a host) gets ~50% of native — between a naive bytecode VM and ESPLiveScript's near-100%. That's the same "portable baseline + native acceleration" shape the bottom-up proposes, but **already built and multi-ISA** (WAMR's AOT backends cover Xtensa, RISC-V, ARM, x86).
- **Sandboxed by design.** WASM is memory-safe and bounds-checked — a runaway/bad script traps instead of bricking the device (the safety story ESPLiveScript's native code lacks; recall the `fix-warnings` null-deref).
- **Mature toolchain + editor path.** Any language that targets WASM (C/C++, Rust, AssemblyScript ≈ TypeScript) becomes a script source; the browser already runs WASM natively, so a web editor could compile *and preview* the exact artifact the device runs.

**The costs / open questions** (the top-down must weigh): the runtime is **heavier** than a hand-rolled VM (WAMR is 200 KB+; wasm3 is light but interpreter-only); the **host-binding** for WASM (imports/exports, linear-memory marshalling of a `Buffer`/`AudioFrame`) is more ceremony than ESPLiveScript's flat pointer registry and must be designed against the MoonModule data model; the **toolchain** (a WASM compiler in the editor path) is a real dependency; and **whether WAMR-AOT actually holds 16K×50FPS** on an S3 is the load-bearing benchmark. But as a way to get *every target on day one with sandboxing for free*, WASM is the candidate to beat — the top-down should evaluate "WAMR as the engine" head-to-head against "our own VM+IR."

### FastLED's WASM — adjacent but **not** what we want

FastLED's `master` WASM support (`src/platforms/wasm/`) compiles **FastLED itself to WebAssembly via Emscripten to run in a *browser*** — a **simulation/preview** of effects, not a runtime that runs user scripts *on the ESP32*. It runs FastLED in a Web Worker (`PROXY_TO_PTHREAD`), bridges C++↔JS via `EMSCRIPTEN_KEEPALIVE` exports (`js_bindings.cpp`), and exports frame/strip/UI data as JSON for the page to draw. FastLED's `FxEngine` is likewise a *compiled* effect manager (switch/transition between C++ effects), not an on-device scripting language. So FastLED gives MoonLight **two adjacent ideas, neither the live-script engine**: (1) "compile your effect library to WASM to **preview it in the browser**" — a preview technique that sits next to MoonLight's own 3D WebGL preview, not the scripting engine; (2) precedent that the WASM toolchain is production-ready. It does **not** answer the on-device live-scripting need. ([FastLED wasm platform](https://github.com/FastLED/FastLED/tree/master/src/platforms/wasm))

### Compiled languages (Rust / TinyGo) and interpreted Python — why neither is the answer (answering the Rust/Python question)

- **Python / MicroPython is too slow for the hot path** — the benchmark literature is consistent: MicroPython is *"many times slower"* than C/Rust/TinyGo on ESP32 ([MDPI study](https://www.mdpi.com/2079-9292/12/1/143)). It's the same wall hpwit hit (Lua 3 fps, Gravity 10 fps) that *caused* ESPLiveScript. So "Python is an interpreter, so live-editing is easy" is true for the *edit loop* but fails the *speed* requirement at 16K×50FPS. Its real value is the **REPL/editor UX** reference, not the engine. (For non-hot-path *core* scripts — a slow sensor rule at 1 Hz — an interpreter's speed is irrelevant and Python-class ergonomics would be fine; this argues again for a **tiered** answer: cheap interpreter acceptable off the hot path, fast path needs VM/native.)
- **Rust / TinyGo are fast but *compiled*, not interpreted** — Rust-on-ESP32 (`esp-hal`, `no_std`) and TinyGo land near C speed, but they are **AOT-compiled and flashed** — they need a toolchain and a reflash, which is *exactly the loop live scripting exists to remove*. They give no live-edit story on their own. (Rust *does* become relevant via WASM: Rust → `.wasm` → WAMR is a legitimate script-authoring path, but then the engine is WASM, not Rust-on-device.)
- **Net:** interpreted-Python solves edit-speed but not run-speed; compiled-Rust solves run-speed but not edit-speed. The only options that give **both** live edit *and* hot-path speed are (a) a custom VM/native engine (ESPLiveScript's path, our redesign) or (b) **WASM+WAMR-AOT** (portable, sandboxed, ~50% native). Those two are the real finalists.

### Other embedded VMs / JIT libraries (textbook back-ends)

- **Lua / eLua / Luau** — the canonical embeddable scripting VM (register-based bytecode); the reference for a clean host C API and a GC'd value model. Too slow per-pixel raw (hpwit measured 3 fps), but the *architecture* (compile → bytecode → register VM) is the textbook.
- **Espruino** — JS on MCUs; full-language, GC-paused; an editor/REPL UX reference, the "too much" end for a per-pixel hot path.
- **LLVM / MIR / GNU lightning** — real JIT libraries with multi-ISA back-ends. LLVM is far too big for an MCU; **MIR** and **GNU lightning** are lightweight JITs that *do* target multiple ISAs and are the closest prior art to "one front-end, many native back-ends" if we go the custom-native-multi-target route (the alternative to adopting WASM).

### The design-space map

| Approach | Example | Speed (per-pixel) | Portability | Runtime size | Sandbox | Editor-friendliness |
|---|---|---|---|---|---|---|
| AST-walk interpreter | WLED **ARTI-FX** | Low (double-everything) | **Highest** (pure C++) | Small | Easy | High |
| Bytecode VM | Lua, AssemblyScript | Medium-High | **High** (one VM, any CPU) | Small-Medium | Easy | High |
| **WASM interpreter** | **wasm3** | Medium | **Highest** (standard) | ~64 KB | **Built-in** | High (any→wasm) |
| **WASM AOT/JIT** | **WAMR** | **~50% native** | **Highest** (standard, multi-ISA) | 200 KB+ | **Built-in** | High (any→wasm) |
| Native JIT (custom) | **ESPLiveScript** | **Highest** (native) | **Lowest** (one backend/ISA) | Medium | None (can crash) | Medium |

MoonLight wants ESPLiveScript's **speed** *and* ARTI-FX's **reach** — no single *custom* corner gives both, which is why one redesign path is a **layered custom** engine (portable VM baseline + optional native back-end behind a shared IR). But **WASM+WAMR-AOT collapses that table into one row**: portable to every target *and* ~50% native *and* sandboxed, off the shelf. The two real finalists for the top-down are therefore **(A) build our own VM+IR (+ optional native backend)** vs **(B) adopt WASM/WAMR as the engine** — weighed on hot-path speed (does WAMR-AOT hold 16K×50FPS?), runtime footprint, and how cleanly each binds to the MoonModule data model.

## Architectural primitives observed (the decisions the redesign must make)

Distilled across all four references, these are the load-bearing choices a clean engine faces — the *questions* the survey raises. Each is **decided and designed in the top-down**; listed here so the survey names what's at stake.

1. **Execution strategy** — AST-walk vs bytecode-VM vs native-JIT (or a tier ladder). *The* decision; everything follows. ([top-down §2](livescripts-analysis-top-down.md#2-why-native-and-why-our-own-expanding-decision-1))
2. **The IR seam** — ESPLiveScript emits Xtensa directly (right for one ISA); a multi-target redesign adds a representation between AST and execution so one front-end feeds many backends. The highest-leverage structural change. ([§3.2](livescripts-analysis-top-down.md#32-the-three-tiers-where-the-ir-seam-lives))
3. **Host-binding model** — all four references use a flat name→pointer registry (`addExternalFunction`/`arti_*`); MoonLight's value-add is a MoonModule binding (controls, producer/consumer structs, module role) with no prior art to copy. (§3.4)
4. **Per-frame contract** — script writes its own `loop()` vs an engine-called `renderLed()`; determines the hot-loop shape and where per-pixel dispatch lands. (§3.4)
5. **Compile-once / persist** — a saved artifact skips device-side recompile; portable (one artifact) with a VM/WASM, per-ISA with native. (§3.7)
6. **Memory placement** — code IRAM/PSRAM, data internal/PSRAM, per-target; routed through `platform::` as one policy. (§3.7)
7. **Concurrency + sync** — in-tick vs a pinned task; the threading contract against the scheduler + no-blocking-hot-path rule. (§3.8)
8. **Live reconfig + robustness** — a re-pushed script swaps in tick-atomically, old freed, no reboot, no mid-render crash. (§3.6)
9. **Safety / sandboxing** — native can crash, a VM can bound; a user-facing editor raises the stakes. Coupled to the execution-strategy choice. (§4)

## Mapping to MoonLight's requirements

| Requirement | ESPLiveScript today | What the redesign must add |
|---|---|---|
| Blazingly fast | ✅ native Xtensa | Keep native speed *where the ISA has a backend*; VM elsewhere |
| Core (sensor data) + light (layout/effect/modifier/driver) | ⚠️ generic funcs only; no module roles | The MoonModule binding + per-role entry-point contracts |
| ESP32 classic + S3 first | ✅ (both Xtensa) | Carry forward (native backend) |
| P4 + other ESP32 | ❌ P4 is RISC-V | RISC-V backend *or* VM fallback |
| Teensy | ❌ ARM | ARM backend *or* VM |
| macOS/Linux/Windows | ❌ x86/ARM + no IRAM | Desktop backend *or* VM (VM is the obvious win here) |
| Smart memory | ⚠️ one `#ifdef`, IRAM/PSRAM | Route through `platform::alloc`; compile-once artifact |
| Infinitely scalable (N scripts) | ❌ examples run one script | Independent `MoonLive` per module; code+data arenas PSRAM-first; ceiling = free heap, not a fixed count |
| Sync with system | ⚠️ FreeRTOS tasks + `sync()` | `Scheduler` tick contract; tick-atomic hot-swap; live reconfig |
| MoonModule-compatible | ❌ flat C registry | Scripted module = MoonModule (controls, loop, role, generic UI) |

The pattern is clear: **ESPLiveScript nails speed on two chips and nothing else on this list.** Every other requirement is new work, and the multi-target + MoonModule-binding pieces are the bulk of it.

## Recommendation — native engine, Xtensa-first, behind an IR seam

The survey lands on a clear direction (mirroring the LED-driver bottom-up's "walk Scenario B" call, not an open fork): **build our own native-codegen engine, ship the Xtensa backend first, put an IR seam between the front-end and the code generator from day one — start small, start beautiful, extend with no dead-ends.** Native speed (near-100%) is the differentiator; the IR seam is the no-dead-end guarantee that makes "Xtensa-first" safe (RISC-V/ARM/desktop are each a new backend behind the unchanged IR); WASM/WAMR is the per-target fallback, never the rival; safety is staged (cheap bounds+watchdog first, true sandbox deferred to the WASM fallback).

The full reasoning — why native over WASM-wholesale, why Xtensa-first isn't a corner, the cost accepted, the load-bearing spike — **is expanded in the top-down** ([§2 Why native](livescripts-analysis-top-down.md#2-why-native-and-why-our-own-expanding-decision-1), §9 Staged plan), and the decisions it rests on are recorded verbatim in § Answers — product-owner direction below. This section is the survey's conclusion; the build spec is the top-down.

## Prior art & credits

Per *Industry standards, our own code*: study the prior art, credit it by name, write our own. This redesign rests on work others did first.

- **ESPLiveScript — Yves Bazin (hpwit).** The native-codegen approach the recommendation builds on is his. A from-scratch C-like compiler — tokenizer, parser, register allocator, Xtensa code generator, save/load compiled-binary path, header-only — that runs a script at near hand-written-C++ speed on an ESP32 (85 fps on a 12,288-LED panel where Lua managed 3 and Gravity 10). That is the result that makes "go native, not interpreted" the right call, and the reason this document exists. The structural notes below (global state, no IR, large files) are what a *rewrite toward a different goal* — multi-target, module-bound — has to account for; they are not a verdict on the original, which does its job and does it fast. We carry the idea forward and add the IR seam + the MoonModule binding.
- **ARTI-FX / ARTI — ewowi.** The prior MoonLight-family answer to the same problem, written by this analysis's author: the interpreted-effects runtime in WLED MoonModules, on the PEG-grammar ARTI interpreter. It proved the live-scripting idea works end-to-end in this ecosystem (load a script, run it live), and it is the source of lessons carried straight into this design — the `renderFrame`/`renderLed` split, the host-binding shape, and, by being the AST-walking design, the clearest demonstration of *why* a 16K hot path wants native or VM execution over tree-walking. The redesign trades its interpreter for native speed; it keeps its product shape and its lessons.
- **MoonLight — MoonModules** (the [effects tutorial](https://moonmodules.org/projectMM/moonlight/effects-tutorial/), the staging spine and the host-binding surface model). See the per-engine sections above for what each contributes.

Activity + credits also in the digest [friend-repos/hpwit-ESPLiveScript.md](../../friend-repos/hpwit-ESPLiveScript.md).

## Risks and unknowns

The open questions the survey surfaced are **resolved in the top-down**, each in its own section: the load-bearing speed unknown — can native-with-safety hold 16K×50FPS, else fall back to WASM — is the first spike ([top-down §9.2](livescripts-analysis-top-down.md#92-the-ladder)); IR design ([§3.2](livescripts-analysis-top-down.md#32-the-three-tiers-where-the-ir-seam-lives)); the per-pixel-vs-per-frame contract (§3.4); the MoonModule-binding mechanics ([§3.3–3.4](livescripts-analysis-top-down.md#33-a-scripted-module-is-a-moonmodule-decision-7)); safety depth (§4); editor + persistence ([§9.2](livescripts-analysis-top-down.md#92-the-ladder), stage 10). The single load-bearing one to flag here: **whether native codegen holds the frame budget on a real S3** — if it doesn't, the IR seam makes the WASM fallback a backend swap, not a restart.

## Answers — product-owner direction (2026-06-25)

Decisions from the design discussion that produced this survey. These are *direction*, terse on purpose; the top-down expands each into full reasoning, an API/architecture, and a staged plan. (Mirrors the LED-driver bottom-up's product-owner-direction section.)

1. **Execution = native, the standout.** Build our own **native-codegen** engine (ESPLiveScript-class speed, near-100%) — the differentiator; MoonLight should stand out with something not done before (a native live-compiler bound to a real module system). *Not* a slow interpreter, *not* WASM-as-flagship.
2. **No dead-ends, start small + beautiful (the LED-driver method).** Ship **one ISA backend first — Xtensa (classic ESP32 + S3)** — as a complete, blazing first increment, then grow. The **IR seam** (front-end → typed IR → per-ISA backend) is the no-dead-end guarantee: RISC-V (P4), ARM (Teensy), x86/ARM64 (desktop) each become a *new backend behind the unchanged IR*, never a rewrite. WASM/WAMR is the **per-target fallback** so no target is ever blocked.
3. **The IR must NOT cost speed (hard constraint).** It is a *compile-time* representation that lowers to the *same* native instructions ESPLiveScript hand-emits — **zero per-pixel runtime overhead**, no interpreted layer. Equally fast as hpwit on Xtensa is non-negotiable; prove it by diffing generated instructions for a hot loop against hand-written Xtensa, and keep an `__asm__` escape hatch for the very hottest paths (as ESPLiveScript has).
4. **Source language = a C-subset, "as close as possible" to the precompiled effect, with pragmatic simplifications.** The effect *body* (types like `uint8_t`/`uint32_t`/`lengthType`, nested `for`, integer + 64-bit math, `static_cast`, `RGB`, `hsvToRgb`, buffer writes) ports **near-verbatim** from a file like `RipplesEffect.h` (our reference effect — it exercises the hard cases: `float` trig `std::sqrt`/`std::sin`/`std::floor`, `std::memset`, 3D with `depth()`, two controls). The C++ *file/class ceremony* that buys nothing in a script (`#pragma`/`#include`/`namespace`, and — accepted as a pragmatic simplification — `class : public EffectBase`/`override`/the `controls_.addControl` host-object dance) is **dropped or lightened**: the engine supplies the module scaffolding around the script. Target: porting an existing effect is the loop body verbatim + a handful of lines changed, *not* a rewrite, and *not* implementing a full C++ object model (inheritance/vtables/host-method binding) in the engine. **Not** a JS-subset (the ARTI-FX surface): JS's double-everything number model is the slow path *and* further from our C++ codebase, so it's worse on both speed and portability of existing effects.
5. **Controls = minimal ceremony.** A scripted control is a near-plain top-level variable (e.g. `uint8_t speed = 60;` with a range annotation); the engine derives the MoonModule control + UI + persistence. Lighter than today's explicit `controls_.addControl(...)`, copy-paste-friendly. (Exact annotation syntax is the top-down's call.)
6. **Safety = staged, climb the tiers, don't pay upfront.** Ship the **cheap** tier first — array **bounds-checking** (a compare-branch per indexed access, low single-digit %, removable in a trusted/fast mode) + **watchdog / instruction budget** (kill a runaway loop, near-free). The **expensive** true-memory-sandbox tier (a script physically can't touch memory outside its arena — what WASM gives free, native can't cheaply) is **deferred**, reachable via the IR→WASM fallback only if a public script editor in the field shows the cheap tier isn't enough. Decided this way because the price of full sandboxing upfront isn't worth paying before evidence demands it.
7. **MoonModule-first.** A scripted module **is** a MoonModule (role, controls, `loop()`, generic UI, lifecycle, robustness, live-reconfig). The script ⇄ MoonModule binding (reach the `Buffer`/`AudioFrame`/LUT via the producer/consumer pull pattern, no copy) is the MoonLight value-add to design — no prior art copies cleanly.
8. **General in core + specific in light.** One engine serves a domain-neutral core script (e.g. transform sensor data) *and* a scripted layout / effect / modifier / driver. **Effect is the first role.** `RipplesEffect.h` is the *reference* effect for the language design (it stresses float trig + 3D + memset), but it is **too complex for the hello-world spike** — the first running script must be trivial (e.g. fill the buffer one color, or a single moving dot), proving the engine end-to-end before any real effect. Ripples is the *graduation* target, not the spike. For how an effect is structured for a newcomer, the [MoonLight effects tutorial](https://moonmodules.org/projectMM/moonlight/effects-tutorial/) is a good read (a sibling project's step-by-step). The simple→Ripples progression is itself the start-small-grow staging applied to the demo.
9. **Infinitely scalable.** Run *as many* live scripts concurrently as memory allows, exploiting PSRAM — each script is an independent compiled unit, the ceiling is free heap, not a fixed slot count. Many small scripted modules coexist; the device hosts what fits and degrades gracefully when it doesn't (the same scaling-to-available-memory contract the light pipeline already honours).
10. **Inline execution by default; task is the exception.** A scripted effect/layout/modifier/driver runs *inline in the `Scheduler` tick*, called exactly like a compiled effect's `loop()` — one mental model, no cross-thread sync to reach the buffer/`AudioFrame`, and it runs on the render task's *internal-RAM* stack (fast). Task-per-script isn't blocked on memory (a task stack can live in PSRAM), but it pays two costs inline doesn't: per-task **scheduling overhead** (a context switch per task per frame — hundreds of tasks thrash the scheduler, a ceiling independent of memory), and a **PSRAM-backed stack is hot-path-slow** (PSRAM latency on every per-pixel local access, ~12 vs ~80 MB/s). So inline keeps the per-script stack fast and free, and PSRAM is spent on script *code + data* (decision 9) rather than per-script stacks. A pinned task is the narrow, documented opt-in *only* for a long/blocking *core* script (e.g. slow sensor I/O) that must not share the render tick — never the default, never for a pipeline script.
11. **Sequencing: hybrid (depth-first to hello-world, then prove the seam on a 2nd ISA early).** Build the full vertical slice on Xtensa just far enough to run hello-world native (classic/S3), then *immediately* prove a minimal second-ISA backend (P4/RISC-V, or desktop x86-64) on that same slice — before deepening to controls/math/2D/3D. This retires the project's biggest risk (does the IR seam genuinely decouple front-end from backend?) at hello-world cost, when fixing it is cheap, rather than discovering a leak after six stages. Then deepen, primarily on Xtensa; the full second backend follows later.
12. **Domain-neutral engine core, thin binding.** The engine (front-end + IR + backends) is domain-neutral core — it never depends on MoonLight; the binding (`MoonLiveHost`/`MoonLiveEffect`) depends on the engine, one-directionally, through the engine's public API + a tiny injectable platform seam (`allocExec`/`alloc`). This clean layering is adopted *because it is what makes MoonLight-as-a-library optimal* (the [*Domain-neutral core*](../../CLAUDE.md) principle applied), and is **never compromised** for any separability — if a separation would cost MoonLight's speed/simplicity/hot-path/principles, it isn't done.
## Out of scope for Stage 1

Final VM-vs-JIT decision; the IR design; the language grammar spec; the web editor; per-engine benchmarking on real hardware; the MoonModule-binding mechanics; the sandboxing depth. All belong to the top-down document the prompt above generates.
