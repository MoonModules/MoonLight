# Testing

What we test and how. The detailed inventory of every test lives in two auto-generated files:

- **[Unit tests](tests/unit-tests.md)**: one row per `TEST_CASE`, grouped by module. Generated from `test/unit/{core,light}/unit_*.cpp`.
- **[Scenario tests](tests/scenario-tests.md)**: one section per scenario JSON, grouped by module. Generated from `test/scenarios/{core,light}/scenario_*.json`. Each scenario runs in two tiers (in-process + live) unless flagged `live_only` (construct-mode scenarios are also skipped on live, since `main.cpp` owns the live shape).

Both are produced by `moondeck/docs/generate_test_docs.py`; the source of truth is the test files themselves (see [Adding Tests](#adding-tests) below).

## Testing strategy

Three test categories, each with a clear purpose:

- **Unit tests** (desktop, `test/unit/{core,light}/unit_*.cpp`), exercise individual MoonModules in isolation with doctest. Each test file declares `// @module <Name>` so it's categorised under that module in the generated inventory. Run via `ctest` or `./build/<host>/test/mm_tests` (`<host>` = `macos`/`linux`/`windows`, the per-host build dir). Verify a module's API, edge cases, and output independent of how it's wired into a pipeline.
- **In-process scenarios** (desktop, `test/scenarios/{core,light}/scenario_*.json`), exercise the system as an integrated pipeline. Each scenario is a declarative JSON file with a sequence of steps (`add_module`, `set_control`, `measure`) and optional performance bounds. The scenario runner (`test/scenario_runner.cpp`) replays the steps in-process and reports tick + heap per `measure` step. Same JSON files run against a live device through the HTTP API, that's the next tier.
- **Live scenarios**: the same scenarios driven against a running device over REST. See [Live scenarios](#live-scenarios) below.

A live run is worth watching once, because it is the tier that proves the device rather than a model of it. The suite walks the cards in the order the interface lists them, and every step happens through the same API the page uses:

<video src="../assets/uiscenarios/04-scenario-testing.webm" autoplay loop muted playsinline width="720" title="The scenario suite driving a running device, card by card, while the interface shows it happening"></video>

**Picking a tier for a new test.** When the behavior you want to pin only makes sense with modules wired together (e.g. "the pipeline reallocates cleanly when the grid resizes," "Drivers correctly hands the source buffer through after a child swap"), reach for a scenario first: that is what scenarios are *for*. When the behavior lives inside a single module (one function's contract, one edge case, one bug regression on a small surface), a unit test is the cheaper and faster fit. Don't extend the scenario runner with new predicates just to migrate an existing unit test, which is adding abstraction without an active need. Add predicates when a *new* scenario you're writing needs them.

**Regression rule:** when a bug is found, the fix includes a new unit test or scenario that reproduces the bug. A comment in the test references the root cause so the connection stays traceable.

**Performance checks** verify architectural rules at runtime:

- **Zero-allocation render loop**: N frames, intercept `malloc` / `free`, fail if any allocation occurs during steady-state rendering.
- **Frame time bounds**: scenarios include `"bounds": {"fps": {"min_pct": N}}` (live, relative to baseline), `"bounds": {"fps": {"min": N}}` (absolute), or `"bounds": {"fps": {"min_fps_led_product": N}}` (throughput floor that scales to grid size).
- **Heap-delta bounds**: scenarios include `"bounds": {"heap": {"max_delta_bytes": N}}` on a measure step to assert this step grew the heap by no more than N bytes vs the previous measure. On desktop `freeHeap()` returns 0 (unlimited), so the assertion is effectively a no-op there; it lights up on ESP32 where heap is real.

Per-module timing, memory, and sizeof measurements per platform live in [performance.md](performance.md).

## Static analysis

Tests pin behavior that runs; static analysis catches what never gets exercised. Five layers, cheapest first, each catches something the layer above cannot:

| # | Layer | Catches | Where |
|---|---|---|---|
| 0 | Compiler warnings | shadowing, double promotion, fallthrough, null deref | Every build, all targets |
| 1 | [clang-tidy](../.clang-tidy) | bug patterns, performance, portability | Editor (clangd) + MoonDeck card |
| 2 | Sanitizers (ASan, TSan) | real memory/threading faults at run time | Desktop CI lanes |
| 3 | RTSan + `[[clang::nonblocking]]` | allocation/blocking in the render path, **transitively** | Compile time + CI |
| 4 | [CodeQL](../.github/codeql-config.yml) | untrusted input, whole-program taint, use-after-free | CI, Security tab |

Alongside them: **lizard** counts complexity per commit for the trend (a fuzzy tokenizer, not a parser, it can never express an architectural rule), **clang-query** is the home for bespoke AST rules we invent, and the Python checks in `moondeck/check/` cover contracts whose other half is a Markdown page, a JSON catalog or a built binary. Every one has a MoonDeck card ([MoonDeck.md](../moondeck/MoonDeck.md)).

**Every one of these is a report, not a gate.** They state what they find; a consumer decides
what to do about it. `WarningsAsErrors` is empty, CodeQL never runs on `pull_request`, and the hot-path check never fails the event. A gate nobody can satisfy gets disabled rather than obeyed, and it pushes people to suppress a finding under time pressure, which is the opposite of why the tool is there. The exception is layer 0: compiler warnings ARE `-Werror`, because they are few, actionable, and fixed at the moment they appear.

**One rule, one owner.** Each rule is enforced in exactly one place, so a finding has one home
and cannot drift between two tools that half-agree:

| Rule | Enforced by |
|---|---|
| No allocation/blocking in the render path | `[[clang::nonblocking]]` + RTSan |
| No platform code outside `src/platform/` | `check_platform_boundary.py` |
| Specs match the code | `check_specs.py` |
| Catalog matches the modules | `check_devices.py` |
| Untrusted input is memory-safe | CodeQL |
| Bug patterns / performance | clang-tidy |
| Complexity does not grow | lizard (baselined) |
| Size/LOC/docs do not grow silently | `repo_health.py` |

### Verify a zero before believing it

An analyzer reporting "0 findings" is indistinguishable from one that read nothing, and every silent-failure mode below produced a plausible clean report rather than an error:

| Defect | What it looked like |
|---|---|
| `#` inside a YAML `>-` folded scalar is not a comment | Every disable after the first was folded into the check string; `abseil-*` stayed on → 12,181 findings |
| `run-clang-tidy` shells out to `clang-tidy` by name | Not on PATH → exits 0 having analysed nothing → "0 findings" |
| `-extra-arg VALUE` (space form) is silently ignored | Only `-extra-arg=VALUE` works → 0 findings |
| Same trap in `-checks` | The filter had never worked |
| Compilation database records a different compiler than the tool runs | `'cstdint' file not found` on 129/129 files; unparsed files are never analysed |
| Missing `-isysroot` | Under-report, not an error: 5 matches where there were 14 |
| A baseline keyed on a name the tool no longer emits | Pins nothing while looking green (see backlog-core.md § lizard) |

So: **run a control check that MUST fire.** After reaching 0, enabling a deliberately-disabled check (`--check readability-magic-numbers`) returned 3,307, that is what proves the pipeline reads the code. `check_clang_tidy.py` also refuses to report when more than ten files fail to compile, because "most files errored" is a broken run, not a result.

A tool's *default* output is not its verdict, either. Evaluating an analyzer without a curated check list measures the configuration, not the tool: an unconfigured clang-tidy run here gave 6,073 findings, two thirds of them from one check that every comparable project disables by name. The curated config takes it to 30.

### A passing test is not evidence until it can fail

A test written to pin a fix is shaped by that fix, so it agrees with the fix whether or not either is right. Three in one session ran, passed, and could not have failed: a MoonLive indexing test that still passed with the emitted shift sabotaged from `<<2` to `<<3` (one array cannot show a wrong offset, because every wrong offset lands on something that array wrote); three scenarios green while their scripts failed to compile, placed no lights, and recorded 0 on every measure; and a golden that pinned a length rather than a behavior, freezing a broken encoding as correct for two weeks.

**The check is mechanical: after a test passes, break the thing it tests and confirm it fails.** Not for every test, but for any test written to pin a fix.

### A suite that stopped testing looks exactly like one that works

Both scenario runners report a scenario that did not run and one that ran clean, and for a while they reported them the same way.
A skip returned the pass code, so ten of eleven scenarios skipped for a missing fixture while the summary read `11 passed`.
The three assertion bugs they would have caught stayed in for a commit.

Two rules close that, and they are properties of the runner rather than of any scenario:

- **A skip is counted as a skip.** The summary reads `N scenario(s), P passed, F failed, S skipped`, so a suite that stopped covering something says so.
- **A scenario that asserts nothing fails.** Zero checks is indistinguishable from every step silently doing nothing, so the runner fails it and names the step count.

### A test that does not reproduce the user's conditions proves nothing

A green result means something only if the test could have gone red.
An agent's shell is a bad witness: it routinely runs with policies, permissions and paths no user has.
A PowerShell script tested fine and would have shipped broken, because the agent's own shell had set `Process: Bypass` over the `RemoteSigned` a user has.
A Defender false positive was declared cleared because a download succeeded, using a different client than the one still being blocked.

**Name what would have to be true for the test to fail, and confirm that condition is present.** Print the setting the test depends on rather than inferring it from the outcome.

## Standards

Two principles drive every standard below:

1. **The test file is the source of truth.** Every fact about a test (which module, which scenario, what each case verifies, what each step does) lives in the test file itself. The generated docs and MoonDeck views read from there, they don't get edited by hand.
2. **One pattern, easy to spot in 30 seconds.** Every unit test follows the same header shape. Every scenario follows the same JSON shape. A new contributor recognises an existing test, then writes the next one by analogy.

### File layout

```text
test/
├── CMakeLists.txt
├── scenario_runner.cpp                       # replays scenarios in-process
├── unit/
│   ├── core/      unit_<Module>[_<topic>].cpp        # mirrors src/core/
│   └── light/     unit_<Module>[_<topic>].cpp        # mirrors src/light/
├── scenarios/
│   ├── core/      scenario_<Module>_<topic>.json     # mirrors src/core/
│   └── light/     scenario_<Module>_<topic>.json     # mirrors src/light/
├── python/        test_<topic>.py                    # host-side: MoonDeck / build-script logic
└── js/            <topic>.test.mjs                    # host-side: mooninstaller logic
```

A test lives under the subfolder of its **primary** `@module`'s source domain (e.g. `Layer` lives in `src/light/`, so `unit_Layer_extrude.cpp` goes in `test/unit/light/`). Cross-domain awareness travels through the `@also` list, not the directory. There's no `platform/` subfolder today, `src/platform/` is a pure abstraction layer whose desktop backend every unit test implicitly exercises; ESP32 platform code never runs on the desktop, so there's nothing to put there yet.

### Host-side tests (Python + JS)

The C++ `ctest` / scenario suites can't reach the **Python** (MoonDeck, build scripts) or **JS** (web installer) code, so those get their own host-side unit tier, `test/python/` (pytest, run `uv run --with pytest --with pyserial pytest test/python`) and `test/js/` (Node's built-in runner, run `node --test "test/js/**/*.test.mjs"`; no `package.json`/`npm install`). Both run in `.github/workflows/test.yml` on every PR and are commit gates (CLAUDE.md Event 1, gate 10) when `moondeck/` / `mooninstaller/` / the test dirs change. Python test files carry their deps in a PEP-723 `# /// script` block (the repo's convention, there's no central `pyproject.toml`); pyserial is a dep only because `improv_provision.py`'s import guard exits without it, not because the frame logic needs it.

#### What's covered today: the Improv frame wire format

The Improv serial frame is implemented **three times**, device C++ (`src/core/ImprovFrame.h`), Python (`moondeck/build/improv_provision.py`), and installer JS (`mooninstaller/improv-frame.js`), so a drift in any one silently breaks provisioning. Both suites assert the **same golden vector** so the Python and JS builders provably agree (hand-verified against the C++ sum-mod-256 checksum):

```
buildImprovFrame(type=0x03, payload=[0x01])  ==  49 4d 50 52 4f 56 01 03 01 01 e3
                                                  └IMPROV┘ v  t  l  p  checksum
```

**`test/python/test_improv_frame.py`** (pytest), pins the *shared envelope* that MoonDeck and the provisioning scripts speak (`build_frame` / `checksum` in `improv_provision.py`):

| Test | Pins |
|---|---|
| `test_checksum_is_sum_mod_256` | checksum is `sum(bytes) & 0xFF`, empty → 0, `0xFF 0xFF` → `0xFE` (wraps) |
| `test_frame_layout` | `IMPROV` magic, version 1, type, length, payload, total length |
| `test_golden_vector_g1` | the exact 11-byte golden frame above (the cross-impl anchor) |
| `test_checksum_covers_header_through_payload` | checksum spans header→payload, excludes the checksum byte itself |

**`test/js/improv-frame.test.mjs`** (node:test), pins the same envelope *plus* the `APPLY_OP` chunking that only the installer JS and device C++ implement (Python's provisioning path does WIFI_SETTINGS, not config push):

| Test | Pins |
|---|---|
| frame layout | magic / version / type / length / payload / checksum positions |
| checksum is sum-mod-256 | matches Python + C++ |
| golden vector G1 | the same 11-byte frame as the pytest anchor |
| G2, small APPLY_OP `set` is a single frame | `[0xFC][seq=0][last=1]` header + the op JSON byte-identical in the payload |
| G3, a >125-byte op chunks into ordered frames | two frames, `seq` 0→1, `last` 0→1, the 125-byte chunk boundary, reassembly reproduces the op JSON exactly |
| at-least-one-frame | an empty op still emits one frame with `last=1` (so `last` always sends) |

The JS suite proves the installer *chunks* an op correctly; the **device side that reassembles those chunks** is pinned by the C++ `unit_ImprovOpReassembler` suite (`src/core/ImprovOpReassembler.h`, the pure state machine behind the device's `APPLY_OP` handler, extracted from `platform_esp32_improv.cpp` so it's desktop-testable). It covers the full receive contract: in-order multi-chunk reassembly + NUL-termination, **duplicate-chunk rejection** and **out-of-order/skipped-seq rejection** (the guard against an installer retry corrupting the buffer), **overflow** rejection at the buffer-minus-NUL boundary, mid-stream `seq 0` abandoning a partial op, and clean recovery after every error. Encode (JS) + reassemble (C++) together prove APPLY_OP end to end without hardware.

**`test/python/test_installer_manifests.py`** (pytest), pins the web installer's per-release file contract. For every `ships: true` firmware in `mooninstaller/firmwares.json` it runs `moondeck/build/generate_manifest.py` (with a synthetic `flasher_args.json`, so no firmware build is needed) and asserts the manifest is valid (a `chipFamily` + non-empty `parts[]`) AND that **every part filename matches one of the globs the release workflow stages onto Pages** (`firmware-*.bin` / `shared-*.bin` / `partition-table-*.bin`, `shared-*.bin` covers the plain ota-data plus a MoonBase manifest's `shared-ota-data-slot0.bin` and `shared-moonbase-<chip>.bin`). A manifest that names a file outside those globs points at something the deploy never stages → the installer 404s at fetch-firmware (the failure that shipped a broken v2.0.0 installer). The test guards the manifest-generation ↔ staged-files contract; the *deploy mechanics* that stage them (per-tag, in `release.yml`) are workflow shell logic a unit test can't reach, so the two are complementary.

**`test/python/test_check_specs_drift.py`** (pytest) pins the two spec-drift guards in `moondeck/check/check_specs.py` (the spec-check commit gate). Some facts live in both the `.h` and the module doc in different forms: a control's **numeric range** (`addControl("floor", floor, 0, 255)` vs the prose "noise floor (0–255)") and the **author/source URL** (`// Author: …, <url>` vs the `Origin:` markdown link). Neither can be single-sourced, they're the same fact for two audiences, so instead the gate *validates* them: if the doc restates a control's range and it conflicts with the `.h`, or an `.h` author URL is missing from the doc, the spec check flags it. The checks are block-scoped on the consolidated catalog pages (a control name shared across modules, `fps`/`fadeRate`, matches only its own module's block), and tolerant of the human range spellings (`1–8` / `1-8` / `1 to 8`): this suite pins both the catch and the no-false-alarm behavior.

MoonDeck's pure logic (catalog reverse-lookup, state migration) and the installer's op-walk / storage are the next host-side candidates as they accrete regression risk.

### Naming convention

- **Unit tests:** `unit_<ExactModuleName>[_<topic>].cpp`, `<ExactModuleName>` is the **CamelCase** class name as it appears in `// @module` (and in the source: `Layer`, `MoonModule`, `MultiplyModifier`, `NetworkSendDriver`). The optional `<topic>` collapses when the file's the only test for its module (`unit_Color.cpp` is fine if `@module Color`); add it when one module has several test files (`unit_Layer_extrude.cpp`, `unit_Layer_zero_grid.cpp`, …) or when the topic genuinely clarifies what the file covers (`unit_FilesystemModule_persistence.cpp`).
- **Scenarios:** `scenario_<ExactModuleName>_<topic>.json`, same module-naming rule; the topic is always present because scenarios always cross multiple modules and the topic distinguishes the focus.
- The **`"name"` field inside each scenario JSON** matches the filename stem exactly (e.g. `"name": "scenario_Layer_base_pipeline"`). The runner, the MoonDeck dropdown, the generated docs and `--name` on the CLI all use this single identifier.

### Unit-test file shape

Every `unit_*.cpp` file starts with **`// @module <Name>`** as its only required metadata header, then the includes, then the TEST_CASEs. Each `TEST_CASE` gets a single `//` line of end-user description on the physical line directly above it. Reading that line should tell someone what the case verifies without opening the body.

```cpp
// @module Color

#include "doctest.h"
#include "core/color.h"

// Hue 0 is pure red.
TEST_CASE("hsvToRgb red at h=0") {
    auto c = mm::hsvToRgb(0, 255, 255);
    CHECK(c.r == 255);
    ...
}

// Zero saturation produces a grey of the given value, regardless of hue.
TEST_CASE("hsvToRgb white when saturation is zero") {
    ...
}
```

Header annotations recognised by the parser:

| Tag | Required? | Shape |
|-----|-----------|-------|
| `// @module <Name>` | **yes** | Exactly one. CamelCase, must match a real module class. |
| `// @also <A>, <B>, ...` | optional | Comma-separated peer modules this test also exercises. Surfaces as “Also touches” in the generated docs and lets the MoonDeck module filter include the test for either domain. |
| `// @description <one-line>` | optional | Short file-level summary. Multiple `@description` lines concatenate. Use only when the tests don't speak for themselves, usually omit. |

Per-`TEST_CASE` description rules:

- **One physical line above the `TEST_CASE`**, no hard-wrapping; the generator and MoonDeck handle layout. A second line is allowed only when the case does something genuinely non-obvious.
- **Missing description** → the generator italicises the raw `TEST_CASE("…")` name in its place.

### Asserting a value, and what survives a restart

Two ops exist for the things a measurement cannot see.

**`expect_control`** asserts a control reads what the scenario says it must, and is the only op that fails a scenario on a value rather than on a timing contract:

```json
{ "name": "the-prefix-is-what-ships", "op": "expect_control", "id": "Mqtt", "key": "topicPrefix", "equals": "MoonLight/563cfe" }
```

It exists for a string some other system keys on, where a change breaks a contract no compiler and no timing measurement can see.

`not_equals` is the same assertion negated, for a value that moves every release where the only stable claim is a negative one:

```json
{ "name": "a-version-is-reported", "op": "expect_control", "id": "Firmware", "key": "version", "not_equals": "" }
```

An empty string is what a read-only control renders when its source is missing.
So `not_equals: ""` asserts that a card reports anything at all, and it holds on every target and every release.

Both tiers compare against the value as the API renders it, so `"equals": 180`, `"180"` and `true` all read the way a client would see them.
They reach it differently, which is worth knowing before asserting an unusual type.
In-process the runner calls `writeControlValue`, the serializer the HTTP layer uses; the live runner reads the parsed JSON back from `/api/modules/<id>`.
The two agree on every scalar a control holds.
A `List` or a long `TextArea` is where they would part company, since the in-process render goes through a fixed buffer.
Assert those through a scalar the list drives rather than through the list itself.

**`reboot`** restarts the device and waits for it to answer, so a later `expect_control` proves what survived rather than what is merely still in memory:

```json
{ "name": "restart", "op": "reboot", "timeout": 60 },
{ "name": "it-survived", "op": "expect_control", "id": "System", "key": "deviceName", "equals": "MM-Bench" }
```

On a board that is the reboot the endpoint performs. On a desktop the endpoint exits the process and nothing restarts it, so the live runner relaunches the binary with the data directory the exiting instance was using: a restart that came back on different files would prove nothing. In-process the op skips, because the scheduler is the process and exiting it would end the run.

### Scenario modes (construct vs mutate)

Every scenario carries a top-level `mode` field that says what shape the scenario expects the world to be in. Two values:

- **`"mode": "construct"`**: the scenario builds the pipeline from an empty scheduler. Lots of `add_module` steps; the first `measure` happens after everything is wired. **Runs in-process only.** The live device's top-level shape is policy-fixed in `main.cpp` (see [src/core/HttpServerModule.cpp:639](../src/core/HttpServerModule.cpp#L639), `/api/modules` rejects top-level adds), so "build from scratch" can't happen on a live device without re-flashing. The live runner skips construct scenarios with a clear note.
- **`"mode": "mutate"`**: the scenario assumes a wired pipeline and tweaks it (`set_control` heavy), and runs in both tiers.
  The in-process runner replays an embedded **`fixture`** array (same shape as `steps`, but all `add_module`) that builds the same pipeline `main.cpp` does, then runs the steps.
  The live runner skips the fixture (device is its own fixture) and pre-flights every id the steps touch: a missing id is a hard fail, not a silent skip.

Picking the right mode:
- If your scenario starts with empty Layouts/Layer/Drivers wiring, it's **construct**. It will not run live.
- If your scenario tweaks an existing pipeline (resize the grid, toggle mirror, change preset), it's **mutate**. Provide a `fixture` so in-process tests can run too.

A `mutate` scenario that needs platform-bound modules (Network mDNS, WiFi, OTA) the in-process runner can't honestly stand up should add `"live_only": true`.

**Bespoke convention.** The `mode` + `fixture` + `reset` trinity is MoonLight-specific: no off-the-shelf BDD or scenario framework was borrowed wholesale.
It exists because the same JSON has to serve both an in-process runner that owns the scheduler and a live runner that does not (main.cpp does).
The closest analogs from widely recognized testing patterns: `fixture` ≈ xUnit fixtures (setup-once, replayed per scenario); `reset` ≈ SQL `BEGIN`/`ROLLBACK` (idempotent state restoration); `mode` ≈ pytest's parameterized execution modes.
A future contributor who finds an off-the-shelf framework capturing this construct/mutate asymmetry is worth migrating to.

### Reset block: idempotent scenarios

`mutate` scenarios mutate shared controls (Grid size, Multiply mirror toggles, Preview detail). Without restoring those controls to known values at scenario start, measurements become coupled to *which other scenarios ran first*, last scenario's leftover state poisons this one's baseline. The fix is a top-level **`reset`** array, an `add_module`/`set_control`-shape list that runs *before* the scenario's own `steps`:

```json
"reset": [
  { "name": "reset-grid-width", "op": "set_control", "id": "Grid", "key": "width", "value": 128 },
  { "name": "reset-mirrorX", "op": "set_control", "id": "Multiply", "key": "mirrorX", "value": true }
],
```

Both runners replay reset before baseline. The baseline measurement is taken *after* reset, so it reflects the normalized pipeline state. Reset steps don't have `measure: true`, they're prep, not assertions.

Convention: reset every control your scenario writes, plus any production-default control its `steps` implicitly depend on. The cost is a few extra steps; the payoff is scenarios you can run in any order and `contract` values you can trust.

### Performance contracts (`contract[<target>]`)

Every measurable step carries a per-target `contract` block, the **performance contract** MoonLight commits to on that platform.
The runner compares each measurement to the contract and fails if the device misses it.

```json
"contract": {
  "esp32-eth-wifi": {
    "tick_us": 90000,
    "free_heap": 105000,
    "set_by": "2026-06-02",
    "reason": "initial contract"
  },
  "desktop-macos": {
    "tick_us": 300,
    "free_heap": 0,
    "set_by": "2026-06-02",
    "reason": "initial contract"
  }
}
```

**Contract semantics:**

- `tick_us` is a **ceiling**, the device must deliver this tick or faster. Going faster than contract is good news, never a failure.
- `free_heap` is a **floor**, the device must deliver at least this much free heap. Growing heap is good news.
- Both are **hand-set promises**, not auto-captured last readings. Renegotiating a contract requires `--update-contract --reason "..."`, see below.
- `set_by` records when the contract was last (re)negotiated; `reason` records why. Both stamped automatically by `--update-contract`.

Target keys match `SystemModule.firmware` on a flashed device (`esp32`, `esp32-eth`, `esp32-eth-wifi`, `esp32s3-n16r8`, …) plus `desktop-macos` / `desktop-linux` / `desktop-windows` for desktop builds. The in-process runner picks the host OS automatically; the live runner reads the device's `firmware` control. (See [MoonInstaller, firmware vs deviceModel vs board](../explanation/architecture/mooninstaller.md#the-three-words) for the distinction.)

**Tolerance** absorbs run-to-run jitter only, not "I don't care":

- Default tick tolerance: **20% on desktop-*** (multi-process OS scheduling), **10% on ESP32** (lwIP / EMAC jitter).
- Default heap tolerance: same percentages.
- Default absolute floor on tick: **200 µs on desktop-***, **5 µs on ESP32**, prevents percentage tolerance from going sub-noise on tiny ticks.
- **On desktop-*, the floor dominates** for any contract under ~1 ms tick (effectively all desktop contracts today: 5–300 µs). The percentage knob only matters for slow desktop tick. This is honest: a 100 µs desktop contract realistically can't assert anything tighter than ±200 µs because multi-process scheduling jitter routinely adds 100+ µs all by itself. If you need a tight assertion, run the contract on ESP32, bounded RTOS clocks are tighter.
- Per-step overrides: `tick_tolerance_pct`, `heap_tolerance_pct`, `tolerance_us` inside the per-target block.

**Predicted-vs-actual heap.** The runner additionally prints the sum of `dynamicBytes()` from the live module tree as `model=N`. This is what the system *says* it allocated; comparing it to `free_heap` reveals framework overhead (lwIP, WiFi, HTTP buffers). The model number is informational, not asserted.

#### Persistent observations (`observed[<target>]`)

Alongside `contract`, every measurable step keeps a **rolling [min, max] range** per scalar in the `observed.<target>` block. The range only ever *widens*: a measurement inside the current bounds doesn't change the JSON at all (no diff churn on routine runs); a measurement outside the bounds pushes the relevant end out and updates the timestamp.

```json
"observed": {
  "esp32-eth-wifi": {
    "tick_us":         [80649, 89483],
    "free_heap":       [95528, 110640],
    "max_alloc_block": [49152, 53248],
    "at":              ["2026-06-02", "2026-06-15"]
  },
  "esp32-eth": {
    "tick_us":         [99514, 99514],
    "free_heap":       [135200, 135200],
    "max_alloc_block": [49152, 49152],
    "at":              ["2026-06-02", "2026-06-02"]
  }
}
```

Read directions match the contract semantics: tick contract is a *ceiling*, so the **max** of the observed range is the value that could fail it; heap and block contracts are *floors*, so the **min** of the observed range is the failure-side. The `at` field carries `[first_seen, last_updated]` so a stale range stands out (e.g. "last_updated is six months old → re-sweep this target").

Updated by **every** successful run of `run_scenario.py` or `run_live_scenario.py` for the active target, no flag needed. A range that never widens produces no diff. When a contract is renegotiated via `--update-contract` (see below), the observed range *resets* to the new single-point measurement because the previous range belonged to the old contract.

Open any scenario JSON and the range tells you both the typical case (the value sits in the range) and the variance (range width). A wide gap between the max of the observed range and the contract ceiling means there's headroom to tighten the promise; a max creeping up against the ceiling means a regression may be imminent.

#### Renegotiating a contract

A contract changes only when there's a reason: a code change improved performance and you want to commit to the new ceiling, or an accepted regression requires loosening it. Either way the diff records *what* changed and *why*:

```bash
# After an optimisation: tighten the ceiling for desktop-*
uv run moondeck/scenario/run_scenario.py --update-contract \
    --reason "Layer LUT inline copy"

# After accepting an ESP32 cost: loosen the ceiling on that target
uv run moondeck/scenario/run_live_scenario.py --host 192.168.1.210 \
    --update-contract --reason "added DMX driver overhead"
```

Both runners require `--reason`. They stamp `set_by` to today's date and write the new contract for the active target only, other targets are untouched. Review the diff carefully; tightening or loosening a contract is a deliberate act that ships to the project's public KPIs.

### Scenario file shape

Every `scenario_*.json` carries top-level metadata plus a `description` per step:

```json
{
  "name": "scenario_example_grid_sweep",
  "module": "GridLayout",
  "mode": "mutate",
  "also": ["Layer", "MultiplyModifier", "Drivers", "NetworkSendDriver"],
  "description": "Illustrative shape only (not a real file). Walk the grid through 16x16 → 32x32 → 64x64 → 128x128 and assert a per-size FPS floor.",
  "fixture": [
    { "name": "fix-layouts", "op": "add_module", "id": "Layouts", "type": "Layouts" },
    { "name": "fix-grid", "op": "add_module", "id": "Grid", "type": "GridLayout", "parent_id": "Layouts", "props": {"width": 16, "height": 16} },
    { "name": "fix-layer", "op": "add_module", "id": "Layer", "type": "Layer" },
    { "name": "fix-noise", "op": "add_module", "id": "Noise", "type": "NoiseEffect", "parent_id": "Layer" },
    { "name": "fix-mirror", "op": "add_module", "id": "Multiply", "type": "MultiplyModifier", "parent_id": "Layer" },
    { "name": "fix-drivers", "op": "add_module", "id": "Drivers", "type": "Drivers", "props": {"layer": "Layer"} },
    { "name": "fix-artnet", "op": "add_module", "id": "ArtNet", "type": "NetworkSendDriver", "parent_id": "Drivers" }
  ],
  "reset": [
    { "name": "reset-grid-width", "op": "set_control", "id": "Grid", "key": "width", "value": 128 },
    { "name": "reset-grid-height", "op": "set_control", "id": "Grid", "key": "height", "value": 128 },
    { "name": "reset-mirrorX", "op": "set_control", "id": "Multiply", "key": "mirrorX", "value": true },
    { "name": "reset-mirrorY", "op": "set_control", "id": "Multiply", "key": "mirrorY", "value": true }
  ],
  "steps": [
    {
      "name": "size-128x128",
      "description": "Set grid to 128x128. Production-size target.",
      "op": "set_control", "id": "Grid", "key": "width", "value": 128,
      "measure": true,
      "bounds": { "fps": { "min_pct": 70 } },
      "contract": {
        "esp32-eth-wifi": {
          "tick_us": 100000, "free_heap": 105000,
          "set_by": "2026-06-02", "reason": "initial contract"
        },
        "desktop-macos": {
          "tick_us": 120, "free_heap": 0,
          "set_by": "2026-06-02", "reason": "initial contract"
        }
      }
    }
  ]
}
```

`name` matches the filename stem exactly; `module` drives the doc grouping and the MoonDeck filter; `also` lists peer modules. `mode` is `construct` or `mutate` (see above). `fixture` is required for `mutate` scenarios that need to run in-process. `reset` (optional but recommended for mutate scenarios) normalizes controls before measurement so contract assertions aren't coupled to run order. `contract[<target>]` carries the per-target performance contract, see § Performance contracts. `observed[<target>]` carries a rolling `[min, max]` range per metric (tick_us, free_heap, max_alloc_block) plus `at: [first_seen_iso, last_seen_iso]` timestamps, widen-only across runs, so each run extends the range rather than replacing it; see § Persistent observations. Per-step `description` is recommended (a missing one falls back to the step's `name`/`op` in the generated view). Optional top-level `"live_only": true` keeps the scenario out of the in-process runner. Optional per-step `"measure": true` triggers a measurement after the step with optional `bounds`:

- `bounds.fps.min`: absolute FPS floor (in-process).
- `bounds.fps.min_pct`: relative to baseline (live runner).
- `bounds.fps.min_fps_led_product`: throughput floor scaling to grid size.
- `bounds.heap.max_delta_bytes`: max heap growth vs the previous measure step (lights up on ESP32; no-op on desktop).

**`bounds` and `contract` co-exist by design**, they catch different things on the same measure step:

- `bounds` are **relative within a single run**: "this step's FPS must be at least 80% of *this run's* baseline." Catches a measurement collapsing mid-scenario (effect silently dropping frames, mirror toggle leaving a degraded state). Cheap because it doesn't depend on history.
- `contract.<target>` is an **absolute promise across runs**: "this step must hit ≤ X µs / ≥ Y bytes heap on this platform." Catches regressions over time that bounds can't see (the baseline itself drifts).

If you only need one, prefer `contract`, it's the load-bearing assertion. `bounds` is the within-run sanity check.

Unknown JSON keys are ignored by both runners (C++ and Python), so adding a new field is safe.

### Generated docs and the shared parser

```text
moondeck/docs/
├── _test_metadata.py        # one parser used by both consumers below
└── generate_test_docs.py    # writes docs/tests/unit-tests.md + scenario-tests.md
moondeck/moondeck.py          # serves the same data as HTML in MoonDeck views
```

Both the markdown generator and the MoonDeck endpoints (`/api/test-modules`, `/api/unit-tests/<Module>`, `/api/scenarios/<name>`) import from `_test_metadata.py`. Adding a new metadata field (e.g. `@since`) means one edit there; both consumers pick it up.

Run the generator after touching any test file:

```bash
uv run moondeck/docs/generate_test_docs.py            # writes both docs
uv run moondeck/docs/generate_test_docs.py --check    # exits non-zero on drift (CI-friendly)
```

`--check` exits non-zero when the docs are stale, a contributor who adds a `TEST_CASE` without re-running the generator gets flagged. Run it in CI or before a commit to keep the generated docs in sync.

## Unit tests

Inventory: **[docs/tests/unit-tests.md](tests/unit-tests.md)** (auto-generated, grouped by `@module`).

Run them with:

```bash
# Replace build/macos with build/linux or build/windows per host. The
# MoonDeck path below and `uv run moondeck/test/test_desktop.py` resolve the
# host build dir automatically; the raw ctest / mm_tests calls don't.
ctest --test-dir build/macos --output-on-failure   # all
./build/macos/test/mm_tests -tc="<case-name>"      # one test case
uv run moondeck/test/test_desktop.py --module Layer # filtered by module
```

Or via MoonDeck (Desktop tab → Unit Test card). Pick a module from the shared module dropdown above the card to filter the run. Tests button shows the per-module inventory.

Output is summary-only on a full run (the doctest `-s` flag is added only on filtered runs, where the assertion-level detail is small enough to be useful).

## In-process scenarios

Inventory: **[docs/tests/scenario-tests.md](tests/scenario-tests.md)** (auto-generated, grouped by `module`).

Run them with:

```bash
uv run moondeck/scenario/run_scenario.py                                      # all
uv run moondeck/scenario/run_scenario.py --name scenario_Layer_base_pipeline  # one by stem
uv run moondeck/scenario/run_scenario.py --module Layer                       # all for one module
```

Or via MoonDeck (Desktop tab → Scenarios card). The module dropdown is shared with the Unit Test card above it: pick a module once and both card's run-set narrows. Steps button shows the per-scenario step list.

**Scenarios with `"live_only": true`** are skipped in-process (they need a real device, e.g. measure real-Ethernet throughput at a given grid size). They appear in the inventory but only run via the live tier below. The symmetric case (scenarios that only make sense in-process) is already handled by `mode: "construct"`, which skips on live.

**Step ops** the in-process runner understands today:

- `add_module`: instantiate a module by `type`, register it under `id`. Top-level when no `parent_id`. Mid-scenario adds (after the first `measure` step) run setup() + prepareTree() immediately, mirroring the live `/api/modules` POST shape.
- `remove_module` / `delete_module`, delete a child module from its parent (release + recursive free + prepareTree). The two names are aliases (the in-process and live runners must never diverge on op names, or a scenario silently no-ops on one tier). Refuses non-editable submodules (`userEditable()==false`) and top-level modules.
- `replace_module`: swap a child for a fresh module of another `type` at the same slot. A default-named module relabels to the new type; a custom/scenario id is preserved so later steps can still address it. Mirrors `/api/modules/<name>/replace`.
- `clear_children`: delete every deletable child of a container (`id`), leaving the container. The "prepare my own canvas" primitive: a scenario assumes nothing about the device's starting tree, clears a container, then adds what it needs. Non-editable children (Board, Preview, Improv) are skipped.
- `set_control`: write a control on an already-added module (`id` + `key` + `value`). Mirrors `handleSetControl`: applies the typed write, calls `onControlChanged()`, and triggers `Scheduler::prepareTree()` if `affectsPrepare` returns true. Today supports Uint8 / Uint16 / Int16 / Bool / Text / Password / Select. A step may carry `"optional": true`, a best-effort write (e.g. shrink a grid that may not exist) that's skipped, not failed, when the target is absent.
- `measure`: pure measurement step. Runs warmup + measure frames, prints per-step tick / FPS / lights / heap-delta, applies any `bounds` assertions for this step.

A step can also set `"measure": true` on a non-measure op (e.g. mark the last `add_module` as the one to measure after); the runner treats either shape identically.

## Live scenarios

Live scenarios run the same JSON against a running device via the REST API.

```bash
uv run moondeck/scenario/run_live_scenario.py --host localhost:8080       # desktop
uv run moondeck/scenario/run_live_scenario.py --host 192.168.1.210        # ESP32
uv run moondeck/scenario/run_live_scenario.py --update-baseline           # save
uv run moondeck/scenario/run_live_scenario.py --compare-baseline          # check
```

MoonDeck's Live tab wraps the same workflow: the Network bar at the top selects the LAN, Discover/Refresh populates the device list, the Live Scenarios card runs the selected scenario against every checked device.

![MoonDeck Live tab](../assets/ui/moondeck_live.png)

All scenarios use relative FPS bounds (`min_pct`) so they pass on any device, desktop at 10K FPS or ESP32 at 17 FPS. Settle time is 3 seconds to let the pipeline stabilise after rebuilds.

Scenarios that add modules (e.g. `scenario_Layer_base_pipeline`, `scenario_Layer_memory_1to1`) create temporary modules on the running device and clean them up at the end (`- Rainbow (cleanup)`). Modules that already exist show `=` instead of `+`.

Memory tracking works on ESP32: `freeHeap` and `freeInternalHeap` report real values. Desktop returns 0 (unlimited). The control-change scenario verifies no memory leaks by checking that heap returns to baseline after a mirror toggle.

One live-tier test lives outside the scenario JSON schema because it spans **multiple devices**: `uv run moondeck/scenario/run_network_live.py` runs a lights-over-UDP matrix (ArtNet, E1.31 and DDP) over every online board in moondeck.json, each board is once the sender, all others listen, and reception is asserted by reading each device's `/ws` preview stream (see [MoonDeck.md § run_network_live](../moondeck/MoonDeck.md#run_network_live)). A device matrix needs loops and per-round state the declarative scenario JSON can't express, so it follows the `improv_smoke_test.py` script shape instead.

## UI scenarios

UI scenarios drive the web interface itself: a run file lists what a person does (open a card, add a module through the picker, drag a slider) and each step checks itself by reading the device back over REST.

```bash
uv run moondeck/test/test_host.py --ui                     # the whole lane
uv run moondeck/uiscenario/uivideo.py --run test/uiscenarios/clips/95-add-a-layer.json
```

The runs live in `test/uiscenarios/clips/`, the engine in `moondeck/uiscenario/`. Data under `test/`, runner under `moondeck/`: the same split the pipeline scenarios use. Tests are parameterized over the directory, so a new run file is a new test with nothing to wire up. Format and actions: [RUNS.md](../../moondeck/uiscenario/RUNS.md).

**REST is read-only here.** Every state change goes through the affordance a person uses, because a step that POSTs its way to the outcome proves nothing about the interface. The reads are what `expect` compares against. That lets one file be both a test and a documentation video: the same run recorded produces the clips under `docs/assets/uiscenarios/`. A failing test means the UI no longer does what a published video shows.

**Opt-in, because it needs something running.** A bare `test_host.py` leaves this lane out, and it is never a gate: it runs on request only. `--ui` skips rather than fails when nothing answers, the same way the JS lane skips without node. A run that drives another surface names its own `host` (the installer's preview server). One that needs particular hardware names a `requires` capability resolved against the bench registry.

## Hardware Verification

All live scenarios pass on both desktop and ESP32 with `min_pct: 80` relative bounds. Per-module timing, memory allocation, and sizeof measurements for each platform are in [performance.md](performance.md).

### ESP32: classic, no PSRAM (measured on an Olimex ESP32-Gateway Rev G)

- 128×128 grid (16,384 lights), all live scenarios pass.
- Memory tracking verified: mirror toggle shows heap changes, returns to baseline (no leaks).
- Ethernet (LAN8720 RMII) connects via DHCP.
- Device discovery from MoonDeck finds the ESP32 on port 80.

## Adding Tests

**Unit test:** add a `TEST_CASE` to the appropriate `test/unit/{core,light}/unit_<ExactModuleName>[_<topic>].cpp` file. Each file carries `// @module <ExactCamelCaseName>` at the top, plus a single `//` description line above each `TEST_CASE`. Add a new file when no existing test covers your module, pick the subfolder matching the module's `src/` domain. After adding cases, run `uv run moondeck/docs/generate_test_docs.py` so the generated inventory matches.

**Scenario test:** create a JSON file under `test/scenarios/{core,light}/` named `scenario_<ExactModuleName>_<topic>.json`. The top-level needs `name` (matching the filename stem), `module`, optional `also`, `description`, `mode` (`construct` or `mutate`), and optional `"live_only": true` if the scenario can only run against a real device. Each `steps[]` entry has an `op` (`add_module`, `set_control`, `measure`), a `name`, a `description` field that the doc generator picks up, optional `"measure": true` to run a measurement after the op, and optional `bounds` (`fps` and/or `heap`). The scenario runner auto-discovers all `.json` files under `test/scenarios/` recursively.

**Regression test:** when fixing a bug, add a test that reproduces it. The test's description (the `//` line for unit tests, the `description` JSON field for scenarios) should mention the root cause so the connection stays traceable in the generated inventory.

**Doc check:** `moondeck/docs/generate_test_docs.py --check` exits non-zero if regeneration would change `docs/tests/*.md`, a CI-friendly way to catch metadata that drifted from the source.
