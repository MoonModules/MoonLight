# MoonLive — live-script engine, top-down redesign

> **Forward-looking research document — exception to CLAUDE.md present-tense rule.** **MoonLive** is MoonLight's live-script engine (the Moon family: MoonLight, MoonDeck, MoonLive — author an effect as text, see it live). Stage-2 companion to [livescripts-analysis-bottom-up.md](livescripts-analysis-bottom-up.md) (read first: it deep-reads the ESPLiveScript fork, surveys WLED ARTI-FX, the embedded-VM field, and a portable WASM fallback, and ends with the product-owner-direction decisions this document expands). It reasons from MoonLight's end goal — *author a script as text, run it on a running device on the next tick* — down to a reference architecture, a concrete API, a performance budget, and a staged spike plan. Modelled on [leddriver-analysis-top-down.md](../../work/future/leddriver-analysis-top-down.md). This expands the eight decisions already made; it does not re-open them. All design is written fresh against MoonLight's architecture — prior art (ESPLiveScript, ARTI-FX, MoonLight) is credited, not traced.

## TL;DR

- **MoonLive is our native-codegen engine** — a real compiler (lex → parse → AST → **IR** → native machine code), executed by direct function-pointer call, so a scripted effect runs at **near-100% native speed** in the render hot path. That speed, bound to a real module system, is MoonLight's standout.
- **One narrow boundary, three tiers.** The seam is `MoonLive::run()` (the analog of `LedDriver::push()`). Above it: a **platform-independent front-end** (tokenizer → parser → typed AST). Below it: a **typed IR** (the seam that lets one front-end feed many backends) → a **per-ISA backend** (Xtensa first). The IR is compile-time only — **zero per-pixel runtime cost**; it lowers to the same native instructions a hand-written backend would emit.
- **Xtensa first, no dead-ends.** Ship the Xtensa backend (classic ESP32 + S3 — MoonLight's first targets, the bench hardware) as a complete, blazing first increment. RISC-V (P4), ARM (Teensy), x86-64/ARM64 (desktop) are each *a new backend behind the unchanged IR* later — additive, never a rewrite. **WASM/WAMR is the per-target fallback** (IR→WASM is one more backend), so no target is ever blocked and a true sandbox stays reachable.
- **Source language: a C-subset, "as close as possible" to a precompiled effect, with pragmatic simplifications.** The effect *body* ports near-verbatim from a `.h` (types, `for`, integer + float math, `static_cast`, `RGB`, `hsvToRgb`, buffer writes). The C++ *file/class ceremony* that buys nothing in a script (`#pragma`/`#include`/`namespace`; lightened: `class : public EffectBase`/`override`/the `controls_`-dance) is supplied by the engine. **Not** JS (doubles = slow + further from our code); **not** full C++ (object model = build + hot-path cost for zero-value boilerplate).
- **Minimal-ceremony controls.** A control is a near-plain top-level variable with a range annotation; the engine derives the MoonModule control + UI + persistence.
- **MoonModule-first.** A scripted module **is** a `MoonModule` (role, controls, `loop()`, generic UI, lifecycle, robustness, live-reconfig). The script ⇄ host binding reaches the `Buffer` / `AudioFrame` / LUT through the producer/consumer pull pattern — no copy. This is the MoonLight value-add with no prior art to trace.
- **Safety staged.** Ship cheap first — array **bounds-checking** (a compare-branch per indexed access, low single-digit %, switchable off) + a **watchdog / instruction budget** (kill a runaway loop, near-free). The expensive **true memory sandbox** (WASM gives it free; native can't cheaply) is deferred, reachable via the IR→WASM fallback only if a public editor in the field demands it.
- **Staging spine = the [MoonLight effects tutorial](https://moonmodules.org/projectMM/moonlight/effects-tutorial/) ladder.** Each tutorial rung (random pixel → control → trails → oscillators → 2D → 3D → audio) is one engine-capability spike with a concrete acceptance bar. **RipplesEffect.h is the language-fidelity *graduation test*** (does the C-subset handle float trig + 3D + `memset` near-verbatim?) — the hard case, not the hello-world.
- **Load-bearing spike:** a minimal native Xtensa engine running the tutorial's hello-world (`setRGB(random16(N), blue)`) live on an ESP32-S3, hitting the frame budget *and* surviving a deliberately-bad script via cheap safety. If native-with-cheap-safety can't hold 16K×50FPS, the fallback is demote-to-WASM/WAMR — a backend swap behind the IR, not a restart.
- **Cost, eyes open.** A real compiler is more work than adopting an off-the-shelf VM — weeks to the first beautiful Xtensa increment, each ISA backend its own increment later. The deliberate trade for native speed + a differentiator, mitigated exactly as the LED drivers were: spike-ordered, one complete increment at a time, the hard multi-target part deferred behind a seam that keeps it reachable.

## 1. The goal, in detail

A user writes a script — an effect, layout, modifier, driver, or a domain-neutral core rule (transform sensor data) — in a text box in the browser. The device compiles it and runs it as a first-class `MoonModule` **on the next tick**: no toolchain, no flash, no reboot. The same leap WLED took with ARTI-FX. The hard requirements (from the bottom-up):

- **Blazingly fast** — the script runs in the render hot path at up to 16K+ lights × 50 FPS, so a slow per-pixel path is fatal. This is *the* constraint that picks native over interpreted.
- **General in core + specific in light** — one engine, many `MoonModule` roles.
- **Target order** — ESP32 classic + S3, then P4 + other ESP32, then Teensy, then desktop.
- **Smart memory** — IRAM/PSRAM-aware via `platform::alloc`, no hot-path allocation, compile-once.
- **Infinitely scalable** — run *as many* live scripts concurrently as memory allows, exploiting PSRAM. No fixed slot count; each script is an independent compiled unit and the only ceiling is free heap. A device hosts as many scripted modules (effects across layers, modifiers, core sensor rules) as fit, and degrades gracefully when it doesn't — the same scaling-to-available-memory contract the light pipeline already honours.
- **Synced** — `Scheduler`-tick, live reconfig (no reboot), tick-atomic hot-swap, robust to add/delete/replace in any order.
- **MoonModule-compatible** — controls, `loop()`, role, generic UI with zero per-script UI code.

## 2. Why native, and why our own (expanding decision 1)

The design space runs from interpreted to native, and the choice is driven by the hot-path requirement:

- **AST-walk** (ARTI-FX): stores values as `double` and walks the tree per frame — which buys maximum flexibility and runs on any platform unchanged, at the cost of per-frame speed. That speed cost is what rules it out *for the 16K×50FPS hot path specifically* — not a flaw, a different trade than MoonLight needs here. (For a slow core script off the hot path, that trade would be fine.)
- **A bytecode VM is the middle ground** — a compact opcode stream run by a dispatch loop, far faster than tree-walking but paying a per-opcode dispatch tax every operation; at 16K×50FPS = 800K px/s that tax is the open question, not a given.
- **Native JIT is the only thing that reaches ~100%** (ESPLiveScript's 85 fps ≈ hand-written C++ — hpwit's result). The differentiator we're after is the *combination*: native speed **and** multi-target **and** bound to a real module system. Each prior engine has part of it — ESPLiveScript has the native speed (Xtensa); ARTI-FX has the live-scripting product shape and runs anywhere (interpreted). Neither combines all three; that combination is the open space.
- **WASM/WAMR** gets portability + free sandbox but tops at ~50% native (WAMR-AOT) with a 200KB+ runtime — kept as the fallback, not the flagship, because native speed is what we're chasing.

**Why our own, not "adopt WAMR":** *Industry standards, our own code.* We take the textbook compiler *shape* (lex → parse → AST → IR → native — the LLVM structure scaled to an MCU) and textbook *names*, written fresh against our architecture. An off-the-shelf runtime would make the engine someone else's and cap us at half speed; building means the language, the MoonModule binding, and the hot path are ours to make beautiful. The cost (a real compiler) is accepted and staged.

**What would flip it to WASM-wholesale:** if the load-bearing spike shows native-with-cheap-safety *cannot* hold 16K×50FPS on an S3 (native machine code makes this unlikely), or if a public script editor proves to demand a true sandbox we can't afford natively. The IR seam makes either flip a backend swap, not a restart.

## 3. Reference architecture

### 3.1 The one narrow boundary

Everything hangs off a single tiny seam — the analog of the LED doc's `LedDriver::push(std::span<...>)`:

```cpp
// A compiled script, ready to run. The host calls run() once per tick.
class MoonLive {
public:
    bool ok() const;                 // compiled cleanly
    const char* error() const;       // human-readable compile/runtime error, "" if none
    void run();                      // execute the script's loop() — the hot path
    void bind(MoonLiveHost& host);     // wire controls + host data (§3.4)
    // lifecycle: free() releases code + data; recompile swaps tick-atomically (§3.6)
};
```

Above the line is the portable front-end; below it the IR and the per-ISA backend. The host (a `MoonLiveModule`, §3.3) owns a `MoonLive` and calls `run()` from its `loop()`.

### 3.2 The three tiers (where the IR seam lives)

```
   source text
        │   ┌─────────────── platform-independent (one implementation, all targets)
   tokenizer (lexer)
        │
   parser → typed AST
        │
   ┌────┴──────────── IR SEAM ──────────────┐   ← one front-end, many backends
        │
   typed IR  (SSA-ish, register/temp model, types resolved, bounds-check nodes inserted)
        │   ┌─────────────── platform-bound (one backend per ISA)
   backend: lower IR → native instructions
        │       Xtensa first; RISC-V / ARM / x86-64 / ARM64 later; WASM as the fallback backend
   encode → executable memory (platform::allocExec) → call as function pointer
```

- **Front-end (portable):** tokenizer + parser + AST live in `src/core/moonlive/` (domain-neutral). They know the *language*, never a CPU. One implementation serves every target.
- **IR (the seam):** a small typed intermediate representation — the AST lowered to a flat list of typed operations with explicit temporaries, types resolved, and **bounds-check / safety nodes inserted here** (so every backend inherits safety for free). This is a *compile-time data structure*; it does not exist at run time.
- **Backend (per-ISA):** lowers the IR to native instructions for one ISA. `src/platform/<target>/moonlive_backend_*.{h,cpp}` — the only place CPU-specific codegen lives, behind the platform boundary. A WASM backend (lower IR → `.wasm`, run by WAMR) is one such backend, the portable fallback.

**Critical: the IR costs nothing at run time (decision 3).** It is consumed during compilation and discarded. The CPU executes only the final native instructions — identical in kind to hand-written assembly. *Matching native speed on Xtensa is non-negotiable.* The Xtensa backend must lower a hot loop to instructions that match hand-written Xtensa; the spike's acceptance bar **diffs generated instructions for the Ripples inner loop against a hand-emitted reference**, and an `__asm__` escape hatch covers the very hottest paths. If the IR ever costs hot-path speed, that's a backend bug to fix, not a tax to accept.

### 3.3 A scripted module IS a MoonModule (decision 7)

The one deliberate class hierarchy in the codebase is the module tree; a scripted module joins it like any other. `MoonLiveEffect` is a normal `EffectBase` whose `loop()` delegates to the compiled `MoonLive`:

```cpp
// src/light/moonlive/MoonLiveEffect.h  — a scripted effect is a first-class EffectBase
class MoonLiveEffect : public EffectBase {
public:
    ModuleRole role() const override { return ModuleRole::Effect; }
    const char* tags() const override { return "📝"; }   // scripted
    Dim dimensions() const override { return engine_.declaredDim(); }  // from the script

    void setup() override {                                // acquire the engine
        engine_.bind(host_);                               // wire the host data (§3.4)
    }

    void onBuildControls() override {                      // DYNAMIC controls — re-runs when the script changes
        controls_.addText("source", source_, kMaxSource);  // the script text (persisted, editable)
        // The engine declares its controls as NEUTRAL data; the binding translates to controls_.
        for (auto& c : engine_.declaredControls())         // {name,type,min,max,default} — no MoonLight type
            controls_.add(c.name, c.type, c.min, c.max);   // binding maps neutral → MoonLight control (§3.5)
    }

    // onBuildState is the rebuild sweep: it fires on a source edit (recompile) AND on a
    // grid/size change (the engine re-sizes its script buffers for the new dimensions).
    // MoonLight has no separate onSizeChanged — resize routes through onBuildState, so the
    // dynamic-memory re-allocation rides the same hook every config change already uses.
    void onBuildState() override {
        engine_.compile(source_);                          // recompile if source changed
        if (!engine_.ok()) { setStatus(engine_.error(), Severity::Error); return; }
        engine_.allocForSize(width(), height(), depth());  // (re)alloc script data for the current size
    }

    void onUpdate(const char* name) override {             // cheap per-control reaction, no full rebuild
        engine_.onControlChanged(name);                    // poke a running script's bound control
    }

    void loop() override {
        if (engine_.ok()) engine_.run();                   // the hot path: native code over our buffer
    }

    void teardown() override {                             // release: free compiled code + script data
        engine_.free();                                    // the "destructor" role — arenas returned to the heap
    }
};
```

The same shape gives `MoonLiveLayout` (role `Layout`, emits coordinates), `MoonLiveModifier` (role `Modifier`, remaps positions), `MoonLiveDriver` (role `Driver`, consumes the buffer), and a core `MoonLiveModule` (domain-neutral, e.g. a sensor rule). One engine, many roles — each a thin `MoonModule` subclass whose `loop()` is `engine_.run()`. The UI renders them generically (the `source` text control + the script-declared controls) with **zero per-script UI code** — exactly the module-tree payoff.

**A scripted module implements the whole `MoonModule` lifecycle, not just `loop()`** — that's what makes it a first-class module and what answers dynamic controls / dynamic memory / cleanup:

- **`onBuildControls()` — dynamic controls.** Re-runs whenever the module rebuilds, so a script that declares different controls (a new `@control` var) gets a different control set in the UI + persistence, live. The controls are *the script's*, not a fixed list.
- **`onBuildState()` — dynamic memory on size change.** MoonLight routes a grid/size change through `onBuildState` (the same rebuild sweep that applies every config change without a reboot), so MoonLive re-allocates its per-size script buffers here (`allocForSize`, PSRAM-first per §3.7). There is no bespoke `onSizeChanged` — using the existing hook means a scripted module resizes exactly like a compiled one, and inherits the no-reboot + robustness contracts for free.
- **`setup()` / `teardown()` — acquire / release (the destructor role).** `teardown` frees the compiled code block + the script's data arena back to the heap, so deleting a scripted module returns all its memory — the lifecycle that makes "as many scripts as memory allows" (§3.7) safe to add *and remove* in any order.
- **`onUpdate(name)` — cheap per-control reaction.** A control edit pokes the running script's bound variable without a full recompile (the fast path for a slider drag); only a *source* edit triggers the heavier `onBuildState` recompile.

So the binding overrides the same hooks any compiled module does; the only difference is that each one delegates to the compiled `MoonLive` instead of hand-written C++.

**Crucially, all of these lifecycle methods live in the *binding* (`MoonLiveEffect`, `src/light/moonlive/`), not in the engine.** `onBuildControls`/`onBuildState`/`onUpdate`/`teardown`, `EffectBase`, `ModuleRole`, `controls_` — every MoonLight type — sit on the binding side of the §3.9 seam. The engine (`MoonLive`, `src/core/moonlive/`) sees none of them; the binding reaches it only through a **neutral public API**: `compile(source)`, `run()`, `free()`, `declaredControls()` → a plain list of `{name, type, min, max, default}` structs the engine owns, and `allocForSize(w, h, d)` → plain ints. The binding *translates* — it reads the engine's neutral `declaredControls()` and calls MoonLight's `controls_.addControl(...)`; it maps a grid resize to `allocForSize`. **The engine never takes a `ControlList`, a `Buffer`, or any MoonLight type** — so the rich MoonModule lifecycle is entirely a property of the binding, and the engine stays the domain-neutral core §3.9 describes. (This is the seam working as intended: a different host writes its own binding with its own lifecycle against the same neutral engine API.)

### 3.4 The host binding — script ⇄ MoonModule (decision 7, the value-add)

Rather than a flat name→pointer registry (the host-binding shape surveyed engines share), MoonLight uses a **MoonModule-aware `MoonLiveHost`** that exposes the producer/consumer data the script needs, by reference, no copy — the same pull pattern effects already use (`EffectBase::buffer()/width()/elapsed()`, [Layer.h:499-504](../../src/light/layers/Layer.h)):

- **Buffer + geometry** — `width()`, `height()`, `depth()`, `channelsPerLight()`, `nrOfLights()`, `elapsed()`, and pixel writers `setRGB(i,c)` / `setRGBXY(x,y,c)` / `setRGBXYZ(x,y,z,c)` (the MoonLight tutorial's exact surface). These compile to direct loads/stores against `layer_->buffer()` — the **identity-mapping fast path** preserved (the script writes the real buffer, no intermediate copy).
- **Controls** — the script's declared variables (§3.5) bind by reference so a UI control edit updates the running script live.
- **Producer structs** — a core or audio script reads `AudioFrame` (level + 16-band spectrum) / sensor structs through the same `const`-pointer pull the C++ effects use (`AudioService::latestFrame()`), so add/remove in any order returns a live or silent-default frame, never null (robustness).
- **Built-ins** — `hsvToRgb`/`hsv`, `random16`, `sin`/`cos`/`sqrt`/`floor` (the trig Ripples needs), `millis`/`elapsed`, `fill`/`memset`. A small, fixed, recognizable library (FastLED-flavoured — the vocabulary effect authors already know), implemented once in the host and callable from any backend.

The binding is generated *around* the script body — the script never writes `#include`, never reaches a raw pointer it shouldn't, and the host decides what's in scope per role (an effect sees the buffer; a core sensor script sees the sensor struct, not the LED buffer).

### 3.5 Controls — minimal ceremony (decision 5)

A control is a near-plain top-level variable with a range annotation; the engine derives the `MoonModule` control + UI + persistence:

```c
byte speed = 50;         // @control 0..99      → controls_.addControl("speed", …, 0, 99)
uint8_t interval = 128;  // @control 1..254
```

The front-end collects annotated top-level vars during parsing and the engine exposes them as a neutral `declaredControls()` list (`{name, type, min, max, default}` — no MoonLight type); the *binding* reads that list and calls the normal `controls_.add(...)` the rest of MoonLight uses (§3.3) — so a scripted control is indistinguishable from a compiled one in the UI, persistence, and the live-reconfig sweep, while the engine stays MoonLight-agnostic. Lighter than today's explicit `onBuildControls` + `addControl` (the engine writes that for you), and copy-paste-friendly: the `uint8_t speed = 50;` line is *already* how RipplesEffect.h declares it. (Exact annotation syntax — `@control`, a trailing comment convention, or a `slider(0,99)` initializer — is settled in the spike; the principle is "declare the var, get the control".)

### 3.6 Live reconfig + tick-atomic hot-swap (decision: sync)

A re-pushed script must swap in on the next tick with the old one freed, no reboot, no crash mid-render — the no-reboot + robustness principles applied to *code*. The mechanism rides the existing `onBuildState()` rebuild sweep (the same one that makes every config change live): a `source` edit marks the module dirty; the next `onBuildState()` compiles the new source into a *second* `MoonLive`, and only on success swaps the active pointer (the old `MoonLive` freed after the swap), so a failed compile leaves the running effect untouched and surfaces the error in the module's status. `run()` reads the active pointer once per tick — the swap is a single pointer store between ticks, never mid-`loop()`.

### 3.7 Memory placement + infinite scalability, routed through `platform::` (decisions: smart memory, infinitely scalable)

Memory placement routes through the existing `platform::` seam, so it's one policy, not scattered per-target branches:

- **Compiled code** → `platform::allocExec(size)` (a new seam: `MALLOC_CAP_EXEC` IRAM on ESP32; `mmap(PROT_EXEC)` on desktop; the platform decides, the engine doesn't know). PSRAM-capable on S3/P4 where the chip allows executable PSRAM.
- **Script data** (globals, stack arena) → `platform::alloc` (PSRAM-first with internal fallback — already the project's policy).
- **Compile-once** → a portable compiled-artifact format persisted to LittleFS, so a known-good script skips device-side recompile on boot. The native artifact is per-ISA; the portable fallback artifact is one file for all targets.

**Infinite scalability — as many scripts as memory allows.** Each `MoonLive` (§3.1) is a **self-contained compiled unit**: its own code block (from `allocExec`) and its own data arena (from `alloc`), owned by the `MoonLiveModule` that holds it, freed when that module is deleted. Nothing is shared or fixed-slot — so running N scripts is just N independent `MoonLive`s, and **the only ceiling is free heap**, not an arbitrary limit. This falls out of the architecture for free:

- **The module tree already hosts N modules.** A scripted module is a `MoonModule` (§3.3); the tree puts no cap on how many effects a Layer holds or how many peripherals System hosts. Ten scripted effects across layers + a scripted modifier + two core sensor rules are just twelve modules — the UI, persistence, and `Scheduler` handle them like any other.
- **PSRAM is where it scales.** On an S3/P4 (8 MB PSRAM) the compiled code + data arenas live in PSRAM (`alloc` is PSRAM-first), so the device holds *far* more scripts than internal RAM alone would allow — exploiting PSRAM is exactly what lifts the ceiling from "a handful" to "as many as the script sizes sum to under PSRAM." A non-PSRAM classic ESP32 holds fewer (internal heap only) — correct and honest, the same internal-vs-PSRAM split the rest of the system has.
- **Graceful degradation when full.** When the next script won't fit, the device does what the light pipeline already does at the memory edge ([architecture.md § scaling to available memory](../../explanation/architecture/moonlight.md#scaling-to-available-memory)): the compile/bind fails cleanly, the module reports a "not enough memory" status, and everything already running keeps running — no crash, no reboot (the robustness + no-reboot principles). The cap is reached by *degrading*, never by bricking.
- **The hot-path cost is per-*running* script, not per-*loaded* script.** Memory scales with how many scripts are loaded; tick time scales with how many are *enabled and rendering*. A device can hold a large library of scripts in PSRAM and run only the active ones, so "infinitely scalable in memory" doesn't mean "infinitely slow" — a disabled scripted module costs RAM but no tick time (and the disable-releases-resources backlog item, when it lands, lets it cost neither).

**Memory-scaling status + the one refactor ahead (watch-item as the language grows).** A memory review at the 3-builtin stage (`setRGB`/`fill`/`random16`) pinned where the per-effect cost lives and what scales with language richness — recorded here so the optimisation is planned, not discovered:

- **Already in place (don't redo):** the exec block is allocated at the *emitted length* (word-rounded), not a worst-case cap — a `fill` is ~50–70 B of native code, a four-call `setRGB` ~400 B, so per-effect heap scales with the script, not a flat reservation. And the effect reports `setDynamicBytes(codeLen())`, so the UI card's "+ dynamic" reflects the JIT'd program (0 when a compile fails and frees it). These two are the right shape for everything below.
- **Flat regardless of language size:** the engine's at-rest members (~48 B/instance) and each `Builtin` descriptor (32 B). A full math/color library is ~30–50 builtins → a ~1.5 KB table that should become a **`static const` built once** (today `lightBuiltins()` returns a ~520 B table by value per `onBuildState` — fine at 3 entries, wasteful at 50; make it static when the library grows).
- **Scales with script complexity (the exec block, heap, per effect):** today's one-liners are 50–400 B; a Ripples-class effect (per-pixel `sqrt`/`sin`, 3D, a fade loop, two controls) is a few hundred instructions → an estimated **1.5–4 KB of native code**, comparable to the equivalent compiled effect (the point of native). PSRAM-first `allocExec` (§ above) absorbs this.
- **The one architectural change ahead — transient compile buffers must move stack → reusable heap arena.** Today `compile()` puts the staging buffer (`kCodeCap`), the `IrProgram` (`kMaxIrOps=64` ops × 32 B ≈ 2 KB), and the assembler buffer on the **stack** (~4 KB peak, off the hot path, in `onBuildState`). This is fine for the current grammar but **will not hold once scripts have loops + real expressions**: a Ripples body easily exceeds 64 IR ops, and growing `kMaxIrOps` to 256–512 makes the IR alone 8–16 KB — too large for an MCU task stack. The planned fix (matches "code+data arenas" above): a **single heap arena the compiler borrows during `onBuildState` and releases**, sized once and reused across recompiles, instead of stack arrays. Introduce it **when the op count first outgrows the stack-safe range — likely Stage 3 (math + loops)**, not before; it's a contained change (the compiler's buffers, not the IR or front-end) and the `allocExec(len)` direction already set the precedent.
- **New at full language — a per-script *data* arena.** Scripts are stateless pure functions today (no per-instance data beyond the source text). Once a script declares controls (`uint8_t speed = 50;`) or state (`static float lut[256];`), each effect needs a **data arena** (`platform::alloc`, PSRAM-first) alongside its code block — a few hundred bytes to a few KB per effect, sized per script. This is the second arena §3.7 already names; it lands with Stage 1 (controls) and grows through the oscillator/LUT stages.
- **Backlog — grow-on-demand source buffer (small *and* big scripts).** The `source` text control today is a fixed member buffer (`MoonLiveEffect::source_[512]`), and the textarea control API now carries a `uint16_t` buffer size (was capped at 255), which covers Stage-1 multi-control scripts. But it's still a *fixed* size on every effect (a 512 B member even for `fill(0,0,255);`), and Ripples-class scripts in later stages will need KBs. The cap is that the generic control write path binds a fixed `(char* ptr, bufSize)` and `parseString` truncates the incoming value to it — so a buffer can't *grow* mid-write. The future design: a **dynamically-sized source buffer** (PSRAM-first, allocated to the script's actual length and freed on teardown) so a small script costs little and a big script is allowed — which means the control write path (or a MoonLive-specific source-set hook) must re-allocate-then-rebind *before* the write lands, instead of truncating. Land it when scripts genuinely outgrow a sensible fixed cap (Stage 6 / Ripples), together with the data + compile arenas above.

Net: no resource cliff — at-rest per-effect cost stays PSRAM-friendly (code + data arenas, a few KB each) — but **one deliberate refactor is queued** (stack compile-buffers → shared reused arena), best landed exactly when a real script body first needs it.

### 3.8 Execution model — inline by default, task as the exception (decision: sync)

**A script runs inline in the `Scheduler` tick by default — not in its own task.** A scripted effect's `loop()` is called exactly like a compiled effect's `loop()`, on the render task, each tick. The task-per-script model some engines use fits when a script *is* the top-level loop and owns the device; in MoonLight a scripted module is one `MoonModule` among many, called from the same single-threaded render loop as every compiled module, so inline is the consistent shape. Three reasons make inline the default, not just a choice:

- **Consistency.** A scripted effect behaves identically to a compiled one — same call site, same hot-path rules, same `Scheduler`. One mental model, and the UI/persistence/lifecycle treat it like any other module.
- **It sidesteps two costs task-per-script can't.** Task stacks *can* live in PSRAM (`xTaskCreateWithCaps`, `MALLOC_CAP_SPIRAM`), so task-per-script is not blocked on *internal* RAM — but it pays two costs inline doesn't: (a) **scheduling overhead** — each task is a TCB + scheduler bookkeeping + a context switch; hundreds of tasks all wanting to run each frame thrash the scheduler instead of rendering, a ceiling that has nothing to do with memory; and (b) **a PSRAM-backed task stack is hot-path-slow** — a per-pixel inner loop touching locals on a PSRAM stack pays PSRAM latency (~12 MB/s vs internal ~80 MB/s) every access, exactly what a 16K×50FPS loop can't afford. An inline script runs on the render task's *internal-RAM* stack, fast, with no per-task scheduler cost. So PSRAM scales the script's *code + data* (§3.7), and inline keeps the per-script *stack* fast and free — the two pull together, where task-per-script would put them in tension.
- **No cross-thread sync.** An inline script reads `buffer()` / `elapsed()` / `AudioFrame` from the thread that owns them — no locks, no race, no memory barriers. A task touching the shared buffer while the render task reads it is exactly the data race the single-threaded hot path avoids by design.

An inline script obeys the no-blocking-hot-path rule (it can't `delay`); a runaway loop is caught by the instruction-budget watchdog (§4), so a bad inline script degrades, it doesn't wedge the tick.

**The exception — a pinned task — is narrow and opt-in.** A *core* script that genuinely blocks or runs at its own cadence and must **not** share the render tick (e.g. a slow I²C sensor transaction, or a rule that ticks at 1 Hz independent of render) may opt into its own task, pinned opposite the render core. This is a per-module, documented exception for off-hot-path core work — never the default, and never for a script in the render pipeline (effect/layout/modifier/driver), which is always inline. Two execution paths exist, but the inline one is the default and the task one earns its place case by case.

### 3.9 Layering — a domain-neutral engine core, a thin binding (decision: domain-neutral core)

The tiers above already separate cleanly along MoonLight's own *Domain-neutral core* principle, and the layering is held to it deliberately:

- **The engine core (MoonLive) is domain-neutral.** "MoonLive" is the engine's *name*, not a coupling — the front-end (`src/core/moonlive/`) and the IR + backends (`src/platform/<target>/moonlive_backend_*`) know the *language* and the *ISA*, never `Buffer`, `EffectBase`, the module tree, or anything light- or MoonLight-specific. The core's only outward contract is a tiny injectable platform seam (`platform::allocExec` / `alloc` / `millis`) — a handful of functions, not a reach into MoonLight's full platform layer.
- **The binding is the only MoonLight-coupled layer**, and it is *thin*. `MoonLiveHost` + `MoonLiveEffect`/`MoonLiveLayout`/… (`src/light/moonlive/`, with a core `MoonLiveModule` for sensor rules) sit **on top of** the engine's public API and consume it; they never reach into engine internals.
- **Dependency direction is one-way:** the binding depends on the engine; the engine never depends on the binding (or on MoonLight). The engine does not `#include` MoonLight; MoonLight `#include`s the engine.

Why this matters concretely: **it is what makes MoonLight-as-a-library optimal.** A clean library needs exactly this — a domain-neutral core with a one-directional dependency and a thin, replaceable binding, so the whole stack composes without circular dependencies or hidden coupling. So this layering is not extra structure for its own sake; it is the *Domain-neutral core* + *Complexity lives in core, domain modules stay simple* principles applied, and it is the same boundary MoonLight needs to be a well-formed library.

A true property of that boundary, worth stating: because the core (MoonLive) knows only the *language* and the *ISA* — never LEDs, buffers, or MoonLight — the same front-end + IR + backends would serve a **wholly different host**: a different output device, or a different application entirely (a script that drives a display, reads a keypad, computes a result). Such a host writes its own thin binding against the same public API + platform seam; nothing in the core changes. The IR seam is what makes that portable, too — the host targets whatever chip it likes by writing one backend behind the unchanged IR. This is a *consequence* of building the core domain-neutral for MoonLight, not a goal we design toward — but it is real, and it is the mark of a well-factored core: it doesn't care what you point it at.

**Hard constraint: the layering is justified entirely by MoonLight's optimality — never compromised for it.** The clean engine/binding split is adopted only because it makes MoonLight architecturally sound, fast, and CLAUDE.md/architecture.md-compliant (domain-neutral core, data-over-objects on the hot path, the platform boundary). If any separability would cost MoonLight's optimality — a slower hot path, a heavier binding, an abstraction the engine doesn't need — it is **not** done. The binding stays thin and the core stays neutral *because that is the optimal MoonLight design*, full stop; nothing in the layering is bent toward a use beyond MoonLight.

## 4. Safety — staged (decision 6)

A user-facing editor means a bad script must degrade, not brick. There are two distinct failure surfaces, and they need different defenses:

- **Compile-time (parser / analysis).** A malformed script must be *rejected cleanly at parse*, never crash the compiler. The `fix-warnings` fork's null-deref is exactly this class: a nested external-call argument produced an AST node missing its children, and the compiler dereferenced them unconditionally. The defense is **AST/analysis validation** — the parser checks node shape (children present, types resolved) and emits a diagnostic instead of dereferencing a null, backed by a **regression test** per malformed input (the `fix-warnings` case is the first such test). This is a correctness property of the front-end, not a runtime cost.
- **Runtime — cheap, ship first.** Once a script *compiles*, a bad *value* at run time must degrade, not brick: (a) **Array bounds-checking** — the IR inserts a compare-branch before each indexed access (a clamp or skip on out-of-range), e.g. a `setRGB(i, …)` with `i` past the buffer. Low single-digit % overhead, inserted at the IR so every backend gets it, and **switchable off** in a trusted/fast mode for vetted built-in scripts. (b) **Watchdog / instruction budget** — a per-tick instruction or time budget that aborts a runaway `while(1)` (near-free; the task WDT already does most of it). Together these catch the common *runtime* failures — out-of-range index, infinite loop — at low cost.
- **Expensive, deferred.** A *true* memory sandbox (the script physically cannot touch memory outside its arena) is what native can't cheaply provide. Don't build it first — it's reachable via the IR→WASM fallback (suspect scripts compiled to the sandboxed backend) only if a public editor in the field shows the cheap tier isn't enough. Safety is a ladder climbed on evidence, not a wall built before any script runs.

## 5. The language — a C-subset as close as possible (decision 4)

### 5.1 The model

A C-subset, not full C++, not JS. The type model is exactly what real effects use: `uint8_t`/`uint16_t`/`uint32_t`/`int`/`lengthType`/`nrOfLightsType` integers (with 64-bit where overflow matters — Rainbow's `uint64_t` phase), `float` (Ripples' trig), `bool`, `char`, a `RGB`/`CRGB` struct, and arrays (incl. multi-dim). Control flow: `if/else`, ternary, `while`, C-style `for`, `break`/`continue`, `return`; user functions; `static_cast` (or C casts). Grammar: **hand-written recursive-descent** — the recognizable textbook default, what most embedded script languages use; a PEG is the alternative but recursive-descent is simpler to make fast and to emit good errors from. Built-ins: the fixed host library (§3.4).

### 5.2 What's dropped vs lightened (the pragmatic simplifications)

- **Dropped** (file ceremony, zero value in a script): `#pragma once`, `#include`, `namespace`. The engine supplies the surrounding module.
- **Lightened** (the C++ object model): no `class : public EffectBase`, no `override`, no `controls_.addControl(...)` host-object dance. The engine synthesizes the `MoonLiveEffect` wrapper (§3.3) around the script body; the role/`dimensions`/controls come from light annotations (§3.5) and the script's `loop()`.
- **Kept verbatim** (the part you iterate on): types, the `loop()` body, all the math, `static_cast`, `RGB c = hsvToRgb(...)`, the loops.

**Why not full C++:** supporting `class`/inheritance/`override`/host-method-binding means implementing a C++ object model (vtables, member-reference binding) in the engine — build cost up front, and the object machinery is the very "object graph in the hot path" the architecture forbids. The wrapper has no runtime value; let the engine write it.

**Why not JS** (the ARTI-FX surface): JS's number model is doubles-everywhere — the per-pixel cost that makes ARTI-FX flexible-but-not-fast — *and* it's further from our C++ effects, so porting an existing effect is harder, not easier. A C-subset is both faster and closer to the source.

### 5.3 RipplesEffect.h → scripted form (the language-fidelity test)

`RipplesEffect.h` is the **graduation test** for the language (float trig + 3D + `memset` + two controls — the hard case, deliberately not the hello-world). The body must port near-verbatim:

**Today (`src/light/effects/RipplesEffect.h`, the C++):**

```cpp
class RipplesEffect : public EffectBase {              // ← dropped (engine supplies)
    const char* tags() const override { return "💫🟦🦅"; }   // ← lightened → annotation
    Dim dimensions() const override { return Dim::D3; } // ← lightened → annotation
    uint8_t speed = 50;                                 // ← kept (becomes a control)
    uint8_t interval = 128;
    void onBuildControls() override {                   // ← dropped (derived from the vars)
        controls_.addControl("speed", speed, 0, 99);
        controls_.addControl("interval", interval, 1, 254);
    }
    void loop() override {                              // ← KEPT VERBATIM (the body)
        uint8_t* buf = buffer(); … std::memset(buf, 0, nrOfLights()*cpl);
        const float rippleInterval = 1.3f * … * std::sqrt((float)h);
        for (lengthType z=0; z<d; z++) for (lengthType x=0; x<w; x++) {
            const float dist = std::sqrt(dx*dx+dz*dz)/9.899f*(float)h;
            const lengthType y = (lengthType)std::floor((float)h/2*(1+std::sin(phase)));
            const RGB c = hsvToRgb(hue,255,255);
            … px[0]=c.r; px[1]=c.g; px[2]=c.b;
        }
    }
};
```

**Scripted (the target — body unchanged, ceremony gone):**

```c
// @effect dim=3D                          // role + dimensions, one line
uint8_t speed = 50;       // @control 0..99
uint8_t interval = 128;   // @control 1..254

void loop() {                              // ← byte-for-byte the C++ loop body
    uint8_t* buf = buffer();
    lengthType w = width(), h = height(), d = depth();
    uint8_t cpl = channelsPerLight();
    if (w<=0 || h<=0 || d<=0) return;
    memset(buf, 0, nrOfLights()*cpl);
    float rippleInterval = 1.3 * ((255.0-interval)/128.0) * sqrt((float)h);
    if (rippleInterval < 0.01) return;
    float timeInterval = (float)elapsed() / (100.0-speed) / 6.4;
    float cx = (float)(w-1)/2, cz = (float)(d-1)/2;
    nrOfLightsType wh = (nrOfLightsType)w*h;
    for (lengthType z=0; z<d; z++)
      for (lengthType x=0; x<w; x++) {
        float dx=(float)x-cx, dz=(float)z-cz;
        float dist = sqrt(dx*dx+dz*dz)/9.899495 * (float)h;
        float phase = dist/rippleInterval + timeInterval;
        lengthType y = (lengthType)floor((float)h/2 * (1+sin(phase)));
        if (y<0 || y>=h) continue;
        uint8_t hue = elapsed()/50 + x*3 + z*7;
        RGB c = hsvToRgb(hue,255,255);
        setRGBXYZ(x, y, z, c);              // replaces manual idx + px[0..2]
      }
}
```

The diff is exactly the ceremony: gone are `class`/`override`/`onBuildControls`/`#include`/`namespace`/`std::`-qualification and the manual buffer-index arithmetic (→ `setRGBXYZ`). The math, the trig, the loops, the types — verbatim. **If the engine can compile this at native speed, the C-subset decision is proven.** (Float-trig native codegen is the part to validate — see the perf budget.)

## 6. Testing — the engine's biggest structural advantage

A live-script engine is one of the **most testable things MoonLight can build**, and MoonLight's two-tier test framework ([testing.md](../../reference/testing.md): doctest unit tests + JSON scenarios, each run in-process *and* live) maps onto it almost perfectly. This is a genuine edge: the bottom-up's structural note that ESPLiveScript ships `.ino` examples and no unit suite isn't a knock on it — it's the gap our framework closes. **Every live-script feature gets pinned by a test, back to back**, because two properties make a compiler exceptionally test-friendly:

1. **Every compiler stage is a pure input→output function.** Lex, parse, IR-lower, codegen each take a known input and produce a deterministic output — the easiest thing in the world to unit-test, with no hardware and no flakiness.
2. **A script's *result* is deterministic and exactly assertable.** A known script over a known grid at a known `elapsed()` produces an exact buffer — byte-for-byte checkable. There is no "looks about right"; there is a golden buffer.

### 6.1 Unit tests (`test/unit/core/unit_moonlive_*.cpp`, doctest)

The compiler front-end and IR are domain-neutral core (§3.9), so they unit-test on the desktop with zero hardware:

- **Tokenizer / parser** — source string → expected token list / AST shape; every language construct (types, `for`/`if`/ternary, functions, arrays, `static_cast`, struct access) and every *error* (unterminated string, type mismatch, undeclared var) pinned to an expected diagnostic. The fuzz-class bug the `fix-warnings` fork fixed (a nested external-call arg) is exactly a parser unit test — the regression that would have caught it for free.
- **IR** — AST → expected IR for representative snippets; the bounds-check / safety nodes (§4) asserted present where the IR should insert them.
- **Codegen (host backend)** — the **desktop/x86-64 backend** is itself a test asset: it runs in-process, so a compiled script *executes during a unit test* and its output buffer is asserted directly. A script that fills blue → assert every pixel is `(0,0,255)`; Ripples at a fixed `elapsed()` → assert the exact lit-column pattern. No device needed to test *the language*; the device tests only *the native ISA backend*.
- **Determinism harness** — the existing `setTestNowMs` clock-override seam (the same one scenarios use) lets a time-dependent script be tested at a fixed tick, so an animated effect is a deterministic assertion, not a guess.

### 6.2 Scenario tests (`test/scenarios/{core,light}/scenario_moonlive_*.json`)

Scenarios exercise a scripted module *as a wired `MoonModule`* — the integration layer unit tests don't reach:

- **The MoonModule binding** — `add_module MoonLiveEffect`, `set_control source=<script>`, `set_control speed=…`, `measure` → assert the module renders, the control edits live, the buffer is non-zero. A scripted effect is tested the same way a compiled effect is, through the same runner.
- **Live reconfig + tick-atomic hot-swap (§3.6)** — a scenario pushes a new `source` mid-run and asserts the swap is clean (old freed, new rendering, no crash). Push a *broken* script → assert the prior effect keeps running and the status reports the compile error (the robustness contract).
- **Robustness (the hard rule)** — add/delete/replace a scripted module in any order, at any grid size, including 0×0×0; a scripted effect alongside compiled ones; ten scripts at once (the scalability claim, §3.7, becomes a measured scenario, not a hope). A discovered crash drives a new scenario that pins the fix — the same regression rule the rest of MoonLight follows.
- **Performance bounds** — a scenario carries `"bounds": {"fps": …}`, so script-Ripples-vs-compiled-Ripples (§7's headline number) is a *enforced* bound that fails CI on regression, not a one-off measurement. The perf budget becomes a guardrail.
- **Two tiers for free** — every scenario runs in-process (desktop backend, the CI workhorse) *and* live against a real S3/P4 over REST (the native backend). The same JSON pins the language on desktop and the ISA backend on hardware — the cross-check the LED-driver doc gets from a logic analyzer, we get from running the *same* script on two backends and asserting the *same* buffer.

### 6.3 Why this compounds the staged plan

Each staging rung (§9.2) lands with its tests: the hello-world spike ships its parser + codegen + render-output unit tests and a "scripted random-pixel" scenario; the controls rung ships a control-binding scenario; Ripples graduation ships the golden-buffer assertion + the perf bound. So "test all live-script features back to back" isn't a phase bolted on at the end — **every increment is a tested increment**, and the suite grows rung by rung. By the time the engine is complete, every language feature, every binding, every role, and the scalability + hot-swap + robustness contracts are each pinned by a unit test or scenario that runs on every commit. That body of tests is also what lets the worker-bee agents implement against a pinned spec (the *Industry standards, our own code* method) — the behaviour is fixed by tests first, so the implementation is independent by construction.

## 7. Performance budget (16K LEDs × 50 FPS, ESP32-S3)

The frame budget at 50 FPS is **20 ms/tick** for everything (render + drivers + network + system). A 16K effect like Ripples touches each lit column once; Rainbow touches all 16K pixels. Reference points from `scenario_perf_full` (in [performance.md](../../reference/performance.md)): the *compiled* heavy effect (Noise) is ~50 ms at 16K on the S3 (≈20 FPS — already the bottleneck), the light effect (Checkerboard) ~8 ms (≈128 FPS). So:

- **Native script ≈ compiled.** A native-codegen script must land within a few % of the equivalent compiled effect — that's the whole point of native, and the spike's headline measurement (script-Ripples µs/tick vs compiled-Ripples µs/tick, same grid).
- **Float trig is the watch-item.** Ripples' per-column `sqrt`/`sin` dominate its cost; the S3 (LX7) has an FPU, so native float is real hardware ops — but the codegen must emit FPU instructions, not a soft-float call. The spike measures this directly.
- **Bytecode-VM comparison (the fallback's ceiling).** A VM pays per-opcode dispatch; at 800K px/s (16K×50) for an all-pixels effect that tax is the question the bottom-up flagged. The spike measures a VM path too (even a throwaway one) so the native-vs-fallback gap is a number, not a guess. If native holds and VM doesn't, native is vindicated; if both hold, the fallback is comfortable.

**Acceptance bar:** native script-Ripples within ~10% of compiled Ripples at 16K on the S3, float trig on the FPU, bounds-checking on, inside the 20 ms budget with headroom for drivers. Miss it → investigate codegen quality (the `__asm__` escape hatch + IR-lowering fixes) before falling back.

## 8. Hot-path do / don't checklist

**Don't** (in the generated code / the `run()` path):
- No heap allocation during `run()` — the script's data arena is allocated at compile/bind, reused every tick.
- No per-pixel function-call dispatch into the host for the common writers — `setRGBXY` lowers to inline loads/stores against the buffer, not a `call`.
- No soft-float where the FPU exists — emit FPU ops on S3/P4; integer-preferred for per-light color work (the project rule), float only for the wavefront math, exactly as the compiled effects do.
- No blocking — a script can't `delay`; a runaway loop hits the instruction budget.

**Do:**
- Bind the buffer pointer + geometry once per tick, then loop in native code (the identity-mapping fast path — the script writes `layer_->buffer()` directly).
- Insert bounds-checks at the IR so they're uniform and removable as one switch.
- Keep the host built-in library small, fixed, and inlinable.
- Provide the `__asm__` escape hatch for the hottest inner loop, so a power user is never capped by codegen quality.

## 9. Staged plan — the MoonLight tutorial ladder as the spine

### 9.1 Sequencing: depth-first vs broad-first → a hybrid

The first sequencing question is **depth-first** (build the whole engine on Xtensa through a real effect, *then* add the second ISA) vs **broad-first** (a trivial example on Xtensa *and* RISC-V early, then deepen both). Each retires a different risk:

- **Depth-first** is the fastest path to a visible bench result, but it defers the project's single biggest unknown — *does the IR seam genuinely decouple the front-end from the backend, or did it quietly leak Xtensa assumptions?* — until a second ISA is built, late. That is exactly the "back to the drawing board" dead-end the IR seam exists to prevent; validating it last is backwards.
- **Broad-first** over-corrects: it stands up the hardest part (a second codegen backend) before the language is even proven pleasant to write effects in — a lot of compiler plumbing before anything user-visible, and P4 is only the third target.

**The hybrid takes broad-first's one real insight without its cost.** Build a *complete vertical slice* on Xtensa just far enough to run the hello-world native (front-end → IR → Xtensa backend → `allocExec` → call), then **immediately prove a minimal second-ISA backend on that same slice** — *before* deepening to controls/math/2D/3D. A hello-world exercises only a sliver of the IR, so a second backend for *just that* is cheap, and it tests the load-bearing claim (the seam decouples) when fixing it is still cheap. Every later stage then rests on a seam that's been *demonstrated* to decouple, not trusted to. (Pragmatic note: the second ISA for this proof is **P4/RISC-V** if a P4 is on the bench, else the **desktop x86-64 backend** — a different ISA than Xtensa either way, so it validates the seam, and the desktop backend is useful anyway for host tests. P4 is the most on-target; desktop is the most convenient.)

### 9.2 The ladder

The [MoonLight effects tutorial](https://moonmodules.org/projectMM/moonlight/effects-tutorial/) is a ready-made *start-small-grow* curriculum (random pixel → control → trails → oscillators → 2D → 3D → audio → Cosmic Noise). Each rung is **one engine-capability spike** with a concrete acceptance bar. **RipplesEffect.h is the graduation test** (the hard real effect, after the 3D rung). Each step is a normal small commit; the multi-target part is sequenced per §9.1 — the *seam* is proven on a second ISA at Stage 0.5, but the *full* second backend (all stages) comes later, opportunistically.

**Next up: Stage 2 (buffer read-modify-write / trails).** Stages 0, 0.5 and 1 (Controls) are done — what landed and why is recorded in the git history and [lessons.md](../../work/past/lessons.md); this ladder tracks the remaining rungs.

| Stage | Capability proven | Acceptance bar (the spike) |
|---|---|---|
| **0. Load-bearing spike (Xtensa)** | Front-end → IR → **Xtensa** backend → `allocExec` → call. The hello-world: `setRGB(random16(N), blue)`. | Runs live on an **ESP32-S3**, lights a random pixel each tick, inside the frame budget; a deliberately-bad script degrades via cheap safety, no crash. **If this can't hit native speed, fall back to WASM/WAMR — a backend swap behind the IR.** |
| **0.5. Seam-proof (2nd ISA, early)** | The IR genuinely decouples: a *minimal* **RISC-V (P4)** backend — or the **desktop x86-64** backend — runs the *same* hello-world behind the *same* IR. | The same script through the same front-end/IR path renders **equivalent output on both backends** (same lit pixel each tick), and **the second backend is added without changing a line of the front-end or IR** — only new backend code. That "front-end + IR untouched" is the no-dead-end proof, paid for at hello-world cost. If the seam leaked (the IR needed backend-specific edits), fix it now — cheap, before anything is built on it. |
| **1. Controls** | Minimal-ceremony control binding (annotated var → `MoonModule` control + UI + persistence + live edit). | A `speed` slider appears, edits the running script live, persists across reboot. |
| **2. Buffer read-modify-write** | Trails / fade — read the buffer, fade, write. | The tutorial's trail effect runs; a moving dot with a fading tail. |
| **3. Math + time** | `float`/fixed math, `elapsed()`, oscillators (`beatsin`/`sin`). FPU codegen on S3. | A smooth oscillator effect; float trig measured on the FPU, within budget. |
| **4. 2D** | `width/height`, `setRGBXY`, 2D addressing. | A 2D tutorial effect at the real grid size. |
| **5. 3D** | `depth`, `setRGBXYZ`, 3D addressing. | A 3D tutorial effect. |
| **6. Graduation: RipplesEffect** | The full C-subset on a real hard effect — `sqrt`/`sin`/`floor`, `memset`, 3D, two controls — ported **near-verbatim** (§5.3). | Script-Ripples within ~10% of compiled Ripples at 16K on the S3. **Proves the language decision.** |
| **7. Full 2nd backend** | Deepen the second ISA from §0.5's minimal proof to the full stage-1–6 surface (Ripples on **P4**). | A tutorial step + Ripples run on the **ESP32-P4** via the RISC-V backend; front-end/IR untouched. |
| **8. WASM fallback backend** | IR → `.wasm` → WAMR. | A target with no native backend yet runs the same script through WAMR; one artifact, sandboxed. Validates the fallback is real. |
| **9. More roles** | `MoonLiveLayout`, `MoonLiveModifier`, `MoonLiveDriver`, core `MoonLiveModule` (sensor). | A scripted layout emits coordinates; a scripted modifier remaps; a core script transforms an `AudioFrame`. One engine, many roles. |
| **10. Editor + delivery** | Scripts in LittleFS, pushed via REST/WS, compile-on-device (and optionally compile-on-host/web for the portable artifact). | Author a script in the browser, push, see it live — the live-edit loop, MoonLight-native. |
| **Later targets** | ARM (Teensy), x86-64/ARM64 (desktop) full backends. | Each a new backend behind the IR, when the target is in scope. |

**Acceptance bar for every step:** runs on real hardware at the target grid, within the frame budget, surviving a bad script; the front-end + IR stay unchanged across backends (the no-dead-end invariant, *proven* at Stage 0.5); a compiled-vs-scripted timing is recorded. The fallback at any speed-failure is demote-to-WASM, never redesign.

## 10. Product-owner decisions (carried from the bottom-up, expanded here)

Settled (bottom-up § *Answers — product-owner direction*), expanded in the sections above:

1. **Native engine, our own** — §2. The standout; WAMR is fallback, not flagship.
2. **Xtensa-first, IR seam, no dead-ends** — §3.2, §9. Start small + beautiful; multi-target stays reachable.
3. **IR costs zero at run time** — §3.2. Equally fast as hpwit on Xtensa is non-negotiable; proven by instruction-diff + `__asm__` escape.
4. **C-subset, as close as possible, pragmatic simplifications** — §5. Body verbatim; ceremony supplied by the engine; not JS, not full C++.
5. **Minimal-ceremony controls** — §3.5. Declare the var, get the control.
6. **Staged safety** — §4. Cheap bounds+watchdog first; true sandbox deferred to the WASM fallback.
7. **MoonModule-first** — §3.3, §3.4. A scripted module is a `MoonModule`; the binding is the value-add.
8. **General core + specific light, effect first** — §3.3, §9. Tutorial hello-world is the spike; Ripples is the graduation test; other roles follow.
9. **Infinitely scalable** — §3.7. As many scripts as memory allows, PSRAM-exploiting; each `MoonLive` self-contained, ceiling = free heap.
10. **Inline execution by default, task the exception** — §3.8. A scripted effect runs in the `Scheduler` tick like a compiled one (consistent, near-zero overhead — what makes scalability real, no cross-thread sync); a pinned task is the narrow opt-in for a long/blocking core script.
11. **Sequencing: hybrid** — §9.1. Full Xtensa vertical slice to hello-world, then prove the IR seam on a second ISA *early* (Stage 0.5) before deepening.
12. **Domain-neutral engine core, thin binding** — §3.9. Engine never depends on MoonLight; the clean layering is what makes MoonLight-as-a-library optimal, and is never compromised for any separability.

## 11. Prior art & credits

This design stands on work others did first; per *Industry standards, our own code*, we study it, credit it by name, and write our own.

- **ESPLiveScript — Yves Bazin (hpwit).** The native-codegen approach this whole redesign builds on is his. ESPLiveScript demonstrates the thing that matters most here: a from-scratch C-like compiler — a complete tokenizer, parser, register allocator, Xtensa code generator, and a save/load compiled-binary path, header-only — that JIT-emits native Xtensa and runs a script at near the speed of hand-written C++ on an ESP32 (85 fps on a 12,288-LED panel where Lua managed 3 and Gravity 10). Our redesign's central choice — go native, not interpreted — is taken directly from that result; what we add is the IR seam (so the same approach reaches RISC-V/ARM/desktop) and the MoonModule binding. Where the bottom-up notes structural costs (global state, no IR, large files), those are observations a *rewrite for a different goal* must make, not a verdict on the original — ESPLiveScript does its job, fast, and is the reference precisely because it works.
- **ARTI-FX / ARTI — ewowi.** The author of this analysis also wrote ARTI-FX (the interpreted-effects runtime in WLED MoonModules, on the PEG-grammar ARTI interpreter). It is the prior MoonLight-family answer to the same problem and the source of hard-won lessons carried here: the `renderFrame`/`renderLed` per-frame/per-LED split, the host-binding shape (`arti_external_function` / `arti_*_variable`), and — by being the AST-walking, double-everything design — the concrete demonstration of *why* the hot path wants native or VM execution rather than tree-walking. ARTI-FX proved the live-scripting *idea* works end-to-end in this ecosystem (load a script, run it, edit live); this redesign trades its interpreter for native speed, but inherits its product shape and its lessons.
- **MoonLight — MoonModules.** The [effects tutorial](https://moonmodules.org/projectMM/moonlight/effects-tutorial/) is the staging spine of §9, and its `setRGB`/`setRGBXY`/`setRGBXYZ` + `addControl` surface is the model for the host binding (§3.4–3.5).

Credits also live in the bottom-up's *Prior art & credits* and the digest [friend-repos/hpwit-ESPLiveScript.md](../../friend-repos/hpwit-ESPLiveScript.md).

### Public credit — to lift into `docs/moonmodules/core/MoonLive.md` when the module spec is written

The credits above are the analysis's internal record. The block below is the **user-facing** version for the eventual `MoonLive.md` "Prior art" section. Drop it in when MoonLive ships; matches the house style of the other modules' Prior-art sections (e.g. AudioService.md, [MoonI80Peripheral.md](../../moonmodules/light/moxygen/MoonI80Peripheral.md)).

> MoonLive's native-codegen approach — compile a small C-like language straight to machine code and call it as a function, so a live-authored effect runs at near hand-written speed — was pioneered by **Yves Bazin (hpwit)** in **[ESPLiveScript](https://github.com/hpwit/ESPLiveScript)**: a from-scratch tokenizer, parser, and Xtensa code generator that drives a 12,288-LED panel at ~85 fps where interpreted languages (Lua, Gravity) managed 3–10. That result is what makes "go native, not interpreted" the right call, and ESPLiveScript is the reference MoonLive is built against — studied closely, credited, and written fresh against MoonLight's architecture, never copied, per [*Industry standards, our own code*](../../CLAUDE.md#principles). MoonLive carries the idea forward where ESPLiveScript stops: a multi-ISA backend behind an IR seam (Xtensa, then RISC-V / ARM / desktop) and a binding that makes a script a first-class MoonModule.
>
> The live-scripting idea in this ecosystem also descends from **ARTI-FX / ARTI** (the interpreted-effects runtime in WLED MoonModules), which proved the load-a-script-and-run-it-live loop end-to-end and contributed the `renderFrame`/`renderLed` shape and the host-binding model MoonLive learns from.


## 12. Reconciliation with the bottom-up

- **Strong agreement.** The bottom-up's recommendation (native, Xtensa-first, IR seam, WASM fallback, staged safety, C-subset) is carried whole — this doc is its expansion into an architecture + plan, not a revision.
- **What this doc adds.** The concrete `MoonLive`/`MoonLiveHost`/`MoonLiveEffect` API; the three-tier diagram with the IR seam placed; the host-binding surface mapped to the real `EffectBase`/`Layer` accessors; the perf budget against `scenario_perf_full` numbers; the RipplesEffect side-by-side; and — the key staging insight — **the MoonLight tutorial ladder as the spike spine**, with Ripples demoted from "first demo" to "graduation test."
- **One refinement vs the bottom-up.** The bottom-up implied Ripples as the first scripted-effect demo; this doc corrects that — Ripples is too complex for hello-world. The tutorial's `setRGB(random16, blue)` is the **spike** (the first thing the engine runs end-to-end); Ripples is the **language-fidelity exam** after the 3D rung. (Separately: that same `setRGB(random16, blue)` line is, by coincidence, also the input the `fix-warnings` fork crashed on — so it doubles as the first compile-time **regression case** for parser validation, §4. Two roles, one line: the spike proves the happy path runs; the regression test proves the malformed-AST path is caught.)

## Out of scope (deferred to implementation)

The exact annotation syntax for controls/role; the compiled-artifact binary format; the web-editor UI; the per-target benchmark numbers (produced by the spikes, not predicted here); the precise instruction-budget tuning; the choice of which built-ins ship in the first library. These are settled in the spikes, concrete-first, not up front.
