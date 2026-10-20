# Testing

What we test and how. The inventory of every test is [the test page](tests.md), drawn from a registry the docs build generates from the test files (`moondeck/docs/gen_tests.py`): one entry per `TEST_CASE` and per scenario JSON, by subject. A scenario runs in two tiers (in-process + live) unless flagged `live_only` (construct-mode scenarios run only in-process). The source of truth is the test files themselves (steps to add one: [Add a test, and pin a bug](../moonwalk.md#add-test)).

## Testing strategy

Three test categories, each with a clear purpose:

- **Unit tests** (desktop, `test/unit/{core,light}/unit_*.cpp`), exercise individual MoonModules in isolation with doctest. Each test file declares `// @module <Name>` so it's categorized under that module in the generated inventory. Run via [test_desktop](../../moondeck/MoonDeck.md#test_desktop), which picks the per-host build dir. Verify a module's API, edge cases, and output independent of how it's wired into a pipeline.
- **In-process scenarios** (desktop, `test/scenarios/{core,light}/scenario_*.json`), exercise the system as an integrated pipeline. Each scenario is a declarative JSON file naming its top-level containers and a sequence of steps (`apply_state` documents, `set_control`, `measure`) with an optional per-target performance contract. The scenario runner (`test/scenario_runner.cpp`) replays the steps in-process and reports tick + heap per `measure` step. Same JSON files run against a live device through the HTTP API, that's the next tier.
- **Live scenarios**: the same scenarios driven against a running device over REST. See [Live scenarios](#live-scenarios) below.

A live run is worth watching once, because it is the tier that proves the device rather than a model of it. This one stacks three layers, blends them, swaps an effect, resizes the grid and takes the stack apart again, and every step happens through the same API the page uses:

<video src="../assets/moontube/04-scenario-testing.webm" autoplay loop muted controls playsinline width="720" title="A scenario stacking three layers on a running device, while the interface and the preview show it happening"></video>

**Picking a tier for a new test.** When the behavior you want to pin only makes sense with modules wired together (e.g. "the pipeline reallocates cleanly when the grid resizes," "Drivers correctly hands the source buffer through after a child swap"), reach for a scenario first: that is what scenarios are *for*. When the behavior lives inside a single module (one function's contract, one edge case, one bug regression on a small surface), a unit test is the cheaper and faster fit. Don't extend the scenario runner with new predicates just to migrate an existing unit test, which is adding abstraction without an active need. Add predicates when a *new* scenario you're writing needs them.

**Regression rule:** when a bug is found, the fix includes a new unit test or scenario that reproduces the bug. A comment in the test references the root cause so the connection stays traceable.

**Performance checks** verify architectural rules at runtime:

- **Zero-allocation render loop**: N frames, intercept `malloc` / `free`, fail if any allocation occurs during steady-state rendering.
- **Performance contracts**: a measure step's `contract[<target>]` is a tick ceiling and a heap floor per target, see § Performance contracts.

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

Alongside them, **check_code** writes one report that only falls. It counts complexity, size, nesting and parameters from lizard, duplication from jscpd, the render path's blocking calls from the compiler, and the two boundaries. Beside that, **clang-query** is the home for bespoke AST rules we invent. The Python checks in `moondeck/check/` cover contracts whose other half is a Markdown page, a JSON catalog or a built binary. Every one has a MoonDeck card ([MoonDeck.md](../../moondeck/MoonDeck.md)).

**Every one of these is a report, not a gate.** They state what they find; a consumer decides
what to do about it. `WarningsAsErrors` is empty, CodeQL never runs on `pull_request`, and the hot-path check never fails the event. A gate nobody can satisfy gets disabled rather than obeyed, and it pushes people to suppress a finding under time pressure, which is the opposite of why the tool is there. The exception is layer 0: compiler warnings ARE `-Werror`, because they are few, actionable, and fixed at the moment they appear.

**One rule, one owner.** Each rule is enforced in exactly one place, so a finding has one home
and cannot drift between two tools that half-agree:

| Rule | Enforced by |
|---|---|
| No allocation/blocking in the render path | `[[clang::nonblocking]]` + RTSan, counted by `check_code.py` |
| No platform code outside `src/platform/`, no light include in core | `check_code.py` |
| Specs match the code | `check_specs.py` |
| Catalog matches the modules | `check_devices.py` |
| Untrusted input is memory-safe | CodeQL |
| Bug patterns / performance | clang-tidy |
| Complexity and duplication do not grow | `check_code.py` (ratcheted per rule and in total) |
| Size/LOC/docs do not grow silently | `repo_health.py`, per commit and since the last release |

### Verify a zero before believing it

An analyzer reporting "0 findings" is indistinguishable from one that read nothing, and every silent-failure mode below produced a plausible clean report rather than an error:

| Defect | What it looked like |
|---|---|
| `#` inside a YAML `>-` folded scalar is not a comment | Every disable after the first was folded into the check string; `abseil-*` stayed on → 12,181 findings |
| `run-clang-tidy` shells out to `clang-tidy` by name | Not on PATH → exits 0 having analyzed nothing → "0 findings" |
| `-extra-arg VALUE` (space form) is silently ignored | Only `-extra-arg=VALUE` works → 0 findings |
| Same trap in `-checks` | The filter had never worked |
| Compilation database records a different compiler than the tool runs | `'cstdint' file not found` on 129/129 files; unparsed files are never analyzed |
| Missing `-isysroot` | Under-report, not an error: 5 matches where there were 14 |
| A baseline keyed on a name the tool no longer emits | Pins nothing while looking green, which is why `check_code` counts per file |

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
2. **One pattern, easy to spot in 30 seconds.** Every unit test follows the same header shape. Every scenario follows the same JSON shape. A new contributor recognizes an existing test, then writes the next one by analogy.

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

The C++ unit and scenario suites cover the C++ code, and the **Python** (MoonDeck, build scripts) and **JS** (web installer) code has its own host-side unit tier. It is `test/python/` (pytest) and `test/js/` (Node's built-in runner; no `package.json`/`npm install`), both run by [test_host](../../moondeck/MoonDeck.md#test_host). Both run in `.github/workflows/test.yml` on a pull request whose changed paths match that workflow's filter, and are commit checks; which paths trigger them is [the commit process](../moonwalk.md#commit). Python test files carry their deps in a PEP-723 `# /// script` block (the repo's convention, there's no central `pyproject.toml`). Pyserial is a dep only because `improv_provision.py`'s import guard exits without it, not because the frame logic needs it.

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

**`test/js/improv-frame.test.mjs`** (node:test), pins the same envelope *plus* the `APPLY_OP` chunking, which the installer JS, MoonDeck's provisioning and the device implement alike:

| Test | Pins |
|---|---|
| frame layout | magic / version / type / length / payload / checksum positions |
| checksum is sum-mod-256 | matches Python + C++ |
| golden vector G1 | the same 11-byte frame as the pytest anchor |
| G2, a small state document is a single APPLY_OP frame | `[0xFC][seq=0][last=1]` header + the document JSON byte-identical in the payload |
| G3, a >125-byte document chunks into ordered frames | two frames, `seq` 0→1, `last` 0→1, the 125-byte chunk boundary, reassembly reproduces the document JSON exactly |
| at-least-one-frame | an empty document still emits one frame with `last=1` (so `last` always sends) |
| one document per container | an entry sends one `{"<Root>": …}` document per top-level `state` container, in key order, each within the device's 512-byte reassembly buffer |

The JS suite proves the installer *chunks* a document correctly; the **device side that reassembles those chunks** is pinned by the C++ `unit_ImprovOpReassembler` suite (`src/core/ImprovOpReassembler.h`, the pure state machine behind the device's `APPLY_OP` handler, extracted from `platform_esp32_improv.cpp` so it's desktop-testable). It covers the full receive contract: in-order multi-chunk reassembly + NUL-termination, **duplicate-chunk rejection** and **out-of-order/skipped-seq rejection** (the guard against an installer retry corrupting the buffer), **overflow** rejection at the buffer-minus-NUL boundary, mid-stream `seq 0` abandoning a partial op, and clean recovery after every error. Encode (JS) + reassemble (C++) together prove APPLY_OP end to end without hardware.

**`test/python/test_installer_manifests.py`** (pytest), pins the web installer's per-release file contract. For every `ships: true` firmware in `mooninstaller/firmwares.json` it runs `moondeck/build/generate_manifest.py` (with a synthetic `flasher_args.json`, so no firmware build is needed) and asserts the manifest is valid (a `chipFamily` + non-empty `parts[]`) AND that **every part filename matches one of the globs the release workflow stages onto Pages** (`firmware-*.bin` / `shared-*.bin` / `partition-table-*.bin`, `shared-*.bin` covers the plain ota-data plus a MoonBase manifest's `shared-ota-data-slot0.bin` and `shared-moonbase-<chip>.bin`). A manifest that names a file outside those globs points at something the deploy never stages → the installer 404s at fetch-firmware (the failure that shipped a broken v2.0.0 installer). The test guards the manifest-generation ↔ staged-files contract; the *deploy mechanics* that stage them (per-tag, in `release.yml`) are workflow shell logic a unit test can't reach, so the two are complementary.

MoonDeck's pure logic (catalog reverse-lookup, state migration) and the installer's op-walk / storage are the next host-side candidates as they accrete regression risk.

### Naming convention

- **Unit tests:** `unit_<ExactModuleName>[_<topic>].cpp`, `<ExactModuleName>` is the **CamelCase** class name as it appears in `// @module` (and in the source: `Layer`, `MoonModule`, `MultiplyModifier`, `NetworkSendDriver`). The optional `<topic>` collapses when the file's the only test for its module (`unit_Color.cpp` is fine if `@module Color`); add it when one module has several test files (`unit_Layer_extrude.cpp`, `unit_Layer_zero_grid.cpp`, …) or when the topic genuinely clarifies what the file covers (`unit_FilesystemModule_persistence.cpp`).
- **Scenarios:** `scenario_<ExactModuleName>_<topic>.json`, same module-naming rule; the topic is always present because scenarios always cross multiple modules and the topic distinguishes the focus.
- The **`"name"` field inside each scenario JSON** matches the filename stem exactly (e.g. `"name": "scenario_Effects_pipeline_builds_and_renders"`). The runner, the MoonDeck dropdown, the generated docs and `--name` on the CLI all use this single identifier.

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

Header annotations recognized by the parser:

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

### Walking first setup, with the computer as the phone

First setup leaves the device's network for its access point and comes back, which no device-side op can follow.
These live-only ops let a scenario walk it with the computer running it as the phone.

- **`host_wifi`** with `"join": "access_point"` joins the device's access point by the `name` its card shows and points the runner at 4.3.2.1; `"join": "home"` returns to the network the run names. Each waits until its target answers, since macOS hides network names.
- **`expect_http`** fetches a `url` without following redirects and checks its `status` and `location`, which is what a captive portal answers. A `url` starting with `/` goes to the device under test, and `"method": "POST"` presses a route as the UI's buttons do. With `"resolve": "access_point"` it resolves the name at 4.3.2.1 as a phone does, since macOS holds a captive network's answers back from other apps. `body_matches` and `body_lacks` are regular expressions the response must, or must not, contain.
- **`within`** on `expect_control`, `list_row` and `expect_http` polls for that many seconds, for what the device reaches on its own time, such as a scan or a join.
- **`"wait": false`** on `reboot` restarts without waiting, for a device that comes back where the computer cannot reach it yet.

A scenario using them sets `"host_network": true` and runs with `--network <name>`, a `moondeck/moondeck.json` network the computer and the device share.
Its steps fill `{network.ssid}`, `{network.password}` and `{device}` from it, so no credential is in the file.
The runner puts the computer back on that network whatever happens.
`"on_request": true` keeps the scenario out of a run of all of them, since it takes the computer off its network for a few minutes.

The run: [Run a scenario against a live device](../moonwalk.md#run-live-scenario), with `--network`; the flags are in [MoonDeck.md](../../moondeck/MoonDeck.md#live_scenario).

#### Several boards

A scenario naming `"boards": ["sender", ...]` drives those boards too, each mapped to a device by `--board NAME=HOST`; it is skipped while one is missing. A step with `"device": "<name>"` runs on that board, as an `apply_state`, a `set_control` or a `measure`. Every step's strings fill `{host}` and `{<name>}` with the boards' addresses, so a sender's `hosts` points at its receivers. Each board is restored from its own snapshot, before the device under test. The observations are keyed by the whole combination, such as `host=esp32s3-n16r8 sender=esp32p4rev1-eth`, so a WiFi pair and an Ethernet pair keep apart.

A `measure` step's `read` list names extra values to report: `"Module.control"` from the state, `"Module.tick"` for a module's tick time, or `"fps"`; `"<name>:"` in front reads another board, such as `"sender:fps"`. The run ends with one row per such step, which is what a sweep is read from.

The run passes `--board sender=<host>` per board: [MoonDeck.md](../../moondeck/MoonDeck.md#live_scenario).

### Scenario modes (construct vs mutate)

Every scenario carries a top-level `mode` field that says what shape the scenario expects the world to be in. Two values:

- **`"mode": "construct"`**: the scenario builds the pipeline from its empty containers, one `apply_state` document at a time, and measures as it goes. **Runs in-process only.** A live device's tree is never empty, so the live runner skips construct scenarios with a clear note.
- **`"mode": "mutate"`**: the scenario assumes a wired pipeline and tweaks it (`set_control` heavy), and runs in both tiers.
  The in-process runner creates the scenario's **`top`** containers as the device boots them, with the apparatus main.cpp wires into each, replays its **`fixture`** array of `apply_state` documents, then runs the steps.
  The live runner lays the fixture as the canvas: each container the fixture names is replaced with exactly the fixture's children.
  A bench that carries another effect or grid then runs the scenario as a fresh device would, and the snapshot restore puts the bench back afterwards.
  It pre-flights every id the steps touch: a missing id is a hard fail, not a silent skip.

Picking the right mode:
- If your scenario starts with empty Layouts/Layer/Drivers wiring, it's **construct**. It will not run live.
- If your scenario tweaks an existing pipeline (resize the grid, toggle mirror, change preset), it's **mutate**. Provide a `fixture` so in-process tests can run too.

A scenario that needs platform-bound modules (Network mDNS, WiFi, OTA) the in-process runner can't honestly stand up adds `"live_only": true`. That flag is the one rule keeping a scenario out of the in-process runner. A `mutate` scenario with neither `top` nor `fixture` that does not say so fails, since in-process it would assert nothing.

**Where a scenario lives.** `core/` and `light/` follow the source domain, the way unit tests do. `device/` is the exception, and it groups by tier instead: a scenario there carries `"live_only": true` because what it measures does not exist on the host. That is real peripheral silicon (RMT, i80, Parlio, and the DMA budgets that read as zero off-target).
It is also internal-heap fragmentation behind `max_alloc_block`, and the cost of float maths on an Xtensa with no FPU. A host run of one of these would report a number that means nothing, which is worse than reporting none, so each carries `live_only`.
Their logic belongs in a unit test rather than here: what a `device/` scenario adds is the measurement on the hardware, not the behavior.

**Bespoke convention.** The `mode` + `fixture` + `reset` trinity is MoonLight-specific: no off-the-shelf BDD or scenario framework was borrowed wholesale.
It exists because the same JSON has to serve both an in-process runner that owns the scheduler and a live runner that does not (main.cpp does).
The closest analogs from widely recognized testing patterns: `fixture` ≈ xUnit fixtures (setup-once, replayed per scenario); `reset` ≈ SQL `BEGIN`/`ROLLBACK` (idempotent state restoration); `mode` ≈ pytest's parameterized execution modes.
A future contributor who finds an off-the-shelf framework capturing this construct/mutate asymmetry is worth migrating to.

### Reset block: idempotent scenarios

`mutate` scenarios mutate shared controls (Grid size, Multiply mirror toggles, Preview detail). Without restoring those controls to known values at scenario start, measurements become coupled to *which other scenarios ran first*, last scenario's leftover state poisons this one's measurements. The fix is a top-level **`reset`** array, a list of steps that runs *before* the scenario's own `steps`:

```json
"reset": [
  { "name": "reset-grid-width", "op": "set_control", "id": "Grid", "key": "width", "value": 128 },
  { "name": "reset-mirrorX", "op": "set_control", "id": "Multiply", "key": "mirrorX", "value": true }
],
```

Both runners replay reset before the first measurement, so every measurement reflects the normalized pipeline state. Reset steps don't have `measure: true`, they're prep, not assertions.

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

A measured step is compared against its `contract` and also **observed**: each passing run takes the step's tick, free heap and largest block into a rolling window of 32 samples per target, and the statistics over it (p50, p95, min, max, n) say what the step normally costs and how bad its tail is. The window is the machine's own history, so it lives on that machine, in `build/moondeck-logs/observations.json`, and a run leaves every tracked file as it found it.

The scenario file carries a **summary** of the window, published on purpose: `--update-observed` writes the statistics of this machine's window into `observed.<target>` (without the samples), and so does a renegotiated contract (`--update-contract`). The test page draws that summary beside the contract, so a reader sees what each board did the last time a bench run was published.

```json
"observed": {
  "esp32-eth-wifi": {
    "tick_us":         {"p50": 84210, "p95": 89483, "min": 80649, "max": 91002, "n": 32},
    "free_heap":       {"p50": 104220, "p95": 110640, "min": 95528, "max": 110640, "n": 32},
    "max_alloc_block": {"p50": 53248, "p95": 53248, "min": 49152, "max": 53248, "n": 32},
    "last_updated":    "2026-06-15"
  }
}
```

A median is what a step normally costs and cannot be dragged by one contended run, and the tail is the p95, which a real regression moves and a single bad run does not. A window of one sample (`n: 1`) is a first impression, not a baseline. A free heap or largest block of 0 is the desktop saying it has no ceiling, so it never enters a window.

#### Renegotiating a contract

A contract changes only when there's a reason: a code change improved performance and you want to commit to the new ceiling, or an accepted regression requires loosening it. Either way the diff records *what* changed and *why*:

Steps: [Renegotiate a performance contract](../moonwalk.md#renegotiate-contract).

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
  "top": ["Layouts", "Effects", "Drivers"],
  "fixture": [
    { "name": "fix-pipeline", "op": "apply_state", "document": {
      "Layouts": { "Grid": { "type": "GridLayout", "width": 16, "height": 16 } },
      "Effects": { "Layer": { "type": "Layer",
        "Noise": { "type": "NoiseEffect" },
        "Multiply": { "type": "MultiplyModifier" } } },
      "Drivers": { "ArtNet": { "type": "NetworkSendDriver" } } } }
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

`name` matches the filename stem exactly; `module` drives the doc grouping and the MoonDeck filter; `also` lists peer modules. `mode` is `construct` or `mutate` (see above). `fixture` is required for `mutate` scenarios that need to run in-process. `reset` (optional but recommended for mutate scenarios) normalizes controls before measurement so contract assertions aren't coupled to run order. `contract[<target>]` carries the per-target performance contract, see § Performance contracts. `observed[<target>]` carries the published summary of the observations per metric (tick_us, free_heap, max_alloc_block), written on request; see § Persistent observations. Per-step `description` is recommended (a missing one falls back to the step's `name`/`op` in the generated view). Optional top-level `"live_only": true` keeps the scenario out of the in-process runner. Optional per-step `"measure": true` triggers a measurement after the step, checked against the step's `contract`.

Unknown JSON keys are ignored by both runners (C++ and Python), so adding a new field is safe.

### Generated docs and the shared parser

```text
moondeck/docs/
├── _test_metadata.py        # one parser used by the consumers below
└── gen_tests.py             # the registry the test page reads
moondeck/moondeck.py          # serves the same data as HTML in MoonDeck views
```

Both the registry generator and the MoonDeck endpoints (`/api/test-modules`, `/api/unit-tests/<Module>`, `/api/scenarios/<name>`) import from `_test_metadata.py`. Adding a new metadata field (e.g. `@since`) means one edit there; both consumers pick it up.

The registry is generated at every docs build, so it cannot drift from the test files.

## Unit tests

Inventory: [the test page](tests.md), by `@module`.

Steps to run them: [Add a test, and pin a bug](../moonwalk.md#add-test). In MoonDeck, the Desktop tab's Unit Test card filters by the shared module dropdown, and Tests shows the per-module inventory.

Output is summary-only on a full run (the doctest `-s` flag is added only on filtered runs, where the assertion-level detail is small enough to be useful).

## In-process scenarios

Inventory: [the test page](tests.md), by `module`.

Steps to run them: [Run a scenario in-process on the desktop](../moonwalk.md#run-scenario). In MoonDeck, the Scenarios card shares the module dropdown with the Unit Test card, and Steps shows the per-scenario step list.

**Scenarios with `"live_only": true`** are skipped in-process (they need a real device, e.g. measure real-Ethernet throughput at a given grid size). They appear in the inventory but only run via the live tier below. The symmetric case (scenarios that only make sense in-process) is already handled by `mode: "construct"`, which skips on live.

**Step ops** the in-process runner understands today:

- `set_control`: write a control on an already-added module (`id` + `key` + `value`). Mirrors `handleSetControl`: applies the typed write, calls `onControlChanged()`, and triggers `Scheduler::prepareTree()` if `affectsPrepare` returns true. Today supports Uint8 / Uint16 / Int16 / Bool / Text / Password / Select. A step may carry `"optional": true`, a best-effort write (e.g. shrink a grid that may not exist) that's skipped, not failed, when the target is absent.
- `apply_state`: a state document (`document`) through the engine `PATCH /api/state` runs, from its text as written so key order holds; it is how a scenario changes the tree. A member with a `type` adds a module and a new `type` under the same name replaces one, `null` removes one, and `"$patch":"replace"` keeps only the children listed, so with none it clears a container (the "prepare my own canvas" primitive), keeping the ones main.cpp wires. With `error`, and optionally `at`, the step expects that failure; with `optional`, an unknown type is a skip, as for a peripheral another chip has. The live runner removes the modules a document created, and restores each container's document at the end.
- `round_trip_state`: read every card back as its document and apply them as one document, which must apply. What the device shows is then proven to apply as it is, and live, the body outgrows the request buffer, so the streaming route runs.
- `list_row`: reach a row of a list control as the list API does. `add` appends one with those fields, and `to` moves it there. Otherwise `match` finds a row by its fields, then the step writes a `field` (a button field presses it), runs the row's action (`"apply": true`, a pad click), deletes it (`"delete": true`), or with none of those only expects it.
- `write_file` / `delete_file`: stage a file the way the editor saves one, and remove it again, so a scenario leaves the device as it found it. Both re-prepare the tree, as the live API does after a file changes.
- `measure`: pure measurement step. Runs warmup + measure frames, prints per-step tick / FPS / lights / heap-delta, and checks the step's `contract`.

A step can also set `"measure": true` on a non-measure op (e.g. mark the last `apply_state` as the one to measure after); the runner treats either shape identically.

## Live scenarios

Live scenarios run the same JSON against a running device via the REST API. Steps: [Run a scenario against a live device](../moonwalk.md#run-live-scenario).

MoonDeck's Live tab wraps the same workflow: the Network bar at the top selects the LAN, Discover/Refresh populates the device list, the Live Scenarios card runs the selected scenario against every checked device.

![MoonDeck Live tab](../assets/ui/moondeck_live.png)

Settle time is 3 seconds to let the pipeline stabilize after rebuilds.

Scenarios that add modules (e.g. `scenario_Effects_pipeline_builds_and_renders`, `scenario_Modifiers_reshape_the_mapping`) create temporary modules on the running device and clean them up at the end (`- Rainbow (cleanup)`). Modules that already exist show `=` instead of `+`.

Memory tracking works on ESP32: `freeHeap` and `freeInternalHeap` report real values. Desktop returns 0 (unlimited). The control-change scenario verifies no memory leaks by checking that heap returns to baseline after a mirror toggle.

One live-tier test lives outside the scenario JSON schema because it spans **multiple devices**. Its script, `run_network_live.py`, sends lights over UDP (ArtNet, E1.31 and DDP) between every online board in `moondeck.json`. Each board is once the sender while all others listen, and reception is asserted by reading each device's `/ws` preview stream (see [MoonDeck.md § run_network_live](../../moondeck/MoonDeck.md#run_network_live)). A device matrix needs loops and per-round state the declarative scenario JSON can't express, so it follows the `improv_smoke_test.py` script shape instead.

## UI scenarios

UI scenarios drive the web interface itself: a run file lists what a person does (open a card, add a module through the picker, drag a slider) and each step checks itself by reading the device back over REST.

How to run one: [MoonTube](https://github.com/MoonModules/MoonLight/blob/main/moondeck/moontube/moontube.md).

The runs live in `moontube/clips/`, the engine in `moondeck/moontube/`. MoonTube sits at the root rather than under `test/`, which every other test family follows. A run file is the source of a published video as much as it is a test, and the series is what someone looks for. Tests are parameterized over the directory, so a new run file is a new test with nothing to wire up. Format and actions: [moontube.md](../../moondeck/moontube/moontube.md).

**REST is read-only here.** Every state change goes through the affordance a person uses, because a step that POSTs its way to the outcome proves nothing about the interface. The reads are what `expect` compares against. That lets one file be both a test and a documentation video: the same run recorded produces the clips under `docs/assets/moontube/`. A failing test means the UI no longer does what a published video shows.

**Opt-in, because it needs something running.** A bare `test_host.py` leaves this lane out, and it is never a gate: it runs on request only. `--ui` skips rather than fails when nothing answers, the same way the JS lane skips without node. A run that drives another surface names its own `host` (the installer's preview server). One that needs particular hardware names a `requires` capability resolved against the bench registry.

## Hardware Verification

All live scenarios pass on both desktop and ESP32. Steps to verify on boards: [Run a live verification on boards](../moonwalk.md#live-verification). Per-module timing, memory allocation, and sizeof measurements for each platform are in [performance.md](performance.md).

### ESP32: classic, no PSRAM (measured on an Olimex ESP32-Gateway Rev G)

- 128×128 grid (16,384 lights), all live scenarios pass.
- Memory tracking verified: mirror toggle shows heap changes, returns to baseline (no leaks).
- Ethernet (LAN8720 RMII) connects via DHCP.
- Device discovery from MoonDeck finds the ESP32 on port 80.
