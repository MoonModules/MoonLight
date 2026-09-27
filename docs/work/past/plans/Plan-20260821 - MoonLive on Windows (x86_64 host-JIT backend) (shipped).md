# Plan — MoonLive on Windows (x86_64 host-JIT backend)

## Context

MoonLive is MoonLight's on-device JIT: user-authored effect / modifier / layout scripts compile to native code and run in the pipeline. Today only `__aarch64__` desktops have a working host backend — `MM_MOONLIVE_HAS_HOST_JIT` is 0 on x86-64 Windows, x86-64 Linux, and Intel macOS. On those targets `MoonLiveCompiler::compileSource` fails cleanly and scripted modules render dark: no crash, no MoonLive.

The PO's dev machine is Windows-x86_64, so the entire scripting subsystem is dormant for them. This branch closes `docs/backlog/backlog-light.md:295-307` ("MoonLive has no x86-64 backend"). It also unlocks two free wins as side effects — SysV x86_64 covers Linux and Intel macOS.

The backend seam already exists: the shared IR walk in `src/core/moonlive/moonlive_lower.h:23-31` binds to whichever assembler the target provides, and three concrete implementations (arm64 host, RISC-V, Xtensa) prove the contract. Adding x86_64 is one more assembler behind the unchanged IR, per `src/platform/desktop/moonlive_asm_host.h`'s own comment ("host ISA (arm64 / x86-64)"). Windows exec-memory allocation is already implemented at `src/platform/desktop/platform_desktop.cpp:183-224` (`VirtualAlloc` + PAGE_EXECUTE_READWRITE + `FlushInstructionCache`).

## Approach

Extend `src/platform/desktop/moonlive_asm_host.cpp` with an `#elif __x86_64__` branch that provides the same class body, splitting on `_WIN32` internally to switch between Microsoft x64 (Windows) and SysV (Linux/Intel-Mac) ABIs. This mirrors the arm64 layout — same header, same class, same test surface. Both ABIs land in this branch, Windows first (the PO's dev target). Ship the Win64 hand-blob for `emitFill`/`emitAnimatedFill` next to the SysV blob that already exists at `src/platform/desktop/moonlive_emit.cpp:86`. Add the `x86_64` entry to the disassembly tooling. Close the backlog entry.

**Why not:**
- **Windows-only backend** — same assembler carries SysV essentially for free (shared encoding tables; only register-map + `call()` argument shuffle differ); skipping SysV leaves Linux/Intel-Mac coverage dark and forfeits a straight-line win.
- **asmjit / xbyak / DynASM dependency** — violates the "one uniform building block, hand-rolled per ISA" principle the other three backends set; would introduce a vendored code path unlike anything else in the codebase.
- **Interpreter fallback** — misses the point (JIT parity is the goal), 10–100× slowdown wrong for the perf envelope.

## Implementation order

Each step compiles clean and either changes visible test-suite counts or lands a discrete unit.

1. **Widen the arch guards** (~10 LOC, 3 files) — this alone makes the test suite start attempting MoonLive on x86_64 and gives us a live failure signal that pinpoints missing pieces as they're implemented.
   - `src/platform/desktop/platform_config.h:148-152` — widen `MM_MOONLIVE_HAS_HOST_JIT` to include `__x86_64__`.
   - `src/platform/desktop/moonlive_asm_host.cpp:13` — widen `#if` to `(__aarch64__ || __x86_64__) && !MM_MOONLIVE_FORCE_NO_HOST_JIT`, add a stub `#elif __x86_64__` branch that compiles empty for now.
   - `src/platform/desktop/moonlive_lower_host.cpp:12` — same guard widening.

2. **x86_64 register map + `prologue`/`epilogue`/`ret`** (Win64 ABI). Register map: R0–R4 → `rcx/rdx/r8/r9` + a spill for the 5th arg (Win64 puts arg 5+ on the stack; kArg4 = ctrls-arena lives at [rsp+8*4+32] before shadow-space adjustment). R5–R13 → caller-saved scratch (`r10/r11/rax` + volatile xmm mirrors as needed). Frame: `push rbp; mov rbp,rsp; sub rsp, N+32` (32-byte shadow space for later `call`s). One-op tests exercising just `ret` start passing.

3. **`movImm` / `movPtr` / `movReg`** — full-width immediate via `movabs r64,imm64` (10 bytes, direct arm64 `movz+movk×3` analog), 32-bit imm via `mov r32,imm32`, register move via `mov r64,r64`. First fill-shape tests (const + store) start progressing.

4. **`addImm` / `addReg` / `mulImm` / `mulReg`** — arithmetic. `imul r64,r64` and `imul r64,r64,imm` for the multiplies. Coverage of tests that compute pixel indices progresses.

5. **`store8` / `load8` / `store16` / `load16` / `load8Idx` / `load16Idx` / `slotAddr` / `spillStore` / `spillLoad`** — memory ops using SIB `[base+index]` addressing and `[rbp+disp]` for spills. Most engine-level fill tests pass by end of this step.

6. **Labels + `branchIfZero` / `branchGeU` / `branchNe`** — always emit rel32 conditional branches (`0x0F 0x8x` + imm32) so the fixup table matches arm64's fixed-width story. Reuse `FixKind::Branch` (imm19 in arm64) — for x86_64 it's imm32 at a known offset from the fixup site. Loop-containing tests start passing.

7. **`call(Reg d, Reg a, Reg b, Reg c, const void* fn)` — Win64 first.** Save the whole vreg pool (live-across-call contract), shuffle a/b/c into `rcx/rdx/r8`, materialize `fn` into `rax` via `movabs`, `call rax`, capture return in stashed reg, restore vregs. Every step accounts for the 32-byte shadow space Win64 requires below the call site. All host-builtin-calling tests pass on Windows.

8. **`call(...)` SysV variant switched on `_WIN32`.** Same save/restore logic, args to `rdi/rsi/rdx/rcx`, no shadow space. Linux/Intel-Mac test coverage catches up.

9. **`callLabel` + `FixKind::Call`** (imm32 rel32 in `E8 xx xx xx xx`). Reloads parked host-arg regs from frame slots before the call, same as arm64. Script-to-script call tests pass.

10. **Win64 hand-blob for `emitFill` / `emitAnimatedFill`** in `src/platform/desktop/moonlive_emit.cpp` — mirror the SysV branch at line 86, wrapped in `#elif defined(__x86_64__) && defined(_WIN32)`. ~50 LOC.

11. **New codegen encoding test:** `test/unit/core/unit_moonlive_codegen_x86_64.cpp`, patterned on `unit_moonlive_codegen_riscv.cpp` + shared `moonlive_device_codegen.inc` / `moonlive_structural.h`. Pins the emitted bytes for each named instruction so wire-format regressions are caught immediately.

12. **Tooling:**
    - `moondeck/moonlive/disasm.py` — add `"x86_64"` entry in the `ISAS` table (lines 36-50); pick a disassembler backend (capstone via pip is the obvious choice, matches how arm64 goes).
    - `moondeck/moonlive/emit_isa.cpp` — matching case so `disasm.py` can drive the x86_64 encoding.

13. **Docs & backlog:**
    - `docs/backlog/backlog-light.md:295-307` — move entry to a shipped-item note or delete per docs model (backlog shrinks under mandatory subtraction).
    - `docs/explanation/architecture/index.md:461-475` — if the wording "arm64-only host backend" appears, update to "arm64 or x86_64 host backend".
    - This plan file lives on the branch, becomes the PR description, is deleted once realized (per CLAUDE.md's plan-lifecycle rule).

## Files touched (grouped)

**Runtime (platform layer):**
- `src/platform/desktop/moonlive_asm_host.cpp` — major addition (~600–800 LOC in the x86_64 branch).
- `src/platform/desktop/moonlive_asm_host.h` — no changes expected; the neutral interface already fits.
- `src/platform/desktop/moonlive_lower_host.cpp:12` — guard widening.
- `src/platform/desktop/platform_config.h:148-152` — guard widening.
- `src/platform/desktop/moonlive_emit.cpp` — Win64 branch (~50 LOC).

**Tests:**
- `test/unit/core/unit_moonlive_codegen_x86_64.cpp` — new file, mirrors riscv/xtensa encoding tests.

**Tooling:**
- `moondeck/moonlive/disasm.py` — ISAS entry.
- `moondeck/moonlive/emit_isa.cpp` — matching case.

**Docs:**
- `docs/backlog/backlog-light.md` — subtract shipped entry.
- `docs/explanation/architecture/index.md` — factual update if needed.

## Verification

**Per stage (every commit-worthy chunk):**
- `cmake --build build` — zero warnings.
- `ctest --test-dir build --output-on-failure` — pass count on `MoonLive*` suites should rise monotonically; regressions on non-MoonLive tests are a stop.

**End-to-end after step 11:**
- Full `ctest --test-dir build --output-on-failure` — every `test/unit/core/unit_moonlive_*.cpp` and `test/unit/light/unit_MoonLive*.cpp` runs (no more `MM_MOONLIVE_HAS_HOST_JIT` skips on Windows) and passes.
- `uv run moondeck/scenario/run_scenario.py` — scenario tests that exercise scripts end-to-end succeed.
- `uv run moondeck/moonlive/disasm.py --isa x86_64 <sample.script>` — the disassembler round-trips a real script.

**Bench / PO test (the final guardrail per CLAUDE.md):**
- `uv run moondeck/build/build_desktop.py` then run the desktop binary on the PO's Windows machine with a scripted effect loaded — invite the PO to see the effect render live (not dark) and confirm behavior matches the arm64 reference by loading the same script on a Mac.
- Once the PO confirms bench-visible correctness, run `uv run moondeck/event/precommit.py` for the gate report.

**Guardrail against ABI drift:**
- `unit_moonlive_codegen_x86_64.cpp` pins expected bytes for each named instruction under both `#ifdef _WIN32` and `#ifndef _WIN32` branches; a wrong ABI shuffle inside `call()` fails this test before it fails a scenario.
