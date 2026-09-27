# MoonDeck Script Reference

MoonDeck is MoonLight's browser-based developer console: one page that builds, flashes, runs, tests, monitors, and checks the project across every target, and discovers and drives devices on the network. Every action it offers is a thin wrapper around a script under `moondeck/`, so the CLI (`uv run moondeck/<group>/<name>.py`) and MoonDeck run exactly the same code: agents typically use the CLI, humans use MoonDeck. For what MoonDeck *is* and where it sits in the workflow see [docs/how-to/building.md § MoonDeck](../docs/how-to/building.md#moondeck--the-dev-console). What follows is the per-script reference.

Launch it with `uv run moondeck/moondeck.py` and open <http://localhost:8420>. The console has three tabs — **Desktop** (build build / run / test), **ESP32** (chip + port, build / flash / monitor), and **Live** (discovery and live runs against networked devices) — above a network bar and per-device deviceModel pickers. Script definitions live in `moondeck/moondeck_config.json` (committed); runtime state (selected network, devices, ports) persists in `moondeck/moondeck.json` (gitignored).

Below: the UI behaviours common to every card, described once, then one section per script grouped by the tab it appears on. Each section gives the equivalent CLI invocation, so the page doubles as the command reference for running anything without the browser.

## UI Features

- **Status dots** on each card: grey (not run), orange (running), green (exit 0), red (exit non-zero).
- **Last-run log** — the **📄** button replays that script's last run in the log pane. It appears **only on cards that have actually run** (and shows up the moment a first run finishes, no reload needed), so its absence is informative too: a card with no 📄 is one nobody has used yet. Every run is teed to `build/moondeck-logs/<id>.log` as it streams (not buffered to the end, so a run you Stop still leaves what it printed), which answers "what did this do last time" after a page reload or a switch to another card — the case a live-only stream cannot. One file per script, overwritten each run: a last-run record, not a history. Gitignored, being derived state.
- **Run count** — every run ends `[exit code: 0] [run #7]`, counting that script's runs in `build/moondeck-logs/run-counts.json`. Two identical reports are otherwise indistinguishable, so the number answers "did this actually re-run since the fix, or am I reading the same output again?". Derived like the logs: deleting `build/` resets it, and a missing or corrupt file costs the count, never the run.
- **Run/Stop toggle** for long-running scripts (Run desktop, Monitor ESP32).
- **Duration hint** — every card shows how long it takes: ⚡ about a second, ⏱️ a few seconds up to ~30, 🐌 more than 30 seconds (a build, a flash, a gate list, clang-tidy). All three are shown rather than only the extremes, so a blank badge reads as "nobody set a speed on this card" instead of being confused with medium. Set per script as `"speed": "instant" | "medium" | "slow"` in `moondeck_config.json`. This is a *label*, not a timeout — nothing enforces it, so a script that grows slower needs its flag updated by hand. Separate from `long_running`, which controls the Run/Stop toggle rather than expected duration.
- **Group headers** in the sidebar (setup, build, flash, run, test, check, scenario).
- **Destructive-action confirm** — scripts flagged `destructive: true` (e.g. Erase Flash) pop a native confirm dialog before running.
- **Tab persistence** — selected tab survives page refresh.
- **Process detection** — on page load, checks if MoonLight or idf.py is already running and shows Stop button.
- **Network bar** (top of the sidebar): switch between known networks. Each network holds its own device list, last-used serial port, and WiFi credentials (consumed by Improv). On startup, MoonDeck auto-selects the network whose subnet matches the host's current LAN — moving the laptop between networks usually requires no clicks. Manual override (the dropdown) pins the selection until the pinned network's subnet stops matching the host. Add / Rename buttons next to the dropdown manage the catalog. State persisted in `moondeck/moondeck.json` under `networks` + `active_network`.
- **Device-model picker** on each device row: dropdown of device models from [mooninstaller/deviceModels.json](../mooninstaller/deviceModels.json) — the same catalog the web installer uses. When the device's firmware uniquely identifies one deviceModel (e.g. `esp32-eth` → Olimex Gateway), MoonDeck auto-deduces and mirrors the value to the device's `deviceModel` control on [SystemModule](../docs/moonmodules/core/SystemModule.md) via `POST /api/control` on next discover. For firmwares with no unique deviceModel (`esp32` runs on multiple), the user picks; MoonDeck pushes that value too. A device-reported deviceModel not in the catalog still shows up as `<key> (unknown)` so the value survives. MoonDeck's picker is a **text dropdown for an already-running device** — distinct from the web installer's flash-time *picture* deviceModel picker; both read the same catalog, but MoonDeck doesn't need the per-deviceModel `image`/`url` fields (those are installer-picker UX). Selecting a deviceModel pushes its full catalog config — each entry is a list of `{type, id, parent_id?, controls?}` module units (the [nested catalog schema](../mooninstaller/README.md), add-then-configure), so MoonDeck adds the deviceModel's modules (`POST /api/modules`) then sets their controls (`POST /api/control`); see `_push_device` in [moondeck.py](moondeck.py).
## Desktop Tab


![Moondeck Desktop](../docs/assets/ui/moondeck_desktop.png)

### build_desktop

Build the desktop firmware binary using CMake.

```bash
uv run moondeck/build/build_desktop.py
```

Runs `cmake -B build/<host> -DCMAKE_BUILD_TYPE=Release` then `cmake --build build/<host> --target MoonLight`, where `<host>` is `macos`, `linux`, or `windows` depending on the OS this script runs on. It builds ONLY the firmware binary — not the ~130-file test suite — so the "just give me the binary to run" path stays fast; compile the tests separately (see `compile_tests`). The per-host directory keeps an experimental Linux build from clobbering a macOS one on the same machine, and mirrors the ESP32 side's `build/esp32-<board>/` shape.

### compile_tests

Compile the test binaries (unit + scenario) without running them.

```bash
uv run moondeck/build/build_desktop.py --tests
```

Builds `mm_tests` + `mm_scenarios` (the `--tests` target set of `build_desktop.py`), in the same per-host build dir. Separate from `build_desktop` so a firmware build doesn't drag the ~130 test translation units through the compiler; run the compiled binaries afterward with `test_desktop` (unit) and `scenario_pipeline` (scenarios).

### test_desktop

Run the desktop test suite.

```bash
uv run moondeck/test/test_desktop.py
```

Runs `./build/<host>/test/mm_tests -s` (doctest with all test cases shown) — same per-host build dir as the desktop build above.

### test_host

Run the host test suites: the Python ones, the JS ones, and the UI ones.

```bash
uv run moondeck/test/test_host.py            # Python and JS
uv run moondeck/test/test_host.py --python   # just Python
uv run moondeck/test/test_host.py --js       # just JS
uv run moondeck/test/test_host.py --ui       # only the UI runs (needs a running device)
```

The tests the C++ binary cannot reach: the cross-language contracts (the Improv frame's wire format,
WLED's `/json` shape), the MoonDeck scripts themselves, the browser code under `src/ui`, and the
claim that every shipped MoonLive script is valid C++ (`test_scripts_are_cpp.py` hands each one to a
real compiler). The commit gate and CI run the same two commands; this is the card in front of them.
JS reports SKIP rather than failing when node is absent, since a Python-only bench is a normal setup.

`--ui` is the odd one and is OPT-IN, which is why a bare run leaves it out. It drives a real browser
against a running MoonLight with [pytest-playwright](https://playwright.dev/python/docs/test-runners),
performing a [run file](uiscenario/RUNS.md) from `test/uiscenarios/clips/` through the interface and checking each step against the
device over REST. The run files are the same ones `moondeck/uiscenario/uivideo.py` records the videos from, so a
failure means the UI no longer does what the video shows. It skips rather than fails when nothing
answers on `localhost:8080` (override with `PROJECTMM_HOST`), for the same reason the JS lane skips
without node.

### run_desktop

Launch the desktop executable as a detached background process and exit. The app keeps running across other MoonDeck scripts and outlives MoonDeck itself — the same model as flashing an ESP32, where the device runs independently of this console.

```bash
uv run moondeck/run/run_desktop.py
```

Re-running is idempotent: any existing `projectMM` instance is stopped first, then a fresh one is launched. Output goes to `build/<host>/projectMM.log`. Build first.

While the app is running, MoonDeck shows the button as **Stop** (a 5-second poll on `/api/running` detects the live process via `process_name`). Pressing Stop terminates the app; pressing Run again restarts it. From the CLI: `pkill -f build/<host>/MoonLight` (or `pkill MoonLight` if you don't have multiple host builds active).

### run_open_stage_control

Launch [Open Stage Control](https://openstagecontrol.ammd.net/) as a control surface for the running
device: eight faders, eight encoders and eight switches, wired to the
[OSC module](../docs/moonmodules/core/services.md) in both directions.

```bash
uv run moondeck/run/run_open_stage_control.py                   # device on this machine
uv run moondeck/run/run_open_stage_control.py --host 192.168.1.42
```

Then open **http://127.0.0.1:8088**. Nothing needs typing into Open Stage Control's settings panel:
the session, the send address and the listen port are all command-line arguments, and the session's
own `autosync` widget asks the device for its current state on every page load, so the widgets are
correct immediately rather than showing the layout file's defaults.

On the device, the OSC module needs `listen` and `feedback` on.

It runs headless (a web server, no desktop window), which is what makes the surface reachable from a
phone or another laptop on the same network. Options: `--host` / `--port` for the device,
`--listen` for where feedback arrives (the device's `feedbackPort`), `--ui-port` for the surface
itself, and `--app` when the binary is not on PATH or in the usual place. Install Open Stage Control
first; it is a free download.

### preview_installer

![Installer](../docs/assets/ui/installer.png)
![Installer2](../docs/assets/ui/installer2.png)
![Installer3](../docs/assets/ui/installer3.png)

Locally preview the web installer page at <https://moonmodules.org/projectMM/install/> without tagging a release. Stages `mooninstaller/index.html` + `src/ui/install-picker.js` into `build/install-preview/` and serves them via Python's `http.server` on port 8421.

```bash
uv run moondeck/run/preview_installer.py
# open http://localhost:8421/ in Chrome / Edge / Opera
```

Long-running — MoonDeck shows **Stop** while the server is up. Two modes, picked automatically:

- **Render-only.** When no `build/esp32-*/projectMM.bin` is present, the picker populates against the real GitHub Releases API and dropdowns work, but clicking **Install** fails because the local server has no `releases/` tree. Useful for iterating on HTML / CSS / JS without burning a build. Equivalent to "Recipe A" in [mooninstaller/README.md](../mooninstaller/README.md).
- **Flash-ready.** When at least one ESP32 build exists, the script additionally stages every `build/esp32-*/projectMM.bin` it finds into `releases/local-dev/` and generates matching Pages-relative manifests via the same `generate_manifest.py` the release workflow uses. The picker shows `local-dev` as the newest tag; clicking **Install** flashes a USB-connected ESP32 and hands off to the repository's custom orchestrator UI (Improv-Serial provisioning + the APPLY_OP config push of the device model's modules and controls, all in `install-orchestrator.js`, not ESP Web Tools). End-to-end, same code paths as the public installer. This is the developer's test ground for the install flow before deploying to GitHub Pages: Web Serial works on `http://localhost` without the secure-origin requirement that gates the public site.

Add `?nocache=1` to the URL to bypass the picker's 5-minute sessionStorage cache while editing.

### check_specs

Verify every implemented MoonModule has a matching, up-to-date spec.

```bash
uv run moondeck/check/check_specs.py
```

Scans `src/` for MoonModule `.h` files and checks each has a `docs/moonmodules/*.md` page whose control names / source facts still agree with the header. The always-run commit gate (fast, <1s) — catches `.h` ↔ doc drift even on doc-only commits.

### check_prose

Verify that prose a change ADDS follows the coding standards: no em-dashes, US spelling.

```bash
uv run moondeck/check/check_prose.py
```

Reads the added lines of the branch diff and the working tree, so pre-existing prose a rename
merely touched is out of scope. Run by hand: the tree still holds instances that predate the
check, so it is not in the gate table until those are swept.

### check_docgen

Report every place the generated documentation breaks the shape the standards define, and refuse a new one. Two surfaces, one check: the catalog pages that render as card tables, and the `///` comments that become the technical pages beside them.

```bash
uv run moondeck/check/check_docgen.py
```

Reads the pages the docs build renders as card tables and reports, per card:

- the sizes: 600 characters of description and 100 for any one control, 400 and 80 on the visual catalogs
- a missing image, or the wrong format for its page
- a details section placed above a card, or named for a card that does not exist
- a details table past 4 columns, or a cell past 300 characters
- any link in the second column that is not Tests, API or Details

And per file, over every `.h` and `.cpp` under `src/`, `test/`, `esp32/main`, `moonbase/main` and `moondeck/moonlive`, excluding the two vendored trees and the bundled test framework:

- a class comment past 10 lines, or any `## ` section of its `@moreinfo` appendix past 10
- a member comment past one line: a deep dive goes after `@moreinfo`
- one sentence in a comment past 30 words
- a public function or variable with no `///` at all

**A finding in a file that generates a page is an error; one in an implementation file is a warning.** A header and a catalog page are published, so a defect there ships and fails the gate. A `.cpp` publishes nothing: its comments are a note to the next reader, worth fixing without stopping a commit. Both are counted and both are reported. The split stages the sweep rather than ranking the two kinds of comment, so it goes and everything blocks once the warning column reaches zero.

Every run writes [docs/reference/metrics/docgen.md](../docs/reference/metrics/docgen.md), the tracked state of the sweep: errors and warnings per rule and per page, then the files ranked within each area. Current state only, so its git history is the trend, the same shape repo-health.md uses. Every erroring file also carries its own findings in full, since an error blocks a commit and its reader needs to know what to fix; warnings stay counted rather than listed, which keeps the page a report rather than a log. The file is the artifact to read, because stdout scrolls away and truncates.

**A ratchet, not a snapshot.** The committed `docs/reference/metrics/docgen.md` is the baseline: the run rewrites the file, then compares what it found against the copy in `HEAD`. An error fails the run, and so does any rule whose warning count ROSE, the total included. Warnings are staged work, and staged work that grows is not a sweep.

The comparison is per rule as well as on the total, because each hides a different move. A total alone hides one rule paying for another: splitting an over-wide line lowers the width count and raises the block count, which is the trade the block cap exists to refuse. Per rule alone misses a rule sitting under its own baseline while the total climbs. A rule whose limit itself changed is the one case where a rise is right, and the commit message is where that is said.

**There is no tolerated list.** The limits are the limits, and the check is red until the tree meets them. A grandfather list was tried and removed: while one exists, the cheapest way to make the check green is to add to it, which is how the comment budget eroded in the first place.

The rules are in [documentation-standards.md](../docs/contributing/documentation-standards.md#the-card), and the check itself is pinned by `test/python/test_check_docgen.py`: every rule is tested firing on a page built to break it, because a regex that silently stopped matching would report a clean run.

### check_platform_boundary

Verify that platform-specific code stays inside `src/platform/`.

```bash
uv run moondeck/check/check_platform_boundary.py
```

Scans all source files outside `src/platform/` for forbidden includes and platform `#ifdef`s.

### check_esp32_built

Check that a firmware binary exists and is newer than every source that feeds it.

```bash
uv run moondeck/check/check_esp32_built.py --firmware esp32s3-n16r8
```

The cheap stand-in for a full `idf.py build` in the commit and merge checks. Freshness is measured against the **sources**, not the clock: a wall-clock rule ("built in the last hour") passes a binary that predates an edit made twenty minutes ago, which is the stale-artifact trap that sends debugging at the wrong image. On failure it names the newer file and prints the rebuild command. `--max-age-hours N` adds an optional age rule on top; the default (0) disables it.

### check_devices

Validate the installer device-model catalog (`mooninstaller/deviceModels.json`).

```bash
uv run moondeck/check/check_devices.py
```

Checks each entry's required fields, that `firmwares` is a non-empty list, every `image` resolves on disk, the `System.deviceModel` control equals the entry name, module `type`s are factory-registered, `pins` controls live only on `*LedDriver` modules, and `flashBaud` (if set) is a standard esptool rate. The catalog's counterpart to `check_specs` for module docs.

### check_firmwares

Verify the firmware projection (`mooninstaller/firmwares.json`) matches the `FIRMWARES` source.

```bash
uv run moondeck/check/check_firmwares.py
```

Regenerates the firmware list from `build_esp32.py`'s `FIRMWARES` dict and fails on drift from the committed `firmwares.json` — so a `FIRMWARES` edit without regenerating is caught.

### collect_kpi

Collect the per-target KPI line (tick/FPS, memory, sizes) for the commit message.

```bash
uv run moondeck/check/collect_kpi.py                          # full interactive report
uv run moondeck/check/collect_kpi.py --commit                 # the commit-message form
uv run moondeck/check/collect_kpi.py --commit --no-live-capture   # skip the serial read
```

Captures a live tick from a connected ESP32 (and the desktop scenario ticks) plus source/test line counts, emitting the `tick:Xus(FPS:Y)` one-liner the commit message records.

The ESP32 half reads `esp32/monitor.log`, and refreshes it by opening the serial port for 15 s when that log is older than 5 minutes — accurate, but ~80 s and only possible with a bench board attached. `--no-live-capture` skips that refresh and uses whatever log exists (a few seconds, no board needed); the ESP32 tick line is then absent rather than stale when no recent log is around. The gate lists pass the flag so their cost stays predictable; omit it when composing a commit message, where the fresh reading is the point.

In `--commit` mode it also writes the repo-health snapshot (below), reusing the tick/FPS it just measured.

### bench_kernels

Kernel micro-bench: nanoseconds per call for the power-function kernels (noise, fBm, warp, and every kernel added since), on this host.

```bash
uv run moondeck/check/bench_kernels.py            # build (Release) and run
uv run moondeck/check/bench_kernels.py --no-build # run the binary already built
```

A report, not a check: the Markdown table it prints is pasted into performance.md, and a kernel swap is accepted against the previous rows (the gradient-noise swap's bound is 1.3x per sample). Host timings; the S3 is 20-40x slower per core, so the ratio between rows is what transfers, and `collect_kpi.py` on a board gives the absolute cost. Builds only the `mm_bench` target, so it does not drag the test suite through the compiler.

### repo_health

Measure the repo's current state into `repo-health.json` — flash per firmware variant, tick/FPS per target, lines of code by area, comment density, test counts, docs inventory.

```bash
uv run moondeck/check/repo_health.py           # measure + print the delta, write nothing
uv run moondeck/check/repo_health.py --write   # rewrite repo-health.json
```

**One small file, current state only — the trend is its git history** (`git log -p repo-health.json`), so the file never grows. The KPI gate rewrites it on every `--commit` run and prints the delta first, so growth is visible while you work and again in the commit's diff. A **soft ratchet**: nothing here fails a build. The numbers count things; they cannot tell a valuable comment from a restating one, so the judgment stays human.

Two properties worth knowing. Measurements read **tracked files only** (`git ls-files`), so a stray build artifact or scratch file can't move a number. And anything this run could not measure — a firmware variant that wasn't built, a tick with no board attached — **carries its previous value forward** rather than disappearing, so a docs-only commit doesn't blank the flash sizes and make the next diff unreadable.

### Tools group

Static-analysis tools, run **manually**: they take minutes rather than seconds, so they are not
in the commit/merge gate lists yet.

**A report shows the real situation.** Array usage, hot-path blocking, complexity — the number
is only worth reading if nothing was hidden to make it smaller. A finding is *fixed*, or it is
*shown with its reason*; it is never suppressed to tidy the output. `ParallelLedDriver::tick`
and `PreviewDriver::tick` genuinely block, so they appear in clang-hotpath every run — hiding
the two worst offenders would have made the report worthless while making the count look better.
The only suppression that earns its place is one where the tool is wrong about our code (e.g.
libc++ not annotating `steady_clock::now`, which does not block), and it carries that reason at
the site.

### check_module

Every static-analysis tool, on ONE module — the repo-wide reports turned around.

**The stack, and why it is six tools.** Each answers a question the others structurally cannot,
which is what keeps them from being redundant:

| | sees | answers |
|---|---|---|
| **clang-tidy** | one statement, with full type information | is this line wrong? |
| **clang-query** | the AST, via matchers we write | does this codebase's own rule hold? |
| **`-Wfunction-effects`** | the whole call graph, from the compiler | can the render path block? |
| **lizard** | tokens, no types | is this function getting more complex over time? |
| **CodeQL** | a queryable database of the program | can LAN bytes reach a `memcpy`? |
| **footprint** | the linked ELF | how many bytes does this cost, and in which memory? |

One rule, one owner: where two tools could report the same thing, one is switched off (lizard owns
complexity, so clang-tidy's `readability-function-*` checks stay disabled). The individual cards
run each of these across the tree; this card inverts that — everything at once, scoped to the
module you are actually working on.

```bash
uv run moondeck/check/check_module.py --module Control
uv run moondeck/check/check_module.py --module Layer --skip clang-tidy
```

The other tool cards sweep the whole repo, which is the wrong shape when you are working on one
file and want to know what the tools say about *it*. This runs clang-tidy, clang-query and
lizard against one module and prints them under one heading. It adds no analysis of its own —
it invokes the same scripts with `--module`, so this and the repo-wide reports can never
disagree about a finding. Each tool also accepts `--module` on its own if you want just one.

**Module is not the same as a translation unit.** A TU is one `.cpp` plus everything it
includes — the unit the compiler processes, and there are 15 under `src/`. A module is one class
with its `.h` and optional `.cpp`, and there are ~90. Most are **header-only**, so they have no
TU of their own: `ParallelLedDriver.h` is analyzed through whichever `.cpp` includes it. That is
why `--module` filters the findings rather than the file list — scoping by TU would analyze
nothing for a header-only module and report a confident, wrong zero.

It still scopes the *parse*, by resolving which TUs actually reach the module's files —
following `#include` edges transitively, so a header-only module is analyzed through the one or
two `.cpp` files that include it rather than all 15. `BouncingBallsEffect` is reached only by
`main.cpp`: **8s for all three tools, down from over four minutes**. The run prints which TUs it
picked, so the scope is visible rather than assumed.

### check_clang_tidy

Run clang-tidy over the whole tree and write a Markdown report.

**What clang-tidy is.** LLVM's linter for C++. It compiles each file for real — same parser as the
compiler, so it sees types, overloads and template instantiations — then matches a library of
named checks against the resulting AST (605 available in the LLVM 22 we run). That is the
difference from a text linter: it knows
`std::atoi(s)` is a call to the C library, not a word that looks like one. Checks come in
families (`bugprone-*`, `performance-*`, `readability-*`, `cert-*`), each independently
switchable, and many carry a machine-applicable fix.

**What it does for us.** It is the per-statement layer: the bug that lives inside one function and
needs type information to see. `bugprone-unchecked-string-to-number-conversion` is the shape —
`atoi` cannot report a failure, which is invisible to grep and uninteresting to a complexity
counter. Enabled checks live in [.clang-tidy](../.clang-tidy) at the repo root, so the editor and
this report agree.

```bash
uv run moondeck/check/check_clang_tidy.py                              # full run, report to stdout
uv run moondeck/check/check_clang_tidy.py --check bugprone-infinite-loop   # one check, for triage
```

Configured by [`.clang-tidy`](../.clang-tidy) at the repo root — the same file clangd reads, so
this report and your editor agree. Takes ~2-3 minutes; the baseline is **zero findings**, so
anything it prints is new.

The findings print straight to the log — a per-check summary, the worst files, then every
finding grouped by file. No report file to open: a run this slow should answer on the spot,
and the old `build/clang-tidy-report.md` was gitignored anyway, so it existed only to be read
once.

**Verify a zero before believing it** ([testing.md](../docs/reference/testing.md#verify-a-zero-before-believing-it)
covers why and lists the known silent-failure modes). This script's own guard: it refuses to
report when more than ten files fail to compile.

### check_clang_query

Our own AST rules — the checks we invent, that no off-the-shelf tool reports.

**What clang-query is.** An interactive REPL over Clang's AST, shipped with LLVM. You write a
matcher in the same domain-specific language clang-tidy checks are built from —
`cxxRecordDecl(hasName("Foo"))`, `callExpr(hasAncestor(ifStmt()))` — and it prints or dumps every
node that matches. It has no opinions and no built-in checks: it answers structural questions
about the code, and what counts as a finding is left to the caller.

**What it does for us.** It is how a project-specific rule gets written without building a
clang-tidy plugin. A plugin is a compiled C++ target with its own build; a matcher is one line of
text, editable in minutes. So the rules here are ours by definition — how much RAM the fixed
arrays cost, where the heap is touched, whether every class carries a `///` — questions no
off-the-shelf linter asks because they are about *this* codebase's constraints. The matchers
match broadly and the filtering happens in Python, because the matcher language has no notion of
"bigger than 64 bytes".

```bash
uv run moondeck/check/check_clang_query.py               # every rule
uv run moondeck/check/check_clang_query.py --rule=arrays # one rule
uv run moondeck/check/check_clang_query.py --min-bytes=256
```

One script holding a **growing list** of rules, not one script per rule: a new rule is a matcher
plus a few lines of Python, so it costs a list entry rather than another card and another help
page. clang-query rather than a compiled clang-tidy plugin — matchers are plain text, there is no
plugin to build and no LLVM ABI to track, and clangd cannot load a compiled plugin anyway.

**Rule `arrays` — fixed arrays that cost RAM.** A fixed array is a fixed size, and the
architecture sizes buffers at runtime from available memory. Reported worst-first, split by where
the RAM lives, because the fix differs:

| Where | Cost | Why it matters |
|---|---|---|
| `local` | Stack | A 2 KB local on a 4 KB task stack is an overflow waiting for the wrong call depth. |
| `member` | Per instance | Multiplied by how many instances exist — 200 bytes × 90 modules is 18 KB. |

`constexpr` and static-storage arrays are **excluded**: they live in flash and cost no RAM.
Including them roughly triples the list with entries nobody should act on. `MoonModule::name_[16]` is the cautionary case: its
comment records that it was *shrunk* from `char[24]` to save 8 bytes per module, so reporting it
would flag a past win as a problem.

Element sizes come from a table of the types we actually use; anything else (a struct, a class)
falls back to 4 bytes. The number is an order-of-magnitude guide, not an ABI-exact figure.

**No size threshold.** Every RAM-costing array is reported, however small — a cutoff hides
things for the wrong reason (`bool birthNumbers_[9]` is 9 bytes and was invisible under the old
10-byte default), and it bought little anyway: 362 findings with no threshold against 290 at
>10 bytes. Volume is capped by `--max-rows` (default 60, worst-first) instead, which truncates
the longest lists rather than silently dropping the smallest entries — and the cut is always
announced, because a table that quietly stops reads as "that is all there is". `--max-rows 0`
prints everything, and `--min-bytes N` restores a size cutoff — both CLI-only, since the card is
one button and the default plus the "N more not shown" line already answer the question from the
browser.

The per-module card keeps the 60-row cap, on every table including arrays: scoping to one module
narrows WHICH findings, not how many fit on a screen — `HttpServerModule` alone reports 212
declarations. Truncation is always announced, so the tail is one `--max-rows 0` away.

**Rule `scratch` — `ScratchBuffer` members.** `ScratchBuffer` *is* the project's heap manager,
so a module that uses one allocates without any `new` or `malloc` appearing in its own source —
`GameOfLifeEffect` has three (`cells_`, `future_`, `colors_`) and the `heap` rule reports zero
for it, because the real `platform::alloc` lives once inside `ScratchBuffer.cpp`. This rule
closes that blind spot: 18 members across 13 files. No size column — a ScratchBuffer is sized at
runtime from the light count, which is the entire point of it.

**Rule `heap` — every allocation site in `src/`.** `new` / `delete` / the `malloc` family
(including `heap_caps_*` and `ps_malloc`), in **two tables — ALLOCATE and FREE**. They answer
different questions: the acquire list is where RAM comes from and what the hot path must not do;
the release list is what pairs with it. Reading them side by side is how an unpaired allocation
shows up — and the split is what reveals, for example, that `HttpServerModule` frees five times
and allocates nothing, or that only **8 places in the whole codebase acquire memory**.

Each row carries what the site is, what it touches, and **the enclosing function** — the column
that turns a location into a lead ("this file allocates" is weak, "`handleConnection` allocates"
says where to look). `realloc` counts as acquiring, since it can move and grow. Not violations:
the driver layer allocates deliberately, but the hot path must not.

Member methods named `free()` are excluded. We have three (`MoonLive`, `Buffer`, `MappingLUT`),
and matching on the name alone counted every call to them as heap deallocation — 15 false
positives pointing at the wrong lines.

**Rule `comments` — which declarations are documented, and with which kind.** The project's rule
is that every class, method and attribute carries a short, readable `///` (that is what moxydoc
publishes), while `//` developer notes belong in the code lines rather than stacked on a
declaration. The report gives the matrix:

```
  DECL            DOC      DEV     NONE   %DOC
  class            53      116       25    27%
  method          415      651     1141    18%
  attribute       133      183      923    10%

  950 declarations carry only a `//` where the rule asks for `///`; 2089 carry no comment at all.
  Public surface: 503 of 2558 documented (19%) — the part doxygen publishes.
```

Three columns because there are three states, each with a different fix: `///` is documented;
`//` documents it in the wrong kind (promote it, or move it into the code lines); `NONE` has no
comment at all (write one). `%DOC` is the share carrying a real doc comment.

Then every declaration, ranked by how far it sits from the ideal:

```
  DOC DEVIATION  DOC WORDS  DEV WORDS  VIS   DECL       NAME               FILE:LINE
         +1135%       1606          0  pub   class      MoonI80Peripheral  MoonI80Peripheral.h:10
          +797%       1166          0  pub   class      HttpServerModule   HttpServerModule.h:17
          -100%          0         61  priv  method     driversOn          Scheduler.h:138
```

**Sizes are WORDS, not lines.** A line is a formatting accident — the same paragraph is 5 lines at
100 columns and 9 at 60 — while words are what a reader absorbs. Measured here, a comment line
carries a median of **13 words** in both kinds, so a line-based yardstick of class 10 / method 3 /
attribute 3 converts to the ideals below.

| Column | Meaning |
|---|---|
| `DOC DEVIATION` | Signed % difference from the ideal doc size — **class 130 words, method and attribute 40**. `0%` is ideal, `-100%` is absent, and over-documenting is unbounded. Not a "ratio": a ratio is a bare quotient (2.3×), this is a deviation from a target. |
| `DOC WORDS` | Raw `///` word count — the number the deviation is derived from, so it sits beside it. |
| `DEV WORDS` | Raw `//` word count. **No deviation column**, because zero is the ideal: measuring against "one line" made the best case (no note at all) read as `-100%`, i.e. worst. |
| `VIS` | `pub` or `priv`. Doxygen publishes only public members, so an undocumented public method is an **API gap** while a private one is a maintenance note. Both are reported — a bloated comment is bloat either way, and clangd's hover shows both kinds to whoever maintains the code — but the reader can weigh them. |

The `Public surface: N of M documented` line is the number the doc site reflects. It differs
sharply from the overall figure: `HueDriver` reads 40% public against 13% overall, because 52 of
its 77 declarations are private.

Rows sort by **|DOC DEVIATION|**, so both failure modes surface together: a bloated header and a
declaration with no comment at all are equally wrong in opposite directions, and ranking on the
signed value would bury one of them.

The ideals are a ruler, not a gate. `MoonI80Peripheral`'s 1606 words may be exactly right — they
ARE the driver's spec. The ratio says how far from typical something sits; a human decides.

**How `//` becomes visible to the AST.** Clang's lexer discards `//` — only `///` and `/** */`
become AST nodes, because the compiler consumes those itself. So the rule parses a SHADOW COPY of
`src/` under `build/comment-shadow/`, where every leading `//` has been rewritten to
`/// MMDEV: …`. The marker survives into the comment text, which is what keeps the two kinds
apart; `src/` is never touched and the reported paths are mapped back.

The marker must be plain text — an `@`-prefixed one is parsed as a doc *command* and stripped,
which silently merges the two kinds again. `-Wdocumentation` would fabricate warnings on
rewritten dev notes (`// @param wrong` becomes a real diagnostic), but clang-query never enables
it, so that risk does not apply here.

Not reported, because none of them is a declaration anyone documents: **forward declarations**
(`class JsonSink;` — no body, and the real class is reported from the header that defines it),
**lambdas** (`unless(isLambda())` on the record and `unless(ofClass(isLambda()))` on the method
— a lambda written inside a function body is code, not a documented declaration, and its
synthesised closure class plus call operator would otherwise report as 53 undocumented
"methods"), `implicit` closure classes, `invalid` declarations that only parse in the TU that
owns them, and `= default` / `= delete` members.

A **real** `operator()` on a named functor class is still reported — the exclusion asks the AST
whether the enclosing record is a lambda, rather than testing the method's name, so writing
`struct Compare { bool operator()(…) const; }` tomorrow does not silently drop it.

Not reported: function PARAMETERS. C++ cannot attach a doc comment to one — 1054 probed, zero
with a comment — they are documented via `@param` inside the method's own comment, and this tree
uses `@param` 8 times, all in a `.js` file. Class ATTRIBUTES are the per-member scope that does
exist, and they are the `attribute` row above. Declarations clang marks `implicit` (a lambda's
closure class) or `invalid` (a parse that only succeeds in the TU that owns it) are skipped:
both are anonymous, so their only "name" is the literal word `definition`.

Takes ~50s cold (a few seconds once the compilation database is warm). clang-query has no
parallel runner of its own and costs ~44s per translation unit, so this runs the 15 `src/` TUs
across cores; serial would be ~11 minutes.

### check_nonblocking

What the render path calls that can block or allocate — checked by the compiler.

**What `-Wfunction-effects` is.** Not a separate tool: a Clang 20+ warning implementing the C++
*function effects* proposal (P2698). Annotate a function `[[clang::nonblocking]]` and the compiler
verifies, **transitively through the whole call graph**, that nothing it reaches allocates, locks,
throws, or otherwise blocks. It is a compiler feature, so it sees exactly what the compiler sees —
including through virtual dispatch and member-pointer calls, which is where a hand-written check
gives up.

**What it does for us.** The render tick has a hard real-time budget, and the failure mode is a
`malloc` four calls deep in something that looks harmless. Nothing else in the stack can answer
that: clang-tidy checks one statement, lizard counts branches, the Python checks read text. This
script's own job is deduplication and framing — a raw build prints ~1350 warning lines for ~175
unique sites, because a header is recompiled once per translation unit that includes it.

```bash
uv run moondeck/check/check_nonblocking.py                # summary by callee, then every site
uv run moondeck/check/check_nonblocking.py --module AudioService
```

`MoonModule::tick/tick20ms/tick1s` carry `MM_NONBLOCKING` ([platform.h](../src/platform/platform.h)),
and Clang 20+ verifies under `-Wfunction-effects` that nothing they reach allocates or blocks —
**transitively**, through the whole call graph ([coding-standards.md § Static checks](../docs/contributing/coding-standards.md#static-checks) owns the rule).

The attribute is inherited by overrides, so three annotations cover every module's tick. It also
sits in `tickChildren`'s **member-pointer type** — without that, the indirect call through `fn`
is a hole the check cannot reason about, and passing an unannotated method now fails to compile.

Reports unique **sites**: a header included by N translation units emits the same warning N
times, so a raw build prints ~1350 lines for ~180 real findings.

**Split by tick tier**, because the same blocking call costs roughly two orders of magnitude
more in one than another: `tick()` runs every frame, `tick20ms()` fifty times a second, `tick1s()` once.
Pooling them hides which findings actually matter. `OTHER` is a site whose enclosing method could
not be resolved from source.

| Column | |
|---|---|
| **COND** | the branch guarding the call: `—` none (runs every time its tick does), `if`, `loop`, `switch`, `?:`, `&&`, and `ret` for a call reached only past an `if (…) return;` above it. `·rate` marks a rate limiter (`if (++n >= kEvery)`, `if (now - last < kIntervalMs) return;`) or a once-only latch (`if (!inited_)`). `?` means the guard analysis did not run — clang-query missing, a matcher it refused, or the query erroring — which is *unknown*, not unguarded |
| **CALLS** | the function that blocks — or `(static local variable)`, a violation with no callee: a static local needs a guard variable and a one-time lock on first use |
| **IN** | the method the call sits in, which is what places it in a tier. Clang names the call and the callee but *not* their enclosing function, so this is read back from the source |
| **WHY IT BLOCKS** | clang's own root cause, e.g. `calls mm::platform::UdpSocket::sendTo`. `—` means a leaf the compiler could not look inside (external or unannotated) |
| **FILE:LINE** | where to go |

**Sorted unconditional-first** within each tier, then by file and line. A call that runs every
time its tick does costs more than the same call behind a branch, and ties keep source order so
findings in one file stay together and the list diffs cleanly between runs.

**Two engines, each doing what it is good at.** The compiler finds the blocking calls, because
`-Wfunction-effects` is transitive and a matcher would have to rebuild the call graph to match it.
clang-query then annotates each site with its guard — a purely local AST question — joined on
`file:line`. Measured ~1s per TU, against a report whose cost is the clean rebuild.

**Guarded is not rare.** `if (enabled_)` is conditional and true every frame; only the `·rate`
hint separates those, and it reads the condition's *spelling*, so it is a lead rather than a
verdict. COND answers "is this reached every tick", never "is this acceptable".

**Desktop-only, and that loses nothing.** On GCC `MM_NONBLOCKING` expands to `noexcept` — the
exception contract still holds; only the clang attribute and the warning are absent. The ESP32 toolchain
has neither the attribute nor the warning, and builds with `-Werror`, so a bare attribute there
is a build break. Every tick method compiles on desktop — modules, effects, and the **LED
drivers** — so the render path itself is covered.

The gap is real but narrow: `src/platform/esp32/` has no tick methods (it is free functions the
tick path calls into), and while a call INTO one of them is reported at the call site, the
function's own body is never analyzed. A platform function that blocks internally without
carrying `MM_NONBLOCKING` is invisible. Closing that needs an xtensa clang — backlogged as
"ESP32 clang/LLVM toolchain" in backlog-core.md.

Not a gate yet: `-Wno-error=function-effects` keeps the build green while the findings are
triaged. Each is a judgment — fix it, annotate the callee, or accept it with a scoped reason.

### check_codeql

CodeQL's open alerts, read from GitHub — the Security tab as a card.

**What CodeQL is.** GitHub's semantic analysis engine. It compiles the codebase into a *relational
database* of every declaration, expression and control-flow edge, then runs queries written in QL,
a logic language, against it. Because it is a database rather than a pass over one file, a query
can follow data **across function and file boundaries** — the standard packs trace values from a
`source` (something attacker-controlled) to a `sink` (somewhere dangerous) and report the path.
That is *taint tracking*, and it is what nothing else in this stack can do.

**What it does for us.** One question: we parse six network packet formats (ArtNet, DDP, E1.31,
WLED audio sync, MQTT, WLED) plus HTTP, doing ~22 `memcpy` operations on bytes arriving from the
LAN, on a device with **no MMU and no process isolation** — a bad length check is not a crash, it
is arbitrary memory. CodeQL is the only layer that can trace a length field from the wire to the
copy. It has already paid for itself once (3 thread-unsafe `localtime` findings, fixed), and its
current answer on the packet parsers is *clean* — which is positive evidence, not silence.

```bash
uv run moondeck/check/check_codeql.py                  # open alerts, worst first
uv run moondeck/check/check_codeql.py --state fixed    # what has been resolved
uv run moondeck/check/check_codeql.py --all            # every state, including dismissed
uv run moondeck/check/check_codeql.py --module HueDriver
```

CodeQL runs in CI ([codeql.yml](../.github/workflows/codeql.yml)), not locally: it is the one
layer of the stack that sees whole-program taint, which is what the six network packet parsers
justify. Its findings then live behind the Security tab — a report nobody opens.

**Fetches, does not scan.** The CodeQL CLI would need a ~1 GB install and minutes per run to
recompute what CI already has, and the alert lifecycle (open / fixed / dismissed, tracked across
runs) is the valuable part — baselining we would otherwise build. The trade is stated in every
run's output: alerts describe the last **analyzed pushed commit**, so local edits are not in them.

Two severity scales are merged into one ordering, because the question is "what matters most", not
"which query pack found it": `security_severity_level` on the security queries
(critical/high/medium/low) and `severity` on the quality ones (error/warning/note/recommendation).

**Split into `src/` and everything else**, because the two are read differently: a finding in
shipping code reaches a device, one in a test does not. Measured at 855 open alerts, **783 sat in
`test/`** — pooling them buried the three `high` findings in `src/` under doctest noise. The
severity tally prints before either table so the counts survive row truncation.

**All states** (`--all`) adds the `fixed` and `dismissed` alerts to the open ones — the lifecycle
GitHub tracks and the reason this fetches rather than rescans. It answers "did that finding go
away, or did someone dismiss it?", so the tables gain a STATE column and a per-state tally
whenever more than one state is present. On a repo with nothing fixed or dismissed yet it reads
the same as the default, which is correct, not a broken flag. Each state is queried **explicitly**:
omitting `state` does not mean "any" — the endpoint defaults to `open`.

`--module` scopes to one module's files from the command line. It is deliberately NOT wired to the
tools-group module dropdown: that selector is promoted above the cards as soon as two cards in a
group declare `needs_module`, which moves it out of *All Tools on Module* where it belongs.

**Exit 2 when the answer is unknown** — no `gh`, not authenticated, code scanning disabled — with
the reason on stderr. An empty list and an unreachable API look identical in a table, and only one
of them means the code is clean.

### check_footprint

Where a module's bytes land on the device: flash, RAM, or strings.

```bash
uv run moondeck/check/check_footprint.py                    # every file, biggest first
uv run moondeck/check/check_footprint.py --module HueDriver
uv run moondeck/check/check_footprint.py --firmware esp32   # another target
```

**What the tool is.** No tool of its own — it reads the ESP32 **ELF** the build already produced,
through the toolchain's `nm` and `objdump`. The linker's accounting is ground truth: it has
already resolved every inline, template instantiation and dead-stripped symbol, so this measures
what actually ships rather than what the source suggests.

**What it does for us.** The other size checks answer "how big is the binary"; this answers
**which module made it that big, and which memory it spends** — because those bytes are not
interchangeable. Flash is ~1.5 MB and cheap; internal RAM is ~320 KB shared with WiFi, the HTTP
stack and every task stack. A report that adds them together hides the only distinction that
matters.

| Column | |
|---|---|
| **CODE** | `.text` — flash (or IRAM). Costs space, not headroom |
| **RODATA** | **named** constants only. String literals carry no symbol, so most of the ~427 KB in `.flash.rodata` cannot be credited to a file — the string table below the main table measures that half wholesale |
| **STATIC** | `.bss` + `.data` — RAM held from boot **whether the module is enabled or not**. This is the "an unused module should cost nothing" check: anything here is a standing tax. Not a synonym for *global*: it counts anything with static STORAGE DURATION, which includes a function-local `static` buffer and a file-local one in an anonymous namespace. Measured here, all 69 are file- or function-local; the codebase has no true globals |

**Sorted STATIC-first**, then by size within each group: every file holding static RAM appears
above every file holding none. A pure size sort buried the rows that matter — `platform_esp32_tasks.cpp`
is 230 B of code against 1440 B of static, so it sat below files with more code and could fall off
a capped table entirely.

**The columns are not interchangeable, and the ratio is why.** Flash is ~1.5 MB and only read when
the code runs; internal RAM is ~320 KB shared with WiFi, the HTTP stack and every task stack. So a
byte of STATIC costs roughly five times what a byte of CODE does, and a module reading STATIC 0 is
the healthy case rather than an empty measurement. The report deliberately does not total the
columns together — a single "size" number would hide the only distinction worth having.

Attribution comes from **DWARF** (`nm --line-numbers`), so a symbol is credited to the source file
that defines it — including free functions and file-statics no name-parsing heuristic could place.

**Strings ride along in the same run** — one report, both halves, no flag: they answer the same
question and a separate mode is one you forget to run.

**Ours are separated from ESP-IDF's**, which is the difference between a number you can act on and
one you cannot. The string table is read from the per-source **object files** under
`__idf_main.dir`, not from the linked image — the link merges every `.rodata.*.str` input section
into one `.flash.rodata` and a literal carries no symbol, so from the ELF alone a string cannot be
traced to who wrote it. Reading the objects also excludes IDF by construction: only our sources
have objects there.

`-ffunction-sections` is what makes the attribution exact: each function's literals land in
`.rodata.<mangled>.str1.N` inside its own object, so **the section name is the owning function and
the object path is the source file**. Both are printed next to each literal, so a long one can be
found and judged rather than just counted.

Measured: **28 KB of the 106 KB** in `.flash.rodata` is ours; the rest is ESP-IDF, WiFi, lfs and
FreeRTOS — which is why the whole-image view was misleading (its longest entries are all IDF
assert expressions). Of our share, prose and identifiers are ~35% each, wire formats 17%, and
error text 13% — so trimming error messages is the smallest of the four levers.

**Architecture-matched binutils.** `.espressif/tools` also holds `esp32ulp-elf-*` (the ULP
coprocessor's) and, on a P4 machine, the RISC-V set. The ULP `nm` reads an Xtensa ELF happily and
returns a plausible symbol list with every `.bss`/`.data` symbol silently unattributed — the
STATIC column read 0 tree-wide until the tool was picked by architecture. A wrong-tool answer that
looks right is worse than no answer.

**Needs a built firmware**, and exits 2 when the ELF is missing or carries no DWARF rather than
printing an empty table — an absent measurement is not a zero.

### check_lizard

Complexity gate: fail on **new** over-complex functions, not the ones already there.

**What lizard is.** A small language-agnostic complexity counter (Python, ~20 languages). It does
not parse C++ properly — it tokenizes, counts branch keywords, and reports cyclomatic complexity
(CCN), line count (NLOC), parameter count and token count per function. That shallowness is the
point: no build, no compile database, no toolchain, so it runs anywhere in about a second.

**What it does for us.** It owns ONE number — how complex a function is — and it is the only tool
here that produces a per-commit trend rather than a verdict. clang-tidy can tell you a function is
complex today; only a series tells you the codebase is drifting, which is what
[repo-health](../docs/reference/metrics/repo-health.json) and `collect_kpi` plot. Its own
`readability-function-*` checks stay off in clang-tidy for exactly that reason (one rule, one
owner). The tokenizer's cost is real: on template- and macro-dense C++ it reports a mangled
function name (`SolidEffect::static_cast<lengthType>` for a method called `tick`), and since the
baseline matches on `file:name`, such an entry pins nothing — see the note below.

```bash
uv run moondeck/check/check_lizard.py             # report NEW violations, exit 1 if any
uv run moondeck/check/check_lizard.py --all       # every violation, baseline ignored
uv run moondeck/check/check_lizard.py --baseline  # rewrite whitelizard.txt from today
```

Results print as one table, sorted worst-first, so the top row is the next thing worth
simplifying. The five numbers are the same ones lizard's own summary reports:

| Column | Meaning | Gated |
|---|---|---|
| **CCN** | Cyclomatic complexity — independent paths through the function: 1, plus one for every branch point (`if`, `for`, `while`, `case`, and each short-circuit `&&` / logical-or operator). The count of things that must hold at once to reason about it, and the number of tests needed to cover it. | **Yes**, > 10 |
| **NLOC** | Non-comment lines of code — the body's real size, blank lines and comments excluded. | **Yes**, > 60 |
| **TOKEN** | Total tokens (identifiers, operators, literals). Density rather than length: a high TOKEN against a modest NLOC means long, packed expressions. | No |
| **PARAM** | Parameter count. A long list usually means the function does several jobs, or wants a struct. | No |
| **LINES** | Raw line span, first to last — **includes** comments and blanks, so `LINES` minus `NLOC` is roughly how much of the function is documentation. | No |

A `*` next to CCN or NLOC marks which threshold tripped. It matters because the two point at
different fixes: `HttpServerModule::handleConnection` is `93* 178*` (both — split it), while
`json::parseString` is `40* 47` — branchy but short, so it wants a lookup table rather than a
split. TOKEN, PARAM and LINES are context for *why* a function is heavy; nothing gates on them.

A raw run reports 162 functions over threshold (CCN > 10 or NLOC > 60), and a metric that can
never reach zero is a poor gate: people stop reading it. So [`docs/reference/metrics/whitelizard.txt`](../docs/reference/metrics/whitelizard.txt)
freezes today's set and the check fails only on something new. The baseline is lizard's own
`--whitelist` format, matched on **file + function name** rather than line numbers, so it
survives edits above a function.

**The list only shrinks.** Simplify a function, delete its line; the check reports baselined
entries that no longer violate so they don't linger. Adding a line means admitting a new
violation, which is the thing this exists to prevent.

Lizard owns the complexity number (`collect_kpi.py` reports the same 162 for the repo-health
trend); clang-tidy's `readability-function-*` checks stay off so one rule has one owner.

### scenario_pipeline

Run scenario tests. Replays JSON scenario files in-process.

```bash
uv run moondeck/scenario/run_scenario.py                       # run all
uv run moondeck/scenario/run_scenario.py --name scenario_Layer_base_pipeline   # run one
```

Scenarios are JSON files in `test/scenarios/`. Use the dropdown to run a single scenario or leave it on **all** to run the full suite.

For a full description of each scenario, see the [scenario inventory](/api/docs/reference/tests/scenario-tests.md), auto-generated from the JSON files.

### history_report

Generate a human-readable history report from `git log` + `gh release list`. Writes a single markdown file at `build/history.md` (gitignored — the report is an artifact, not source; storing it in the repo would duplicate what git already carries).

```bash
uv run moondeck/report/history_report.py              # default: build/history.md
uv run moondeck/report/history_report.py --out /tmp/h.md
```

Output shape:

- **Releases** table: the most-recent 10 tagged releases with tag, date, and channel (stable / rc / latest).
- **History** section: combined graph + commits, newest first. Each commit row shows its graph-rail (`*`, `| *`, `*   `, …) as a monospace prefix to the SHA + date + subject. Merge commits get a ⤴ badge. The full body lives in a left-bordered blockquote underneath, visually extending the rail's vertical line into the description. Branch connector rows (`|\`, `|/`, `| |`) render as standalone monospace lines between commits. Inside each body, `- foo` lines render as nested bullet lists. Each SHA links to the corresponding GitHub commit page when an origin remote is configured.
- **Summary** footer: commit count, release count, generation timestamp.

The MoonDeck button writes the file, prints a `MOONDECK_VIEW: /api/history-report` marker that the log renderer auto-opens in the View pane (and renders as an "Open in View pane → …" clickable link). Re-runs on identical git state produce a deterministic file except for the timestamp line in the footer.

### screenshot_modules

Capture UI screenshots of every module that has controls and save them to `docs/assets/`.

```bash
uv run moondeck/docs/install_playwright.py    # one-time (or use Install Playwright button in MoonDeck)
uv run moondeck/docs/screenshot_modules.py    # requires MoonLight running on localhost:8080
uv run moondeck/docs/screenshot_modules.py --host 192.168.1.210:8080
uv run moondeck/docs/screenshot_modules.py --gif    # also record 3-second GIF previews
uv run moondeck/docs/screenshot_modules.py --force  # re-capture and overwrite existing screenshots
uv run moondeck/docs/screenshot_modules.py --all-registered  # every registered module, not just the listed ones
```

`--all-registered` is what keeps the set complete. The script carries a hand-written MODULES list
for the few modules that need particular props or a parent that is not a Layer; every other
registered effect and modifier is captured on a Layer with its defaults. Without it a module added
today is silently skipped until someone remembers to edit the list, which is how 42 effects came to
have no preview.

The **GIF** and **Force** checkboxes in MoonDeck toggle these flags.

Connects to a running MoonLight server, builds a minimal pipeline scaffold (Layouts → Grid, Layer, Drivers), adds each module, screenshots its card, then removes it. Saves:

- `<TypeName>.png` — module card screenshot for every module in the catalogue
- `<TypeName>.gif` — 3-second preview animation for effects and modifiers (requires `--gif`)
- `ui_overview.png` — full-page screenshot of the MoonLight UI
- `moondeck_desktop.png`, `moondeck_esp32.png`, `moondeck_live.png` — MoonDeck tab screenshots (requires MoonDeck running on port 8420)
- `installer.png` — web installer preview (requires `preview_installer` running on port 8421)

Without `--force`, existing screenshots are skipped — only missing files are captured. Run with `--force` to re-capture everything (e.g. after a UI change).

GIF capture uses ffmpeg (install with `brew install ffmpeg`). Each GIF is assembled from frames captured via Playwright — the WebGL canvas is read via `page.screenshot(clip=...)` rather than `canvas.toDataURL()` to work correctly in headless mode.

After capture, run `update_module_docs` to insert the references into the module spec files.

### update_module_docs

Insert screenshot and GIF references into `docs/moonmodules/**/*.md` files.

```bash
uv run moondeck/docs/update_module_docs.py            # update all
uv run moondeck/docs/update_module_docs.py --dry-run  # preview without writing
```

For each `.md` file, if `docs/assets/<type-folder>/<TypeName>.png` exists and the file doesn't already contain a screenshot reference, inserts the image after the first heading. If a matching `<TypeName>.gif` also exists, inserts the GIF reference on the next line. Safe to re-run — skips files that already have all references.

Also inserts MoonDeck tab screenshots and the installer screenshot into `moondeck/MoonDeck.md` and `README.md` at fixed anchor points (defined in the `EXTRA_SHOTS` list in the script).

Reports unreferenced screenshots — any PNG or GIF in `docs/assets/` not mentioned anywhere in `docs/` or `moondeck/`.

### build_docs

**Preview Docs Site** — serve the documentation site (Material for MkDocs) from the `docs/` tree with live-reload, so you can view and iterate on it. Long-running: MoonDeck shows **Stop** while the server is up (like Installer Preview); a stray `mkdocs serve` is killed before a new one starts. The button passes `--serve`.

```bash
uv run moondeck/docs/build_docs.py --serve     # what the button runs → http://localhost:8422/MoonLight/ (auto-reload)
uv run moondeck/docs/build_docs.py            # one-shot build to site/ (CI parity; no server)
uv run moondeck/docs/build_docs.py --strict    # promote every warning to an error (local anchor audit)
```

The preview binds **:8422** — the [Installer Preview](#preview_installer) owns :8421 and MoonDeck :8420, so all three servers run at once. Config is `mkdocs.yml`; deps (`mkdocs-material`) are declared inline in the script, so `uv run` provisions them on first use. The two test-inventory pages and each effect/modifier's inline test list are generated from the test files at build time (`moondeck/docs/mkdocs_hooks.py`), so they're never committed and can't drift; `history/` and `backlog/` are built but kept off the nav. Warnings for links to repo files outside `docs/` (rewritten to GitHub URLs) and pre-existing stale anchors are expected — the build still succeeds.

## Live Tab


![Moondeck Live](../docs/assets/ui/moondeck_live.png)

### live_scenario

Run scenario tests against a live running device via HTTP.

```bash
uv run moondeck/scenario/run_live_scenario.py                                    # all scenarios vs localhost:8080
uv run moondeck/scenario/run_live_scenario.py --host 192.168.1.210               # vs ESP32
uv run moondeck/scenario/run_live_scenario.py --name scenario_MoonModule_control_change   # one scenario
uv run moondeck/scenario/run_live_scenario.py --update-baseline                  # save baseline
uv run moondeck/scenario/run_live_scenario.py --compare-baseline                 # detect regressions
```

Executes scenario steps (add_module, set_control, delete_module) via REST API. Collects per-step FPS and heap measurements. Compares against stored baselines to detect performance regressions. Use the dropdown to run a single scenario or leave it on **all** to run the full suite.

For a full description of each scenario, see the [scenario inventory](/api/docs/reference/tests/scenario-tests.md), auto-generated from the JSON files.

### ui_clip

Record one UI clip: perform a run file against the interface while Playwright records, then publish a compressed clip for the docs.

```bash
uv run moondeck/uiscenario/uivideo.py --run test/uiscenarios/clips/95-add-a-layer.json
```

The dropdown lists every run under `test/uiscenarios/clips/`. A run drives the interface and nothing else: the `+` tab, the type picker, the card's own buttons, the real inputs. REST is read-only, and is what each step's `expect` block checks against, which lets the same file be a UI test (`test_host.py --ui`) as well as a video source. The raw take lands in `media/video/` (ignored). The published clip lands in `docs/assets/uiscenarios/` (tracked, embed this one) only when the run was clean: a take whose steps failed, or that left modules behind, is refused so it cannot overwrite a good clip. Format and actions: [RUNS.md](uiscenario/RUNS.md).

A run names its own `host` when it drives something other than the desktop UI, so the installer clip records against the installer preview and the audio clip against a board with a microphone. Start what a run needs before recording it.

### ui_project

Cut published clips into one video, on the beat, with a music track.

```bash
uv run moondeck/uiscenario/uicompose.py --project test/uiscenarios/projects/getting-started.json
```

The dropdown lists every project under `test/uiscenarios/projects/`. A project is the edit: which clips, in what order, how many BARS each gets, and the audio underneath. Bars rather than seconds, because a cut lands on the music or it does not, and changing the track re-times the whole edit from one number. The finished cut lands in `media/video/<project>.mp4`; the per-segment intermediates are deleted once it exists, and kept only when a cut fails.

### run_network_live

End-to-end lights-over-UDP matrix test across every online board in moondeck.json's active network — the live proof for [NetworkReceiveEffect](../docs/moonmodules/light/effects.md#networkreceive) and [NetworkSendDriver](../docs/moonmodules/light/drivers.md#networksend). Each round one device is the sender and every other device listens: the desktop seeds the sender **three times — once per protocol (ArtNet, E1.31, DDP), each with its own color** — asserting the sender's `/ws` preview stream shows each one, then points the sender's own NetworkSendDriver at each listener with the protocol control cycled round-robin and asserts the listener's preview shows the sender's corrected color (brightness + channel order replicated host-side). With one device online only the desktop→device sweep runs.

```bash
uv run moondeck/scenario/run_network_live.py                      # full matrix over all online devices
uv run moondeck/scenario/run_network_live.py --device MM-70BC     # only rounds with this sender
uv run moondeck/scenario/run_network_live.py --tolerance 1        # loosen the per-channel byte match
```

Everything it mutates (grid size → 16×16 for the run, NetworkSend `ip`/`protocol`/`enabled`, the temporarily added NetworkReceive effect) is restored afterwards, also on failure. Exit codes: `0` = all legs passed, `1` = a leg failed, `2` = environment problem (no online devices / no moondeck.json). Desktop listeners may need the OS firewall to allow UDP 6454/5568/4048.

### run_network_roundtrip

Minimal **desktop→device→desktop latency probe** across **all three protocols**: per device, the desktop sends one solid-color frame over ArtNet, then E1.31, then DDP, each time timing how long until that color appears in the device's `/ws` preview stream (desktop → NetworkReceiveEffect → PreviewDriver → desktop). The receiver autodetects each protocol on its own port, so there's no device reconfig between them. Reports min / median / max over N repeats per protocol and a per-device median-per-protocol comparison line — the spread is the signal for the latency / hiccup symptom, the protocol comparison shows which transport is fastest on a given board, and running across boards makes the per-chip difference visible (a classic ESP32 measures slower than an S3). Runs against **every device checked in the Live tab** (the same `selected` set the matrix test uses); unreachable checked devices are warned and skipped. The measured time includes the PreviewDriver's own fps quantisation (≈42 ms at the 24 fps default), so it's "state visible within" latency, not wire latency; raise the device's Preview fps to tighten it. Deliberately minimal — per-frame sequence matching, the device→device chain, and jitter/drop histograms are left as later extensions.

```bash
uv run moondeck/scenario/run_network_roundtrip.py                  # every checked device, 10 probes each
uv run moondeck/scenario/run_network_roundtrip.py --host 192.168.1.156 --repeats 20   # one explicit device
```

Captures and restores each device's grid and removes the temporary NetworkReceive on exit (also on failure). Exit codes match the matrix test: `0` = at least one device measured, `1` = none returned a frame, `2` = environment problem (no checked/reachable devices).

### preview_health

Browser-faithful **3D-preview stream health probe** — measures the device's `/ws` preview the way a real browser tab experiences it, so the numbers match what a person watching the [PreviewDriver](../docs/moonmodules/light/drivers.md#preview) preview sees. A plain one-shot WebSocket reader gives up the moment the device closes the socket, so it reports stalls a browser never shows (the browser reconnects) and misses the brief blips a browser does show; this probe replicates the real client in [app.js](../src/ui/app.js)'s `connectWs` — reads the binary frames, sends a `"ping"` text frame every 25 s, and **auto-reconnects on close with 500 ms→5 s backoff** — so a momentary device-side close registers as a short blip, not a frozen preview. Pure WebSocket client: **no device-side changes**, it observes the unmodified stream the device already broadcasts. Reports, per device: color frames + sustained fps, reconnects (each a visible blip), `maxgap` (the longest stretch with no color frame — the real "did it freeze?" number), and a `SMOOTH` / `CHOPPY` / `DEAD` verdict. Diagnostic, not a gate — it always exits `0`; read the verdict. Runs against **every device checked in the Live tab** (or an explicit `--host`); with no host it sweeps every online device on the active network.

```bash
uv run moondeck/diag/preview_health.py                              # every online device, 30s each
uv run moondeck/diag/preview_health.py --host 192.168.1.156         # one explicit device
uv run moondeck/diag/preview_health.py localhost:8080 --grid 128    # desktop build, force a 128×128 grid first
uv run moondeck/diag/preview_health.py 192.168.1.132 --seconds 60   # longer window to catch rare stalls
```

When the verdict is `CHOPPY`/`DEAD`, the *cause* (which close path fired on the device) needs device-side serial logging — that scaffolding is added on-demand during diagnosis, separate from this always-on probe. Stamps nothing on the device; safe to run against a live preview a browser is also watching (subject to the 4-client `/ws` limit).

## ESP32 Tab


![Moondeck Esp32](../docs/assets/ui/moondeck_esp32.png)

The tab is laid out top-to-bottom along the firmware workflow. Each dropdown sits between the script groups that consume it, so picking a dropdown is the natural prelude to the buttons below it.

```text
[Setup ESP-IDF] [Clean]            ← board-independent
Firmware: [esp32 / esp32-eth / esp32-16mb / esp32s3-n16r8 / …]
[Build]                            ← uses the selected Firmware
Port:     [/dev/tty.usbserial-XXXX] [↻]
[Flash] [Erase Flash]              ← uses the selected Port
[Monitor] [Improv WiFi] [Improv Probe]
```

The Firmware dropdown drives **Build** and **Flash**. Each board has its own build dir at `build/esp32-<board>/`, so multiple firmwares coexist on disk — switching the dropdown is free, no rebuild penalty. Flash reads the dir matching the dropdown; if you haven't Built that board yet, Flash exits with a clear "no build for <board>" message. The Port dropdown drives every script in the Flash and Run groups; the **↻** refresh next to it re-scans USB-serial devices without a page reload. Erase Flash uses the Port but doesn't care about Firmware (it wipes everything).

### setup_esp_idf

Set up ESP-IDF Python environment.

```bash
uv run moondeck/build/setup_esp_idf.py
```

Finds the ESP-IDF installation and runs `install.sh` to create the Python venv. Run once after installing ESP-IDF or after a Python version change. When the installed checkout has drifted from the pinned commit (`PINNED_IDF_COMMIT`), it offers to check the pin out (a new dev converges on the validated IDF); `--no-checkout` keeps it warn-only for a dev migrating to a newer release. Building for the ESP32-S31 (a RISC-V preview target) needs its toolchain fetched once with `(cd ~/esp/esp-idf && ./install.sh esp32s31)` — the default install only pulls the classic-`esp32` toolchains.

### clean_esp32

Clean the ESP32 build directory.

```bash
uv run moondeck/build/clean_esp32.py
```

Removes one ESP32 per-firmware build dir (`--firmware <name>`) or every `build/esp32-*/` plus a leftover `esp32/build/` if present (`--all`). Run a per-firmware clean after ESP-IDF updates, Python version changes, or anything else that should force a from-scratch build of that variant. Other firmwares' build dirs aren't touched.

### build_esp32

Build one of the shipping ESP32 firmware variants. The MoonDeck **Build** button reads the **Firmware** dropdown and forwards `--firmware <selected>` to `build_esp32.py`. The dropdown is populated from the `FIRMWARES` dict, the single source of truth. ("Firmware" is the compiled binary; the physical product (deviceModel) is a separate concept: see [MoonInstaller, firmware vs deviceModel vs board](../docs/explanation/architecture/mooninstaller.md#the-three-words).)

| Firmware key | Chip | What's in the image |
|---|---|---|
| `esp32` | `esp32` | WiFi **and** RMII Ethernet in one binary. Ethernet comes up only when a PHY responds; PHY type + pins are runtime config from `deviceModels.json` (default LAN8720 RMII pins). The default classic build. |
| `esp32-eth` | `esp32` | Ethernet only (WiFi compiled out → smaller image, more free RAM). Same runtime PHY/pin config. |
| `esp32-16mb` | `esp32` | Same as `esp32` but for 16 MB-flash classic boards (bigger OTA slots + filesystem). |
| `esp32s3-n16r8` | `esp32s3` | ESP32-S3 DevKitC-1 (N16R8: 16 MB flash, 8 MB octal PSRAM). WiFi + W5500 SPI Ethernet (external module, pins per board in `deviceModels.json`). |

CLI equivalent:

```bash
uv run moondeck/build/build_esp32.py --firmware esp32
uv run moondeck/build/build_esp32.py --firmware esp32-eth
uv run moondeck/build/build_esp32.py --firmware esp32-16mb
uv run moondeck/build/build_esp32.py --firmware esp32s3-n16r8
```

Auto-detects ESP-IDF installation, sets target if needed, builds, and shows flash/RAM usage summary. Each firmware writes into `build/esp32-<firmware>/`, so switching firmwares (or building several in one session) keeps every variant on disk — no clean rebuild on switch.

The Ethernet PHY type and pin map are runtime config, not baked in: each firmware carries the driver(s) its chip can host (RMII EMAC for classic, W5500 SPI for S3), and `deviceModels.json` supplies the per-board PHY/pins. The `esp32` / `esp32-eth` builds default to the common LAN8720 RMII pins (PHY reset on GPIO 5, MDIO addr 0, clock GPIO 17 — e.g. the [Olimex ESP32-Gateway](https://www.olimex.com/Products/IoT/ESP32/ESP32-GATEWAY/open-source-hardware)); a board with different pins (e.g. WT32-ETH01: reset on GPIO 16) just needs a different `deviceModels.json` entry — no rebuild.

Each ESP32-S3 SKU has its own firmware key because the sdkconfig fragment encodes flash size, partition layout, and PSRAM mode — flashing an `n16r8` binary onto a different module (e.g. N8R2) misaligns the partition table or fails PSRAM init. New SKUs become new keys (e.g. `esp32s3-n8r8`); we don't ship a generic `esp32s3` shortcut.

`--profile` is deprecated and accepted one release for migration: `--profile default` → `--firmware esp32`, `--profile eth-only` → `--firmware esp32-eth`.

### flash_esp32

Flash firmware to an ESP32 device. Reads `build/esp32-<firmware>/projectMM.bin` — each firmware lives in its own dir (plan-19.1), so multiple firmwares can coexist on disk and switching firmwares is free.

The MoonDeck button forwards the Firmware dropdown as `--firmware`. Flash exits cleanly with a "no build for <firmware> — run Build first" message when that dir doesn't exist. The log line up front confirms which build is being flashed and how old it is, e.g.:

**Flash baud** defaults to **921600** here (the CLI/MoonDeck path assumes a modern bench bridge — ~2x faster than the installer's safe 460800). A board with a flaky bridge pins a lower `flashBaud` in `deviceModels.json` to slow down (the LOLIN's CH340 → 460800); `--baud` overrides either. To resolve that per the *exact* board rather than the shared firmware, MoonDeck maps the selected Port → the device last flashed on it (its `last_port`) and forwards that device's deviceModel as `--device-model` — so one board's opt-down never leaks to a firmware-sibling with a fine bridge.

```text
==> flashing esp32 build (1267 KB, built 3m ago) to /dev/tty.usbserial-0001
```

```bash
uv run moondeck/build/flash_esp32.py --firmware esp32 --port /dev/tty.usbserial-0001
```

`--firmware` is required — there's no longer a single canonical `esp32/build/` to fall back to. For a rack flash, loop over ports AND specify the firmware explicitly:

```bash
for port in /dev/tty.usbserial-*; do
  uv run moondeck/build/flash_esp32.py --firmware esp32 --port "$port"
done
```

### erase_flash_esp32

Wipe the entire flash on an ESP32 device, including the LittleFS partition where persisted state lives (WiFi credentials, module list, control values). Flagged `destructive: true` so MoonDeck prompts a confirmation dialog before running.

```bash
uv run moondeck/build/erase_flash_esp32.py --port /dev/tty.usbserial-0001
```

Typical use: forcing a fresh-first-boot after firmware experiments leave the LittleFS partition in a state the new firmware can't migrate from, or before testing the post-flash Improv provisioning flow as if the device just came out of the factory. After erase, re-run **Build** then **Flash** — the device boots with empty persistence and goes straight to AP-fallback / Improv-awaiting-credentials.

### serve_firmware

Serve a built firmware over HTTP so a board can install it by URL. Long-running.

```bash
uv run moondeck/run/serve_firmware.py esp32 --port 8099
uv run moondeck/run/serve_firmware.py build/moonbase-esp32/projectMM-moonbase.bin --port 8098
```

Takes a firmware name (resolved to `build/esp32-<name>/projectMM.bin`) or a path to any `.bin`, and
prints the LAN URL to paste into the Firmware card or MoonBase's own page. The file is re-read per
request, so a rebuild needs no restart.

**It exists because `python -m http.server` speaks HTTP/1.0**, whose default is no keep-alive and a
body that ends at connection close rather than at Content-Length. `esp_https_ota` asks for
keep-alive and a 32 KB receive buffer, and against a 1.0 server the transfer crawls and then fails
with `0xffffffff` partway through. That cost two failed OTA attempts on a bench board before the
`HTTP/1.0` in the response line was spotted, while the same URL from GitHub worked first time.

### monitor_esp32

Monitor serial output. Long-running — shows Stop button.

```bash
uv run moondeck/run/monitor_esp32.py --port /dev/tty.usbserial-0001 --firmware esp32s3-n16r8
```

Reads serial at 115200 baud. Output streams to MoonDeck's log and is saved to `esp32/monitor.log` for later inspection (useful when crashes flood the output).

**Panic backtraces are decoded.** A crash prints `Backtrace: 0x4038456d:0x3fcae310 …`, which says nothing on its own; with `--firmware` each address is resolved against that build's ELF and the function, file and line print underneath:

```text
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
Backtrace: 0x4210b93b:0x3fcc8fa0 0x4200fbf8:0x3fcc8fc0
  #0 src/light/moonlive/MoonLiveLayout.h:119
  #1 src/light/moonlive/MoonLiveBuiltins_light.h:82
```

So a panic names its source line in the monitor rather than starting a separate addr2line session. Same purpose as PlatformIO's `esp32_exception_decoder` monitor filter; here it is the toolchain's own `addr2line` against `build/esp32-<firmware>/projectMM.elf`, picking the Xtensa or RISC-V tool from the firmware name. Without `--firmware`, or when that build has no ELF, addresses print raw and the monitor runs as before — decoding must never cost you the serial output.

### check_encodings

Verify every instruction MoonLive emits against the toolchain's own assembler.

```bash
uv run moondeck/moonlive/check_encodings.py            # every ISA that has a toolchain
uv run moondeck/moonlive/check_encodings.py --isa xtensa
```

MoonLive hand-encodes machine instructions, which is the right call for a JIT (an assembler cannot ship to a device) but means a wrong offset field, a truncated displacement or a misplaced register nibble is a bug nothing else notices: the golden-bytes tests compare our emission to our *previous* emission, and the structural checks compare it to a model we also wrote. Both agree with a consistent mistake.

This compares it to something nobody here wrote: `xtensa-esp32-elf-as` and `riscv32-esp-elf-as`, which *are* the definition of a valid encoding for these ISAs. For each instruction we emit, it assembles the same mnemonic and requires identical bytes. It caught a hand-computed `add.n` whose register nibbles were transposed, before that instruction ever ran.

What it covers, and what it does not: it proves each instruction is **encoded** correctly. It cannot prove the **sequence** is right, since a correctly encoded instruction can still save the wrong register. Execution-level checks ([run_qemu](#run_qemu), the bench) are what cover that.

### run_qemu

Run the firmware on an **emulated ESP32** on this machine, no board attached. Long-running: shows a Stop button.

```bash
uv run moondeck/qemu/run_qemu.py                  # boot it, web UI on :8410
uv run moondeck/qemu/run_qemu.py --erase          # wipe the emulated flash first
uv run moondeck/qemu/run_qemu.py --gdb            # freeze at reset, wait for a debugger on :1234
```

Uses [Espressif's QEMU fork](https://github.com/espressif/qemu) ([docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/qemu.html)), which emulates the ESP32's CPU, memory and enough peripherals to boot a real firmware image. Install it with `python3 $IDF_PATH/tools/idf_tools.py install qemu-xtensa`.

**Why it earns its place: it EXECUTES the code.** Every other check compares emitted bytes against a model of what they should be, so none can catch a mistake the model shares. The emulator runs the instructions the way silicon does, including Xtensa's register window, `entry`/`retw` and the exception path, so a JIT defect faults here, on this machine, in seconds, instead of on a bench with only a crash dump to read. That is what it was built for ([the register-window frame bug](../docs/work/past/lessons.md#lessons-from-the-moonlive-on-xtensa-branch-the-register-window-frame-bug)).

The emulated board is a full device, not a console toy: the `qemu` firmware variant swaps WiFi (no radio exists) for QEMU's emulated OpenCores MAC, so the guest gets a DHCP address and the REST API and web UI work exactly as on hardware. The same scripts, tests and browser drive it. Host port 8410 forwards to the guest's HTTP server, deliberately not 8080 so a desktop build can run alongside.

**Erase flash** (the checkbox) deletes the merged flash image so every data partition, settings and scripts, comes back blank. On a real board that is `erase_flash_esp32`; here the whole chip is one file, so removing it is the same operation. Without it, everything persists across restarts.

Two things to know when reading a QEMU run: the guest clock is emulated, so **timings are never KPI material**, and after any crash the emulator can boot-loop until the QEMU *process* itself is restarted (`--stop`, then start again).

### improv_provision

Push WiFi credentials to a running MoonLight device over USB-serial. Uses the [Improv-WiFi](https://www.improv-wifi.com/serial/) protocol — the same wire format the browser flow at improv-wifi.com uses. Device must be running a firmware that includes the Improv listener.

**One-click flow**: pick the device's port in MoonDeck, hit **Improv WiFi**. The script reads SSID + password from the **active network's WiFi block in `moondeck/moondeck.json`** (the one shown in the network bar at the top of the sidebar). If that block is empty, it falls back to detecting the host machine's currently-joined WiFi (macOS Keychain / Linux NetworkManager / Windows `netsh`). The device replies with its new URL when STA comes up — typically 5-10 s end to end.

**Device-model dropdown (pre-association injection)**: pick your device model next to the Firmware dropdown and the flow forwards `--device-model`: the script then resolves the deviceModel's `deviceModels.json` settings and pushes the TX-power cap over the `SET_TX_POWER` vendor RPC **before** the credentials, then applies the entry's modules and controls over serial as `APPLY_OP` ops (the same push the web installer does; the model name is one of those controls, `System.deviceModel`). One-way on boards whose LED pins include GPIO 1/3: once the driver claims the UART pins the board can no longer receive over serial, so a provisioned QuinLED board is reconfigured from its web UI, not by re-running this. This matters for brown-out-prone weak-powered device models (cap 8 dBm): at full TX power they fail their very first WiFi association, so the cap can't wait for the post-online HTTP injection. Leave the dropdown on "(any model)" for device models without special settings.

```bash
# Equivalent CLI for a weak-powered board (cap resolved from deviceModels.json):
uv run moondeck/build/improv_provision.py --port /dev/cu.usbmodem-XXX --device-model "ESP32-S3 N16R8 Dev"
# Or set the cap explicitly without a catalog entry:
uv run moondeck/build/improv_provision.py --port /dev/cu.usbmodem-XXX --tx-power 8
```

```bash
# Use host's currently-joined WiFi (one click in MoonDeck → equivalent CLI):
uv run moondeck/build/improv_provision.py --port /dev/tty.usbserial-XXXX

# Override SSID + password (rack / CI / different network):
uv run moondeck/build/improv_provision.py \
  --port /dev/tty.usbserial-XXXX \
  --ssid "MyWiFi" \
  --password "hunter2"

# Self-test the framing — no serial port needed (CI / pre-commit):
uv run moondeck/build/improv_provision.py --self-test
```

Exits 0 with `==> provisioned: http://<ip>/` on success. On a USB hub, shell-loop over the ports:

```bash
for port in /dev/tty.usbserial-*; do
  uv run moondeck/build/improv_provision.py --port "$port"
done
```

The host-WiFi reader lives at [moondeck/build/host_wifi.py](build/host_wifi.py) and runs standalone for diagnosis (`uv run moondeck/build/host_wifi.py` prints the resolved SSID + password). It first checks `moondeck/moondeck.json`'s active network's `wifi` block; if empty, falls back to OS auto-detect. The first macOS auto-detect run pops a Keychain access dialog — the OS doing its job; we don't try to bypass it. The retired `moondeck/build/wifi_credentials.json` source is gone — credentials now live per-network in moondeck.json, so moving the laptop between networks is just a dropdown switch.

Replaces v1's `deploy/wifi.py` + `deploy/flashfs.py --wifi` partition-baking flow — the device stays running, no flash mode required. Full module + protocol details: [docs/moonmodules/core/ImprovProvisioningModule.md](../docs/moonmodules/core/ImprovProvisioningModule.md).

### improv_probe

Non-destructive Improv health check. Sends `GET_DEVICE_INFO` + `GET_CURRENT_STATE` Improv RPCs and prints whatever the device reports — no credentials are exchanged, no WiFi state changes. Useful when ESP Web Tools shows the minimal popup instead of the rich panel and you want to know whether the device's Improv listener is actually answering on the wire.

**One-click flow**: pick the device's port in MoonDeck, hit **Improv Probe**. Typical output on a provisioned device:

```text
==> probing /dev/tty.usbserial-XXXX
    → GET_DEVICE_INFO
      firmware: 'MoonLight'
      version: '1.0.0-rc2'
      chip: 'ESP32'
      name: 'MM-BD3C'
    → GET_CURRENT_STATE
      state: provisioned
      url: http://192.168.1.207/
==> Improv healthy (device info + state + URL follow-up)
```

Exits 0 if both RPCs answered, 1 if the device didn't respond (Improv listener not running, wrong port, or a USB-CDC stall — try power-cycling). Reads `improv_provision.py`'s framing helpers, so the two scripts stay byte-identical on the wire.

### improv_smoke_test

End-to-end Improv test against a USB-connected ESP32. Three sequential checks; PASS only when all three pass within timeout:

1. **Probe** — device answers `GET_DEVICE_INFO` + `GET_CURRENT_STATE` (same checks `improv_probe` does standalone).
2. **Provision** — sends `WIFI_SETTINGS` with the host's resolved SSID + password and waits for the device to reach `PROVISIONED` (same flow `improv_provision` drives standalone).
3. **Reachable** — HTTP `GET /` on the device's reported URL, confirming the device actually joined the LAN. Skippable with `--no-network` for isolated provisioning networks the host can't route to.

**One-click flow**: pick the device's port in MoonDeck, hit **Improv Smoke Test**. Credentials come from the active network's `wifi` block (same source as Improv WiFi). Typical output:

```text
==> [1/3] probe   (timeout 10s)
  ==> probing /dev/tty.usbserial-XXXX
  ==> Improv healthy (device info + state)
==> [2/3] provision   (timeout 60s)
  ==> sending WIFI_SETTINGS to /dev/tty.usbserial-XXXX (SSID: 'MoonModules')
  ==> provisioned: http://192.168.1.207/
==> [3/3] network   GET http://192.168.1.207/   (timeout 10s)
     OK (HTTP 200)

PASS improv smoke test: probe + provision + reachable (took 12.4s)
     device: http://192.168.1.207/
```

Exit codes: `0` = all checks passed, `1` = device-side failure (probe or provision didn't complete), `2` = provision succeeded but device unreachable on LAN (distinct so CI can decide whether to retry).

**Why this exists.** The browser-side Improv flow (ESP Web Tools' modal) is awkward to automate and harder to reproduce on demand: needs Chrome, Web Serial, and a click-through. This script exercises the **device-side** Improv implementation — which is the part we own and the part most likely to break across firmware changes. ESP Web Tools' Improv handling is upstream-maintained and stable. Recommended pre-commit test for any change to:

- [src/core/util/ImprovFrame.h](../src/core/util/ImprovFrame.h) — the on-device parser
- [src/platform/esp32/platform_esp32_improv.cpp](../src/platform/esp32/platform_esp32_improv.cpp) — the UART listener task
- [mooninstaller/index.html](../mooninstaller/index.html) — the web installer page
- [src/ui/install-picker.js](../src/ui/install-picker.js) — the picker driving the install flow
- [moondeck/build/improv_*.py](build/) — the host-side framing helpers

Pair with `preview_installer`'s flash-ready mode (above) for a complete dev-environment proof that the install flow works before deploying to GitHub Pages.


### show_crash_log

Print the most recent MoonLight crash report and run log.

```bash
uv run moondeck/run/show_crash_log.py
```

On macOS, finds the newest `projectMM-*.ips` in `~/Library/Logs/DiagnosticReports/`, parses the JSON crash report, and prints the exception type, signal, faulting thread, and top 20 stack frames. If no crash report exists it falls back to the last 40 lines of `build/<host>/projectMM.log` so the run log is always reachable from one place.

Typical output (crash present):

```text
=== macOS crash report: MoonLight-2026-05-27-120000.ips ===
Type    : EXC_BAD_ACCESS — SIGSEGV
Subtype : KERN_INVALID_ADDRESS
PID     : 12345  uptime: 4321 ms
Captured: 2026-05-27T12:00:00Z

Faulting thread 0 (com.apple.main-thread):
  #0  mm::PreviewDriver::renderFrame()  +12
  #1  mm::Scheduler::tick()  +88
  ...
```

Typical output (no crash, log tail):

```text
No MoonLight crash reports found in DiagnosticReports.

=== Last 40 lines of projectMM.log ===
tick: 1234us (FPS: 800)  free: 0  ...
```
